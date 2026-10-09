#include "SpeechGenRadio/SpeechGenRadioChannel.h"

#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Math/RandomStream.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundWaveProcedural.h"
#include "SourceEffects/SourceEffectDynamicsProcessor.h"
#include "SourceEffects/SourceEffectFilter.h"
#include "SourceEffects/SourceEffectWaveShaper.h"
#include "TimerManager.h"
#include "Engine/World.h"

namespace
{
	// Transmission layout, in seconds: open carrier, talk-permit beep, carrier, voice, squelch tail.
	constexpr float CarrierBeforeBeep = 0.04f;
	constexpr float BeepLength = 0.07f;
	constexpr float CarrierAfterBeep = 0.05f;
	constexpr float TailLength = 0.16f;
	constexpr float KeptAir = 0.025f;
	// Below this the voice counts as silence when trimming (about -48 dBFS).
	constexpr int32 SilenceThreshold = 130;

	float DbToGain(const float Db)
	{
		return FMath::Pow(10.0f, Db / 20.0f);
	}

	void TrimSilence(TConstArrayView<int16> Voice, const int32 SampleRate, int32& OutStart, int32& OutEnd)
	{
		int32 First = 0;
		while (First < Voice.Num() && FMath::Abs(static_cast<int32>(Voice[First])) < SilenceThreshold)
		{
			++First;
		}
		int32 Last = Voice.Num() - 1;
		while (Last > First && FMath::Abs(static_cast<int32>(Voice[Last])) < SilenceThreshold)
		{
			--Last;
		}
		const int32 Air = FMath::RoundToInt(KeptAir * SampleRate);
		OutStart = FMath::Max(0, First - Air);
		OutEnd = FMath::Min(Voice.Num(), Last + 1 + Air);
	}

	// USoundEffectPreset::AddEffectInstance runs Init() on first use, which copies the preset's Settings
	// property over whatever SetSettings stored. A preset made at runtime with only SetSettings plays with
	// the class defaults (for the filter, a resonant 800 Hz low-pass), so set the property too.
	template <typename PresetType, typename SettingsType>
	PresetType* MakePreset(UObject* Outer, const SettingsType& Settings)
	{
		PresetType* Preset = NewObject<PresetType>(Outer);
		Preset->Settings = Settings;
		Preset->SetSettings(Settings);
		return Preset;
	}

	USourceEffectFilterPreset* MakeFilter(UObject* Outer, const ESourceEffectFilterType Type, const float Cutoff)
	{
		FSourceEffectFilterSettings Settings;
		Settings.FilterCircuit = ESourceEffectFilterCircuit::StateVariable;
		Settings.FilterType = Type;
		Settings.CutoffFrequency = Cutoff;
		Settings.FilterQ = 0.707f;
		return MakePreset<USourceEffectFilterPreset>(Outer, Settings);
	}
}

FSpeechGenRadioChainSettings FSpeechGenRadioChainSettings::ForGrit(const ESpeechGenRadioGrit Grit)
{
	// Tuned on a short call and a long broadcast against two references at clarity 1, measured inside transmissions:
	// the offline sox chain (crest 6.8, hiss 32 dB under the voice, "too clear") and the first in-engine
	// demo (crest 5.7, hiss 27 dB under, "way too distorted"). Mid sits halfway; light and heavy bracket it.
	FSpeechGenRadioChainSettings Settings;
	switch (Grit)
	{
	case ESpeechGenRadioGrit::Light:
		Settings.CompressorThresholdDb = -31.0f;
		Settings.SaturationAmount = 1.5f;
		Settings.HissDb = -61.5f;
		break;
	case ESpeechGenRadioGrit::Mid:
		break;
	case ESpeechGenRadioGrit::Heavy:
		Settings.SaturationAmount = 2.0f;
		Settings.SaturationOutputDb = 1.0f;
		Settings.HissDb = -57.0f;
		break;
	}
	return Settings;
}

