#include "SpeechGenRadioDemo.h"

#include "AudioDevice.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SpeechGen/SpeechGenAudioCache.h"
#include "SpeechGen/SpeechGenSubsystem.h"
#include "SpeechGenRadio/SpeechGenRadioChannel.h"

DEFINE_LOG_CATEGORY_STATIC(LogSpeechGenRadio, Log, All);

namespace
{
	struct FCastMember
	{
		const TCHAR* CallSign;
		const TCHAR* Voice;
		float Speed;
	};

	enum ESpeaker : int32 { Dispatch, EchoTwo, KiloFive, SierraNine, VictorThree };

	// The cast, as in Tools/RadioFx/dispatch-traffic.tsv: the male voices sit at least 25 Hz of median pitch apart.
	// Speeds sit near 1.0; pitch carries the separation.
	const FCastMember Crew[] = {
		{TEXT("Dispatch"), TEXT("am_michael:0.7,bm_george:0.3"), 1.0f},
		{TEXT("Echo Two"), TEXT("am_eric:0.7,am_santa:0.3"), 1.0f},
		{TEXT("Kilo Five"), TEXT("af_bella:0.7,af_sarah:0.3"), 1.0f},
		{TEXT("Sierra Nine"), TEXT("am_onyx:0.7,bm_lewis:0.3"), 0.95f},
		{TEXT("Victor Three"), TEXT("bf_emma:0.7,bf_isabella:0.3"), 1.0f},
	};

	/**
	 * How a unit opens a transmission:
	 * Call      first contact: call Dispatch, wait for "go ahead", then the traffic.
	 * FollowUp  an update on an established call: one transmission, no handshake.
	 * Reply     traffic answering earlier traffic (e.g. a broadcast): one shot if it comes seconds after the
	 *           related transmission, a full call if minutes later. The decision keys on sim time.
	 * Literal   Dispatch's lines and anything already in final form.
	 */
	enum class EForm : uint8 { Literal, Call, FollowUp, Reply };

	struct FTraffic
	{
		int32 Speaker;
		EForm Form;
		const TCHAR* Text;
		double SimAt;
		int32 RelatedTo;
		float Gap;
	};

	// A reply seconds after the related traffic is one shot, minutes later is a full call. 30 s is the default
	// cutoff; -SpeechGenRadioHailAfter=<s> sets it.
	constexpr double DefaultHailAfterSimSeconds = 30.0;

