#pragma once

#include "CoreMinimal.h"
#include "Phonemizer/KokoroLanguageFrontend.h"
#include "SpeechGen/SpeechGenTypes.h"

struct FSpeechGenPronunciation;

class FKokoroPhonemizer
{
public:
	~FKokoroPhonemizer();
	bool Initialize(const FString& RuntimeDirectory, FString& OutError);
	/** Adds or replaces English dictionary entries; returns how many applied, with a warning per rejected entry. */
	int32 ApplyPronunciationOverrides(const TArray<FSpeechGenPronunciation>& Overrides,
		TArray<FString>& OutWarnings);
	bool Encode(ESpeechGenLanguage Language, const FString& Text, TArray<int64>& OutTokenIds,
		FString& OutPhonemes, FString& OutError) const;

private:
	static FString NormalizeEnglishText(const FString& Text);
	static FString NumberToWords(int64 Value);
	static TArray<FString> ApplyFliteLts(const FString& Word);
	static FString ArpabetToIpa(const TArray<FString>& Phones);
	static void AppendWordPhones(const FString& OriginalWord, const TMap<FString, TArray<FString>>& Dictionary,
		TArray<FString>& OutPhones);

	TMap<FString, TArray<FString>> Dictionary;
	TMap<FString, int64> Vocab;
	TUniquePtr<FKokoroLanguageFrontend> LanguageFrontend;
};
