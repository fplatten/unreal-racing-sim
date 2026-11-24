#include "Sim/TrackSpline.h"

void FTrackSpline::FSegment::PointAt(float U, float D, float& OutX, float& OutY) const
{
	if (!bArc)
	{
		OutX = X0 + Fx * U + Rx * D;
		OutY = Y0 + Fy * U + Ry * D;
		return;
	}
	const float P = Psi0 - U / R;
	const float RxA = -FMath::Sin(P), RyA = FMath::Cos(P);
	OutX = Cx + RxA * (R + D);
	OutY = Cy + RyA * (R + D);
}

float FTrackSpline::FSegment::HeadingAt(float U) const
{
	return bArc ? Psi0 - U / R : Psi;
}

void FTrackSpline::FSegment::ProjectXY(float X, float Y, float& OutU, float& OutD) const
{
	if (!bArc)
	{
		const float Px = X - X0, Py = Y - Y0;
		OutU = Px * Fx + Py * Fy;
		OutD = Px * Rx + Py * Ry;
		return;
	}
	const float Vx = X - Cx, Vy = Y - Cy;
	const float Dist = FMath::Sqrt(Vx * Vx + Vy * Vy);
	const float P = FMath::Atan2(-Vx, Vy);
	// angle traveled from the arc start, centered on the arc's mid-point so that positions
	// beyond either end clamp to the nearest end
	const float Sweep = Len / R;
	const float Delta = WrapAngle(Psi0 - P - Sweep / 2) + Sweep / 2;
	OutU = Delta * R;
	OutD = Dist - R;
}

FTrackSpline::FTrackSpline()
{
	const float Lens[] = { Straight / 2, Turn, Chute, Turn, Straight, Turn, Chute, Turn, Straight / 2 };
	const bool IsArc[] = { false, true, false, true, false, true, false, true, false };
	float X = 0, Y = 0, Psi = 0, S = 0;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Lens); i++)
	{
		FSegment Seg;
		Seg.bArc = IsArc[i];
		Seg.Len = Lens[i];
		Seg.StartS = S;
		if (Seg.bArc)
		{
			Seg.Psi0 = Psi;
			Seg.R = TurnRadius;
			// center lies to the left: C = P - right * R
			const float Rx = -FMath::Sin(Psi), Ry = FMath::Cos(Psi);
			Seg.Cx = X - Rx * TurnRadius;
			Seg.Cy = Y - Ry * TurnRadius;
		}
		else
		{
			Seg.X0 = X; Seg.Y0 = Y; Seg.Psi = Psi;
			Seg.Fx = FMath::Cos(Psi); Seg.Fy = FMath::Sin(Psi);
			Seg.Rx = -Seg.Fy; Seg.Ry = Seg.Fx;
		}
		Seg.PointAt(Seg.Len, 0, X, Y);
		Psi = Seg.HeadingAt(Seg.Len);
		S += Seg.Len;
		if (Seg.bArc) Arcs.Add(Segments.Num());
		Segments.Add(Seg);
	}
	float MinX = TNumericLimits<float>::Max(), MaxX = -MinX, MinY = MinX, MaxY = -MinX;
	for (float SS = 0; SS < Length; SS += 10.f)
	{
		const FVector C = Centerline(SS);
		MinX = FMath::Min(MinX, (float)C.X); MaxX = FMath::Max(MaxX, (float)C.X);
		MinY = FMath::Min(MinY, (float)C.Y); MaxY = FMath::Max(MaxY, (float)C.Y);
	}
	Center = FVector((MinX + MaxX) / 2, (MinY + MaxY) / 2, 0);
}

float FTrackSpline::WrapAngle(float A)
{
	while (A > UE_PI) A -= UE_TWO_PI;
	while (A <= -UE_PI) A += UE_TWO_PI;
	return A;
}

float FTrackSpline::WrapS(float S)
{
	S = FMath::Fmod(S, Length);
	return S < 0 ? S + Length : S;
}

float FTrackSpline::DeltaS(float A, float B)
{
	float Ds = FMath::Fmod(B - A, Length);
	if (Ds > Length / 2) Ds -= Length;
	if (Ds < -Length / 2) Ds += Length;
	return Ds;
}

const FTrackSpline::FSegment& FTrackSpline::SegmentAt(float S) const
{
	S = WrapS(S);
	for (const FSegment& Seg : Segments)
	{
		if (S < Seg.StartS + Seg.Len) return Seg;
	}
	return Segments.Last();
}

FVector FTrackSpline::Centerline(float S) const
{
	FVector P = Point(S, 0);
	P.Z = 0;
	return P;
}

FVector FTrackSpline::Point(float S, float D) const
{
	S = WrapS(S);
	const FSegment& Seg = SegmentAt(S);
	float X, Y;
	Seg.PointAt(S - Seg.StartS, D, X, Y);
	return FVector(X, Y, Height(S, D));
}

float FTrackSpline::Heading(float S) const
{
	S = WrapS(S);
	const FSegment& Seg = SegmentAt(S);
	return Seg.HeadingAt(S - Seg.StartS);
}

FVector FTrackSpline::Forward(float S) const
{
	const float Psi = Heading(S);
	return FVector(FMath::Cos(Psi), FMath::Sin(Psi), 0);
}

FVector FTrackSpline::Right(float S) const
{
	const float Psi = Heading(S);
	return FVector(-FMath::Sin(Psi), FMath::Cos(Psi), 0);
}

float FTrackSpline::Curvature(float S) const
{
	return SegmentAt(S).Curvature();
}