	// Two seconds between transmissions leaves room for priority traffic to break in; a priority call goes as soon as
	// the channel is clear, and Dispatch answers it at once. "[pNNN]" is SpeechGen pause markup, used only between
	// clauses of long lines: a short chunk cut off from the words after it loses vowel quality, so calls, replies and
	// leading call signs stay whole.
	constexpr float Gap = 2.0f;
	const FTraffic Traffic[] = {
		{EchoTwo, EForm::Call, TEXT("I need medical at the north gate, [p250] for a visitor with a twisted ankle."), 0.0, -1, 0.0f},
		{Dispatch, EForm::Literal, TEXT("Copy, sending medical to the north gate, [p200] for a twisted ankle."), 20.0, -1, Gap + 0.2f},
		{KiloFive, EForm::Call, TEXT("We've got a noise complaint at the loading dock. [p250] A generator running, facing the hotel."), 120.0, -1, Gap + 0.4f},
		{Dispatch, EForm::Literal, TEXT("Copy, Kilo Five. See if they'll shut it down."), 125.0, -1, Gap},
		{EchoTwo, EForm::FollowUp, TEXT("medical is on scene and released us. [p250] Resuming patrol."), 260.0, 0, Gap + 0.6f},
		{Dispatch, EForm::Literal, TEXT("Copy, Echo Two."), 262.0, -1, Gap},
		{SierraNine, EForm::Literal, TEXT("Priority, priority, priority."), 400.0, -1, 0.8f},
		{Dispatch, EForm::Literal, TEXT("Station with priority traffic, go ahead."), 402.0, -1, 1.0f},
		{SierraNine, EForm::Literal, TEXT("Dispatch, Sierra Nine: missing person at the main entrance. [p300] Elderly man, [p200] grey coat, [p200] walking cane."), 405.0, -1, 1.2f},
		{Dispatch, EForm::Literal, TEXT("All stations, all stations, this is Dispatch. We have a missing person, [p200] last seen at the main entrance. [p300] Grey coat, [p200] walking cane. [p400] Please hold routine traffic."), 420.0, -1, Gap},
		{Dispatch, EForm::Literal, TEXT("Sierra Nine, Dispatch: anything else you can tell me about him?"), 425.0, -1, Gap + 0.4f},
		{SierraNine, EForm::Literal, TEXT("Dispatch, Sierra Nine: his name is Walter. [p300] Last seen about ten minutes ago, [p200] heading for the parking garage."), 430.0, -1, Gap},
		{VictorThree, EForm::Reply, TEXT("I have eyes on possible missing person with grey coat."), 440.0, 9, Gap + 0.2f},
		{Dispatch, EForm::Literal, TEXT("Copy, Victor Three. Stay with him."), 442.0, -1, Gap},
		{VictorThree, EForm::Reply, TEXT("the man is confirmed, [p200] and reunited with his family at the main entrance."), 600.0, 9, Gap + 0.6f},
		{Dispatch, EForm::Literal, TEXT("All stations, all stations, this is Dispatch. The missing person is found. [p300] Thank you all. [p400] You may resume routine traffic."), 615.0, -1, Gap + 0.2f},
		{KiloFive, EForm::FollowUp, TEXT("they shut it down. [p250] We're clear, [p150] resuming patrol."), 640.0, 2, Gap + 0.4f},
		{Dispatch, EForm::Literal, TEXT("Copy, Kilo Five. Thanks for taking care of that."), 642.0, -1, Gap},
	};

	// The grit ladder: the same call and broadcast at each grit.
	const TCHAR* LadderCall = TEXT("Dispatch, Echo Two.");
	const TCHAR* LadderBroadcast = TEXT("All stations, all stations, this is Dispatch. We have a missing person, [p200] last seen at the main entrance. [p300] Grey coat, [p200] walking cane.");

	FString Capitalized(const FString& Text)
	{
		return Text.IsEmpty() ? Text : Text.Left(1).ToUpper() + Text.Mid(1);
	}

	void ParseVoiceBlend(const FString& Spec, TArray<FSpeechGenVoiceWeight>& OutBlend)
	{
		TArray<FString> Parts;
		Spec.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Part : Parts)
		{
			FString VoiceId = Part;
			FString Weight = TEXT("1");
			Part.Split(TEXT(":"), &VoiceId, &Weight);
			FSpeechGenVoiceWeight& Entry = OutBlend.AddDefaulted_GetRef();
			Entry.VoiceId = FName(*VoiceId.TrimStartAndEnd());
			Entry.Weight = FCString::Atof(*Weight);
		}
	}

	// Playback is scheduled on the audio device's clock, which follows the audio actually rendered.
	// It matches the wall clock on a hardware device; the non-realtime renderer can run ahead of it.
	double PlaybackClock(const UWorld* World, const double WallNow)
	{
		const FAudioDevice* AudioDevice = World ? World->GetAudioDeviceRaw() : nullptr;
		return AudioDevice ? AudioDevice->GetAudioClock() : WallNow;
	}

	double Percentile(TArray<double> Values, const double Fraction)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Index = FMath::Clamp(FMath::CeilToInt(Fraction * Values.Num()) - 1, 0, Values.Num() - 1);
		return Values[Index];
	}
}

bool USpeechGenRadioDemo::ShouldCreateSubsystem(UObject* Outer) const
{
	return FParse::Param(FCommandLine::Get(), TEXT("SpeechGenBench"))
		|| FParse::Param(FCommandLine::Get(), TEXT("SpeechGenRadioDemo"));
}

