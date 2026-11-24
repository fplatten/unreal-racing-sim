#include "Track/GroundShade.h"

namespace
{
	constexpr float Stripe = 9.f;
	constexpr float MownEnd = 140.f, MeadowEnd = 200.f;

	float Smooth(float T) { return FTrackSpline::Smooth(T); }

	/** Java int arithmetic (wrapping) and >>> (logical shift). */
	float Hash(int32 X, int32 Y, int32 Seed)
	{
		uint32 H = (uint32)X * 374761393u + (uint32)Y * 668265263u + (uint32)Seed * 1442695041u;
		H = (H ^ (H >> 13)) * 1274126177u;
		H ^= H >> 16;
		return (H & 0xffffff) / (float)0x1000000;
	}
}

FGroundShade::FGroundShade(const FTrackSpline* InTrack)
	: Track(InTrack)
{
	const FVector F = Track->Forward(0.f);   // stripes run along the straights
	StripeX = -F.Y;
	StripeY = F.X;
}

float FGroundShade::Noise(float X, float Y, int32 Seed)
{
	const int32 X0 = FMath::FloorToInt(X), Y0 = FMath::FloorToInt(Y);
	float Fx = X - X0, Fy = Y - Y0;
	Fx = Fx * Fx * (3 - 2 * Fx);
	Fy = Fy * Fy * (3 - 2 * Fy);
	const float A = Hash(X0, Y0, Seed), B = Hash(X0 + 1, Y0, Seed);
	const float C = Hash(X0, Y0 + 1, Seed), E = Hash(X0 + 1, Y0 + 1, Seed);
	return (A * (1 - Fx) + B * Fx) * (1 - Fy) + (C * (1 - Fx) + E * Fx) * Fy;
}

float FGroundShade::Fbm(float X, float Y, int32 Octaves, int32 Seed)
{
	float Sum = 0.f, Amp = 0.5f, Norm = 0.f;
	for (int32 i = 0; i < Octaves; i++)
	{
		Sum += Amp * Noise(X, Y, Seed + i * 101);
		Norm += Amp;
		Amp *= 0.5f;
		X *= 2.03f;
		Y *= 2.03f;
	}
	return Sum / Norm;
}

float FGroundShade::WoodDensity(float X, float Y, float D) const
{
	if (D < 95.f) return 0.f;
	const float Patches = Fbm(X / 140.f, Y / 140.f, 3, 51);
	const float Edge = Smooth((D - 95.f) / 140.f);   // thicker further out
	return Smooth((Patches + 0.35f * Edge - 0.62f) / 0.18f) * Smooth((D - 95.f) / 25.f);
}

FLinearColor FGroundShade::Color(float X, float Y) const
{
	const float D = OffsetAt(X, Y);
	const float Macro = Fbm(X / 210.f, Y / 210.f, 3, 11);
	const float Mid = Fbm(X / 38.f, Y / 38.f, 2, 23);
	const float Fine = Noise(X / 9.f, Y / 9.f, 37);

	// mown lawn: even, slightly blue-green, with stripes
	const float StripeV = FMath::Sin((X * StripeX + Y * StripeY) * UE_PI / Stripe);
	float Stripes = FMath::Clamp(StripeV * 2.2f, -1.f, 1.f);
	const float Mown = D < 0.f ? 1.f : 1.f - Smooth((D - MownEnd) / (MeadowEnd - MownEnd));
	if (D > -26.f && D < 45.f) Stripes *= 0.3f;   // right beside the track: edged, not striped
	const float Mb = 0.92f + 0.14f * Macro + 0.06f * (Mid - 0.5f) + 0.065f * Stripes;
	const float Mr = Mb * 0.96f, Mg = Mb, MbB = Mb * 0.95f;

	// rough meadow: yellower, patchy, dry spots
	const float Dry = Smooth((Mid - 0.55f) / 0.25f) * 0.7f + Smooth((Macro - 0.6f) / 0.3f) * 0.3f;
	const float Rb = 0.88f + 0.22f * Macro + 0.08f * (Fine - 0.5f);
	const float Rr = Rb * (1.08f + 0.35f * Dry), Rg = Rb * (1.0f + 0.05f * Dry), RbB = Rb * (0.80f - 0.1f * Dry);

	float R = Mr * Mown + Rr * (1 - Mown);
	float G = Mg * Mown + Rg * (1 - Mown);
	float B = MbB * Mown + RbB * (1 - Mown);

	// forest floor under the woods: darker, browner
	const float Wood = WoodDensity(X, Y, D);
	const float Shade = 1.f - 0.38f * Wood;
	R *= Shade * (1.f + 0.08f * Wood);
	G *= Shade;
	B *= Shade * (1.f - 0.1f * Wood);
	// (vertex colors are stored as 8-bit, so the Java version's headroom above 1 is clipped)
	return FLinearColor(FMath::Min(R, 1.f), FMath::Min(G, 1.f), FMath::Min(B, 1.f), 1.f);
}
