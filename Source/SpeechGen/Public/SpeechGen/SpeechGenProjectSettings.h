#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SpeechGenProjectSettings.generated.h"

UENUM()
enum class ESpeechGenEditorLifecycleMode : uint8
{
	EditorSession UMETA(DisplayName = "Editor Session"),
	PIESession UMETA(DisplayName = "PIE Session"),
	Manual UMETA(DisplayName = "Manual")
};

USTRUCT()
struct SPEECHGEN_API FSpeechGenDevelopmentDefaults
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "Development")
	ESpeechGenEditorLifecycleMode EditorLifecycle = ESpeechGenEditorLifecycleMode::Manual;
};

USTRUCT()
struct SPEECHGEN_API FSpeechGenPackagedDefaults
{
	GENERATED_BODY()

	UPROPERTY(Config, EditAnywhere, Category = "Packaged Game")
	bool bStartRuntimeOnGameInstance = true;
};

/** One English word pronounced the project's way, overriding CMUdict and the letter-to-sound fallback. */
USTRUCT()
struct SPEECHGEN_API FSpeechGenPronunciation
{
	GENERATED_BODY()

	/** A single word, matched case-insensitively: letters and apostrophes only. */
	UPROPERTY(Config, EditAnywhere, Category = "Pronunciation")
	FString Word;

	/** CMUdict-style ARPAbet with stress digits, e.g. "L IY1 D" for lead (the verb). */
	UPROPERTY(Config, EditAnywhere, Category = "Pronunciation")
	FString Arpabet;
};

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "SpeechGen"))
class SPEECHGEN_API USpeechGenProjectSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(Config, EditAnywhere, Category = "Development")
	FSpeechGenDevelopmentDefaults DevelopmentDefaults;

	UPROPERTY(Config, EditAnywhere, Category = "Packaged Game")
	FSpeechGenPackagedDefaults PackagedDefaults;

	/**
	 * English words the project pronounces its own way (names, jargon, words whose first CMUdict entry
	 * is the wrong variant). Applied when the runtime loads; each entry wins over CMUdict.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Pronunciation")
	TArray<FSpeechGenPronunciation> PronunciationOverrides;

	virtual FName GetCategoryName() const override;
};
