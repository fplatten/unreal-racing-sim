#include "Audio/SoundSynth.h"
#include "Sim/RaceLogic.h"

namespace SoundSynth
{
	namespace
	{
		constexpr double Pi = 3.14159265358979323846;

		int64 JRound(double X) { return (int64)FMath::FloorToDouble(X + 0.5); }   // Java Math.round

		/** White noise whose end cross-fades into its start (seamless when looped). */
		TArray<float> LoopNoise(int32 N, FJavaRandom& Rnd)
		{
			const int32 Fade = Rate / 10;
			TArray<float> Raw;
			Raw.SetNumUninitialized(N + Fade);
			for (float& V : Raw) V = (float)(Rnd.NextDouble() * 2 - 1);
			TArray<float> Out;
			Out.SetNumUninitialized(N);
			for (int32 i = 0; i < N; i++) Out[i] = Raw[i];
			for (int32 i = 0; i < Fade; i++)
			{
				const float W = (float)i / Fade;
				Out[i] = Raw[i] * W + Raw[N + i] * (1 - W);
			}
			return Out;
		}

		float Peak(const TArray<float>& X)
		{
			float P = 0;
			for (float V : X) P = FMath::Max(P, FMath::Abs(V));
			return P;
		}

		void Saturate(TArray<float>& X, float Drive)
		{
			const float P = Peak(X);
			if (P == 0) return;
			for (float& V : X) V = (float)FMath::Tanh((double)Drive * V / P);
		}

		void Normalize(TArray<float>& X, float Level)
		{
			const float P = Peak(X);
			if (P == 0) return;
			const float K = Level / P;
			for (float& V : X) V *= K;
		}

		/** One-pole low pass, run twice around the loop so the filter state is seamless. */
		void LowPass(TArray<float>& X, double Cutoff)
		{
			const double A = FMath::Exp(-2 * Pi * Cutoff / Rate);
			double Y = 0;
			const TArray<float> In = X;
			for (int32 Pass = 0; Pass < 2; Pass++)
			{
				for (int32 i = 0; i < In.Num(); i++)
				{
					Y = (1 - A) * In[i] + A * Y;
					if (Pass == 1) X[i] = (float)Y;
				}
			}
		}

		void HighPass(TArray<float>& X, double Cutoff)
		{
			TArray<float> Low = X;
			LowPass(Low, Cutoff);
			for (int32 i = 0; i < X.Num(); i++) X[i] -= Low[i];
		}

		/** RBJ band-pass biquad, also primed by a first pass around the loop. */
		void BandPass(TArray<float>& X, double Center, double Q)
		{
			const double W0 = 2 * Pi * Center / Rate, Alpha = FMath::Sin(W0) / (2 * Q);
			const double B0 = Alpha, B2 = -Alpha, A0 = 1 + Alpha, A1 = -2 * FMath::Cos(W0), A2 = 1 - Alpha;
			double X1 = 0, X2 = 0, Y1 = 0, Y2 = 0;
			const TArray<float> In = X;
			for (int32 Pass = 0; Pass < 2; Pass++)
			{
				for (int32 i = 0; i < In.Num(); i++)
				{
					const double Y = (B0 * In[i] + B2 * X2 - A1 * Y1 - A2 * Y2) / A0;
					X2 = X1;
					X1 = In[i];
					Y2 = Y1;
					Y1 = Y;
					if (Pass == 1) X[i] = (float)Y;
				}
			}
		}

		double PosMod(double A, double B) { return FMath::Fmod(FMath::Fmod(A, B) + B, B); }

