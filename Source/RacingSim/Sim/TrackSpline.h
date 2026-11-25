#pragma once

#include "CoreMinimal.h"

/** The circuits the game can race on. */
enum class ETrackLayout : uint8 { Oval, Road };

/**
 * Port of com.racingsim.track.TrackSpline: the 2.5-mile rectangular oval, and the road course
 * (see TrackSpline.cpp): one track is loaded at a time, chosen with
 * SelectLayout before the track is built, and the lap length, road edges and walls below are
 * that track's.
 *
 * All values are in meters, in Unreal's world axes (Z up). The Java version is Y up; its
 * (x, y, z) maps to Unreal (x, z, y), which keeps the handedness right, so the formulas carry
 * over with the Java z coordinate read as Unreal's y.
 *
 * Track coordinates:
 *  - s: progress along the centerline, 0..Length meters, 0 = start/finish line.
 *  - d: horizontal lateral offset from the centerline, positive = the driver's right (on the
 *       oval: outward, toward the outer wall, since all turns are left-handers).
 *
 * The road course is sampled every metre: centerline, heading, curvature, elevation and the
 * walls (barriers) on each side; positions between samples are interpolated, and Project uses
 * a spatial grid of the samples.
 */
class RACINGSIM_API FTrackSpline
{
public:
	static constexpr float Mile = 1609.344f;
	static constexpr float Straight = Mile * 5.f / 8.f;
	static constexpr float Chute = Mile / 8.f;
	static constexpr float Turn = Mile / 4.f;
	static constexpr float OvalLength = 2 * Straight + 2 * Chute + 4 * Turn;
	static constexpr float TurnRadius = Turn / UE_HALF_PI;
	/** The road course: 4.352 miles, clockwise, 102.2 m from its lowest point (z = 0) to its highest. */
	static constexpr float RoadLength = 7004.f;
	static constexpr float RoadHalfWidth = 6.5f;
	/** Kerbs: painted, raised a little, as grippy as the road. */
	static constexpr float KerbWidth = 1.2f;
	static constexpr float BarrierHeight = 1.0f;

	/** The track being raced; set before the track actor is spawned. */
	static inline ETrackLayout Layout = ETrackLayout::Oval;
	static void SelectLayout(ETrackLayout L) { Layout = L; }
	static bool IsOval() { return Layout == ETrackLayout::Oval; }
	static bool IsRoadCourse() { return !IsOval(); }
	/** "oval" / "road" (the -Track= / ?Track= option). */
	static const TCHAR* LayoutKey(ETrackLayout L) { return L == ETrackLayout::Road ? TEXT("road") : TEXT("oval"); }
	static ETrackLayout LayoutFromKey(const FString& Key) { return Key.Equals(TEXT("road"), ESearchCase::IgnoreCase) ? ETrackLayout::Road : ETrackLayout::Oval; }
	static const TCHAR* LayoutName(ETrackLayout L) { return L == ETrackLayout::Road ? TEXT("4.352-MILE ROAD COURSE") : TEXT("2.5-MILE OVAL"); }

	/** Lap length (m) of the current track. */
	static inline float Length = OvalLength;

	static constexpr float BankAngle = 9.2f * UE_PI / 180.f;
	static constexpr float BankRamp = 80.f;

	// road edges and walls of the current track (oval values; the road course has no apron and
	// its walls follow the track, see MinD / MaxD)
	static inline float InnerWall = -18.f;
	static inline float ApronInner = -15.f;
	static inline float RoadInner = -7.5f;
	static inline float RoadOuter = 8.0f;
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
	bool InTurn(float S) const;
	float DistanceToNextTurn(float S) const;
	/** Distance to the end of the turn we are in, or else of the next one. */
	float DistanceToTurnExit(float S) const;
	/** +1 when the turn we are in (or the next one) is a left-hander, -1 a right-hander. */
	float TurnDirection(float S) const;
	TArray<float> TurnStarts() const;
	/** The walls: the lowest and highest d a car can reach at s. */
	float MinD(float S) const;
	float MaxD(float S) const;
	/** A kerb beside the road at s on the right (d > 0) or left side. */
	bool KerbAt(float S, bool bRight) const;
	/** Elevation of the centerline (m) and its slope (rise per metre). */
	float Elevation(float S) const;
	float Grade(float S) const;
	/** Under or beyond a wall (for the wall contact filter). */
	bool BeyondWalls(float S, float D, float Margin) const;
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
	void BuildOval();
	void BuildRoadCourse();
	void BuildRoadWalls();
	void Locate(float S, int32& I0, int32& I1, float& T) const;
	float Sampled(const TArray<float>& A, float S) const;
	int32 NearestSample(float X, float Y, int32 MaxRing) const;

	TArray<FSegment> Segments;
	TArray<int32> Arcs;
	FVector Center = FVector::ZeroVector;

	// ---- road course: samples every Ds metres ----
	int32 N = 0;
	float Ds = 1.f;
	TArray<FVector2D> Pts;
	TArray<float> Psi;          // heading, unwrapped (Psi[N] would be Psi[0] + total turning)
	float PsiWrap = 0.f;        // total turning over the lap
	TArray<float> Kappa;        // signed curvature, + = left
	TArray<float> Elev;
	TArray<float> Slope;
	TArray<float> WallLeft;     // d of the left wall (negative)
	TArray<float> WallRight;    // d of the right wall
	TArray<float> ToTurn;       // distance to the next turn (0 in one)
	TArray<float> ToExit;       // distance to the end of this / the next turn
	TArray<int8> TurnDir;
	TArray<uint8> Kerbs;        // bit 0 left, bit 1 right
	TArray<float> Corners;      // where each turn starts
	// spatial grid over the samples (compressed rows)
	float GridX0 = 0.f, GridY0 = 0.f, GridCell = 20.f;
	int32 GridNx = 0, GridNy = 0;
	TArray<int32> CellStart, CellItems;
};

/** Port of com.racingsim.track.PitLane: the Indianapolis-style pit lane layout (the oval only:
 *  on a road course nothing is ever in the pit lane). */
namespace PitLane
{
	constexpr int32 Stalls = 33;
	constexpr int32 PlayerStall = 16;
	constexpr float StallLength = 24.f;
	constexpr float Start = -600.f, WallStart = -470.f, WallEnd = 470.f, End = 600.f;
	constexpr float WallD = -18.f;   // the oval's inner wall
	constexpr float FastLaneInner = -26.f, BoxInner = -34.f, BackD = -40.f;
	constexpr float LaneDividerD = -9.6f;
	constexpr float EntryLaneStart = -850.f;
	constexpr float LaneOuter = -17.f, LaneInner = -26.f, MedianOuter = -10.5f;
	constexpr float TrackEdge = -7.5f - 0.3f;   // the oval's road edge
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
