#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SpeechGenRadioChannel.generated.h"

class UAudioComponent;
class USoundEffectSourcePreset;
class USoundEffectSourcePresetChain;

/** How hard the handheld colors the voice. Mid is the default. */
UENUM(BlueprintType)
enum class ESpeechGenRadioGrit : uint8
{
	Light,
	Mid,
	Heavy
};

/** The channel's sound: the effect chain and the levels of what the radio adds around the voice. */
USTRUCT(BlueprintType)
struct SPEECHGENRADIO_API FSpeechGenRadioChainSettings
{
	GENERATED_BODY()

	/** Band edges of the two-pole filters, applied twice; set outside 300-3400 Hz so that band stays flat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float HighPassHz = 180.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float LowPassHz = 4200.0f;

	/** The compressor is the handheld's AGC; its makeup drives the voice into the wave shaper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float CompressorThresholdDb = -30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float CompressorRatio = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float CompressorMakeupDb = 20.0f;

	/** Wave shaper drive, atan(Amount * x) / atan(Amount); below 1 the stage is bypassed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float SaturationAmount = 1.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float SaturationOutputDb = 2.0f;

	/**
	 * Hiss, beep, tail and dropout bursts are written into the PCM before the chain, so the compressor's makeup
	 * lifts them too; their levels are set low to land where the offline sox chain puts them.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float HissDb = -58.5f;

	/** How much louder the hiss gets at clarity 0 (linear in 1 - clarity). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float HissRiseDb = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float BeepDb = -43.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float TailDb = -38.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float DropoutBurstDb = -24.0f;

	static FSpeechGenRadioChainSettings ForGrit(ESpeechGenRadioGrit Grit);
};

/** How one transmission sounds on the channel. */
USTRUCT(BlueprintType)
struct SPEECHGENRADIO_API FSpeechGenRadioTransmissionSettings
{
	GENERATED_BODY()

	/** 1 = a clean handheld; lower adds hiss, fading and dropouts (wind, distance, loud music next to the talker). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Clarity = 1.0f;

	/** Seeds the hiss, fading and dropouts so a transmission renders the same way every time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	int32 Seed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Radio")
	float BeepFrequency = 1250.0f;
};

/**
 * One radio channel as heard on a handheld.
 *
 * Each transmission is assembled as PCM (key-up carrier, talk-permit beep, the voice with hiss and
 * clarity-driven dropouts, squelch tail) and played as a procedural sound through a source effect chain
 * that colors it like a handheld: high-pass and low-pass at 300 and 3400 Hz, compressor, wave shaper,
 * band-pass again. Generators live in code because the voice is procedural PCM; coloring lives in UE's
 * effect classes so it can be retuned or replaced by an authored chain.
 */
UCLASS(BlueprintType)
class SPEECHGENRADIO_API USpeechGenRadioChannel : public UObject
{
	GENERATED_BODY()

public:
	/** Builds the default (Mid) handheld chain if none is set. Called on first Transmit. */
	void Initialize();

	/** Rebuilds the chain with these settings; transmissions already playing keep the old one. */
	void Configure(const FSpeechGenRadioChainSettings& InSettings);
	const FSpeechGenRadioChainSettings& GetChainSettings() const { return ChainSettings; }

	/** Replaces the default chain, e.g. with an authored asset. */
	void SetEffectChain(USoundEffectSourcePresetChain* InChain);
	USoundEffectSourcePresetChain* GetEffectChain() const { return EffectChain; }

	/** Wraps a line of speech into one transmission; pure PCM, no audio device needed. Returns the dropouts cut in. */
	static int32 BuildTransmission(TConstArrayView<int16> Voice, int32 SampleRate,
		const FSpeechGenRadioChainSettings& Chain, const FSpeechGenRadioTransmissionSettings& Settings,
		TArray<int16>& OutPcm);

	/** Plays a line as a transmission on this channel; returns its length in seconds (0 if it could not play). */
	float Transmit(const UObject* WorldContextObject, TConstArrayView<int16> Voice, int32 SampleRate,
		const FSpeechGenRadioTransmissionSettings& Settings, int32* OutDropouts = nullptr);

	/** Stops every transmission still playing. */
	void StopAll();

private:
	UPROPERTY(Transient)
	FSpeechGenRadioChainSettings ChainSettings;

	UPROPERTY(Transient)
	TObjectPtr<USoundEffectSourcePresetChain> EffectChain;

	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundEffectSourcePreset>> Presets;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> Playing;
};
