#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SpeechGenSynthCommandlet.generated.h"

/**
 * Headless synthesis to WAV, for testing and benchmarking the Kokoro runtime without the editor UI.
 *
 * The module prefix is required: SpeechGenEditor loads at PostEngineInit, after the engine looks up
 * commandlet classes, and "Module.Name" makes the engine load the module first.
 *
 * Single line:
 *   UnrealEditor-Cmd <Project>.uproject -run=SpeechGenEditor.SpeechGenSynth -Text="Copy that, heading over now."
 *     -Voice=am_adam:0.7,bm_george:0.3 -Out=/path/out.wav [-Speed=1.0] -unattended -nullrhi -nosplash
 *
 * Batch (one model load, so warm latency is measurable):
 *   ... -run=SpeechGenEditor.SpeechGenSynth -Batch=/path/lines.tsv [-OutDir=/path] [-Timings=/path/timings.csv]
 *   Each non-empty, non-# line is: out.wav <TAB> voice blend <TAB> text [<TAB> speed]
 *   Relative output paths resolve against -OutDir, else the batch file's directory.
 *
 * Phonemize only (no audio): -Phonemize=/path/words.txt [-Report=/path/report.tsv] writes, for each word of each
 * line, the IPA, its source (project override, CMUdict with its variant count, or letter-to-sound) and every
 * CMUdict variant, to vet names and jargon before they reach a voice.
 *
 * The generated audio cache is cleared before every line so each result is a real inference;
 * pass -KeepCache to allow cache hits.
 */
UCLASS()
class USpeechGenSynthCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USpeechGenSynthCommandlet();

	virtual int32 Main(const FString& Params) override;
};
