#pragma once

#include "CoreMinimal.h"
#include "Sim/TrackSpline.h"

/**
 * Port of com.racingsim.track.GroundShade: the color of the land around the track (a multiplier
 * on the tiling grass texture, written into vertex colors) and where the woods are. Shared by the
 * ground mesh, the grass strips beside the track and the tree placement, so the grass under a
 * wood is darker and the woods sit where the ground says they do.
 *  - infield and the lawns just outside the track: mown, with stripes along the straights;
 *  - further out (d > ~150 m): rough meadow, yellower and patchier;
 *  - large-scale noise breaks up the texture tiling everywhere.
 * World positions are Unreal (x, y) in meters (the Java (x, z)).
 */
class RACINGSIM_API FGroundShade
{
public:
	explicit FGroundShade(const FTrackSpline* InTrack);

	FLinearColor Color(float X, float Y) const;
	/** 0..1 woodland density at (x, y) with track offset d. */
	float WoodDensity(float X, float Y, float D) const;
	float OffsetAt(float X, float Y) const { return Track->Project(FVector(X, Y, 0)).D; }

	static float Fbm(float X, float Y, int32 Octaves, int32 Seed);
	static float Noise(float X, float Y, int32 Seed);

private:
	const FTrackSpline* Track;
	float StripeX, StripeY;
};
