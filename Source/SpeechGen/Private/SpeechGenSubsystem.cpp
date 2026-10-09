#include "SpeechGen/SpeechGenSubsystem.h"

#include "SpeechGen/SpeechGenProjectSettings.h"
#include "SpeechGen/SpeechGenAudioCache.h"

#include "Async/Async.h"
#include "Engine/GameInstance.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "Phonemizer/KokoroPhonemizer.h"
#include "SpeechGen/SpeechGenVoiceProfile.h"

DEFINE_LOG_CATEGORY_STATIC(LogSpeechGen, Log, All);

namespace
{
	constexpr int32 StyleDimension = 256;
	constexpr int32 VoiceRows = 510;
	constexpr int32 MaxSamplesPerToken = 8192;

	struct FResolvedVoiceWeight
	{
		FName VoiceId;
		float Weight = 0.0f;
	};

	bool LoadVoiceStyle(const FString& RuntimeDirectory, const TArray<FResolvedVoiceWeight>& Blend,
		const int32 PhonemeCount, TArray<float>& OutStyle, FString& OutError)
	{
		if (PhonemeCount < 1 || PhonemeCount > VoiceRows)
		{
			OutError = TEXT("Voice style row is outside the Kokoro context window.");
			return false;
		}

		float WeightSum = 0.0f;
		for (const FResolvedVoiceWeight& Voice : Blend)
		{
			WeightSum += FMath::Max(0.0f, Voice.Weight);
		}
		if (WeightSum <= UE_SMALL_NUMBER)
		{
			OutError = TEXT("Voice profile has no positive blend weights.");
			return false;
		}

		OutStyle.Init(0.0f, StyleDimension);
		for (const FResolvedVoiceWeight& Voice : Blend)
		{
			if (Voice.Weight <= 0.0f)
			{
				continue;
			}

			const FString VoicePath = FPaths::Combine(RuntimeDirectory, TEXT("voices"),
				Voice.VoiceId.ToString() + TEXT(".bin"));
			TArray64<uint8> VoiceBytes;
			if (!FFileHelper::LoadFileToArray(VoiceBytes, *VoicePath)
				|| VoiceBytes.Num() != static_cast<int64>(VoiceRows * StyleDimension * sizeof(float)))
			{
				OutError = FString::Printf(TEXT("Voice data is missing or invalid: %s"), *Voice.VoiceId.ToString());
				return false;
			}

			const float* VoiceData = reinterpret_cast<const float*>(VoiceBytes.GetData());
			const float* Row = VoiceData + (PhonemeCount - 1) * StyleDimension;
			const float NormalizedWeight = Voice.Weight / WeightSum;
			for (int32 Index = 0; Index < StyleDimension; ++Index)
			{
				OutStyle[Index] += Row[Index] * NormalizedWeight;
			}
		}
		return true;
	}
	// Below this a sample counts as silence when trimming chunk seams (about -48 dBFS).
	constexpr float SeamSilence = 0.004f;
	constexpr int32 MaxPauseMilliseconds = 5000;

	bool RunKokoro(UE::NNE::IModelInstanceCPU& Instance, TArray<int64>& TokenIds, TArray<float>& Style,
		float Speed, TArray<float>& OutWaveform, FString& OutError)
	{
		TArray<UE::NNE::FTensorShape> InputShapes;
		TArray<UE::NNE::FTensorBindingCPU> InputBindings;
		for (const UE::NNE::FTensorDesc& Desc : Instance.GetInputTensorDescs())
		{
			if (Desc.GetName() == TEXT("tokens"))
			{
				InputShapes.Add(UE::NNE::FTensorShape::Make({1, static_cast<uint32>(TokenIds.Num())}));
				InputBindings.Add({TokenIds.GetData(), static_cast<uint64>(TokenIds.Num() * sizeof(int64))});
			}
			else if (Desc.GetName() == TEXT("style"))
			{
				InputShapes.Add(UE::NNE::FTensorShape::Make({1, StyleDimension}));
				InputBindings.Add({Style.GetData(), static_cast<uint64>(Style.Num() * sizeof(float))});
			}
			else if (Desc.GetName() == TEXT("speed"))
			{
				InputShapes.Add(UE::NNE::FTensorShape::Make({1}));
				InputBindings.Add({&Speed, sizeof(float)});
			}
			else
			{
				OutError = FString::Printf(TEXT("Unexpected Kokoro input tensor: %s"), *Desc.GetName());
				return false;
			}
		}

		OutWaveform.SetNumUninitialized(TokenIds.Num() * MaxSamplesPerToken);
		const UE::NNE::FTensorBindingCPU OutputBinding{OutWaveform.GetData(),
			static_cast<uint64>(OutWaveform.Num() * sizeof(float))};
		if (Instance.SetInputTensorShapes(InputShapes) != UE::NNE::IModelInstanceCPU::ESetInputTensorShapesStatus::Ok
			|| Instance.RunSync(InputBindings, {OutputBinding}) != UE::NNE::IModelInstanceCPU::ERunSyncStatus::Ok)
		{
			OutError = TEXT("Kokoro ONNX inference failed.");
			return false;
		}
		const TConstArrayView<UE::NNE::FTensorShape> OutputShapes = Instance.GetOutputTensorShapes();
		if (OutputShapes.Num() != 1 || OutputShapes[0].Volume() > static_cast<uint64>(OutWaveform.Num()))
		{
			OutError = TEXT("Kokoro output exceeded the deterministic waveform buffer.");
			return false;
		}
		OutWaveform.SetNum(static_cast<int32>(OutputShapes[0].Volume()), EAllowShrinking::No);
		return true;
	}