float FTrackSpline::Radius(float S, float D) const
{
	const float K = Curvature(S);
	return K == 0 ? TNumericLimits<float>::Max() : (1.f / K) + D;
}

float FTrackSpline::DistanceToNextTurn(float S) const
{
	S = WrapS(S);
	if (InTurn(S)) return 0;
	float Best = TNumericLimits<float>::Max();
	for (int32 A : Arcs) Best = FMath::Min(Best, WrapS(Segments[A].StartS - S));
	return Best;
}

TArray<float> FTrackSpline::TurnStarts() const
{
	TArray<float> R;
	for (int32 A : Arcs) R.Add(Segments[A].StartS);
	return R;
}

float FTrackSpline::Smooth(float X)
{
	X = FMath::Clamp(X, 0.f, 1.f);
	return X * X * (3 - 2 * X);
}

float FTrackSpline::Bank(float S) const
{
	S = WrapS(S);
	float W = 0;
	for (int32 Idx : Arcs)
	{
		const FSegment& A = Segments[Idx];
		const float In = Smooth((S - (A.StartS - BankRamp / 2)) / BankRamp);
		const float Out = Smooth(((A.StartS + A.Len + BankRamp / 2) - S) / BankRamp);
		W = FMath::Max(W, FMath::Min(In, Out));
	}
	return BankAngle * W;
}

float FTrackSpline::Height(float S, float D) const
{
	if (D <= RoadInner) return 0.f;
	return (D - RoadInner) * FMath::Tan(Bank(S));
}

FVector FTrackSpline::SurfaceNormal(float S, float D) const
{
	if (D <= RoadInner) return FVector::UpVector;
	const float B = Bank(S);
	const FVector R = Right(S);
	return (FVector(0, 0, FMath::Cos(B)) - R * FMath::Sin(B)).GetSafeNormal();
}

FTrackSpline::FTrackPos FTrackSpline::Project(const FVector& World) const
{
	FTrackPos Out;
	float BestCost = TNumericLimits<float>::Max();
	for (const FSegment& Seg : Segments)
	{
		float U, D;
		Seg.ProjectXY(World.X, World.Y, U, D);
		float Cost;
		if (U >= 0 && U <= Seg.Len)
		{
			Cost = FMath::Abs(D);
		}
		else
		{
			U = FMath::Clamp(U, 0.f, Seg.Len);
			float Px, Py;
			Seg.PointAt(U, 0, Px, Py);
			const float Dx = World.X - Px, Dy = World.Y - Py;
			Cost = FMath::Sqrt(Dx * Dx + Dy * Dy);
		}
		if (Cost < BestCost)
		{
			BestCost = Cost;
			Out.S = WrapS(Seg.StartS + U);
			Out.D = D;
		}
	}
	return Out;
}

// ---------------------------------------------------------------------------------- PitLane

namespace PitLane
{
	static constexpr float MedianGrow = 50.f;

	float Rel(float S) { return FTrackSpline::DeltaS(0.f, S); }

	bool InExitLane(float S)
	{
		const float R = Rel(S);
		return R > WallEnd && R <= LaneEnd;
	}

	bool InPitRoad(float S)
	{
		const float R = Rel(S);
		return R >= Start && R <= WallEnd;
	}

	static float BlendT(float R) { return FTrackSpline::Smooth((R - BlendStart) / (LaneEnd - BlendStart)); }

	float ApronInnerAt(float S)
	{
		if (!InExitLane(S)) return FTrackSpline::ApronInner;
		const float R = Rel(S);
		const float Steady = R < WallEnd + MedianGrow
			? FMath::Lerp(FTrackSpline::ApronInner, MedianOuter, FTrackSpline::Smooth((R - WallEnd) / MedianGrow))
			: MedianOuter;
		return FMath::Lerp(Steady, TrackEdge, BlendT(R));
	}

	float LaneOuterAt(float S) { return FMath::Lerp(LaneOuter, TrackEdge, BlendT(Rel(S))); }

	float LaneInnerAt(float S)
	{
		const float R = Rel(S);
		if (R < End) return BackDAt(S);
		return FMath::Lerp(LaneInner, FTrackSpline::ApronInner, BlendT(R));
	}

	bool InRange(float S)
	{
		const float R = Rel(S);
		return R >= Start && R <= End;
	}

	bool PitWallAt(float S)
	{
		const float R = Rel(S);
		return R >= WallStart && R <= WallEnd;
	}

	float BackDAt(float S)
	{
		const float R = FMath::Clamp(Rel(S), Start, End);
		if (R < WallStart) return FMath::Lerp(WallD, BackD, FTrackSpline::Smooth((R - Start) / (WallStart - Start)));
		if (R > WallEnd) return FMath::Lerp(LaneInner, BackD, FTrackSpline::Smooth((End - R) / (End - WallEnd)));
		return BackD;
	}

	float InnerLimit(float S)
	{
		if (InPitRoad(S)) return BackDAt(S);
		if (InExitLane(S)) return LaneInnerAt(S) - 25.f;
		return FTrackSpline::InnerWall;
	}

	float StallStart(int32 I) { return (I - Stalls / 2.f) * StallLength; }

	float DividerD(float S)
	{
		const float R = Rel(S);
		if (R >= EntryLaneStart && R < Start)
		{
			return FMath::Lerp(TrackEdge, LaneDividerD, FTrackSpline::Smooth((R - EntryLaneStart) / 90.f));
		}
		return NAN;
	}
}