void USpeechGenRadioDemo::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	SpeechGen = Collection.InitializeDependency<USpeechGenSubsystem>();
	const TCHAR* CommandLine = FCommandLine::Get();
	bBench = FParse::Param(CommandLine, TEXT("SpeechGenBench"));
	bRecord = FParse::Param(CommandLine, TEXT("SpeechGenRadioRecord"));
	if (!FParse::Value(CommandLine, TEXT("SpeechGenOut="), OutDir))
	{
		OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SpeechGenOut/engine"));
	}
	OutDir = FPaths::ConvertRelativePathToFull(OutDir);
	IFileManager::Get().MakeDirectory(*OutDir, true);
	FParse::Value(CommandLine, TEXT("SpeechGenSimWorkMs="), SimWorkTargetMs);

	bLadder = FParse::Param(CommandLine, TEXT("SpeechGenRadioLadder"));
	HailAfterSimSeconds = DefaultHailAfterSimSeconds;
	FParse::Value(CommandLine, TEXT("SpeechGenRadioHailAfter="), HailAfterSimSeconds);
	FString GritName = TEXT("mid");
	FParse::Value(CommandLine, TEXT("SpeechGenRadioGrit="), GritName);
	const ESpeechGenRadioGrit Grit = GritName == TEXT("light") ? ESpeechGenRadioGrit::Light
		: GritName == TEXT("heavy") ? ESpeechGenRadioGrit::Heavy : ESpeechGenRadioGrit::Mid;
	FString LadderSpec;
	if (FParse::Value(CommandLine, TEXT("SpeechGenRadioLadderSpec="), LadderSpec, false))
	{
		bLadder = true;
		TArray<FString> Specs;
		LadderSpec.TrimQuotes().ParseIntoArray(Specs, TEXT(";"), true);
		for (int32 Index = 0; Index < Specs.Num(); ++Index)
		{
			TArray<FString> Values;
			Specs[Index].ParseIntoArray(Values, TEXT(","), true);
			if (Values.Num() < 6)
			{
				continue;
			}
			FSpeechGenRadioChainSettings Chain;
			Chain.CompressorThresholdDb = FCString::Atof(*Values[0]);
			Chain.CompressorRatio = FCString::Atof(*Values[1]);
			Chain.CompressorMakeupDb = FCString::Atof(*Values[2]);
			Chain.SaturationAmount = FCString::Atof(*Values[3]);
			Chain.SaturationOutputDb = FCString::Atof(*Values[4]);
			Chain.HissDb = FCString::Atof(*Values[5]);
			if (Values.Num() >= 8)
			{
				Chain.HighPassHz = FCString::Atof(*Values[6]);
				Chain.LowPassHz = FCString::Atof(*Values[7]);
			}
			FPass& Pass = Passes.AddDefaulted_GetRef();
			Pass.Name = FString::Printf(TEXT("sweep-%02d"), Index + 1);
			Pass.Chain = Chain;
		}
	}
	else if (bLadder)
	{
		Passes = {
			{1.0f, ESpeechGenRadioGrit::Light, TEXT("grit-1-light")},
			{1.0f, ESpeechGenRadioGrit::Mid, TEXT("grit-2-mid")},
			{1.0f, ESpeechGenRadioGrit::Heavy, TEXT("grit-3-heavy")}
		};
	}
	else
	{
		FString ClarityList = TEXT("1.0");
		FParse::Value(CommandLine, TEXT("SpeechGenRadioClarity="), ClarityList, false);
		TArray<FString> ClarityParts;
		ClarityList.ParseIntoArray(ClarityParts, TEXT(","), true);
		for (const FString& Part : ClarityParts)
		{
			FPass& Pass = Passes.AddDefaulted_GetRef();
			Pass.Clarity = FMath::Clamp(FCString::Atof(*Part), 0.0f, 1.0f);
			Pass.Grit = Grit;
			Pass.Name = FString::Printf(TEXT("radio-%s-clarity-%03d"), *GritName, FMath::RoundToInt(Pass.Clarity * 100.0f));
		}
	}

	if (USpeechGenSubsystem* Runtime = SpeechGen.Get())
	{
		Runtime->OnSegmentReady.AddUObject(this, &USpeechGenRadioDemo::HandleSegmentReady);
		Runtime->OnRequestFailed.AddUObject(this, &USpeechGenRadioDemo::HandleRequestFailed);
	}
	Channel = NewObject<USpeechGenRadioChannel>(this);
	BuildScript();

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USpeechGenRadioDemo::Tick));
	SamplingInputHandle = FCoreDelegates::OnSamplingInput.AddUObject(this, &USpeechGenRadioDemo::HandleSamplingInput);
	EndFrameHandle = FCoreDelegates::OnEndFrame.AddUObject(this, &USpeechGenRadioDemo::HandleEndFrame);

	FString Threading;
	GConfig->GetString(TEXT("/Script/NNERuntimeORT.NNERuntimeORTSettings"), TEXT("EditorThreadingOptions"), Threading, GEngineIni);
	UE_LOG(LogSpeechGenRadio, Display, TEXT("SpeechGen %s: %d lines, %d pass(es), out %s, ORT editor threading %s"),
		bBench ? TEXT("bench") : TEXT("radio demo"), Lines.Num(), Passes.Num(), *OutDir, Threading.IsEmpty() ? TEXT("(default)") : *Threading);
}

