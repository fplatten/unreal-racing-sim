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
	if (IsOval()) BuildOval();
	else BuildRoadCourse();
}

void FTrackSpline::BuildOval()
{
	Length = OvalLength;
	InnerWall = -18.f;
	ApronInner = -15.f;
	RoadInner = -7.5f;
	RoadOuter = 8.0f;
	const float Lens[] = { Straight / 2, Turn, Chute, Turn, Straight, Turn, Chute, Turn, Straight / 2 };
	const bool IsArc[] = { false, true, false, true, false, true, false, true, false };
	float X = 0, Y = 0, Hd = 0, S = 0;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Lens); i++)
	{
		FSegment Seg;
		Seg.bArc = IsArc[i];
		Seg.Len = Lens[i];
		Seg.StartS = S;
		if (Seg.bArc)
		{
			Seg.Psi0 = Hd;
			Seg.R = TurnRadius;
			// center lies to the left: C = P - right * R
			const float Rx = -FMath::Sin(Hd), Ry = FMath::Cos(Hd);
			Seg.Cx = X - Rx * TurnRadius;
			Seg.Cy = Y - Ry * TurnRadius;
		}
		else
		{
			Seg.X0 = X; Seg.Y0 = Y; Seg.Psi = Hd;
			Seg.Fx = FMath::Cos(Hd); Seg.Fy = FMath::Sin(Hd);
			Seg.Rx = -Seg.Fy; Seg.Ry = Seg.Fx;
		}
		Seg.PointAt(Seg.Len, 0, X, Y);
		Hd = Seg.HeadingAt(Seg.Len);
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
	if (!IsOval())
	{
		int32 I0, I1;
		float T;
		Locate(S, I0, I1, T);
		const FVector2D C = Pts[I0] + (Pts[I1] - Pts[I0]) * T;
		const FVector R = Right(S);
		return FVector(C.X + R.X * D, C.Y + R.Y * D, Height(S, D));
	}
	const FSegment& Seg = SegmentAt(S);
	float X, Y;
	Seg.PointAt(S - Seg.StartS, D, X, Y);
	return FVector(X, Y, Height(S, D));
}

float FTrackSpline::Heading(float S) const
{
	S = WrapS(S);
	if (!IsOval())
	{
		int32 I0, I1;
		float T;
		Locate(S, I0, I1, T);
		const float P1 = I1 == 0 ? Psi[0] + PsiWrap : Psi[I1];
		return Psi[I0] + (P1 - Psi[I0]) * T;
	}
	const FSegment& Seg = SegmentAt(S);
	return Seg.HeadingAt(S - Seg.StartS);
}

FVector FTrackSpline::Forward(float S) const
{
	const float H = Heading(S);
	return FVector(FMath::Cos(H), FMath::Sin(H), 0);
}

FVector FTrackSpline::Right(float S) const
{
	const float H = Heading(S);
	return FVector(-FMath::Sin(H), FMath::Cos(H), 0);
}

float FTrackSpline::Curvature(float S) const
{
	if (!IsOval()) return Sampled(Kappa, S);
	return SegmentAt(S).Curvature();
}

float FTrackSpline::Radius(float S, float D) const
{
	const float K = Curvature(S);
	if (FMath::Abs(K) < 1e-6f) return TNumericLimits<float>::Max();
	// d grows to the right: away from the center of a left turn, toward that of a right turn
	return FMath::Max(1.f / FMath::Abs(K) + (K > 0 ? D : -D), 1.f);
}

namespace
{
	/** A road-course turn: tighter than this radius (m). */
	constexpr float RoadTurnRadius = 600.f;
}

bool FTrackSpline::InTurn(float S) const
{
	if (!IsOval()) return FMath::Abs(Curvature(S)) > 1.f / RoadTurnRadius;
	return Curvature(S) != 0.f;
}

float FTrackSpline::DistanceToTurnExit(float S) const
{
	if (!IsOval()) return Sampled(ToExit, S);
	for (const float Start : TurnStarts())
	{
		const float Into = DeltaS(Start, S);
		if (Into >= 0.f && Into < Turn) return Turn - Into;
	}
	return DistanceToNextTurn(S) + Turn;
}