	// Joins one chunk onto the line. At a seam the model's own lead-in and tail silence are trimmed (keeping
	// 20 ms of air) so the pause between chunks is the one the markup asked for.
	void AppendChunkWaveform(TConstArrayView<float> Chunk, const bool bTrimLead, const bool bTrimTail,
		const int32 PauseSamples, TArray<float>& InOutWaveform)
	{
		const int32 Air = USpeechGenSubsystem::SampleRate / 50;
		int32 First = 0;
		int32 Last = Chunk.Num() - 1;
		if (bTrimLead)
		{
			while (First < Chunk.Num() && FMath::Abs(Chunk[First]) < SeamSilence)
			{
				++First;
			}
			First = FMath::Max(0, First - Air);
		}
		if (bTrimTail)
		{
			while (Last > First && FMath::Abs(Chunk[Last]) < SeamSilence)
			{
				--Last;
			}
			Last = FMath::Min(Chunk.Num() - 1, Last + Air);
		}
		InOutWaveform.AddZeroed(PauseSamples);
		if (Last >= First)
		{
			InOutWaveform.Append(Chunk.Slice(First, Last - First + 1));
		}
	}
}

void USpeechGenSubsystem::SplitPauseMarkup(const FString& Text, TArray<FSpeechGenTextChunk>& OutChunks)
{
	OutChunks.Reset();
	FString Current;
	auto Flush = [&OutChunks, &Current](const int32 PauseAfterMs)
	{
		if (!Current.TrimStartAndEnd().IsEmpty())
		{
			FSpeechGenTextChunk& Chunk = OutChunks.AddDefaulted_GetRef();
			Chunk.Text = Current.TrimStartAndEnd();
			Chunk.PauseAfterMs = PauseAfterMs;
		}
		else if (!OutChunks.IsEmpty())
		{
			// Back-to-back markers add up; a marker before any text has nothing to follow and is dropped.
			OutChunks.Last().PauseAfterMs = FMath::Min(OutChunks.Last().PauseAfterMs + PauseAfterMs, MaxPauseMilliseconds);
		}
		Current.Reset();
	};
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		if (Text[Index] == TEXT('[') && Index + 2 < Text.Len() && (Text[Index + 1] == TEXT('p') || Text[Index + 1] == TEXT('P')))
		{
			int32 Cursor = Index + 2;
			int32 Milliseconds = 0;
			while (Cursor < Text.Len() && FChar::IsDigit(Text[Cursor]))
			{
				Milliseconds = FMath::Min(Milliseconds * 10 + (Text[Cursor] - TEXT('0')), MaxPauseMilliseconds);
				++Cursor;
			}
			if (Cursor > Index + 2 && Cursor < Text.Len() && Text[Cursor] == TEXT(']'))
			{
				Flush(Milliseconds);
				Index = Cursor;
				continue;
			}
		}
		Current.AppendChar(Text[Index]);
	}
	Flush(0);
	if (!OutChunks.IsEmpty())
	{
		OutChunks.Last().PauseAfterMs = 0;
	}
}

struct USpeechGenSubsystem::FResolvedRequest
{
	FGuid TurnId;
	FGuid SegmentId;
	FString Text;
	ESpeechGenLanguage Language = ESpeechGenLanguage::English;
	TArray<FResolvedVoiceWeight> VoiceBlend;
	float Speed = 1.0f;
	float Gain = 1.0f;
	float PauseScale = 1.0f;
	bool bEnableDiagnostics = false;
	double EnqueuedAtSeconds = 0.0;
	int32 QueueDepthAtSubmission = 0;
	TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe> CancellationFlag;
};

void USpeechGenSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bShuttingDown = false;
	RuntimeStatus.RuntimePath = ResolveRuntimeDirectory();
	SetRuntimeState(ESpeechGenRuntimeState::Stopped);
#if !WITH_EDITOR
	if (GetDefault<USpeechGenProjectSettings>()->PackagedDefaults.bStartRuntimeOnGameInstance)
	{
		StartRuntime();
	}