void USpeechGenRadioChannel::Initialize()
{
	if (!EffectChain)
	{
		Configure(FSpeechGenRadioChainSettings::ForGrit(ESpeechGenRadioGrit::Mid));
	}
}

void USpeechGenRadioChannel::Configure(const FSpeechGenRadioChainSettings& InSettings)
{
	ChainSettings = InSettings;

	USourceEffectDynamicsProcessorPreset* Compressor = nullptr;
	{
		FSourceEffectDynamicsProcessorSettings Settings;
		Settings.DynamicsProcessorType = ESourceEffectDynamicsProcessorType::Compressor;
		Settings.PeakMode = ESourceEffectDynamicsPeakMode::Peak;
		Settings.LookAheadMsec = 3.0f;
		Settings.AttackTimeMsec = 2.0f;
		Settings.ReleaseTimeMsec = 100.0f;
		Settings.ThresholdDb = ChainSettings.CompressorThresholdDb;
		Settings.Ratio = ChainSettings.CompressorRatio;
		Settings.KneeBandwidthDb = 6.0f;
		Settings.OutputGainDb = ChainSettings.CompressorMakeupDb;
		Compressor = MakePreset<USourceEffectDynamicsProcessorPreset>(this, Settings);
	}

	// Mic band, dynamics, grit, then the speaker band so the grit stays in band.
	Presets = {
		MakeFilter(this, ESourceEffectFilterType::HighPass, ChainSettings.HighPassHz),
		MakeFilter(this, ESourceEffectFilterType::LowPass, ChainSettings.LowPassHz),
		Compressor
	};
	if (ChainSettings.SaturationAmount >= 1.0f)
	{
		// atan(Amount * x) / atan(Amount): rounds the peaks and lifts quiet parts by Amount / atan(Amount).
		FSourceEffectWaveShaperSettings Settings;
		Settings.Amount = ChainSettings.SaturationAmount;
		Settings.OutputGainDb = ChainSettings.SaturationOutputDb;
		Presets.Add(MakePreset<USourceEffectWaveShaperPreset>(this, Settings));
	}
	Presets.Add(MakeFilter(this, ESourceEffectFilterType::HighPass, ChainSettings.HighPassHz));
	Presets.Add(MakeFilter(this, ESourceEffectFilterType::LowPass, ChainSettings.LowPassHz));

	EffectChain = NewObject<USoundEffectSourcePresetChain>(this);
	for (USoundEffectSourcePreset* Preset : Presets)
	{
		FSourceEffectChainEntry& Entry = EffectChain->Chain.AddDefaulted_GetRef();
		Entry.Preset = Preset;
		Entry.bBypass = false;
	}
}

void USpeechGenRadioChannel::SetEffectChain(USoundEffectSourcePresetChain* InChain)
{
	EffectChain = InChain;
	Presets.Reset();
}

