#include "SpeechGenSynthCommandlet.h"

#include "Async/TaskGraphInterfaces.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SpeechGen/SpeechGenAudioCache.h"
#include "SpeechGen/SpeechGenProjectSettings.h"
#include "SpeechGen/SpeechGenSubsystem.h"
#include "SpeechGen/SpeechGenVoiceProfile.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogSpeechGenSynth, Log, All);

namespace
{
	constexpr double PumpTimeoutSeconds = 300.0;

	struct FSynthLine
	{
		FString OutPath;
		FString Voice;
		FString Text;
		float Speed = 1.0f;
	};

	struct FSynthTiming
	{
		FSynthLine Line;
		double WallMilliseconds = 0.0;
		double AudioSeconds = 0.0;
		FString Phonemes;
		bool bSucceeded = false;
		bool bFromCache = false;
	};

	void PumpGameThread()
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.001f);
	}

	bool ParseVoiceBlend(const FString& Spec, TArray<FSpeechGenVoiceWeight>& OutBlend, FString& OutError)
	{
		TArray<FString> Parts;
		Spec.ParseIntoArray(Parts, TEXT(","), true);
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			FString VoiceId = Part;
			FString WeightText;
			float Weight = 1.0f;
			if (Part.Split(TEXT(":"), &VoiceId, &WeightText))
			{
				Weight = FCString::Atof(*WeightText);
			}
			VoiceId.TrimStartAndEndInline();
			FSpeechGenVoiceWeight& Entry = OutBlend.AddDefaulted_GetRef();
			Entry.VoiceId = FName(*VoiceId);
			Entry.Weight = Weight;
		}
		if (OutBlend.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Voice blend '%s' is empty."), *Spec);
			return false;
		}
		return true;
	}

	bool WriteWav(const FString& Path, const TArray<int16>& Samples, const int32 SampleRate, FString& OutError)
	{
		const uint32 DataBytes = static_cast<uint32>(Samples.Num() * sizeof(int16));
		TArray<uint8> Bytes;
		Bytes.Reserve(44 + DataBytes);
		auto AppendTag = [&Bytes](const char* Tag) { Bytes.Append(reinterpret_cast<const uint8*>(Tag), 4); };
		auto Append16 = [&Bytes](const uint16 Value)
		{
			Bytes.Add(static_cast<uint8>(Value & 0xff));
			Bytes.Add(static_cast<uint8>(Value >> 8));
		};
		auto Append32 = [&Bytes](const uint32 Value)
		{
			for (int32 Shift = 0; Shift < 32; Shift += 8)
			{
				Bytes.Add(static_cast<uint8>((Value >> Shift) & 0xff));
			}
		};

		// Mono 16-bit PCM; samples are little-endian on every platform SpeechGen targets.
		AppendTag("RIFF");
		Append32(36 + DataBytes);
		AppendTag("WAVE");
		AppendTag("fmt ");
		Append32(16);
		Append16(1);
		Append16(1);
		Append32(static_cast<uint32>(SampleRate));
		Append32(static_cast<uint32>(SampleRate) * sizeof(int16));
		Append16(sizeof(int16));
		Append16(16);
		AppendTag("data");
		Append32(DataBytes);
		Bytes.Append(reinterpret_cast<const uint8*>(Samples.GetData()), DataBytes);

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
		{
			OutError = FString::Printf(TEXT("Unable to write %s"), *Path);
			return false;
		}
		return true;
	}

	FString CsvField(const FString& Value)
	{
		return TEXT("\"") + Value.Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
	}

	bool ReadBatch(const FString& BatchPath, const FString& OutDir, TArray<FSynthLine>& OutLines, FString& OutError)
	{
		TArray<FString> Rows;
		if (!FFileHelper::LoadFileToStringArray(Rows, *BatchPath))
		{
			OutError = FString::Printf(TEXT("Unable to read batch file %s"), *BatchPath);
			return false;
		}
		for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
		{
			const FString Row = Rows[RowIndex].TrimStartAndEnd();
			if (Row.IsEmpty() || Row.StartsWith(TEXT("#")))
			{
				continue;
			}
			TArray<FString> Fields;
			Rows[RowIndex].ParseIntoArray(Fields, TEXT("\t"), false);
			if (Fields.Num() < 3)
			{
				OutError = FString::Printf(TEXT("%s:%d needs out<TAB>voice<TAB>text[<TAB>speed]"),
					*BatchPath, RowIndex + 1);
				return false;
			}
			FSynthLine& Line = OutLines.AddDefaulted_GetRef();
			Line.OutPath = Fields[0].TrimStartAndEnd();
			if (FPaths::IsRelative(Line.OutPath))
			{
				Line.OutPath = FPaths::Combine(OutDir, Line.OutPath);
			}
			Line.Voice = Fields[1].TrimStartAndEnd();
			Line.Text = Fields[2].TrimStartAndEnd();
			if (Fields.Num() > 3 && !Fields[3].TrimStartAndEnd().IsEmpty())
			{
				Line.Speed = FCString::Atof(*Fields[3]);
			}
		}
		return true;
	}
}