#endif
}

#if WITH_EDITOR
USpeechGenSubsystem* USpeechGenSubsystem::CreateEditorPreviewRuntime(UGameInstance& Owner)
{
	USpeechGenSubsystem* Runtime = NewObject<USpeechGenSubsystem>(&Owner);
	Runtime->RuntimeStatus.RuntimePath = ResolveRuntimeDirectory();
	Runtime->SetRuntimeState(ESpeechGenRuntimeState::Stopped);
	return Runtime;
}
#endif

bool USpeechGenSubsystem::StartRuntime()
{
	if (RuntimeStatus.State == ESpeechGenRuntimeState::Loading
		|| RuntimeStatus.State == ESpeechGenRuntimeState::Ready)
	{
		return true;
	}

	bShuttingDown = false;
	WorkerPool.Reset(FQueuedThreadPool::Allocate());
	if (!WorkerPool || !WorkerPool->Create(1, 256 * 1024, TPri_Normal, TEXT("SpeechGenWorker")))
	{
		WorkerPool.Reset();
		SetRuntimeState(ESpeechGenRuntimeState::Failed, TEXT("Unable to create SpeechGen worker thread."));
		return false;
	}
	++RuntimeGeneration;
	BeginRuntimeLoad();
	return RuntimeStatus.State == ESpeechGenRuntimeState::Loading
		|| RuntimeStatus.State == ESpeechGenRuntimeState::Ready;
}

void USpeechGenSubsystem::StopRuntime()
{
	bShuttingDown = true;
	++RuntimeGeneration;
	CancelAll();
	if (WorkerPool)
	{
		WorkerPool->Destroy();
		WorkerPool.Reset();
	}
	Phonemizer.Reset();
	ModelInstance.Reset();
	Model.Reset();
	ModelData = nullptr;
	SetRuntimeState(ESpeechGenRuntimeState::Stopped);
	bShuttingDown = false;
}

void USpeechGenSubsystem::Deinitialize()
{
	StopRuntime();
	bShuttingDown = true;
	Super::Deinitialize();
}

FString USpeechGenSubsystem::ResolveRuntimeDirectory()
{
	// The runtime data is platform-neutral; the folder carries the UBT platform name (Win64, Mac, ...)
	// so it matches the directory SpeechGen.Build.cs stages for packaged builds.
	const FString PlatformRuntimeDirectory = FPaths::Combine(TEXT("SpeechGen/Runtimes/Kokoro"),
		FPlatformMisc::GetUBTPlatform(), RuntimeVersion);
#if WITH_EDITOR
	return FPaths::Combine(FPaths::ProjectSavedDir(), PlatformRuntimeDirectory);
#else
	return FPaths::Combine(FPlatformProcess::BaseDir(), PlatformRuntimeDirectory);
#endif
}

TArray<FString> USpeechGenSubsystem::GetAvailableVoiceIds()
{
	const FString VoiceDirectory = FPaths::Combine(ResolveRuntimeDirectory(), TEXT("voices"));
	TArray<FString> VoiceFiles;
	IFileManager::Get().FindFiles(VoiceFiles, *FPaths::Combine(VoiceDirectory, TEXT("*.bin")), true, false);

	TArray<FString> VoiceIds;
	constexpr int64 ExpectedVoiceBytes = static_cast<int64>(VoiceRows) * StyleDimension * sizeof(float);
	for (const FString& VoiceFile : VoiceFiles)
	{
		const FName VoiceId(*FPaths::GetBaseFilename(VoiceFile));
		if (USpeechGenVoiceProfile::GetVoiceLanguage(VoiceId).IsSet()
			&& IFileManager::Get().FileSize(*FPaths::Combine(VoiceDirectory, VoiceFile)) == ExpectedVoiceBytes)
		{
			VoiceIds.Add(VoiceId.ToString());
		}
	}
	VoiceIds.Sort();
	return VoiceIds;
}