float FTrackSpline::TurnDirection(float S) const
{
	if (IsOval()) return 1.f;
	int32 I0, I1;
	float T;
	Locate(WrapS(S), I0, I1, T);
	return TurnDir[T < 0.5f ? I0 : I1];
}

float FTrackSpline::MinD(float S) const
{
	if (IsOval()) return PitLane::InnerLimit(S);
	return Sampled(WallLeft, S);
}

float FTrackSpline::MaxD(float S) const
{
	if (IsOval()) return RoadOuter;
	return Sampled(WallRight, S);
}

bool FTrackSpline::KerbAt(float S, bool bRight) const
{
	if (IsOval()) return false;
	int32 I0, I1;
	float T;
	Locate(WrapS(S), I0, I1, T);
	return (Kerbs[T < 0.5f ? I0 : I1] & (bRight ? 2 : 1)) != 0;
}

float FTrackSpline::Elevation(float S) const
{
	return IsOval() ? 0.f : Sampled(Elev, S);
}

float FTrackSpline::Grade(float S) const
{
	return IsOval() ? 0.f : Sampled(Slope, S);
}

bool FTrackSpline::BeyondWalls(float S, float D, float Margin) const
{
	if (IsOval()) return D > RoadOuter - Margin || D < ApronInner;
	return D > MaxD(S) - Margin || D < MinD(S) + Margin;
}

float FTrackSpline::DistanceToNextTurn(float S) const
{
	S = WrapS(S);
	if (!IsOval()) return Sampled(ToTurn, S);
	if (InTurn(S)) return 0;
	float Best = TNumericLimits<float>::Max();
	for (int32 A : Arcs) Best = FMath::Min(Best, WrapS(Segments[A].StartS - S));
	return Best;
}

TArray<float> FTrackSpline::TurnStarts() const
{
	if (!IsOval()) return Corners;
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
	if (!IsOval()) return 0.f;   // the road course is flat across (its slopes run along the track)
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
	if (!IsOval()) return Elevation(S);
	if (D <= RoadInner) return 0.f;
	return (D - RoadInner) * FMath::Tan(Bank(S));
}

FVector FTrackSpline::SurfaceNormal(float S, float D) const
{
	if (!IsOval()) return (FVector::UpVector - Forward(S) * Grade(S)).GetSafeNormal();
	if (D <= RoadInner) return FVector::UpVector;
	const float B = Bank(S);
	const FVector R = Right(S);
	return (FVector(0, 0, FMath::Cos(B)) - R * FMath::Sin(B)).GetSafeNormal();
}