namespace
{
	/**
	 * Phonemize-only report: for every word of every line, the IPA the model gets and where it came from
	 * (a project override, CMUdict, or Flite letter-to-sound), with all of CMUdict's variants, since the
	 * phonemizer keeps only the first and that is how "lead" the verb comes out as the metal.
	 */
	int32 RunPhonemizeReport(const USpeechGenSubsystem& Runtime, const FString& InputPath, const FString& ReportPath)
	{
		TArray<FString> Rows;
		if (!FFileHelper::LoadFileToStringArray(Rows, *InputPath))
		{
			UE_LOG(LogSpeechGenSynth, Error, TEXT("Unable to read %s"), *InputPath);
			return 1;
		}

		TMap<FString, TArray<FString>> Variants;
		TArray<FString> DictionaryLines;
		FFileHelper::LoadFileToStringArray(DictionaryLines,
			*FPaths::Combine(USpeechGenSubsystem::ResolveRuntimeDirectory(), TEXT("phonemizer/cmudict.dict")));
		for (const FString& Line : DictionaryLines)
		{
			int32 Space = INDEX_NONE;
			if (!Line.FindChar(TEXT(' '), Space) || Space <= 0)
			{
				continue;
			}
			FString Word = Line.Left(Space).ToLower();
			int32 Paren = INDEX_NONE;
			if (Word.FindLastChar(TEXT('('), Paren))
			{
				Word.LeftInline(Paren);
			}
			Variants.FindOrAdd(Word).Add(Line.Mid(Space + 1).TrimStartAndEnd());
		}
		TMap<FString, FString> Overrides;
		for (const FSpeechGenPronunciation& Entry : GetDefault<USpeechGenProjectSettings>()->PronunciationOverrides)
		{
			Overrides.Add(Entry.Word.TrimStartAndEnd().ToLower(), Entry.Arpabet);
		}

		FString Report = TEXT("line\tword\tsource\tipa\tcmudict variants\n");
		int32 Fallbacks = 0;
		int32 MultiVariant = 0;
		for (const FString& RawRow : Rows)
		{
			const FString Row = RawRow.TrimStartAndEnd();
			if (Row.IsEmpty() || Row.StartsWith(TEXT("#")))
			{
				continue;
			}
			TArray<FString> Words;
			FString Word;
			for (int32 Index = 0; Index <= Row.Len(); ++Index)
			{
				const TCHAR Character = Index < Row.Len() ? Row[Index] : TEXT(' ');
				if (FChar::IsAlpha(Character) || Character == TEXT('\''))
				{
					Word.AppendChar(Character);
				}
				else if (!Word.IsEmpty())
				{
					Words.AddUnique(Word);
					Word.Reset();
				}
			}
			for (const FString& Each : Words)
			{
				const FString Key = Each.ToLower();
				FString Ipa;
				FString Error;
				Runtime.PhonemizeText(ESpeechGenLanguage::English, Each, Ipa, Error);
				const TArray<FString>* Found = Variants.Find(Key);
				FString Source;
				if (Overrides.Contains(Key))
				{
					Source = TEXT("override");
				}
				else if (Found)
				{
					Source = Found->Num() > 1 ? FString::Printf(TEXT("cmudict, %d variants"), Found->Num()) : TEXT("cmudict");
					MultiVariant += Found->Num() > 1 ? 1 : 0;
				}
				else
				{
					Source = TEXT("letter-to-sound");
					++Fallbacks;
				}
				Report += FString::Printf(TEXT("%s\t%s\t%s\t%s\t%s\n"), *Row, *Each, *Source, *Ipa,
					Found ? *FString::Join(*Found, TEXT(" | ")) : TEXT(""));
			}
		}
		FFileHelper::SaveStringToFile(Report, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogSpeechGenSynth, Display, TEXT("Phonemize report %s: %d letter-to-sound words, %d with several CMUdict variants"),
			*ReportPath, Fallbacks, MultiVariant);
		return 0;
	}
}

