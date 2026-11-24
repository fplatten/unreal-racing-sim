#pragma once

#include "CoreMinimal.h"

/**
 * Port of com.racingsim.audio.SoundSynth: procedural, seamless mono loops (44.1 kHz) for the
 * engines, the tire squeal and the wind, generated at start-up (no audio files to ship). The
 * same seeds give the same sounds as the Java version.
 */
namespace SoundSynth
{
	constexpr int32 Rate = 44100;

	/** Racing V8 at a fixed rpm (in-car): exhaust pulses, crank-order harmonics, combustion noise. */
	RACINGSIM_API TArray<float> Engine(float Rpm, float Brightness, int64 Seed);
	/** An opponent's engine heard from outside the car: a deep, rough V8 rumble. */
	RACINGSIM_API TArray<float> ExteriorEngine(float Rpm, int64 Seed);
	/** Tire squeal: wobbling tonal squeal plus band-passed scrub noise. */
	RACINGSIM_API TArray<float> Squeal(int64 Seed);
	/** Wind / road rush. */
	RACINGSIM_API TArray<float> Wind(int64 Seed);
}