void USpeechGenSubsystem::BeginRuntimeLoad()
{
	check(WorkerPool);
	FModuleManager::Get().LoadModule(TEXT("NNERuntimeORT"));

	const FString RuntimeDirectory = ResolveRuntimeDirectory();
	const FString ModelPath = FPaths::Combine(RuntimeDirectory, TEXT("onnx/model_cpu_mixed.onnx"));
	if (IFileManager::Get().FileSize(*ModelPath) <= 0)
	{
		SetRuntimeState(ESpeechGenRuntimeState::Unavailable,
			FString::Printf(TEXT("SpeechGen runtime is not installed: %s"), *ModelPath));
		return;
	}

	SetRuntimeState(ESpeechGenRuntimeState::Loading);
	// Created here, filled on the worker: reading and copying the 178 MB model on the game thread
	// cost a 66 ms frame. The UPROPERTY keeps it alive, and StopRuntime drains the worker before
	// releasing it.
	ModelData = NewObject<UNNEModelData>(this);

	TWeakObjectPtr<USpeechGenSubsystem> WeakThis(this);
	TObjectPtr<UNNEModelData> LoadedModelData = ModelData;
	const uint64 Generation = RuntimeGeneration;
	// Copied on the game thread; the worker never touches the settings object.
	TArray<FSpeechGenPronunciation> PronunciationOverrides =
		GetDefault<USpeechGenProjectSettings>()->PronunciationOverrides;
	AsyncPool(*WorkerPool, [WeakThis, LoadedModelData, RuntimeDirectory, ModelPath, Generation,
		PronunciationOverrides = MoveTemp(PronunciationOverrides)]() mutable
	{
		TSharedPtr<UE::NNE::IModelCPU> LoadedModel;
		TSharedPtr<UE::NNE::IModelInstanceCPU> LoadedInstance;
		TSharedPtr<FKokoroPhonemizer> LoadedPhonemizer = MakeShared<FKokoroPhonemizer>();
		FString Error;

		const TWeakInterfacePtr<INNERuntimeCPU> Runtime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));
		TArray64<uint8> ModelBytes;
		if (!FFileHelper::LoadFileToArray(ModelBytes, *ModelPath))
		{
			Error = FString::Printf(TEXT("Unable to read the Kokoro model: %s"), *ModelPath);
		}
		else if (!Runtime.IsValid())
		{
			Error = TEXT("NNERuntimeORTCpu is unavailable.");
		}
		else
		{
			LoadedModelData->Init(TEXT("onnx"), ModelBytes);
			ModelBytes.Empty();
			LoadedModel = Runtime->CreateModelCPU(LoadedModelData);
			if (!LoadedModel)
			{
				Error = TEXT("NNERuntimeORTCpu rejected the Kokoro CPU mixed model.");
			}
			else
			{
				LoadedInstance = LoadedModel->CreateModelInstanceCPU();
				if (!LoadedInstance)
				{
					Error = TEXT("Unable to create the Kokoro CPU model instance.");
				}
			}
		}

		if (Error.IsEmpty() && !LoadedPhonemizer->Initialize(RuntimeDirectory, Error))
		{
			LoadedInstance.Reset();
			LoadedModel.Reset();
		}
		else if (Error.IsEmpty() && !PronunciationOverrides.IsEmpty())
		{
			TArray<FString> Warnings;
			const int32 Applied = LoadedPhonemizer->ApplyPronunciationOverrides(PronunciationOverrides, Warnings);
			for (const FString& Warning : Warnings)
			{
				UE_LOG(LogSpeechGen, Warning, TEXT("%s"), *Warning);
			}
			UE_LOG(LogSpeechGen, Log, TEXT("Applied %d of %d pronunciation overrides."), Applied,
				PronunciationOverrides.Num());
		}

		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Generation, LoadedModel = MoveTemp(LoadedModel), LoadedInstance = MoveTemp(LoadedInstance),
				LoadedPhonemizer = MoveTemp(LoadedPhonemizer), Error]() mutable
			{
				if (USpeechGenSubsystem* Subsystem = WeakThis.Get())
				{
					Subsystem->CompleteRuntimeLoad(Generation, MoveTemp(LoadedModel), MoveTemp(LoadedInstance),
						MoveTemp(LoadedPhonemizer), Error);
				}
			});
	});
}

void USpeechGenSubsystem::CompleteRuntimeLoad(const uint64 Generation, TSharedPtr<UE::NNE::IModelCPU> InModel,
	TSharedPtr<UE::NNE::IModelInstanceCPU> InInstance, TSharedPtr<FKokoroPhonemizer> InPhonemizer,
	const FString& Error)
{
	if (Generation != RuntimeGeneration || bShuttingDown)
	{
		return;
	}
	if (!Error.IsEmpty())
	{
		SetRuntimeState(ESpeechGenRuntimeState::Failed, Error);
		return;
	}
	Model = MoveTemp(InModel);
	ModelInstance = MoveTemp(InInstance);
	Phonemizer = MoveTemp(InPhonemizer);
	SetRuntimeState(ESpeechGenRuntimeState::Ready);
}

