#pragma once

#include "CoreMinimal.h"

/**
 * Port of com.racingsim.track.TrackSpline: the 2.5-mile rectangular oval.
 *
 * All values are in meters, in Unreal's world axes (Z up). The Java version is Y up; its
 * (x, y, z) maps to Unreal (x, z, y), which keeps the handedness right, so the formulas carry
 * over with the Java z coordinate read as Unreal's y.
 *
 * Track coordinates:
 *  - s: progress along the centerline, 0..Length meters, 0 = start/finish line.
 *  - d: horizontal lateral offset from the centerline, positive = outward (toward the outer
 *       wall, the driver's right, since all turns are left-handers).
 */
class RACINGSIM_API FTrackSpline
{
public:
	static constexpr float Mile = 1609.344f;
	static constexpr float Straight = Mile * 5.f / 8.f;
	static constexpr float Chute = Mile / 8.f;
	static constexpr float Turn = Mile / 4.f;
	static constexpr float Length = 2 * Straight + 2 * Chute + 4 * Turn;
	static constexpr float TurnRadius = Turn / UE_HALF_PI;

	static constexpr float BankAngle = 9.2f * UE_PI / 180.f;
	static constexpr float BankRamp = 80.f;

	static constexpr float InnerWall = -18.f;
	static constexpr float ApronInner = -15.f;
	static constexpr float RoadInner = -7.5f;
	static constexpr float RoadOuter = 8.0f;
	static constexpr float OuterWallThickness = 0.6f;
	static constexpr float OuterWallHeight = 1.2f;
	static constexpr float InnerWallHeight = 1.0f;

	struct FTrackPos
	{
		float S = 0.f;
		float D = 0.f;
	};

	FTrackSpline();

	static float WrapAngle(float A);
	static float WrapS(float S);
	/** Signed shortest longitudinal distance from A to B (positive when B is ahead of A). */
	static float DeltaS(float A, float B);

	/** Geometric center of the oval (z = 0). */
	FVector GetCenter() const { return Center; }

	/** Centerline point (z = 0). */
	FVector Centerline(float S) const;
	/** World position (m) for (s, d) with z on the racing surface. */
	FVector Point(float S, float D) const;
	float Heading(float S) const;
	/** Unit forward (direction of travel), horizontal. */
	FVector Forward(float S) const;
	/** Unit outward (right-hand) vector, horizontal. */
	FVector Right(float S) const;
	/** Signed curvature of the centerline (positive = left turn). */
	float Curvature(float S) const;
	float Radius(float S, float D) const;
	bool InTurn(float S) const { return Curvature(S) != 0.f; }
	float DistanceToNextTurn(float S) const;
	TArray<float> TurnStarts() const;
	/** Banking angle (rad) at s, smoothly ramped across turn boundaries. */
	float Bank(float S) const;
	/** Height of the racing surface (the road banks about its inner edge; apron and infield are flat). */
	float Height(float S, float D) const;
	FVector SurfaceNormal(float S, float D) const;
	/** Projects a world position (m) onto track coordinates. */
	FTrackPos Project(const FVector& World) const;

	static float Smooth(float X);

private:
	struct FSegment
	{
		bool bArc = false;
		float StartS = 0.f;
		float Len = 0.f;
		// line
		float X0 = 0.f, Y0 = 0.f, Psi = 0.f, Fx = 0.f, Fy = 0.f, Rx = 0.f, Ry = 0.f;
		// arc
		float Cx = 0.f, Cy = 0.f, R = 0.f, Psi0 = 0.f;

		void PointAt(float U, float D, float& OutX, float& OutY) const;
		float HeadingAt(float U) const;
		void ProjectXY(float X, float Y, float& OutU, float& OutD) const;
		float Curvature() const { return bArc ? 1.f / R : 0.f; }
	};

	const FSegment& SegmentAt(float S) const;

	TArray<FSegment> Segments;
	TArray<int32> Arcs;
	FVector Center = FVector::ZeroVector;
};

/** Port of com.racingsim.track.PitLane: the Indianapolis-style pit lane layout. */
namespace PitLane
{
	constexpr int32 Stalls = 33;
	constexpr int32 PlayerStall = 16;
	constexpr float StallLength = 24.f;
	constexpr float Start = -600.f, WallStart = -470.f, WallEnd = 470.f, End = 600.f;
	constexpr float WallD = FTrackSpline::InnerWall;
	constexpr float FastLaneInner = -26.f, BoxInner = -34.f, BackD = -40.f;
	constexpr float LaneDividerD = -9.6f;
	constexpr float EntryLaneStart = -850.f;
	constexpr float LaneOuter = -17.f, LaneInner = -26.f, MedianOuter = -10.5f;
	constexpr float TrackEdge = FTrackSpline::RoadInner - 0.3f;
	constexpr float LaneEnd = FTrackSpline::Straight / 2 + 2 * FTrackSpline::Turn + FTrackSpline::Chute + 50.f;
	constexpr float BlendStart = LaneEnd - 160.f;

	RACINGSIM_API float Rel(float S);
	RACINGSIM_API bool InExitLane(float S);
	RACINGSIM_API bool InPitRoad(float S);
	RACINGSIM_API float ApronInnerAt(float S);
	RACINGSIM_API float LaneOuterAt(float S);
	RACINGSIM_API float LaneInnerAt(float S);
	RACINGSIM_API bool InRange(float S);
	RACINGSIM_API bool PitWallAt(float S);
	RACINGSIM_API float BackDAt(float S);
	RACINGSIM_API float InnerLimit(float S);
	RACINGSIM_API float StallStart(int32 I);
	/** NaN where there is no divider. */
	RACINGSIM_API float DividerD(float S);
}