void USpeechGenRadioDemo::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	FCoreDelegates::OnSamplingInput.Remove(SamplingInputHandle);
	FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
	if (USpeechGenSubsystem* Runtime = SpeechGen.Get())
	{
		Runtime->OnSegmentReady.RemoveAll(this);
		Runtime->OnRequestFailed.RemoveAll(this);
	}
	if (Channel)
	{
		Channel->StopAll();
	}
	Super::Deinitialize();
}

void USpeechGenRadioDemo::BuildScript()
{
	auto Add = [this](const int32 Speaker, const FString& Text, const float GapBefore, const FString& Note)
	{
		FLine& Line = Lines.AddDefaulted_GetRef();
		Line.Speaker = Speaker;
		Line.Text = Text;
		Line.GapBefore = GapBefore;
		Line.Note = Note;
	};
	auto AddCall = [&](const FTraffic& Entry, const FString& Note)
	{
		const FString CallSign = Crew[Entry.Speaker].CallSign;
		Add(Entry.Speaker, FString::Printf(TEXT("Dispatch, %s."), *CallSign), Entry.Gap, Note);
		Add(Dispatch, FString::Printf(TEXT("%s, go ahead."), *CallSign), Gap, TEXT(""));
		Add(Entry.Speaker, Capitalized(Entry.Text), Gap, TEXT(""));
	};
	auto AddOneShot = [&](const FTraffic& Entry, const FString& Note)
	{
		Add(Entry.Speaker, FString::Printf(TEXT("Dispatch, %s, %s"), Crew[Entry.Speaker].CallSign, Entry.Text), Entry.Gap, Note);
	};

	if (bLadder)
	{
		Add(EchoTwo, LadderCall, 0.0f, TEXT("grit ladder: call"));
		Add(Dispatch, LadderBroadcast, Gap, TEXT("grit ladder: broadcast"));
		return;
	}

	for (const FTraffic& Entry : Traffic)
	{
		switch (Entry.Form)
		{
		case EForm::Literal:
			Add(Entry.Speaker, Entry.Text, Entry.Gap, TEXT(""));
			break;
		case EForm::Call:
			AddCall(Entry, TEXT("first contact: call and wait"));
			break;
		case EForm::FollowUp:
			AddOneShot(Entry, TEXT("follow-up on an established call: one shot"));
			break;
		case EForm::Reply:
		{
			const double Since = Entry.SimAt - Traffic[Entry.RelatedTo].SimAt;
			if (Since > HailAfterSimSeconds)
			{
				AddCall(Entry, FString::Printf(TEXT("reply %.0f sim seconds after the related traffic: call and wait"), Since));
			}
			else
			{
				AddOneShot(Entry, FString::Printf(TEXT("reply %.0f sim seconds after the related traffic: one shot"), Since));
			}
			break;
		}
		}
	}
}