int32 USpeechGenRadioChannel::BuildTransmission(TConstArrayView<int16> Voice, const int32 SampleRate,
	const FSpeechGenRadioChainSettings& Chain, const FSpeechGenRadioTransmissionSettings& Settings,
	TArray<int16>& OutPcm)
{
	int32 Dropouts = 0;
	const float Clarity = FMath::Clamp(Settings.Clarity, 0.0f, 1.0f);
	const float Grit = 1.0f - Clarity;
	FRandomStream Random(Settings.Seed);

	int32 VoiceStart = 0;
	int32 VoiceEnd = 0;
	TrimSilence(Voice, SampleRate, VoiceStart, VoiceEnd);
	const int32 VoiceLength = FMath::Max(0, VoiceEnd - VoiceStart);

	const int32 CarrierSamples = FMath::RoundToInt(CarrierBeforeBeep * SampleRate);
	const int32 BeepSamples = FMath::RoundToInt(BeepLength * SampleRate);
	const int32 GapSamples = FMath::RoundToInt(CarrierAfterBeep * SampleRate);
	const int32 TailSamples = FMath::RoundToInt(TailLength * SampleRate);
	const int32 VoiceOffset = CarrierSamples + BeepSamples + GapSamples;
	const int32 TailOffset = VoiceOffset + VoiceLength;

	TArray<float> Mix;
	Mix.SetNumZeroed(TailOffset + TailSamples);

	// Hiss for as long as the carrier is open; full band here, the chain's band-pass shapes it.
	// White noise in [-A, A] has RMS A / sqrt(3). The chain's makeup gain and the wave shaper's lift on
	// quiet signals raise it, so it starts low.
	const float HissAmplitude = DbToGain(Chain.HissDb + Chain.HissRiseDb * Grit) * UE_SQRT_3;
	for (float& Sample : Mix)
	{
		Sample = Random.FRandRange(-HissAmplitude, HissAmplitude);
	}

	// Talk-permit beep with short fades so it doesn't click; the chain lifts it like the hiss.
	const float BeepGain = DbToGain(Chain.BeepDb);
	const int32 BeepFadeIn = FMath::RoundToInt(0.004f * SampleRate);
	const int32 BeepFadeOut = FMath::RoundToInt(0.012f * SampleRate);
	for (int32 Index = 0; Index < BeepSamples; ++Index)
	{
		const float Envelope = FMath::Min3(1.0f, static_cast<float>(Index) / BeepFadeIn,
			static_cast<float>(BeepSamples - Index) / BeepFadeOut);
		Mix[CarrierSamples + Index] += BeepGain * Envelope
			* FMath::Sin(UE_TWO_PI * Settings.BeepFrequency * Index / SampleRate);
	}

	// The voice, with fading and dropouts as clarity falls.
	TArray<float> VoiceGain;
	VoiceGain.Init(1.0f, VoiceLength);
	if (Grit > UE_KINDA_SMALL_NUMBER && VoiceLength > 0)
	{
		const float FlutterDepth = 0.6f * Grit;
		const float FlutterRate = Random.FRandRange(3.0f, 7.0f);
		const float FlutterPhase = Random.FRandRange(0.0f, UE_TWO_PI);
		for (int32 Index = 0; Index < VoiceLength; ++Index)
		{
			const float Wave = 0.5f * (1.0f + FMath::Sin(UE_TWO_PI * FlutterRate * Index / SampleRate + FlutterPhase));
			VoiceGain[Index] = 1.0f - FlutterDepth * Wave;
		}

		// Dropouts arrive as a Poisson process: about 2.5 a second at clarity 0, none at clarity 1.
		const float DropoutsPerSecond = 2.5f * Grit;
		const float BurstAmplitude = DbToGain(Chain.DropoutBurstDb) * UE_SQRT_3;
		const int32 Ramp = FMath::RoundToInt(0.005f * SampleRate);
		int32 Position = 0;
		for (;;)
		{
			Position += FMath::RoundToInt(-FMath::Loge(FMath::Max(Random.FRand(), 1e-6f)) / DropoutsPerSecond * SampleRate);
			if (Position >= VoiceLength)
			{
				break;
			}
			const int32 Length = FMath::RoundToInt(Random.FRandRange(0.04f, 0.18f) * SampleRate);
			++Dropouts;
			for (int32 Offset = 0; Offset < Length && Position + Offset < VoiceLength; ++Offset)
			{
				const float Edge = FMath::Min3(1.0f, static_cast<float>(Offset) / Ramp,
					static_cast<float>(Length - Offset) / Ramp);
				VoiceGain[Position + Offset] *= FMath::Lerp(1.0f, 0.05f, Edge);
				Mix[VoiceOffset + Position + Offset] += Edge * Random.FRandRange(-BurstAmplitude, BurstAmplitude);
			}
			Position += Length;
		}
	}
	for (int32 Index = 0; Index < VoiceLength; ++Index)
	{
		Mix[VoiceOffset + Index] += VoiceGain[Index] * Voice[VoiceStart + Index] / 32768.0f;
	}

	// Squelch tail: an unkey burst, loud at the front, gone fast.
	const float TailAmplitude = DbToGain(Chain.TailDb) * UE_SQRT_3;
	const int32 TailHold = FMath::RoundToInt(0.2f * TailSamples);
	for (int32 Index = 0; Index < TailSamples; ++Index)
	{
		const float Fade = Index < TailHold ? 1.0f
			: FMath::Cos(0.5f * UE_PI * (Index - TailHold) / static_cast<float>(TailSamples - TailHold));
		Mix[TailOffset + Index] += Fade * Random.FRandRange(-TailAmplitude, TailAmplitude);
	}

	OutPcm.SetNumUninitialized(Mix.Num());
	for (int32 Index = 0; Index < Mix.Num(); ++Index)
	{
		OutPcm[Index] = static_cast<int16>(FMath::Clamp(Mix[Index], -1.0f, 1.0f) * 32767.0f);
	}
	return Dropouts;
}

