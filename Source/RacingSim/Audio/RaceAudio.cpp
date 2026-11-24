#include "Audio/RaceAudio.h"
#include "Audio/SoundSynth.h"
#include "Car/RaceCar.h"
#include "Components/AudioComponent.h"
#include "AudioDevice.h"
#include "Engine/Engine.h"
#include "Misc/App.h"

namespace
{
	constexpr float LowRpm = 4500.f, HighRpm = 9500.f;
	constexpr float SpeedOfSound = 343.f;
	constexpr float Master = 0.9f;
	constexpr int32 AiVoices = 6;
	/** Bank indices: player engine low / high, squeal, wind, then the AI voicings. */
	enum { BankLow = 0, BankHigh = 1, BankSqueal = 2, BankWind = 3, BankAi = 4 };
	/** AI engines: OpenAL-style inverse distance attenuation. */
	constexpr float RefDistance = 9.f, MaxDistance = 2000.f;

	float Pitch(float P) { return FMath::Clamp(P, 0.5f, 2.0f); }   // the Java (OpenAL) pitch range

	float Smooth(float X)
	{
		X = FMath::Clamp(X, 0.f, 1.f);
		return X * X * (3 - 2 * X);
	}
}

// --------------------------------------------------------------------------- URaceMixWave

URaceMixWave::URaceMixWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetSampleRate(SoundSynth::Rate);
	NumChannels = 2;
	Duration = INDEFINITELY_LOOPING_DURATION;
	bLooping = false;
	SampleByteSize = 2;
}

void URaceMixWave::SetBanks(TArray<TArray<float>>&& InBanks)
{
	FScopeLock L(&Lock);
	Banks = MoveTemp(InBanks);
}

void URaceMixWave::SetVoices(const TArray<FVoice>& InVoices, bool bInActive)
{
	FScopeLock L(&Lock);
	Voices = InVoices;
	bActive = bInActive;
}

int32 URaceMixWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	const int32 Frames = NumSamples / 2;
	TArray<float> Mix;
	Mix.SetNumZeroed(Frames * 2);
	TArray<FVoice> V;
	bool bOn;
	{
		FScopeLock L(&Lock);
		V = Voices;
		bOn = bActive;
		if (bOn)
		{
			if (Positions.Num() < V.Num())
			{
				// new voices start at spread-out positions so identical loops never line up
				const int32 Old = Positions.Num();
				Positions.SetNum(V.Num());
				for (int32 i = Old; i < V.Num(); i++)
				{
					const int32 Len = Banks.IsValidIndex(V[i].Bank) ? Banks[V[i].Bank].Num() : 1;
					Positions[i] = (double)((i * 7919LL) % FMath::Max(Len, 1));
				}
			}
			if (Previous.Num() < V.Num()) Previous.SetNum(V.Num());
			for (int32 v = 0; v < V.Num(); v++)
			{
				if (!Banks.IsValidIndex(V[v].Bank)) continue;
				const TArray<float>& S = Banks[V[v].Bank];
				const int32 Len = S.Num();
				if (Len < 2) continue;
				// ramp the gains across the block (no zipper noise)
				const FVoice& A = Previous[v];
				const FVoice& B = V[v];
				const float L0 = A.Volume * A.GainL, R0 = A.Volume * A.GainR;
				const float L1 = B.Volume * B.GainL, R1 = B.Volume * B.GainR;
				double P = Positions[v];
				for (int32 i = 0; i < Frames; i++)
				{
					const float T = (float)i / Frames;
					const int32 I0 = (int32)P;
					const float F = (float)(P - I0);
					const float Sample = S[I0] * (1.f - F) + S[(I0 + 1) % Len] * F;
					Mix[i * 2] += Sample * (L0 + (L1 - L0) * T);
					Mix[i * 2 + 1] += Sample * (R0 + (R1 - R0) * T);
					P += B.Pitch;
					if (P >= Len) P -= Len;
				}
				Positions[v] = P;
				Previous[v] = B;
			}
		}
	}
	float Peak = 0.f;
	for (float M : Mix) Peak = FMath::Max(Peak, FMath::Abs(M));
	Calls++;
	SamplesOut += Frames * 2;
	LastPeak = Peak;
	OutAudio.SetNumUninitialized(Frames * 2 * sizeof(int16));
	int16* Out = (int16*)OutAudio.GetData();
	for (int32 i = 0; i < Frames * 2; i++)
	{
		// soft limit: a full field is louder than any single loop
		const float X = bOn ? FMath::Tanh(Mix[i] * 0.6f) : 0.f;
		Out[i] = (int16)FMath::RoundToInt(FMath::Clamp(X, -1.f, 1.f) * 32000.f);
	}
	return Frames * 2;
}