void USpeechGenRadioDemo::SubmitAll()
{
	USpeechGenSubsystem* Runtime = SpeechGen.Get();
	if (!Runtime)
	{
		return;
	}
	// The generated cache would turn repeat runs into file reads; every line is a real inference here.
	IFileManager::Get().DeleteDirectory(*FSpeechGenAudioCache::GetGeneratedDirectory(), false, true);
	for (FLine& Line : Lines)
	{
		FSpeechGenRequest Request;
		Request.TurnId = FGuid::NewGuid();
		Request.SegmentId = FGuid::NewGuid();
		Request.Text = Line.Text;
		Request.Language = ESpeechGenLanguage::English;
		Request.Speed = Crew[Line.Speaker].Speed;
		ParseVoiceBlend(Crew[Line.Speaker].Voice, Request.VoiceBlend);
		Line.SegmentId = Request.SegmentId;
		Line.SubmittedAt = FPlatformTime::Seconds();
		Runtime->SynthesizeAsync(Request);
	}
}

void USpeechGenRadioDemo::SetPhase(const EPhase InPhase)
{
	Phase = InPhase;
	PhaseStartedAt = FPlatformTime::Seconds();
	UE_LOG(LogSpeechGenRadio, Display, TEXT("Phase: %s"), PhaseName(InPhase));
}

bool USpeechGenRadioDemo::Tick(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	const UGameInstance* GameInstance = GetGameInstance();
	UWorld* World = GameInstance ? GameInstance->GetWorld() : nullptr;
	USpeechGenSubsystem* Runtime = SpeechGen.Get();

	switch (Phase)
	{
	case EPhase::Boot:
		if (World && World->HasBegunPlay() && Runtime)
		{
			if (bBench)
			{
				SetPhase(EPhase::Idle);
			}
			else
			{
				SetPhase(EPhase::Load);
				Runtime->StartRuntime();
			}
		}
		break;
	case EPhase::Idle:
		// The first frames calibrate the simulated work; the rest is the idle baseline.
		if (SimIterations > 0 && Now - PhaseStartedAt > 3.0)
		{
			SetPhase(EPhase::Load);
			Runtime->StartRuntime();
		}
		break;
	case EPhase::Load:
	{
		const ESpeechGenRuntimeState State = Runtime->GetRuntimeStatus().State;
		if (State == ESpeechGenRuntimeState::Ready)
		{
			LoadMilliseconds = (Now - PhaseStartedAt) * 1000.0;
			UE_LOG(LogSpeechGenRadio, Display, TEXT("Model ready after %.1f ms"), LoadMilliseconds);
			if (bBench)
			{
				SetPhase(EPhase::Settle);
			}
			else
			{
				SetPhase(EPhase::Prepare);
				SubmitAll();
			}
		}
		else if (State == ESpeechGenRuntimeState::Failed || State == ESpeechGenRuntimeState::Unavailable)
		{
			UE_LOG(LogSpeechGenRadio, Error, TEXT("Runtime failed to load: %s"), *Runtime->GetRuntimeStatus().Error);
			Finish(1);
		}
		break;
	}
	case EPhase::Settle:
		if (Now - PhaseStartedAt > 2.0)
		{
			SetPhase(EPhase::Synth);
			SubmitAll();
		}
		break;
	case EPhase::Synth:
		if (LinesReady + LinesFailed == Lines.Num())
		{
			SetPhase(EPhase::Tail);
		}
		break;
	case EPhase::Tail:
		if (Now - PhaseStartedAt > 1.0)
		{
			WriteBenchReport();
			Finish(LinesFailed > 0 ? 1 : 0);
		}
		break;
	case EPhase::Prepare:
		// Synthesis runs several times faster than playback, so the exchange starts with the first line and the
		// rest arrive before they're due; TickPlay waits on any line that isn't.
		if (LinesFailed > 0)
		{
			Finish(1);
			break;
		}
		if (!Lines.IsEmpty() && Lines[0].SampleRate > 0)
		{
			UE_LOG(LogSpeechGenRadio, Display, TEXT("First line ready %.1f s after the model; playing %d pass(es)"),
				Now - PhaseStartedAt, Passes.Num());
			SetPhase(EPhase::Play);
			PassIndex = 0;
			NextLine = 0;
			NextStartAt = PlaybackClock(World, Now) + 0.5;
		}
		break;
	case EPhase::Play:
		TickPlay(PlaybackClock(World, Now));
		break;
	case EPhase::Done:
		break;
	}

	if (bBench)
	{
		RunSimWork();
	}
	return true;
}

