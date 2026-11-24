#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include <atomic>
#include "RaceAudio.generated.h"

class ARaceCar;
class UAudioComponent;

/**
 * The whole race soundtrack as one procedural stereo stream: every loop (player engine low /
 * high, tire squeal, wind, one engine per AI car) is resampled at its own pitch, scaled and
 * panned here, on the audio render thread. This is the Java CarAudio.mixdown approach, which
 * gives exact control of pitch (and Doppler) for every source.
 */
UCLASS()
class RACINGSIM_API URaceMixWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	URaceMixWave(const FObjectInitializer& ObjectInitializer);

	struct FVoice
	{
		int32 Bank = 0;
		float Volume = 0.f;
		float Pitch = 1.f;
		float GainL = 1.f, GainR = 1.f;
	};

	/** The loops (mono, 44.1 kHz); set once before playing. */
	void SetBanks(TArray<TArray<float>>&& InBanks);
	/** Game thread: this frame's voice parameters (indices are stable voice ids). */
	void SetVoices(const TArray<FVoice>& InVoices, bool bInActive);

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;

	/** Diagnostics (-AudioDebug): render callbacks, samples rendered, peak output level of the last block. */
	std::atomic<int32> Calls{ 0 };
	std::atomic<int64> SamplesOut{ 0 };
	std::atomic<float> LastPeak{ 0.f };
	int32 NumVoices() { FScopeLock L(&Lock); return Voices.Num(); }
	bool IsActive() { FScopeLock L(&Lock); return bActive; }

private:
	TArray<TArray<float>> Banks;
	FCriticalSection Lock;
	TArray<FVoice> Voices;
	bool bActive = false;
	// audio thread state
	TArray<double> Positions;
	TArray<FVoice> Previous;
};

/**
 * Port of com.racingsim.audio.CarAudio: drives the mix from the race state every frame.
 *  - Player: engine loops recorded at 4,500 and 9,500 rpm, pitch-shifted to the real rpm and
 *    cross-faded, louder on throttle; tire squeal from slip beyond the grip peak; wind with speed.
 *  - AI cars: darker exterior engine loops in six voicings with a small per-car detune, Doppler
 *    relative to the listener (the camera), distance attenuation and stereo pan.
 */
class RACINGSIM_API FRaceAudio
{
public:
	/** Synthesizes the loops (about a second of CPU) and starts the stream. */
	void Init(UObject* Owner);
	/** @param bActive false = silent (menu, pause prompt) */
	void Update(float Dt, ARaceCar* Player, const TArray<TObjectPtr<ARaceCar>>& Field, const FVector& ListenerPos,
		const FVector& ListenerLeft, bool bActive);
	void AddReferencedObjects(FReferenceCollector& Collector);
	FString DebugState() const;

private:
	TObjectPtr<URaceMixWave> Wave;
	TObjectPtr<UAudioComponent> Component;
	float ThrottleSmooth = 0.f;
	/** Per AI car: smoothed throttle, smoothed Doppler, detune, voice bank. */
	TMap<const ARaceCar*, FVector4f> AiState;
	TMap<const ARaceCar*, int32> AiVoiceId;
	int32 NextAiVoice = 0;
};