FGuid USpeechGenSubsystem::SynthesizeAsync(const FSpeechGenRequest& Request)
{
	FResolvedRequest Resolved;
	FString Error;
	if (!ResolveRequest(Request, Resolved, Error))
	{
		const FGuid SegmentId = Request.SegmentId.IsValid() ? Request.SegmentId : FGuid::NewGuid();
		UE_LOG(LogSpeechGen, Warning, TEXT("Rejected speech request Turn=%s Segment=%s: %s"),
			*Request.TurnId.ToString(EGuidFormats::DigitsWithHyphensLower),
			*SegmentId.ToString(EGuidFormats::DigitsWithHyphensLower),
			Error.IsEmpty() ? TEXT("SpeechGen runtime is not ready.") : *Error);
		OnRequestFailed.Broadcast(Request.TurnId, SegmentId,
			Error.IsEmpty() ? TEXT("SpeechGen runtime is not ready.") : Error);
		return SegmentId;
	}

	{
		FScopeLock Lock(&CancellationMutex);
		Resolved.CancellationFlag = MakeShared<std::atomic_bool, ESPMode::ThreadSafe>(false);
		ActiveCancellationFlags.FindOrAdd(Resolved.TurnId).Add(Resolved.CancellationFlag);
	}
	Resolved.EnqueuedAtSeconds = FPlatformTime::Seconds();
	Resolved.QueueDepthAtSubmission = PendingRequestCount.fetch_add(1, std::memory_order_relaxed) + 1;
	if (Resolved.bEnableDiagnostics)
	{
		UE_LOG(LogSpeechGen, Log,
			TEXT("Queued request Turn=%s Segment=%s Chars=%d QueueDepth=%d Speed=%.2f Voices=%d Language=%d"),
			*Resolved.TurnId.ToString(EGuidFormats::DigitsWithHyphensLower),
			*Resolved.SegmentId.ToString(EGuidFormats::DigitsWithHyphensLower), Resolved.Text.Len(),
			Resolved.QueueDepthAtSubmission, Resolved.Speed, Resolved.VoiceBlend.Num(),
			static_cast<int32>(Resolved.Language));
	}
	const FGuid SegmentId = Resolved.SegmentId;
	TWeakObjectPtr<USpeechGenSubsystem> WeakThis(this);
	if (RuntimeStatus.State == ESpeechGenRuntimeState::Ready && WorkerPool)
	{
		AsyncPool(*WorkerPool, [WeakThis, Resolved = MoveTemp(Resolved)]() mutable
		{
			if (USpeechGenSubsystem* Subsystem = WeakThis.Get())
			{
				Subsystem->ExecuteRequest(MoveTemp(Resolved));
			}
		});
	}
	else
	{
		Async(EAsyncExecution::ThreadPool, [WeakThis, Resolved = MoveTemp(Resolved)]() mutable
		{
			if (USpeechGenSubsystem* Subsystem = WeakThis.Get())
			{
				Subsystem->ExecuteRequest(MoveTemp(Resolved));
			}
		});
	}
	return SegmentId;
}

bool USpeechGenSubsystem::PhonemizeText(const ESpeechGenLanguage Language, const FString& Text,
	FString& OutPhonemes, FString& OutError) const
{
	const TSharedPtr<FKokoroPhonemizer> Loaded = Phonemizer;
	if (!Loaded)
	{
		OutError = TEXT("SpeechGen runtime is not ready.");
		return false;
	}
	TArray<int64> TokenIds;
	return Loaded->Encode(Language, Text, TokenIds, OutPhonemes, OutError);
}

void USpeechGenSubsystem::SetAuthoredCacheDirectory(const FString& Directory)
{
	check(IsInGameThread());
	AuthoredCacheDirectory = Directory;
}

bool USpeechGenSubsystem::ResolveRequest(const FSpeechGenRequest& Request, FResolvedRequest& OutRequest,
	FString& OutError) const
{
	if (Request.Text.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("Speech request text is empty.");
		return false;
	}

	if (!USpeechGenVoiceProfile::ValidateVoiceBlend(Request.Language, Request.VoiceBlend, OutError))
	{
		return false;
	}

	OutRequest.TurnId = Request.TurnId.IsValid() ? Request.TurnId : FGuid::NewGuid();
	OutRequest.SegmentId = Request.SegmentId.IsValid() ? Request.SegmentId : FGuid::NewGuid();
	OutRequest.Text = Request.Text;
	OutRequest.Language = Request.Language;
	OutRequest.Speed = FMath::Clamp(Request.Speed, 0.7f, 1.3f);
	OutRequest.Gain = FMath::Clamp(Request.Gain, 0.0f, 2.0f);
	OutRequest.PauseScale = FMath::Clamp(Request.PauseScale, 0.0f, 1.0f);
	OutRequest.bEnableDiagnostics = Request.bEnableDiagnostics;
	for (const FSpeechGenVoiceWeight& Voice : Request.VoiceBlend)
	{
		OutRequest.VoiceBlend.Add({Voice.VoiceId, Voice.Weight});
	}
	return true;
}