float USpeechGenRadioChannel::Transmit(const UObject* WorldContextObject, TConstArrayView<int16> Voice,
	const int32 SampleRate, const FSpeechGenRadioTransmissionSettings& Settings, int32* OutDropouts)
{
	Initialize();
	UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	if (!World || Voice.IsEmpty() || SampleRate <= 0)
	{
		return 0.0f;
	}

	TArray<int16> Pcm;
	const int32 Dropouts = BuildTransmission(Voice, SampleRate, ChainSettings, Settings, Pcm);
	if (OutDropouts)
	{
		*OutDropouts = Dropouts;
	}
	const float Seconds = static_cast<float>(Pcm.Num()) / SampleRate;

	USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this);
	Wave->SetSampleRate(SampleRate);
	Wave->NumChannels = 1;
	Wave->Duration = Seconds;
	Wave->SoundGroup = SOUNDGROUP_Voice;
	Wave->bLooping = false;
	// Dev switch for level checks: -SpeechGenRadioNoChain plays the raw transmission PCM.
	static const bool bNoChain = FParse::Param(FCommandLine::Get(), TEXT("SpeechGenRadioNoChain"));
	Wave->SourceEffectChain = bNoChain ? nullptr : EffectChain.Get();
	Wave->QueueAudio(reinterpret_cast<const uint8*>(Pcm.GetData()), Pcm.Num() * sizeof(int16));

	// Measured in UE 5.8 (-game, both the CoreAudio and non-realtime devices): a mono procedural wave played 2D
	// comes out about 12 dB under its PCM on each channel, chain or no chain. Not traced further; the
	// component's maximum volume multiplier (4.0, +12 dB) puts it back at the level it was written at.
	constexpr float PlaybackMakeup = 4.0f;
	UAudioComponent* Component = UGameplayStatics::CreateSound2D(World, Wave, PlaybackMakeup, 1.0f, 0.0f, nullptr, false, false);
	if (!Component)
	{
		return 0.0f;
	}
	Component->Play();
	Playing.Add(Component);

	// A procedural wave plays until stopped; stop it once the transmission has played out.
	TWeakObjectPtr<USpeechGenRadioChannel> WeakThis(this);
	TWeakObjectPtr<UAudioComponent> WeakComponent(Component);
	FTimerHandle Handle;
	World->GetTimerManager().SetTimer(Handle, [WeakThis, WeakComponent]()
	{
		if (UAudioComponent* Finished = WeakComponent.Get())
		{
			Finished->Stop();
			if (USpeechGenRadioChannel* Channel = WeakThis.Get())
			{
				Channel->Playing.Remove(Finished);
			}
			Finished->DestroyComponent();
		}
	}, Seconds + 0.1f, false);
	return Seconds;
}

void USpeechGenRadioChannel::StopAll()
{
	for (UAudioComponent* Component : Playing)
	{
		if (Component)
		{
			Component->Stop();
			Component->DestroyComponent();
		}
	}
	Playing.Reset();
}