// ---------------------------------------------------------------------------- FRaceAudio

void FRaceAudio::Init(UObject* Owner)
{
	TArray<TArray<float>> Banks;
	Banks.Add(SoundSynth::Engine(LowRpm, 0.2f, 11));
	Banks.Add(SoundSynth::Engine(HighRpm, 0.85f, 12));
	Banks.Add(SoundSynth::Squeal(5));
	Banks.Add(SoundSynth::Wind(9));
	for (int32 v = 0; v < AiVoices; v++) Banks.Add(SoundSynth::ExteriorEngine(HighRpm, 21 + 13LL * v));

	Wave = NewObject<URaceMixWave>(Owner);
	Wave->SetBanks(MoveTemp(Banks));
	Wave->SoundGroup = SOUNDGROUP_Default;

	AActor* Actor = Cast<AActor>(Owner);
	Component = NewObject<UAudioComponent>(Owner);
	Component->bAutoActivate = false;
	Component->bAutoDestroy = false;
	Component->bIsUISound = true;          // not spatialized: the panning is done in the mix
	Component->bAllowSpatialization = false;
	Component->SetSound(Wave);
	if (Actor) Component->SetupAttachment(Actor->GetRootComponent());
	Component->RegisterComponent();
	Component->Play();
}

FString FRaceAudio::DebugState() const
{
	if (!Wave) return TEXT("no wave");
	FAudioDeviceHandle Dev = GEngine ? GEngine->GetMainAudioDevice() : FAudioDeviceHandle();
	const FString Device = FString::Printf(TEXT(" | app volume %.2f focus %d unfocused %.2f muted %d"),
		FApp::GetVolumeMultiplier(), FApp::HasFocus() ? 1 : 0, FApp::GetUnfocusedVolumeMultiplier(),
		Dev ? (Dev->IsAudioDeviceMuted() ? 1 : 0) : -1);
	return Device + FString::Printf(TEXT(" | component %s playing %d | wave calls %d samples %lld peak %.3f voices %d active %d"),
		Component ? TEXT("ok") : TEXT("none"), Component ? (Component->IsPlaying() ? 1 : 0) : -1,
		Wave->Calls.load(), Wave->SamplesOut.load(), Wave->LastPeak.load(), Wave->NumVoices(), Wave->IsActive() ? 1 : 0);
}

void FRaceAudio::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Wave);
	Collector.AddReferencedObject(Component);
}

