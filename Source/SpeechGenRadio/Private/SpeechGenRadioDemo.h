#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SpeechGen/SpeechGenTypes.h"
#include "SpeechGenRadio/SpeechGenRadioChannel.h"
#include "SpeechGenRadioDemo.generated.h"

class USpeechGenRadioChannel;
class USpeechGenSubsystem;

/**
 * Test harness for running SpeechGen inside a game session. Exists only when the command line asks for it:
 *
 *   -SpeechGenBench       Load the model, synthesize the demo lines back to back, and measure frame period,
 *                         game-thread work and a fixed slice of simulated game work per frame. Writes
 *                         bench-frames.csv and logs a per-phase summary, then quits.
 *   -SpeechGenRadioDemo   Synthesize a scripted dispatch exchange and play it through USpeechGenRadioChannel,
 *                         once per clarity in -SpeechGenRadioClarity=1.0,0.4 (default 1.0), then quit.
 *                         -SpeechGenRadioGrit=light|mid|heavy picks the chain (default mid).
 *                         -SpeechGenRadioHailAfter=<sim seconds> is when a reply turns from one shot into a full
 *                         call (default 30).
 *                         -SpeechGenRadioLadder instead plays a call and a broadcast once per grit, at clarity 1.
 *                         -SpeechGenRadioLadderSpec="threshold,ratio,makeup,saturation,saturation out,hiss[,hp,lp];..."
 *                         plays the ladder lines once per explicit chain, for tuning the presets.
 *
 * Options: -SpeechGenOut=<dir> (default Saved/SpeechGenOut/engine), -SpeechGenSimWorkMs=<ms> (bench, default 4),
 * -SpeechGenRadioRecord (demo: record the master submix of each pass to <dir>/<pass name>.wav).
 */
UCLASS()
class USpeechGenRadioDemo : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	enum class EPhase : uint8 { Boot, Idle, Load, Settle, Synth, Tail, Prepare, Play, Done };

	struct FLine
	{
		int32 Speaker = 0;
		FString Text;
		float GapBefore = 0.5f;
		FString Note;
		FGuid SegmentId;
		TArray<int16> Pcm;
		int32 SampleRate = 0;
		double SubmittedAt = 0.0;
		double ReadyAt = 0.0;
	};

	struct FPass
	{
		float Clarity = 1.0f;
		ESpeechGenRadioGrit Grit = ESpeechGenRadioGrit::Mid;
		FString Name;
		/** Set by -SpeechGenRadioLadderSpec: explicit chain settings instead of a grit preset. */
		TOptional<FSpeechGenRadioChainSettings> Chain;
	};

	struct FFrame
	{
		EPhase Phase = EPhase::Boot;
		double PeriodMs = 0.0;
		double WorkMs = 0.0;
		double SimMs = 0.0;
		int32 Results = 0;
	};

	bool Tick(float DeltaTime);
	void BuildScript();
	void SubmitAll();
	void SetPhase(EPhase InPhase);
	void RunSimWork();
	void HandleSamplingInput();
	void HandleEndFrame();
	void HandleSegmentReady(const FSpeechGenResult& Result);
	void HandleRequestFailed(FGuid TurnId, FGuid SegmentId, const FString& Error);
	void TickPlay(double Now);
	void WriteBenchReport() const;
	void Finish(int32 ExitCode);
	static const TCHAR* PhaseName(EPhase Phase);

	UPROPERTY(Transient)
	TObjectPtr<USpeechGenRadioChannel> Channel;

	TWeakObjectPtr<USpeechGenSubsystem> SpeechGen;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle SamplingInputHandle;
	FDelegateHandle EndFrameHandle;

	bool bBench = false;
	bool bRecord = false;
	FString OutDir;
	TArray<FPass> Passes;
	bool bLadder = false;
	double HailAfterSimSeconds = 30.0;
	TArray<FLine> Lines;
	int32 LinesReady = 0;
	int32 LinesFailed = 0;

	EPhase Phase = EPhase::Boot;
	double PhaseStartedAt = 0.0;
	double LoadMilliseconds = 0.0;

	// Frame measurement.
	TArray<FFrame> Frames;
	double LastSamplingAt = 0.0;
	double WorkStartedAt = 0.0;
	double LastSimMs = 0.0;
	int32 ResultsThisFrame = 0;

	// Simulated game work: a fixed amount of arithmetic per frame, calibrated to SimWorkTargetMs while idle.
	double SimWorkTargetMs = 4.0;
	int64 SimIterations = 0;
	int32 CalibrationFrames = 0;
	double CalibrationMs = 0.0;
	double SimSink = 0.0;

	// Playback.
	int32 PassIndex = 0;
	int32 NextLine = 0;
	double NextStartAt = 0.0;
	double PassEndsAt = 0.0;
	bool bPassRecording = false;
};