void USpeechGenSubsystem::ExecuteRequest(FResolvedRequest Request)
{
	const double ExecutionStartedAtSeconds = FPlatformTime::Seconds();
	const double QueueWaitMilliseconds = (ExecutionStartedAtSeconds - Request.EnqueuedAtSeconds) * 1000.0;
	FSpeechGenRequest CacheRequest;
	CacheRequest.Text = Request.Text;
	CacheRequest.Language = Request.Language;
	CacheRequest.Speed = Request.Speed;
	CacheRequest.Gain = Request.Gain;
	CacheRequest.PauseScale = Request.PauseScale;
	CacheRequest.TurnId = Request.TurnId;
	CacheRequest.SegmentId = Request.SegmentId;
	for (const FResolvedVoiceWeight& Voice : Request.VoiceBlend)
	{
		FSpeechGenVoiceWeight& Entry = CacheRequest.VoiceBlend.AddDefaulted_GetRef();
		Entry.VoiceId = Voice.VoiceId;
		Entry.Weight = Voice.Weight;
	}
	FSpeechGenResult CachedResult;
	FString CacheError;
	const bool bAuthoredHit = !AuthoredCacheDirectory.IsEmpty()
		&& FSpeechGenAudioCache::TryLoad(AuthoredCacheDirectory, CacheRequest, CachedResult, CacheError);
	const bool bGeneratedHit = !bAuthoredHit && FSpeechGenAudioCache::TryLoad(
		FSpeechGenAudioCache::GetGeneratedDirectory(), CacheRequest, CachedResult, CacheError);
	if (bAuthoredHit || bGeneratedHit)
	{
		CachedResult.Source = bAuthoredHit
			? ESpeechGenResultSource::AuthoredCache : ESpeechGenResultSource::GeneratedCache;
		AsyncTask(ENamedThreads::GameThread,
			[WeakThis = TWeakObjectPtr<USpeechGenSubsystem>(this), Request, Result = MoveTemp(CachedResult)]() mutable
			{
				if (USpeechGenSubsystem* Subsystem = WeakThis.Get())
				{
					if (!IsRequestCancelled(Request))
					{
						Subsystem->OnSegmentReady.Broadcast(Result);
					}
					Subsystem->MarkRequestFinished(Request.TurnId, Request.CancellationFlag);
				}
			});
		return;
	}
	if (!CacheError.IsEmpty())
	{
		UE_LOG(LogSpeechGen, Warning, TEXT("%s"), *CacheError);
	}
	if (bShuttingDown || IsRequestCancelled(Request) || !ModelInstance || !Phonemizer)
	{
		if (!bShuttingDown && !IsRequestCancelled(Request))
		{
			AsyncTask(ENamedThreads::GameThread,
				[WeakThis = TWeakObjectPtr<USpeechGenSubsystem>(this), Request]()
				{
					if (USpeechGenSubsystem* Subsystem = WeakThis.Get())
					{
						if (!IsRequestCancelled(Request))
						{
							Subsystem->OnRequestFailed.Broadcast(Request.TurnId, Request.SegmentId,
								NSLOCTEXT("SpeechGen", "RuntimeUnavailableNoCache", "SpeechGen runtime is not ready and no exact cached clip exists.").ToString());
						}
						Subsystem->MarkRequestFinished(Request.TurnId, Request.CancellationFlag);
					}
				});
			return;
		}
		if (Request.bEnableDiagnostics)
		{
			UE_LOG(LogSpeechGen, Log,
				TEXT("Discarded request before inference Turn=%s Segment=%s QueueWaitMs=%.2f ShuttingDown=%s Cancelled=%s RuntimeReady=%s"),
				*Request.TurnId.ToString(EGuidFormats::DigitsWithHyphensLower),
				*Request.SegmentId.ToString(EGuidFormats::DigitsWithHyphensLower), QueueWaitMilliseconds,
				bShuttingDown ? TEXT("true") : TEXT("false"),
				IsRequestCancelled(Request) ? TEXT("true") : TEXT("false"),
				ModelInstance && Phonemizer ? TEXT("true") : TEXT("false"));
		}
		MarkRequestFinished(Request.TurnId, Request.CancellationFlag);
		return;
	}

	// "[p400]" splits the line: each part is its own inference, joined by 400 ms of silence (see SplitPauseMarkup).
	TArray<FSpeechGenTextChunk> Chunks;
	SplitPauseMarkup(Request.Text, Chunks);

	TArray<float> Waveform;
	FString Phonemes;
	FString Error;
	int32 TokenCount = 0;
	double PhonemizeMilliseconds = 0.0;
	double StyleMilliseconds = 0.0;
	double InferenceMilliseconds = 0.0;
	// Every chunk of a line speaks with the style row for the whole line's length. Kokoro's voice tables hold
	// one style per phoneme count; a short chunk's own row measurably shifted vowels (the first vowel of a
	// short leading chunk lost 100-170 Hz of F2 against the same line unsplit), and one row per line keeps
	// a transmission in one voice.
	TArray<TArray<int64>> ChunkTokens;
	TArray<FString> ChunkPhonemes;
	int32 LinePhonemeCount = 0;
	double StepStartedAtSeconds = FPlatformTime::Seconds();
	for (const FSpeechGenTextChunk& Chunk : Chunks)
	{
		TArray<int64>& TokenIds = ChunkTokens.AddDefaulted_GetRef();
		FString& Encoded = ChunkPhonemes.AddDefaulted_GetRef();
		if (!Phonemizer->Encode(Request.Language, Chunk.Text, TokenIds, Encoded, Error))
		{
			break;
		}
		LinePhonemeCount += TokenIds.Num() - 2 + (LinePhonemeCount > 0 ? 1 : 0);
	}
	PhonemizeMilliseconds = (FPlatformTime::Seconds() - StepStartedAtSeconds) * 1000.0;

	TArray<float> Style;
	if (Error.IsEmpty())
	{
		StepStartedAtSeconds = FPlatformTime::Seconds();
		LoadVoiceStyle(ResolveRuntimeDirectory(), Request.VoiceBlend, FMath::Min(LinePhonemeCount, VoiceRows), Style, Error);
		StyleMilliseconds = (FPlatformTime::Seconds() - StepStartedAtSeconds) * 1000.0;
	}

	for (int32 ChunkIndex = 0; Error.IsEmpty() && ChunkIndex < Chunks.Num(); ++ChunkIndex)
	{
		TArray<int64>& TokenIds = ChunkTokens[ChunkIndex];
		TArray<float> ChunkWaveform;
		StepStartedAtSeconds = FPlatformTime::Seconds();
		const bool bInferred = RunKokoro(*ModelInstance, TokenIds, Style, Request.Speed, ChunkWaveform, Error);
		InferenceMilliseconds += (FPlatformTime::Seconds() - StepStartedAtSeconds) * 1000.0;
		if (!bInferred)
		{
			break;
		}

		TokenCount += TokenIds.Num();
		const int32 PauseBefore = ChunkIndex > 0 ? Chunks[ChunkIndex - 1].PauseAfterMs : 0;
		AppendChunkWaveform(ChunkWaveform, ChunkIndex > 0, ChunkIndex < Chunks.Num() - 1,
			FMath::RoundToInt(SampleRate * PauseBefore / 1000.0f), Waveform);
		Phonemes += ChunkIndex > 0
			? FString::Printf(TEXT(" [p%d] "), PauseBefore) + ChunkPhonemes[ChunkIndex] : ChunkPhonemes[ChunkIndex];
	}

	if (!Error.IsEmpty())
	{
		UE_LOG(LogSpeechGen, Warning, TEXT("Synthesis failed Turn=%s Segment=%s Tokens=%d InferenceMs=%.2f: %s"),
			*Request.TurnId.ToString(EGuidFormats::DigitsWithHyphensLower),
			*Request.SegmentId.ToString(EGuidFormats::DigitsWithHyphensLower), TokenCount,
			InferenceMilliseconds, *Error);
		AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<USpeechGenSubsystem>(this), Request, Error]()
		{
			if (USpeechGenSubsystem* Subsystem = WeakThis.Get())
			{
				if (!IsRequestCancelled(Request))
				{
					Subsystem->OnRequestFailed.Broadcast(Request.TurnId, Request.SegmentId, Error);
				}
				Subsystem->MarkRequestFinished(Request.TurnId, Request.CancellationFlag);
			}
		});
		return;
	}
	if (IsRequestCancelled(Request))
	{
		if (Request.bEnableDiagnostics)
		{
			UE_LOG(LogSpeechGen, Log,
				TEXT("Discarded completed inference after cancellation Turn=%s Segment=%s InferenceMs=%.2f"),
				*Request.TurnId.ToString(EGuidFormats::DigitsWithHyphensLower),
				*Request.SegmentId.ToString(EGuidFormats::DigitsWithHyphensLower), InferenceMilliseconds);
		}
		MarkRequestFinished(Request.TurnId, Request.CancellationFlag);
		return;
	}

	const double PcmStartedAtSeconds = FPlatformTime::Seconds();
	FSpeechGenResult Result;
	Result.TurnId = Request.TurnId;
	Result.SegmentId = Request.SegmentId;
	Result.Phonemes = MoveTemp(Phonemes);
	Result.SampleRate = SampleRate;
	Result.PcmSamples.Reserve(Waveform.Num() + SampleRate / 2);
	for (const float Sample : Waveform)
	{
		Result.PcmSamples.Add(static_cast<int16>(FMath::Clamp(Sample * Request.Gain, -1.0f, 1.0f) * 32767.0f));
	}
	const float BasePause = Request.Text.EndsWith(TEXT("?")) || Request.Text.EndsWith(TEXT("!")) ? 0.18f : 0.12f;
	Result.PcmSamples.AddZeroed(FMath::RoundToInt(SampleRate * BasePause * Request.PauseScale));
	Result.DurationSeconds = static_cast<float>(Result.PcmSamples.Num()) / SampleRate;
	if (!FSpeechGenAudioCache::SaveGenerated(CacheRequest, Result, CacheError))
	{
		UE_LOG(LogSpeechGen, Warning, TEXT("Generated speech could not be cached: %s"), *CacheError);
	}
	const double PcmMilliseconds = (FPlatformTime::Seconds() - PcmStartedAtSeconds) * 1000.0;
	if (Request.bEnableDiagnostics)
	{
		const double TotalMilliseconds = (FPlatformTime::Seconds() - Request.EnqueuedAtSeconds) * 1000.0;
		const double RealTimeFactor = Result.DurationSeconds > UE_SMALL_NUMBER
			? (InferenceMilliseconds / 1000.0) / Result.DurationSeconds
			: 0.0;
		UE_LOG(LogSpeechGen, Log,
			TEXT("Completed request Turn=%s Segment=%s Chars=%d Tokens=%d QueueWaitMs=%.2f PhonemizeMs=%.2f StyleMs=%.2f InferenceMs=%.2f PcmMs=%.2f AudioSec=%.2f RTF=%.3f TotalMs=%.2f"),
			*Request.TurnId.ToString(EGuidFormats::DigitsWithHyphensLower),
			*Request.SegmentId.ToString(EGuidFormats::DigitsWithHyphensLower), Request.Text.Len(),
			TokenCount, QueueWaitMilliseconds, PhonemizeMilliseconds, StyleMilliseconds,
			InferenceMilliseconds, PcmMilliseconds, Result.DurationSeconds, RealTimeFactor, TotalMilliseconds);
	}

	AsyncTask(ENamedThreads::GameThread,
		[WeakThis = TWeakObjectPtr<USpeechGenSubsystem>(this), Request, Result = MoveTemp(Result)]() mutable
		{
			if (USpeechGenSubsystem* Subsystem = WeakThis.Get())
			{
				if (!IsRequestCancelled(Request))
				{
					Subsystem->OnSegmentReady.Broadcast(Result);
				}
				Subsystem->MarkRequestFinished(Request.TurnId, Request.CancellationFlag);
			}
		});
}