void USpeechGenRadioDemo::RunSimWork()
{
	constexpr int64 CalibrationIterations = 200000;
	const bool bCalibrating = SimIterations == 0;
	if (bCalibrating && Phase != EPhase::Idle)
	{
		LastSimMs = 0.0;
		return;
	}
	const int64 Iterations = bCalibrating ? CalibrationIterations : SimIterations;
	const double StartedAt = FPlatformTime::Seconds();
	double Accumulator = SimSink;
	for (int64 Index = 0; Index < Iterations; ++Index)
	{
		Accumulator = Accumulator * 0.9999999 + FMath::Sqrt(static_cast<double>(Index & 1023));
	}
	SimSink = Accumulator;
	LastSimMs = (FPlatformTime::Seconds() - StartedAt) * 1000.0;

	if (bCalibrating && ++CalibrationFrames > 3)
	{
		CalibrationMs += LastSimMs;
		if (CalibrationFrames == 18)
		{
			SimIterations = FMath::Max<int64>(1, static_cast<int64>(CalibrationIterations * SimWorkTargetMs / (CalibrationMs / 15.0)));
			PhaseStartedAt = FPlatformTime::Seconds();
			UE_LOG(LogSpeechGenRadio, Display, TEXT("Simulated game work: %lld iterations for %.1f ms"), SimIterations, SimWorkTargetMs);
		}
	}
}

void USpeechGenRadioDemo::HandleSamplingInput()
{
	const double Now = FPlatformTime::Seconds();
	WorkStartedAt = Now;
	if (LastSamplingAt > 0.0)
	{
		FFrame& Frame = Frames.AddDefaulted_GetRef();
		Frame.PeriodMs = (Now - LastSamplingAt) * 1000.0;
	}
	LastSamplingAt = Now;
}

void USpeechGenRadioDemo::HandleEndFrame()
{
	if (Frames.IsEmpty() || WorkStartedAt <= 0.0)
	{
		return;
	}
	FFrame& Frame = Frames.Last();
	// Calibration frames carry a different workload; keep them out of the idle baseline.
	Frame.Phase = Phase == EPhase::Idle && SimIterations == 0 ? EPhase::Boot : Phase;
	Frame.WorkMs = (FPlatformTime::Seconds() - WorkStartedAt) * 1000.0;
	Frame.SimMs = LastSimMs;
	Frame.Results = ResultsThisFrame;
	ResultsThisFrame = 0;
}

void USpeechGenRadioDemo::HandleSegmentReady(const FSpeechGenResult& Result)
{
	for (FLine& Line : Lines)
	{
		if (Line.SegmentId == Result.SegmentId)
		{
			Line.Pcm = Result.PcmSamples;
			Line.SampleRate = Result.SampleRate;
			Line.ReadyAt = FPlatformTime::Seconds();
			++LinesReady;
			++ResultsThisFrame;
			return;
		}
	}
}

void USpeechGenRadioDemo::HandleRequestFailed(FGuid TurnId, const FGuid SegmentId, const FString& Error)
{
	for (const FLine& Line : Lines)
	{
		if (Line.SegmentId == SegmentId)
		{
			UE_LOG(LogSpeechGenRadio, Error, TEXT("Synthesis failed for \"%s\": %s"), *Line.Text, *Error);
			++LinesFailed;
			return;
		}
	}
}