USpeechGenSynthCommandlet::USpeechGenSynthCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 USpeechGenSynthCommandlet::Main(const FString& Params)
{
	const TCHAR* CommandLine = *Params;
	const bool bKeepCache = FParse::Param(CommandLine, TEXT("KeepCache"));
	TArray<FSynthLine> Lines;
	FString OutDir;
	FString TimingsPath;
	FString Error;

	FString PhonemizePath;
	FString ReportPath;
	FString BatchPath;
	if (FParse::Value(CommandLine, TEXT("Phonemize="), PhonemizePath))
	{
		PhonemizePath = FPaths::ConvertRelativePathToFull(PhonemizePath);
		if (!FParse::Value(CommandLine, TEXT("Report="), ReportPath))
		{
			ReportPath = FPaths::ChangeExtension(PhonemizePath, TEXT("report.tsv"));
		}
		ReportPath = FPaths::ConvertRelativePathToFull(ReportPath);
	}
	else if (FParse::Value(CommandLine, TEXT("Batch="), BatchPath))
	{
		BatchPath = FPaths::ConvertRelativePathToFull(BatchPath);
		if (!FParse::Value(CommandLine, TEXT("OutDir="), OutDir))
		{
			OutDir = FPaths::GetPath(BatchPath);
		}
		OutDir = FPaths::ConvertRelativePathToFull(OutDir);
		if (!FParse::Value(CommandLine, TEXT("Timings="), TimingsPath))
		{
			TimingsPath = FPaths::Combine(OutDir, TEXT("timings.csv"));
		}
		if (!ReadBatch(BatchPath, OutDir, Lines, Error))
		{
			UE_LOG(LogSpeechGenSynth, Error, TEXT("%s"), *Error);
			return 1;
		}
	}
	else
	{
		FSynthLine& Line = Lines.AddDefaulted_GetRef();
		FParse::Value(CommandLine, TEXT("Text="), Line.Text, false);
		FParse::Value(CommandLine, TEXT("Voice="), Line.Voice, false);
		FParse::Value(CommandLine, TEXT("Out="), Line.OutPath, false);
		FParse::Value(CommandLine, TEXT("Speed="), Line.Speed);
		FParse::Value(CommandLine, TEXT("Timings="), TimingsPath);
		if (Line.Text.IsEmpty() || Line.Voice.IsEmpty() || Line.OutPath.IsEmpty())
		{
			UE_LOG(LogSpeechGenSynth, Error,
				TEXT("Usage: -run=SpeechGenEditor.SpeechGenSynth -Text=\"...\" -Voice=am_adam:0.7,bm_george:0.3 -Out=out.wav [-Speed=1.0]")
				TEXT(" | -Batch=lines.tsv [-OutDir=dir] [-Timings=timings.csv] [-KeepCache]")
				TEXT(" | -Phonemize=words.txt [-Report=report.tsv]"));
			return 1;
		}
		Line.OutPath = FPaths::ConvertRelativePathToFull(Line.OutPath);
	}
	if (Lines.IsEmpty() && PhonemizePath.IsEmpty())
	{
		UE_LOG(LogSpeechGenSynth, Error, TEXT("Nothing to synthesize."));
		return 1;
	}

	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	Owner->AddToRoot();
	USpeechGenSubsystem* Runtime = USpeechGenSubsystem::CreateEditorPreviewRuntime(*Owner);
	Runtime->AddToRoot();
	UE_LOG(LogSpeechGenSynth, Display, TEXT("Runtime directory: %s"), *USpeechGenSubsystem::ResolveRuntimeDirectory());

	auto Shutdown = [Owner, Runtime]()
	{
		Runtime->OnSegmentReady.Clear();
		Runtime->OnRequestFailed.Clear();
		Runtime->StopRuntime();
		Runtime->RemoveFromRoot();
		Owner->RemoveFromRoot();
	};

	const double LoadStartedAtSeconds = FPlatformTime::Seconds();
	Runtime->StartRuntime();
	while (Runtime->GetRuntimeStatus().State == ESpeechGenRuntimeState::Loading
		&& FPlatformTime::Seconds() - LoadStartedAtSeconds < PumpTimeoutSeconds)
	{
		PumpGameThread();
	}
	const double LoadMilliseconds = (FPlatformTime::Seconds() - LoadStartedAtSeconds) * 1000.0;
	if (Runtime->GetRuntimeStatus().State != ESpeechGenRuntimeState::Ready)
	{
		UE_LOG(LogSpeechGenSynth, Error, TEXT("Runtime did not become ready after %.0f ms: %s"),
			LoadMilliseconds, *Runtime->GetRuntimeStatus().Error);
		Shutdown();
		return 1;
	}
	UE_LOG(LogSpeechGenSynth, Display, TEXT("Model load: %.1f ms"), LoadMilliseconds);
	if (!PhonemizePath.IsEmpty())
	{
		const int32 Result = RunPhonemizeReport(*Runtime, PhonemizePath, ReportPath);
		Shutdown();
		return Result;
	}

	FGuid ActiveSegmentId;
	bool bRequestDone = false;
	FSpeechGenResult ReceivedResult;
	FString RequestError;
	Runtime->OnSegmentReady.AddLambda([&](const FSpeechGenResult& Result)
	{
		if (Result.SegmentId == ActiveSegmentId)
		{
			ReceivedResult = Result;
			bRequestDone = true;
		}
	});
	Runtime->OnRequestFailed.AddLambda([&](FGuid, const FGuid SegmentId, const FString& Message)
	{
		if (SegmentId == ActiveSegmentId)
		{
			RequestError = Message.IsEmpty() ? TEXT("Request failed.") : Message;
			bRequestDone = true;
		}
	});

	TArray<FSynthTiming> Timings;
	for (const FSynthLine& Line : Lines)
	{
		FSynthTiming& Timing = Timings.AddDefaulted_GetRef();
		Timing.Line = Line;

		FSpeechGenRequest Request;
		if (!ParseVoiceBlend(Line.Voice, Request.VoiceBlend, Error))
		{
			UE_LOG(LogSpeechGenSynth, Error, TEXT("%s"), *Error);
			continue;
		}
		const TOptional<ESpeechGenLanguage> Language =
			USpeechGenVoiceProfile::GetVoiceLanguage(Request.VoiceBlend[0].VoiceId);
		if (!Language.IsSet())
		{
			UE_LOG(LogSpeechGenSynth, Error, TEXT("Unknown voice: %s"), *Request.VoiceBlend[0].VoiceId.ToString());
			continue;
		}
		if (!bKeepCache)
		{
			IFileManager::Get().DeleteDirectory(*FSpeechGenAudioCache::GetGeneratedDirectory(), false, true);
		}

		Request.TurnId = FGuid::NewGuid();
		Request.SegmentId = FGuid::NewGuid();
		Request.Text = Line.Text;
		Request.Language = Language.GetValue();
		Request.Speed = Line.Speed;
		Request.bEnableDiagnostics = true;
		ActiveSegmentId = Request.SegmentId;
		bRequestDone = false;
		RequestError.Reset();

		const double StartedAtSeconds = FPlatformTime::Seconds();
		Runtime->SynthesizeAsync(Request);
		while (!bRequestDone && FPlatformTime::Seconds() - StartedAtSeconds < PumpTimeoutSeconds)
		{
			PumpGameThread();
		}
		Timing.WallMilliseconds = (FPlatformTime::Seconds() - StartedAtSeconds) * 1000.0;

		if (!bRequestDone || !RequestError.IsEmpty())
		{
			UE_LOG(LogSpeechGenSynth, Error, TEXT("Failed \"%s\": %s"), *Line.Text,
				bRequestDone ? *RequestError : TEXT("timed out"));
			continue;
		}
		if (!WriteWav(Line.OutPath, ReceivedResult.PcmSamples, ReceivedResult.SampleRate, Error))
		{
			UE_LOG(LogSpeechGenSynth, Error, TEXT("%s"), *Error);
			continue;
		}
		Timing.AudioSeconds = ReceivedResult.DurationSeconds;
		Timing.Phonemes = ReceivedResult.Phonemes;
		Timing.bFromCache = ReceivedResult.Source != ESpeechGenResultSource::Synthesized;
		Timing.bSucceeded = true;
		UE_LOG(LogSpeechGenSynth, Display, TEXT("Wrote %s | %.1f ms for %.2f s audio | RTF %.3f%s | \"%s\""),
			*Line.OutPath, Timing.WallMilliseconds, Timing.AudioSeconds,
			Timing.AudioSeconds > 0.0 ? Timing.WallMilliseconds / 1000.0 / Timing.AudioSeconds : 0.0,
			Timing.bFromCache ? TEXT(" (cache)") : TEXT(""), *Line.Text);
		UE_LOG(LogSpeechGenSynth, Display, TEXT("  phonemes: %s"), *Timing.Phonemes);
	}

	int32 Failures = 0;
	double WarmWallMilliseconds = 0.0;
	double WarmAudioSeconds = 0.0;
	int32 WarmCount = 0;
	for (int32 Index = 0; Index < Timings.Num(); ++Index)
	{
		const FSynthTiming& Timing = Timings[Index];
		if (!Timing.bSucceeded)
		{
			++Failures;
		}
		else if (Index > 0 && !Timing.bFromCache)
		{
			WarmWallMilliseconds += Timing.WallMilliseconds;
			WarmAudioSeconds += Timing.AudioSeconds;
			++WarmCount;
		}
	}
	const FSynthTiming& First = Timings[0];
	UE_LOG(LogSpeechGenSynth, Display,
		TEXT("Summary: load %.1f ms | first line %.1f ms (RTF %.3f) | warm lines %d, mean %.1f ms, RTF %.3f | failures %d"),
		LoadMilliseconds, First.WallMilliseconds,
		First.AudioSeconds > 0.0 ? First.WallMilliseconds / 1000.0 / First.AudioSeconds : 0.0,
		WarmCount, WarmCount > 0 ? WarmWallMilliseconds / WarmCount : 0.0,
		WarmAudioSeconds > 0.0 ? WarmWallMilliseconds / 1000.0 / WarmAudioSeconds : 0.0, Failures);

	if (!TimingsPath.IsEmpty())
	{
		FString Csv = FString::Printf(TEXT("# model_load_ms=%.1f\nindex,out,voice,speed,text,wall_ms,audio_s,rtf,source,ok,phonemes\n"),
			LoadMilliseconds);
		for (int32 Index = 0; Index < Timings.Num(); ++Index)
		{
			const FSynthTiming& Timing = Timings[Index];
			Csv += FString::Printf(TEXT("%d,%s,%s,%.2f,%s,%.1f,%.3f,%.4f,%s,%d,%s\n"), Index,
				*CsvField(Timing.Line.OutPath), *CsvField(Timing.Line.Voice), Timing.Line.Speed,
				*CsvField(Timing.Line.Text), Timing.WallMilliseconds, Timing.AudioSeconds,
				Timing.AudioSeconds > 0.0 ? Timing.WallMilliseconds / 1000.0 / Timing.AudioSeconds : 0.0,
				Timing.bFromCache ? TEXT("cache") : TEXT("synth"), Timing.bSucceeded ? 1 : 0,
				*CsvField(Timing.Phonemes));
		}
		// IPA would make AutoDetect pick UTF-16; keep the CSV readable by ordinary tools.
		FFileHelper::SaveStringToFile(Csv, *FPaths::ConvertRelativePathToFull(TimingsPath),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	Shutdown();
	return Failures > 0 ? 1 : 0;
}