void USpeechGenSubsystem::CancelTurn(const FGuid TurnId)
{
	if (!TurnId.IsValid())
	{
		return;
	}
	FScopeLock Lock(&CancellationMutex);
	if (TArray<TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe>>* Flags = ActiveCancellationFlags.Find(TurnId))
	{
		UE_LOG(LogSpeechGen, Log, TEXT("Cancelling speech turn Turn=%s Requests=%d"),
			*TurnId.ToString(EGuidFormats::DigitsWithHyphensLower), Flags->Num());
		for (const TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe>& Flag : *Flags)
		{
			Flag->store(true, std::memory_order_release);
		}
	}
}

void USpeechGenSubsystem::CancelAll()
{
	FScopeLock Lock(&CancellationMutex);
	for (const TPair<FGuid, TArray<TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe>>>& Turn : ActiveCancellationFlags)
	{
		for (const TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe>& Flag : Turn.Value)
		{
			Flag->store(true, std::memory_order_release);
		}
	}
}

bool USpeechGenSubsystem::IsRequestCancelled(const FResolvedRequest& Request)
{
	return !Request.CancellationFlag || Request.CancellationFlag->load(std::memory_order_acquire);
}

void USpeechGenSubsystem::MarkRequestFinished(const FGuid TurnId,
	const TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe>& CancellationFlag)
{
	PendingRequestCount.fetch_sub(1, std::memory_order_relaxed);
	FScopeLock Lock(&CancellationMutex);
	TArray<TSharedPtr<std::atomic_bool, ESPMode::ThreadSafe>>* Flags = ActiveCancellationFlags.Find(TurnId);
	if (!Flags)
	{
		return;
	}
	Flags->RemoveSingleSwap(CancellationFlag, EAllowShrinking::No);
	if (Flags->IsEmpty())
	{
		ActiveCancellationFlags.Remove(TurnId);
	}
}

void USpeechGenSubsystem::SetRuntimeState(const ESpeechGenRuntimeState State, const FString& Error)
{
	RuntimeStatus.State = State;
	RuntimeStatus.Error = Error;
	OnRuntimeStateChanged.Broadcast(RuntimeStatus);
	if (State == ESpeechGenRuntimeState::Failed)
	{
		UE_LOG(LogSpeechGen, Error, TEXT("%s"), *Error);
	}
	else if (State == ESpeechGenRuntimeState::Unavailable)
	{
		UE_LOG(LogSpeechGen, Warning, TEXT("%s"), *Error);
	}
	else if (State == ESpeechGenRuntimeState::Ready)
	{
		UE_LOG(LogSpeechGen, Log, TEXT("SpeechGen runtime ready: %s (%s)."),
			*RuntimeStatus.Model, *RuntimeStatus.Backend);
	}
}