void USpeechGenRadioDemo::TickPlay(const double Now)
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}
	if (PassIndex >= Passes.Num())
	{
		if (Now > NextStartAt)
		{
			Finish(0);
		}
		return;
	}

	const FPass& Pass = Passes[PassIndex];
	const float Clarity = Pass.Clarity;
	if (NextLine == 0 && Now >= NextStartAt - 0.4 && bRecord && !bPassRecording)
	{
		UAudioMixerBlueprintLibrary::StartRecordingOutput(World, 300.0f);
		bPassRecording = true;
	}
	if (NextLine < Lines.Num())
	{
		if (LinesFailed > 0)
		{
			Finish(1);
			return;
		}
		if (Lines[NextLine].SampleRate <= 0)
		{
			// Not synthesized yet: hold the channel until it is.
			NextStartAt = FMath::Max(NextStartAt, Now);
			return;
		}
		if (Now >= NextStartAt)
		{
			if (NextLine == 0)
			{
				Channel->Configure(Pass.Chain.IsSet() ? Pass.Chain.GetValue() : FSpeechGenRadioChainSettings::ForGrit(Pass.Grit));
			}
			const FLine& Line = Lines[NextLine];
			FSpeechGenRadioTransmissionSettings Settings;
			Settings.Clarity = Clarity;
			Settings.Seed = PassIndex * 1000 + NextLine;
			int32 Dropouts = 0;
			const float Seconds = Channel->Transmit(World, Line.Pcm, Line.SampleRate, Settings, &Dropouts);
			UE_LOG(LogSpeechGenRadio, Display, TEXT("[%s] %s: %s (%.2f s, %d dropouts)%s%s"), *Pass.Name,
				Crew[Line.Speaker].CallSign, *Line.Text, Seconds, Dropouts, Line.Note.IsEmpty() ? TEXT("") : TEXT("  <- "), *Line.Note);
			++NextLine;
			PassEndsAt = Now + Seconds;
			NextStartAt = PassEndsAt + (NextLine < Lines.Num() ? Lines[NextLine].GapBefore : 0.0f);
		}
	}
	else if (Now >= PassEndsAt + 0.6)
	{
		if (bPassRecording)
		{
			UAudioMixerBlueprintLibrary::StopRecordingOutput(World, EAudioRecordingExportType::WavFile, Pass.Name, OutDir);
			UE_LOG(LogSpeechGenRadio, Display, TEXT("Recorded %s/%s.wav"), *OutDir, *Pass.Name);
			bPassRecording = false;
		}
		++PassIndex;
		NextLine = 0;
		NextStartAt = Now + (PassIndex < Passes.Num() ? 2.0 : 1.0);
	}
}