FTrackSpline::FTrackPos FTrackSpline::Project(const FVector& World) const
{
	FTrackPos Out;
	if (!IsOval())
	{
		// the nearest sample, then the exact spot between it and its neighbours: along a curve
		// the centerline moves 1 / (1 + k d) as fast as a point at offset d
		int32 I = NearestSample(World.X, World.Y, 400);
		for (int32 Iter = 0; Iter < 4; Iter++)
		{
			const FVector2D F(FMath::Cos(Psi[I]), FMath::Sin(Psi[I]));
			const FVector2D V(World.X - Pts[I].X, World.Y - Pts[I].Y);
			const float U = V.X * F.X + V.Y * F.Y;
			const float D = -V.X * F.Y + V.Y * F.X;
			if (U > Ds * 0.5f && Iter < 3) { I = (I + 1) % N; continue; }
			if (U < -Ds * 0.5f && Iter < 3) { I = (I + N - 1) % N; continue; }
			const float Stretch = FMath::Max(1.f + Kappa[I] * D, 0.2f);
			Out.S = WrapS(I * Ds + U / Stretch);
			Out.D = D;
			break;
		}
		return Out;
	}
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

// ------------------------------------------------------------------------------ road course

namespace
{
	const FVector2D RoadPoints[] = {
#include "Sim/RoadLayout.inl"
	};

	/**
	 * The road course's height (m above its lowest point) at points along the lap (s from the
	 * start/finish line): down from the first-turn hairpin into the valley, up the steep climb
	 * (~17% at its steepest) and the long straight to the summit (102.2 m), down through the
	 * hairpin and the long double left to the lowest point, and back up to the last chicane.
	 * Monotone cubic in between, so the summit and the lowest point are exactly these.
	 */
	struct FElevKey { float S, Z; };
	const FElevKey RoadElevation[] = {
		{ 0.f, 34.2f }, { 215.f, 37.2f }, { 780.f, 12.2f }, { 1140.f, 53.2f }, { 2240.f, 102.2f }, { 2360.f, 100.2f },
		{ 2500.f, 94.2f }, { 2910.f, 82.2f }, { 3140.f, 74.2f }, { 3790.f, 50.2f }, { 4110.f, 38.2f }, { 4355.f, 32.2f },
		{ 4520.f, 27.2f }, { 4800.f, 12.2f }, { 5070.f, 0.0f }, { 5410.f, 4.2f }, { 5780.f, 14.2f }, { 6050.f, 23.2f },
		{ 6590.f, 31.2f }, { 6680.f, 32.7f } };

	/** Centripetal Catmull-Rom point between P1 and P2 (U 0..1). */
	FVector2D CatmullRom(const FVector2D& P0, const FVector2D& P1, const FVector2D& P2, const FVector2D& P3, float U)
	{
		auto Knot = [](const FVector2D& A, const FVector2D& B) { return FMath::Max(FMath::Sqrt((float)FVector2D::Distance(A, B)), 1e-3f); };
		const float T0 = 0.f, T1 = T0 + Knot(P0, P1), T2 = T1 + Knot(P1, P2), T3 = T2 + Knot(P2, P3);
		const float T = T1 + (T2 - T1) * U;
		const FVector2D A1 = P0 * ((T1 - T) / (T1 - T0)) + P1 * ((T - T0) / (T1 - T0));
		const FVector2D A2 = P1 * ((T2 - T) / (T2 - T1)) + P2 * ((T - T1) / (T2 - T1));
		const FVector2D A3 = P2 * ((T3 - T) / (T3 - T2)) + P3 * ((T - T2) / (T3 - T2));
		const FVector2D B1 = A1 * ((T2 - T) / (T2 - T0)) + A2 * ((T - T0) / (T2 - T0));
		const FVector2D B2 = A2 * ((T3 - T) / (T3 - T1)) + A3 * ((T - T1) / (T3 - T1));
		return B1 * ((T2 - T) / (T2 - T1)) + B2 * ((T - T1) / (T2 - T1));
	}

	/** Circular Gaussian blur of a lap-long array (Sigma in samples). */
	void BlurLoop(TArray<float>& A, float Sigma)
	{
		const int32 Num = A.Num(), R = FMath::CeilToInt(3.f * Sigma);
		TArray<float> W;
		float Sum = 0.f;
		for (int32 k = -R; k <= R; k++) { W.Add(FMath::Exp(-0.5f * FMath::Square(k / Sigma))); Sum += W.Last(); }
		TArray<float> Out;
		Out.SetNumZeroed(Num);
		for (int32 i = 0; i < Num; i++)
		{
			float Acc = 0.f;
			for (int32 k = -R; k <= R; k++) Acc += W[k + R] * A[((i + k) % Num + Num) % Num];
			Out[i] = Acc / Sum;
		}
		A = MoveTemp(Out);
	}

	int32 Wrap(int32 I, int32 Num) { return ((I % Num) + Num) % Num; }
}

void FTrackSpline::Locate(float S, int32& I0, int32& I1, float& T) const
{
	const float X = S / Ds;
	I0 = FMath::Clamp((int32)X, 0, N - 1);
	T = FMath::Clamp(X - I0, 0.f, 1.f);
	I1 = (I0 + 1) % N;
}

float FTrackSpline::Sampled(const TArray<float>& A, float S) const
{
	int32 I0, I1;
	float T;
	Locate(WrapS(S), I0, I1, T);
	return A[I0] + (A[I1] - A[I0]) * T;
}

int32 FTrackSpline::NearestSample(float X, float Y, int32 MaxRing) const
{
	const int32 Cx = FMath::FloorToInt((X - GridX0) / GridCell), Cy = FMath::FloorToInt((Y - GridY0) / GridCell);
	int32 Best = INDEX_NONE;
	float BestD2 = TNumericLimits<float>::Max();
	// rings of cells outwards from the point's cell; once something is found, one more ring
	// settles which sample is nearest
	int32 FoundRing = -1;
	for (int32 Ring = 0; Ring <= MaxRing; Ring++)
	{
		if (FoundRing >= 0 && Ring > FoundRing + 1) break;
		// skip rings that lie entirely outside the grid
		if (Cx + Ring < 0 && Cx - Ring < 0) continue;
		for (int32 Gy = Cy - Ring; Gy <= Cy + Ring; Gy++)
		{
			if (Gy < 0 || Gy >= GridNy) continue;
			const bool bEdgeRow = Gy == Cy - Ring || Gy == Cy + Ring;
			const int32 StepX = (bEdgeRow || Ring == 0) ? 1 : 2 * Ring;
			for (int32 Gx = Cx - Ring; Gx <= Cx + Ring; Gx += StepX)
			{
				if (Gx < 0 || Gx >= GridNx) continue;
				const int32 Cell = Gy * GridNx + Gx;
				for (int32 k = CellStart[Cell]; k < CellStart[Cell + 1]; k++)
				{
					const int32 I = CellItems[k];
					const float D2 = FMath::Square((float)Pts[I].X - X) + FMath::Square((float)Pts[I].Y - Y);
					if (D2 < BestD2) { BestD2 = D2; Best = I; }
				}
			}
		}
		if (Best != INDEX_NONE && FoundRing < 0) FoundRing = Ring;
		if (Ring > GridNx + GridNy) break;
	}
	if (Best == INDEX_NONE)
	{
		for (int32 I = 0; I < N; I++)
		{
			const float D2 = FMath::Square((float)Pts[I].X - X) + FMath::Square((float)Pts[I].Y - Y);
			if (D2 < BestD2) { BestD2 = D2; Best = I; }
		}
	}
	return Best;
}

void FTrackSpline::BuildRoadCourse()
{
	Length = RoadLength;
	RoadInner = -RoadHalfWidth;
	RoadOuter = RoadHalfWidth;
	ApronInner = RoadInner;
	InnerWall = RoadInner - 8.f;

	// ---- centerline: centripetal Catmull-Rom through the control points, scaled to the lap
	// length and resampled every metre ----
	const int32 NumCtrl = UE_ARRAY_COUNT(RoadPoints);
	TArray<FVector2D> Dense;
	const int32 Sub = 20;
	for (int32 i = 0; i < NumCtrl; i++)
	{
		const FVector2D& P0 = RoadPoints[Wrap(i - 1, NumCtrl)];
		const FVector2D& P1 = RoadPoints[i];
		const FVector2D& P2 = RoadPoints[Wrap(i + 1, NumCtrl)];
		const FVector2D& P3 = RoadPoints[Wrap(i + 2, NumCtrl)];
		for (int32 k = 0; k < Sub; k++) Dense.Add(CatmullRom(P0, P1, P2, P3, k / (float)Sub));
	}
	TArray<double> Cum;
	Cum.Add(0.0);
	for (int32 i = 0; i < Dense.Num(); i++) Cum.Add(Cum.Last() + FVector2D::Distance(Dense[i], Dense[(i + 1) % Dense.Num()]));
	const double ScaleXY = Length / Cum.Last();
	N = FMath::RoundToInt(Length);
	Ds = Length / N;
	Pts.SetNum(N);
	int32 Seg = 0;
	for (int32 i = 0; i < N; i++)
	{
		const double Target = i * Ds / ScaleXY;
		while (Seg < Dense.Num() - 1 && Cum[Seg + 1] < Target) Seg++;
		const double U = (Target - Cum[Seg]) / FMath::Max(Cum[Seg + 1] - Cum[Seg], 1e-9);
		const FVector2D A = Dense[Seg], B = Dense[(Seg + 1) % Dense.Num()];
		Pts[i] = (A + (B - A) * U) * ScaleXY;
	}

	// ---- heading (unwrapped; it gains a whole turn over the lap) and curvature ----
	Psi.SetNum(N);
	for (int32 i = 0; i < N; i++)
	{
		const FVector2D D = Pts[(i + 1) % N] - Pts[Wrap(i - 1, N)];
		Psi[i] = FMath::Atan2((float)D.Y, (float)D.X);
		if (i > 0)
		{
			while (Psi[i] - Psi[i - 1] > UE_PI) Psi[i] -= UE_TWO_PI;
			while (Psi[i] - Psi[i - 1] < -UE_PI) Psi[i] += UE_TWO_PI;
		}
	}
	PsiWrap = Psi[N - 1] > Psi[0] ? UE_TWO_PI : -UE_TWO_PI;
	auto PsiAt = [this](int32 I) { return I < 0 ? Psi[I + N] - PsiWrap : I >= N ? Psi[I - N] + PsiWrap : Psi[I]; };
	Kappa.SetNum(N);
	for (int32 i = 0; i < N; i++) Kappa[i] = -(PsiAt(i + 2) - PsiAt(i - 2)) / (4.f * Ds);   // heading grows turning right: + = left
	BlurLoop(Kappa, 2.f);

	// ---- elevation: monotone cubic (Fritsch-Carlson) through the keys, around the loop ----
	{
		const int32 K = UE_ARRAY_COUNT(RoadElevation);
		TArray<float> Ks, Zs;
		for (int32 i = 0; i < K; i++)
		{
			Ks.Add(RoadElevation[i].S * (Length / RoadLength));
			Zs.Add(RoadElevation[i].Z);
		}
		auto H = [&](int32 i) { const int32 j = (i + 1) % K; return (j == 0 ? Ks[0] + Length : Ks[j]) - Ks[i]; };
		auto Delta = [&](int32 i) { return (Zs[(i + 1) % K] - Zs[i]) / H(i); };
		TArray<float> M;
		M.SetNum(K);
		for (int32 i = 0; i < K; i++)
		{
			const int32 Prev = Wrap(i - 1, K);
			const float D0 = Delta(Prev), D1 = Delta(i);
			if (D0 * D1 <= 0.f) { M[i] = 0.f; continue; }
			const float H0 = H(Prev), H1 = H(i);
			const float W1 = 2 * H1 + H0, W2 = H1 + 2 * H0;
			M[i] = (W1 + W2) / (W1 / D0 + W2 / D1);
		}
		Elev.SetNum(N);
		for (int32 i = 0; i < N; i++)
		{
			const float S = i * Ds;
			int32 k = K - 1;
			for (int32 j = K - 1; j >= 0; j--) if (Ks[j] <= S) { k = j; break; }
			const float Hk = H(k), T = (S - Ks[k]) / Hk;
			const float T2 = T * T, T3 = T2 * T;
			const int32 k1 = (k + 1) % K;
			Elev[i] = (2 * T3 - 3 * T2 + 1) * Zs[k] + (T3 - 2 * T2 + T) * Hk * M[k] + (-2 * T3 + 3 * T2) * Zs[k1] + (T3 - T2) * Hk * M[k1];
		}
		Slope.SetNum(N);
		for (int32 i = 0; i < N; i++) Slope[i] = (Elev[(i + 1) % N] - Elev[Wrap(i - 1, N)]) / (2.f * Ds);
	}

	// ---- turns: where the curvature is tighter than RoadTurnRadius ----
	TurnDir.SetNum(N);
	ToTurn.SetNum(N);
	ToExit.SetNum(N);
	auto IsTurn = [this](int32 i) { return FMath::Abs(Kappa[i]) > 1.f / RoadTurnRadius; };
	{
		int32 Start = 0;
		while (Start < N && IsTurn(Start)) Start++;          // begin on a straight
		for (int32 k = 0; k < N; k++)
		{
			const int32 i = (Start + k) % N;
			if (IsTurn(i) && !IsTurn(Wrap(i - 1, N))) Corners.Add(i * Ds);
		}
		Corners.Sort();
		// backwards round the lap (twice, for the wrap): distance to the next turn, its
		// direction, and the distance to the end of the turn we are in or the next one
		float Dist = 0.f, Exit = 0.f, Dir = 1.f;
		for (int32 Pass = 0; Pass < 2; Pass++)
		{
			for (int32 k = N - 1; k >= 0; k--)
			{
				const int32 i = (Start + k) % N;
				if (IsTurn(i))
				{
					Exit = IsTurn((i + 1) % N) ? Exit + Ds : Ds;
					Dist = 0.f;
					Dir = Kappa[i] > 0.f ? 1.f : -1.f;
				}
				else
				{
					Dist += Ds;
					Exit += Ds;
				}
				ToTurn[i] = Dist;
				ToExit[i] = Exit;
				TurnDir[i] = Dir > 0.f ? 1 : -1;
			}
		}
	}

	// ---- spatial grid over the samples ----
	{
		float MinX = TNumericLimits<float>::Max(), MaxX = -MinX, MinY = MinX, MaxY = -MinX;
		for (const FVector2D& P : Pts)
		{
			MinX = FMath::Min(MinX, (float)P.X); MaxX = FMath::Max(MaxX, (float)P.X);
			MinY = FMath::Min(MinY, (float)P.Y); MaxY = FMath::Max(MaxY, (float)P.Y);
		}
		GridX0 = MinX - GridCell;
		GridY0 = MinY - GridCell;
		GridNx = FMath::CeilToInt((MaxX - GridX0) / GridCell) + 2;
		GridNy = FMath::CeilToInt((MaxY - GridY0) / GridCell) + 2;
		TArray<int32> Count;
		Count.SetNumZeroed(GridNx * GridNy);
		auto CellOf = [this](const FVector2D& P) { return FMath::FloorToInt(((float)P.Y - GridY0) / GridCell) * GridNx + FMath::FloorToInt(((float)P.X - GridX0) / GridCell); };
		for (const FVector2D& P : Pts) Count[CellOf(P)]++;
		CellStart.SetNumZeroed(GridNx * GridNy + 1);
		for (int32 c = 0; c < GridNx * GridNy; c++) CellStart[c + 1] = CellStart[c] + Count[c];
		CellItems.SetNum(N);
		TArray<int32> Fill = CellStart;
		for (int32 i = 0; i < N; i++) CellItems[Fill[CellOf(Pts[i])]++] = i;
		Center = FVector((MinX + MaxX) / 2, (MinY + MaxY) / 2, 0);
	}

	BuildRoadWalls();
}

void FTrackSpline::BuildRoadWalls()
{
	// Grass run-off out to a wall (barrier) on each side: wider on the outside of the turns,
	// narrower where the road passes close to another part of the circuit (each side keeps to its
	// half of the gap) and on the inside of a tight turn (never near the turn's center).
	const float Half = RoadHalfWidth;
	TArray<float> CornerK = Kappa;      // the turn around here (+ left)
	BlurLoop(CornerK, 25.f);
	WallLeft.SetNum(N);
	WallRight.SetNum(N);
	for (int32 i = 0; i < N; i++)
	{
		const float Turniness = FMath::Clamp(FMath::Abs(CornerK[i]) * 120.f, 0.f, 1.f);   // 1: tighter than ~120 m
		const float Outside = Half + 10.f + 12.f * Turniness;
		const float Inside = Half + 7.f;
		WallRight[i] = CornerK[i] > 0.f ? Outside : Inside;      // left turn: the right side is the outside
		WallLeft[i] = -(CornerK[i] > 0.f ? Inside : Outside);
	}
	// the inside of a tight turn
	for (int32 i = 0; i < N; i++)
	{
		float KMax = 0.f, LocalK = 0.f;
		for (int32 k = -30; k <= 30; k++)
		{
			const float K = Kappa[Wrap(i + k, N)];
			KMax = FMath::Max(KMax, FMath::Abs(K));
			LocalK += K;
		}
		if (KMax < 1e-4f) continue;
		const float Limit = FMath::Max(Half + 2.5f, 0.7f / KMax);
		if (LocalK > 0.f) WallLeft[i] = FMath::Max(WallLeft[i], -Limit);
		else WallRight[i] = FMath::Min(WallRight[i], Limit);
	}
	// other parts of the circuit nearby: keep to this side's half of the gap
	for (int32 i = 0; i < N; i++)
	{
		const FVector2D P = Pts[i];
		const FVector2D R(-FMath::Sin(Psi[i]), FMath::Cos(Psi[i]));
		const int32 Cx = FMath::FloorToInt(((float)P.X - GridX0) / GridCell), Cy = FMath::FloorToInt(((float)P.Y - GridY0) / GridCell);
		for (int32 Gy = Cy - 4; Gy <= Cy + 4; Gy++)
		{
			for (int32 Gx = Cx - 4; Gx <= Cx + 4; Gx++)
			{
				if (Gx < 0 || Gy < 0 || Gx >= GridNx || Gy >= GridNy) continue;
				const int32 Cell = Gy * GridNx + Gx;
				for (int32 k = CellStart[Cell]; k < CellStart[Cell + 1]; k++)
				{
					const int32 J = CellItems[k];
					const int32 Gap = FMath::Abs(J - i);
					if (FMath::Min(Gap, N - Gap) * Ds < 150.f) continue;
					const FVector2D V = Pts[J] - P;
					const float Dist = V.Size();
					const float Side = V.X * R.X + V.Y * R.Y;
					if (FMath::Abs(Side) < 0.5f * Dist) continue;      // ahead or behind, not off to one side
					const float Room = FMath::Max(Half + 1.5f, 0.5f * Dist - 1.f);
					if (Side > 0.f) WallRight[i] = FMath::Min(WallRight[i], Room);
					else WallLeft[i] = FMath::Max(WallLeft[i], -Room);
				}
			}
		}
	}
	// no sudden steps: the tightest value within 12 m, then smoothed
	for (int32 Side = 0; Side < 2; Side++)
	{
		TArray<float>& W = Side == 0 ? WallLeft : WallRight;
		TArray<float> Tight = W;
		for (int32 i = 0; i < N; i++)
		{
			float V = W[i];
			for (int32 k = -12; k <= 12; k++) V = Side == 0 ? FMath::Max(V, W[Wrap(i + k, N)]) : FMath::Min(V, W[Wrap(i + k, N)]);
			Tight[i] = V;
		}
		BlurLoop(Tight, 6.f);
		for (float& V : Tight) V = Side == 0 ? FMath::Min(V, -(Half + 1.5f)) : FMath::Max(V, Half + 1.5f);
		W = MoveTemp(Tight);
	}
	// kerbs on both sides through the turns, and a little before and after
	Kerbs.SetNumZeroed(N);
	for (int32 i = 0; i < N; i++)
	{
		if (FMath::Abs(Kappa[i]) < 1.f / 250.f) continue;
		for (int32 k = -18; k <= 18; k++) Kerbs[Wrap(i + k, N)] = 3;
	}
	UE_LOG(LogTemp, Display, TEXT("Road course: %d samples, %d turns, heights %.1f .. %.1f m"), N, Corners.Num(),
		FMath::Min(Elev), FMath::Max(Elev));
}

// ---------------------------------------------------------------------------------- PitLane

namespace PitLane
{
	static constexpr float MedianGrow = 50.f;

	float Rel(float S) { return FTrackSpline::DeltaS(0.f, S); }

	bool InExitLane(float S)
	{
		if (!FTrackSpline::IsOval()) return false;
		const float R = Rel(S);
		return R > WallEnd && R <= LaneEnd;
	}

	bool InPitRoad(float S)
	{
		if (!FTrackSpline::IsOval()) return false;
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
		if (!FTrackSpline::IsOval()) return false;
		const float R = Rel(S);
		return R >= Start && R <= End;
	}

	bool PitWallAt(float S)
	{
		if (!FTrackSpline::IsOval()) return false;
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
		if (!FTrackSpline::IsOval()) return FTrackSpline::InnerWall;   // (the road course: see FTrackSpline::MinD)
		if (InPitRoad(S)) return BackDAt(S);
		if (InExitLane(S)) return LaneInnerAt(S) - 25.f;
		return FTrackSpline::InnerWall;
	}

	float StallStart(int32 I) { return (I - Stalls / 2.f) * StallLength; }

	float DividerD(float S)
	{
		if (!FTrackSpline::IsOval()) return NAN;
		const float R = Rel(S);
		if (R >= EntryLaneStart && R < Start)
		{
			return FMath::Lerp(TrackEdge, LaneDividerD, FTrackSpline::Smooth((R - EntryLaneStart) / 90.f));
		}
		return NAN;
	}
}