		TArray<float> EngineImpl(float Rpm, float Brightness, int64 Seed, double Unevenness)
		{
			FJavaRandom Rnd(Seed);
			const double CrankHz = Rpm / 60.0;
			const int32 SamplesPerCycle = (int32)JRound(Rate * 2.0 / CrankHz);   // 720 degrees
			const int32 Cycles = FMath::Max(1, (int32)JRound(1.2 * Rate / SamplesPerCycle));
			const int32 N = SamplesPerCycle * Cycles;
			const double CycleHz = (double)Rate / SamplesPerCycle;
			const double Crank = CycleHz * 2.0;
			TArray<float> Out;
			Out.SetNumZeroed(N);

			// per-cylinder character, identical every cycle -> periodic
			const int32 Cylinders = 8;
			double Amp[8], Offset[8], Res[8];
			for (int32 c = 0; c < Cylinders; c++)
			{
				Amp[c] = 1.0 + Unevenness * (Rnd.NextDouble() - 0.5);
				Offset[c] = (Rnd.NextDouble() - 0.5) * 0.04;
				Res[c] = 1.0 + (Rnd.NextDouble() - 0.5) * 0.12;
			}
			const double ResLow = 160 + 260 * Brightness;
			const double ResHigh = 900 + 1000 * Brightness;
			const double DecayLow = 0.0045 - 0.002 * Brightness, DecayHigh = 0.0012;
			TArray<float> Pulse;
			Pulse.SetNumZeroed(SamplesPerCycle);
			for (int32 c = 0; c < Cylinders; c++)
			{
				const double Start = (c + Offset[c]) / Cylinders * SamplesPerCycle;
				for (int32 k = 0; k < SamplesPerCycle; k++)
				{
					const double T = PosMod(k - Start, SamplesPerCycle) / Rate;
					const double Body = FMath::Exp(-T / DecayLow) * FMath::Sin(2 * Pi * ResLow * Res[c] * T);
					const double Rasp = FMath::Exp(-T / DecayHigh) * FMath::Sin(2 * Pi * ResHigh * Res[c] * T);
					Pulse[k] += (float)(Amp[c] * (Body + (0.25 + 0.2 * Brightness) * Rasp));
				}
			}
			// combustion noise bursts, also periodic per cycle
			TArray<float> Burst;
			Burst.SetNumZeroed(SamplesPerCycle);
			for (int32 c = 0; c < Cylinders; c++)
			{
				const double Start = (double)c / Cylinders * SamplesPerCycle;
				FJavaRandom Nr(Seed * 31 + c);
				for (int32 k = 0; k < SamplesPerCycle; k++)
				{
					const double T = PosMod(k - Start, SamplesPerCycle) / Rate;
					Burst[k] += (float)(FMath::Exp(-T / 0.0008) * (Nr.NextDouble() * 2 - 1));
				}
			}
			for (int32 i = 0; i < N; i++)
			{
				const int32 k = i % SamplesPerCycle;
				const double T = (double)i / Rate;
				double Mech = 0;
				for (int32 h = 1; h <= 24; h++)   // crank orders
				{
					double A = (h % 4 == 0 ? 0.5 : 0.12) / h;   // firing orders dominate
					if (h == 2) A = 0.25;
					Mech += A * FMath::Sin(2 * Pi * Crank * h * T + h * 0.7);
				}
				const double Whine = 0.04 * Brightness * FMath::Sin(2 * Pi * Crank * 32 * T);   // gears / valve train
				Out[i] = (float)(0.75 * Pulse[k] + 0.25 * Mech + 0.12 * Burst[k] + Whine);
			}
			Saturate(Out, 1.6f + Brightness);
			HighPass(Out, 40);
			Normalize(Out, 0.9f);
			return Out;
		}
	}

	TArray<float> Engine(float Rpm, float Brightness, int64 Seed)
	{
		return EngineImpl(Rpm, Brightness, Seed, 0.4);
	}