void USpeechGenRadioDemo::WriteBenchReport() const
{
	FString Csv = TEXT("frame,phase,period_ms,work_ms,sim_ms,results\n");
	TMap<EPhase, TArray<const FFrame*>> ByPhase;
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		const FFrame& Frame = Frames[Index];
		Csv += FString::Printf(TEXT("%d,%s,%.3f,%.3f,%.3f,%d\n"), Index, PhaseName(Frame.Phase), Frame.PeriodMs,
			Frame.WorkMs, Frame.SimMs, Frame.Results);
		ByPhase.FindOrAdd(Frame.Phase).Add(&Frame);
	}
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(OutDir, TEXT("bench-frames.csv")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	FString Threading;
	GConfig->GetString(TEXT("/Script/NNERuntimeORT.NNERuntimeORTSettings"), TEXT("EditorThreadingOptions"), Threading, GEngineIni);
	FString Summary = FString::Printf(TEXT("ORT editor threading: %s\nmodel load: %.1f ms\n"),
		Threading.IsEmpty() ? TEXT("(default)") : *Threading, LoadMilliseconds);
	Summary += TEXT("phase    frames  period med/p99/max ms     work med/p99/max ms      sim med/p99/max ms\n");
	for (const EPhase Each : {EPhase::Idle, EPhase::Load, EPhase::Settle, EPhase::Synth, EPhase::Tail})
	{
		const TArray<const FFrame*>* PhaseFrames = ByPhase.Find(Each);
		if (!PhaseFrames)
		{
			continue;
		}
		TArray<double> Period, Work, Sim;
		for (const FFrame* Frame : *PhaseFrames)
		{
			Period.Add(Frame->PeriodMs);
			Work.Add(Frame->WorkMs);
			Sim.Add(Frame->SimMs);
		}
		Summary += FString::Printf(TEXT("%-8s %6d  %6.2f %6.2f %7.2f      %6.2f %6.2f %7.2f      %6.2f %6.2f %7.2f\n"),
			PhaseName(Each), PhaseFrames->Num(),
			Percentile(Period, 0.5), Percentile(Period, 0.99), Percentile(Period, 1.0),
			Percentile(Work, 0.5), Percentile(Work, 0.99), Percentile(Work, 1.0),
			Percentile(Sim, 0.5), Percentile(Sim, 0.99), Percentile(Sim, 1.0));
	}

	// The worker runs one request at a time, so a line's own time starts when the previous one finished.
	double Previous = 0.0;
	double TotalAudio = 0.0;
	double First = 0.0;
	double Last = 0.0;
	for (const FLine& Line : Lines)
	{
		const double Start = FMath::Max(Line.SubmittedAt, Previous);
		const double Audio = Line.SampleRate > 0 ? static_cast<double>(Line.Pcm.Num()) / Line.SampleRate : 0.0;
		Summary += FString::Printf(TEXT("line %-10s %6.0f ms for %5.2f s audio  \"%s\"\n"), Crew[Line.Speaker].CallSign,
			(Line.ReadyAt - Start) * 1000.0, Audio, *Line.Text);
		Previous = Line.ReadyAt;
		TotalAudio += Audio;
		First = First == 0.0 ? Line.SubmittedAt : First;
		Last = FMath::Max(Last, Line.ReadyAt);
	}
	Summary += FString::Printf(TEXT("synthesis: %d lines, %.2f s audio in %.2f s wall, RTF %.3f\n"), Lines.Num(),
		TotalAudio, Last - First, TotalAudio > 0.0 ? (Last - First) / TotalAudio : 0.0);
	FFileHelper::SaveStringToFile(Summary, *FPaths::Combine(OutDir, TEXT("bench-summary.txt")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	TArray<FString> SummaryLines;
	Summary.ParseIntoArrayLines(SummaryLines);
	for (const FString& Line : SummaryLines)
	{
		UE_LOG(LogSpeechGenRadio, Display, TEXT("%s"), *Line);
	}
}

void USpeechGenRadioDemo::Finish(const int32 ExitCode)
{
	if (Phase == EPhase::Done)
	{
		return;
	}
	SetPhase(EPhase::Done);
	UE_LOG(LogSpeechGenRadio, Display, TEXT("SpeechGen %s finished with exit code %d"), bBench ? TEXT("bench") : TEXT("radio demo"), ExitCode);
	FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(ExitCode), TEXT("SpeechGenRadioDemo"));
}

const TCHAR* USpeechGenRadioDemo::PhaseName(const EPhase InPhase)
{
	switch (InPhase)
	{
	case EPhase::Boot: return TEXT("boot");
	case EPhase::Idle: return TEXT("idle");
	case EPhase::Load: return TEXT("load");
	case EPhase::Settle: return TEXT("settle");
	case EPhase::Synth: return TEXT("synth");
	case EPhase::Tail: return TEXT("tail");
	case EPhase::Prepare: return TEXT("prepare");
	case EPhase::Play: return TEXT("play");
	case EPhase::Done: return TEXT("done");
	}
	return TEXT("?");
}