void FRaceAudio::Update(float Dt, ARaceCar* Player, const TArray<TObjectPtr<ARaceCar>>& Field, const FVector& ListenerPos,
	const FVector& ListenerLeft, bool bActive)
{
	if (!Wave) return;
	TArray<URaceMixWave::FVoice> Voices;
	Voices.SetNum(4);
	for (int32 i = 0; i < 4; i++) Voices[i].Bank = i;
	if (!Player || !bActive)
	{
		Wave->SetVoices(Voices, false);
		return;
	}
	const FCarSnapshot P = Player->GetSnapshot();

	// ---- player engine ----
	const float Rpm = FMath::Max(P.Rpm, 2500.f);
	float Throttle = P.bShifting ? 0.f : P.Throttle;
	if (P.Gear == 0) Throttle = P.Brake;
	ThrottleSmooth += (Throttle - ThrottleSmooth) * FMath::Min(1.f, Dt / 0.08f);
	const float Load = 0.42f + 0.58f * ThrottleSmooth;
	const float Xf = Smooth((Rpm - 6000.f) / 2500.f);
	Voices[BankLow].Pitch = Pitch(Rpm / LowRpm);
	Voices[BankHigh].Pitch = Pitch(Rpm / HighRpm);
	Voices[BankLow].Volume = Master * Load * (1.f - Xf) * 0.9f;
	Voices[BankHigh].Volume = Master * Load * Xf;

	// ---- tires ----
	Voices[BankSqueal].Volume = Master * 0.85f * P.Squeal;
	Voices[BankSqueal].Pitch = Pitch(0.92f + 0.25f * P.Squeal);

	// ---- wind ----
	const float W = FMath::Clamp(P.Speed / 100.f, 0.f, 1.2f);
	Voices[BankWind].Volume = Master * 0.45f * W * W;
	Voices[BankWind].Pitch = Pitch(0.8f + 0.35f * W);

	// ---- AI engines: Doppler, distance and pan relative to the listener ----
	const FVector ListenerVel = Player->GetVelocity() / 100.0;
	// cars that left the field (the menu rebuilds it) free their voice
	for (auto It = AiVoiceId.CreateIterator(); It; ++It)
	{
		if (!Field.Contains(It.Key()))
		{
			AiState.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
	for (ARaceCar* Car : Field)
	{
		if (!Car || Car == Player) continue;
		int32* Id = AiVoiceId.Find(Car);
		if (!Id)
		{
			int32 N = 0;
			TArray<int32> Used;
			AiVoiceId.GenerateValueArray(Used);
			while (Used.Contains(N)) N++;
			// detune +-2.5 %: cars at the same rpm drift apart instead of beating together
			const float Detune = 1.f + 0.05f * (FMath::Fmod(N * 0.618034f, 1.f) - 0.5f);
			AiState.Add(Car, FVector4f(0.5f, 1.f, Detune, (float)(BankAi + N % AiVoices)));
			Id = &AiVoiceId.Add(Car, N);
		}
		FVector4f& St = AiState[Car];
		const FCarSnapshot C = Car->GetSnapshot();
		FVector To = Car->GetActorLocation() / 100.0 - ListenerPos;
		const float Dist = To.Size();
		float Doppler = 1.f;
		float Pan = 0.f;
		if (Dist > 0.5f)
		{
			To /= Dist;
			const float Vs = FVector::DotProduct(Car->GetVelocity() / 100.0, To);   // + = moving away
			const float Vl = FVector::DotProduct(ListenerVel, To);                  // + = moving toward
			Doppler = (SpeedOfSound + Vl) / (SpeedOfSound + Vs);
			Pan = FVector::DotProduct(To, ListenerLeft);                            // +1 = left
		}
		St.X += (C.Throttle - St.X) * FMath::Min(1.f, Dt / 0.25f);
		St.Y += (Doppler - St.Y) * FMath::Min(1.f, Dt / 0.12f);
		const float D = FMath::Clamp(Dist, RefDistance, MaxDistance);
		const float Gain = RefDistance / (RefDistance + (D - RefDistance));
		const int32 Slot = 4 + *Id;
		if (Voices.Num() <= Slot) Voices.SetNum(Slot + 1);
		URaceMixWave::FVoice& V = Voices[Slot];
		V.Bank = (int32)St.W;
		V.Pitch = Pitch(FMath::Max(C.Rpm, 3000.f) / HighRpm * St.Y * St.Z);
		V.Volume = Master * (0.6f + 0.4f * St.X) * 0.75f;
		V.GainL = Gain * (1.f + 0.5f * Pan);
		V.GainR = Gain * (1.f - 0.5f * Pan);
	}
	Wave->SetVoices(Voices, true);
}