	TArray<float> ExteriorEngine(float Rpm, int64 Seed)
	{
		FJavaRandom Rnd(Seed * 17 + 3);
		const int32 Spc = (int32)JRound(Rate * 2.0 / (Rpm / 60.0));   // samples per 720 degrees
		const int32 Cycles = FMath::Max(1, (int32)JRound(1.5 * Rate / Spc));
		const int32 N = Spc * Cycles;
		const int32 Cylinders = 8, Firings = Cylinders * Cycles;
		const double Interval = (double)N / Firings;
		double CylAmp[8], CylRes[8];
		for (int32 c = 0; c < Cylinders; c++)
		{
			CylAmp[c] = 1.0 + 0.7 * (Rnd.NextDouble() - 0.5);
			CylRes[c] = 1.0 + 0.10 * (Rnd.NextDouble() - 0.5);
		}
		const double Body = 150 + 40 * Rnd.NextDouble();   // exhaust body resonance (Hz)
		const double Rasp = 700 + 200 * Rnd.NextDouble();
		const int32 Len = (int32)(0.03 * Rate);
		TArray<float> E;
		E.SetNumZeroed(N);
		for (int32 j = 0; j < Firings; j++)
		{
			const int32 c = j % Cylinders;
			const double Start = j * Interval + (Rnd.NextDouble() - 0.5) * 0.16 * Interval;
			const double A = CylAmp[c] * (0.9 + 0.2 * Rnd.NextDouble());
			const double Fb = Body * CylRes[c] * (1 + 0.08 * (Rnd.NextDouble() - 0.5));
			const double Fr = Rasp * (1 + 0.15 * (Rnd.NextDouble() - 0.5));
			const int32 I0 = (int32)FMath::CeilToDouble(Start);
			for (int32 k = 0; k < Len; k++)
			{
				const double T = (I0 + k - Start) / Rate;
				const double V = FMath::Exp(-T / 0.0050) * FMath::Sin(2 * Pi * Fb * T)
					+ 0.22 * FMath::Exp(-T / 0.0011) * FMath::Sin(2 * Pi * Fr * T);
				const int32 Idx = ((I0 + k) % N + N) % N;
				E[Idx] += (float)(A * V);
			}
		}
		LowPass(E, 2000);
		LowPass(E, 2000);
		Normalize(E, 1.f);
		TArray<float> Roar = LoopNoise(N, Rnd);
		LowPass(Roar, 260);
		LowPass(Roar, 260);
		Normalize(Roar, 1.f);
		TArray<float> Out;
		Out.SetNumUninitialized(N);
		for (int32 i = 0; i < N; i++) Out[i] = 0.9f * E[i] + 0.10f * Roar[i];
		Saturate(Out, 1.5f);
		HighPass(Out, 30);
		Normalize(Out, 0.9f);
		return Out;
	}

	TArray<float> Squeal(int64 Seed)
	{
		FJavaRandom Rnd(Seed);
		const int32 N = (int32)(Rate * 1.5);
		const double Dur = (double)N / Rate;
		TArray<float> Out;
		Out.SetNumUninitialized(N);
		// loop-safe LFOs: whole numbers of cycles per loop
		const double WobbleHz = JRound(5.0 * Dur) / Dur, FlutterHz = JRound(43.0 * Dur) / Dur;
		const double Base = 870;
		double Phase1 = 0, Phase2 = 0, Phase3 = 0;
		for (int32 i = 0; i < N; i++)
		{
			const double T = (double)i / Rate;
			const double Wobble = 1 + 0.035 * FMath::Sin(2 * Pi * WobbleHz * T) + 0.012 * FMath::Sin(2 * Pi * WobbleHz * 3 * T + 1);
			const double F = Base * Wobble;
			Phase1 += 2 * Pi * F / Rate;
			Phase2 += 2 * Pi * F * 2.02 / Rate;
			Phase3 += 2 * Pi * F * 3.1 / Rate;
			const double Flutter = 0.75 + 0.25 * FMath::Sin(2 * Pi * FlutterHz * T);
			Out[i] = (float)(Flutter * (FMath::Sin(Phase1) + 0.45 * FMath::Sin(Phase2) + 0.2 * FMath::Sin(Phase3)));
		}
		TArray<float> Noise = LoopNoise(N, Rnd);
		BandPass(Noise, 1800, 0.8);
		Normalize(Noise, 1.f);
		for (int32 i = 0; i < N; i++) Out[i] = 0.7f * Out[i] + 0.45f * Noise[i];
		Saturate(Out, 1.3f);
		Normalize(Out, 0.85f);
		return Out;
	}

	TArray<float> Wind(int64 Seed)
	{
		FJavaRandom Rnd(Seed);
		const int32 N = Rate * 2;
		TArray<float> Low = LoopNoise(N, Rnd);
		LowPass(Low, 380);
		LowPass(Low, 380);
		Normalize(Low, 1.f);
		TArray<float> Hiss = LoopNoise(N, Rnd);
		BandPass(Hiss, 2500, 0.5);
		Normalize(Hiss, 1.f);
		TArray<float> Out;
		Out.SetNumUninitialized(N);
		for (int32 i = 0; i < N; i++) Out[i] = 0.8f * Low[i] + 0.25f * Hiss[i];
		Normalize(Out, 0.8f);
		return Out;
	}
}
