#include "Sim/RaceLogic.h"
#include "Misc/CommandLine.h"

// ----------------------------------------------------------------------------- FRacingLine

namespace
{
	constexpr float Spread = 140.f;
	constexpr float Resolution = 2.f;
	constexpr float MaxSpeed = 150.f;
	/** Road course braking: how much of the grip the AI plans to use (tuning: -AiBrakeGrip=); the
	 *  brakes' own limit is FVehicleSpec::MaxBrakeDecel. */
	float BrakeConfidence()
	{
		static const float C = [] { float V = 0.82f; FParse::Value(FCommandLine::Get(), TEXT("AiBrakeGrip="), V); return V; }();
		return C;
	}
}

FRacingLine::FRacingLine(const FTrackSpline* InTrack)
	: Track(InTrack)
{
	if (!FTrackSpline::IsOval())
	{
		BuildRoadLine();
		return;
	}
	for (float Start : Track->TurnStarts()) Centers.Add(Start + FTrackSpline::Turn / 2.f);
	HalfWidth = FTrackSpline::Turn / 2.f + Spread;
}

float FRacingLine::Offset(float S) const
{
	if (Offsets.Num() > 0)
	{
		const float X = FTrackSpline::WrapS(S) / Step;
		const int32 I = FMath::Min((int32)X, Offsets.Num() - 1);
		const float T = X - I;
		return Offsets[I] + (Offsets[(I + 1) % Offsets.Num()] - Offsets[I]) * T;
	}
	float W = 0.f;
	for (float C : Centers)
	{
		const float X = FMath::Abs(FTrackSpline::DeltaS(C, S)) / HalfWidth;
		if (X < 1.f) W = FMath::Max(W, 0.5f * (1.f + FMath::Cos(UE_PI * X)));
	}
	return High + (Low - High) * W;
}

float FRacingLine::PathCurvature(float S) const
{
	if (Curv.Num() == 0) return Track->Curvature(S);
	const float X = FTrackSpline::WrapS(S) / Step;
	const int32 I = FMath::Min((int32)X, Curv.Num() - 1);
	const float T = X - I;
	return Curv[I] + (Curv[(I + 1) % Curv.Num()] - Curv[I]) * T;
}

void FRacingLine::BuildRoadLine()
{
	// Minimum curvature: each point of the line is pulled toward the straight line between its
	// neighbours (Gauss-Seidel with over-relaxation), within the road less EdgeMargin. Solved on
	// an 8 m spacing first (the long-range shape of each turn), then refined every 2 m.
	const int32 M = FMath::RoundToInt(FTrackSpline::Length / 2.f);
	Step = FTrackSpline::Length / M;
	const float Lim = FTrackSpline::RoadOuter - EdgeMargin;
	TArray<FVector2D> C, R;
	C.SetNum(M);
	R.SetNum(M);
	for (int32 i = 0; i < M; i++)
	{
		const FVector P = Track->Centerline(i * Step), Rt = Track->Right(i * Step);
		C[i] = FVector2D(P.X, P.Y);
		R[i] = FVector2D(Rt.X, Rt.Y);
	}
	Offsets.SetNumZeroed(M);
	auto Relax = [&](int32 Stride, int32 Iterations, float Omega)
	{
		const int32 K = M / Stride;
		for (int32 It = 0; It < Iterations; It++)
		{
			for (int32 j = 0; j < K; j++)
			{
				const int32 Ia = ((j + K - 1) % K) * Stride, I = j * Stride, Ib = ((j + 1) % K) * Stride;
				const FVector2D Pa = C[Ia] + R[Ia] * Offsets[Ia], P = C[I] + R[I] * Offsets[I], Pb = C[Ib] + R[Ib] * Offsets[Ib];
				// the point on the chord at the same fraction of the way along
				const float La = FTrackSpline::DeltaS(Ia * Step, I * Step), Lb = FTrackSpline::DeltaS(I * Step, Ib * Step);
				const FVector2D Target = Pa + (Pb - Pa) * (La / FMath::Max(La + Lb, 1e-3f));
				const float Move = FVector2D::DotProduct(Target - P, R[I]);
				Offsets[I] = FMath::Clamp(Offsets[I] + Omega * Move, -Lim, Lim);
			}
		}
		if (Stride > 1)
		{
			// fill in between the solved points
			for (int32 j = 0; j < K; j++)
			{
				const int32 I = j * Stride, Ib = ((j + 1) % K) * Stride;
				const int32 Span = (j + 1 < K) ? Stride : M - I;
				for (int32 k = 1; k < Span; k++) Offsets[(I + k) % M] = FMath::Lerp(Offsets[I], Offsets[Ib], k / (float)Span);
			}
		}
	};
	Relax(4, 4000, 1.6f);
	Relax(1, 2500, 1.6f);
	// curvature of the line itself
	Curv.SetNumZeroed(M);
	for (int32 i = 0; i < M; i++)
	{
		const int32 Ia = (i + M - 1) % M, Ib = (i + 1) % M;
		const FVector2D Pa = C[Ia] + R[Ia] * Offsets[Ia], P = C[i] + R[i] * Offsets[i], Pb = C[Ib] + R[Ib] * Offsets[Ib];
		const FVector2D A = P - Pa, B = Pb - P;
		const float Turn = FMath::Atan2(A.X * B.Y - A.Y * B.X, A.X * B.X + A.Y * B.Y);   // + = turning toward +y (right)
		Curv[i] = -Turn / FMath::Max(0.5f * (float)(A.Size() + B.Size()), 1e-3f);
	}
	TArray<float> Smoothed = Curv;
	for (int32 i = 0; i < M; i++)
	{
		float Acc = 0.f, W = 0.f;
		for (int32 k = -4; k <= 4; k++)
		{
			const float Wk = FMath::Exp(-0.5f * FMath::Square(k / 2.f));
			Acc += Wk * Curv[(i + k + M) % M];
			W += Wk;
		}
		Smoothed[i] = Acc / W;
	}
	Curv = MoveTemp(Smoothed);
	float MinR = TNumericLimits<float>::Max();
	for (float K : Curv) if (FMath::Abs(K) > 1e-6f) MinR = FMath::Min(MinR, 1.f / FMath::Abs(K));
	UE_LOG(LogTemp, Display, TEXT("Racing line: %d points, tightest radius %.1f m"), M, MinR);
}

// --------------------------------------------------------------------------- FSpeedProfile

FSpeedProfile::FSpeedProfile(const FRacingLine* InLine, const FVehicleSpec& InSpec, const FAeroModel& Aero, float InGrip, float InBraking)
	: Line(InLine), Spec(InSpec), GripFactor(InGrip), BrakingDecel(InBraking)
{
	const FTrackSpline& Track = Line->GetTrack();
	const int32 N = FMath::CeilToInt(FTrackSpline::Length / Resolution);
	Speeds.SetNumUninitialized(N);
	const float M = Spec.Mass;
	const float K = 0.5f * FAeroModel::AirDensity * Aero.LiftArea;
	if (!FTrackSpline::IsOval())
	{
		BuildRoad(Speeds, [this](float S) { return Line->PathCurvature(S); }, K);
		for (int32 L = 0; L < NumLanes; L++)
		{
			const float D = (L - NumLanes / 2) * LaneSpacing;
			BuildRoad(LaneSpeeds[L], [&Track, D](float S)
			{
				const float Kc = Track.Curvature(S);
				return Kc / FMath::Max(1.f + Kc * D, 0.1f);
			}, K);
		}
		return;
	}
	for (int32 i = 0; i < N; i++)
	{
		const float S = i * Resolution;
		const float R = Track.Radius(S, Line->Offset(S));
		Speeds[i] = R >= TNumericLimits<float>::Max() ? MaxSpeed : CornerSpeed(M, R, Track.Bank(S), GripFactor, K);
	}
	// backward braking pass (twice around to handle the wrap)
	for (int32 Pass = 0; Pass < 2; Pass++)
	{
		for (int32 i = N - 1; i >= 0; i--)
		{
			const float Next = Speeds[(i + 1) % N];
			const float Allowed = FMath::Sqrt(Next * Next + 2.f * BrakingDecel * Resolution);
			if (Allowed < Speeds[i]) Speeds[i] = Allowed;
		}
	}
}

void FSpeedProfile::BuildRoad(TArray<float>& Out, TFunctionRef<float(float)> CurvatureAt, float K) const
{
	// Corner speeds, then backwards from every corner: how fast the car can arrive and still
	// slow down in time. The braking grip grows with downforce, but whatever the turn already
	// asks of the tires is not available for braking (friction circle), and braking downhill
	// takes longer (the runs downhill into the valley and the hairpins).
	const FTrackSpline& Track = Line->GetTrack();
	const int32 N = FMath::CeilToInt(FTrackSpline::Length / Resolution);
	const float M = Spec.Mass, G = 9.81f;
	TArray<float> Kabs;
	Kabs.SetNumUninitialized(N);
	Out.SetNumUninitialized(N);
	for (int32 i = 0; i < N; i++)
	{
		const float Kp = FMath::Abs(CurvatureAt(i * Resolution));
		Kabs[i] = Kp;
		Out[i] = Kp < 1e-4f ? MaxSpeed : CornerSpeed(M, 1.f / Kp, 0.f, GripFactor, K);
	}
	// the AI aims a little short of the grip
	for (int32 Pass = 0; Pass < 2; Pass++)
	{
		for (int32 i = N - 1; i >= 0; i--)
		{
			const float Next = Out[(i + 1) % N];
			const float GripAcc = GripFactor * (G + K * Next * Next / M);
			const float Lat = Next * Next * Kabs[i];
			const float Used = FMath::Clamp(Lat / GripAcc, 0.f, 0.95f);
			float Decel = FMath::Min(GripAcc * BrakeConfidence(), Spec.MaxBrakeDecel) * FMath::Sqrt(1.f - Used * Used);
			Decel = FMath::Max(Decel + G * Track.Grade(i * Resolution), 2.f);
			const float Allowed = FMath::Sqrt(Next * Next + 2.f * Decel * Resolution);
			if (Allowed < Out[i]) Out[i] = Allowed;
		}
	}
}

float FSpeedProfile::LaneAt(float S, float D) const
{
	if (LaneSpeeds[0].Num() == 0) return At(S);
	const float X = FMath::Clamp(D / LaneSpacing + NumLanes / 2, 0.f, NumLanes - 1.001f);
	const int32 L = (int32)X;
	const float U = X - L;
	const float P = FTrackSpline::WrapS(S) / Resolution;
	const int32 I = FMath::Min((int32)P, LaneSpeeds[L].Num() - 1);
	const float T = P - I;
	auto Sample = [I, T](const TArray<float>& A) { return A[I] + (A[(I + 1) % A.Num()] - A[I]) * T; };
	return Sample(LaneSpeeds[L]) * (1.f - U) + Sample(LaneSpeeds[L + 1]) * U;
}

FSpeedProfile FSpeedProfile::WithAero(const FAeroModel& Aero) const
{
	return FSpeedProfile(Line, Spec, Aero, GripFactor, BrakingDecel);
}

float FSpeedProfile::CornerSpeed(float M, float R, float Bank, float Mu, float K)
{
	// v^2 * (m(cos - mu sin)/R - mu k) = m g (sin + mu cos)
	const float Cos = FMath::Cos(Bank), Sin = FMath::Sin(Bank);
	const float Denom = M * (Cos - Mu * Sin) / R - Mu * K;
	if (Denom <= 0.f) return MaxSpeed;
	return FMath::Min(MaxSpeed, FMath::Sqrt(M * 9.81f * (Sin + Mu * Cos) / Denom));
}

float FSpeedProfile::At(float S) const
{
	const float X = FTrackSpline::WrapS(S) / Resolution;
	const int32 I = (int32)X;
	const float T = X - I;
	const float A = Speeds[I % Speeds.Num()], B = Speeds[(I + 1) % Speeds.Num()];
	return A + (B - A) * T;
}

// -------------------------------------------------------------------------------- Drafting

namespace Drafting
{
	static constexpr float MinGap = 4.5f;
	static constexpr float TrainBonus = 0.25f;
	static constexpr float ResponseTime = 0.12f;

	float WakeStrength(const FCarCore& Me, const FCarCore& Lead)
	{
		const float Gap = FTrackSpline::DeltaS(Me.TrackS(), Lead.TrackS());
		if (Gap <= 0.f || Gap > WakeLength) return 0.f;
		const float Lateral = FMath::Abs(Lead.TrackD() - Me.TrackD());
		if (Lateral >= WakeHalfWidth) return 0.f;
		// a wake only exists if the leader is actually moving through the air
		const float LeadSpeed = Lead.Speed();
		if (LeadSpeed < 15.f) return 0.f;
		const float SpeedScale = FMath::Clamp((LeadSpeed - 15.f) / 30.f, 0.f, 1.f);
		const float LongT = FMath::Clamp((Gap - MinGap) / (WakeLength - MinGap), 0.f, 1.f);
		const float Longitudinal = FMath::Pow(1.f - LongT, 1.3f);
		// the wake also narrows further back (cone shape)
		const float HalfWidth = WakeHalfWidth * (0.75f + 0.25f * (1.f - LongT));
		const float LatT = Lateral / HalfWidth;
		const float LateralFall = LatT >= 1.f ? 0.f : 1.f - LatT * LatT;
		return MaxDraft * Longitudinal * LateralFall * SpeedScale;
	}

	void Update(const TArray<FCarCore*>& Cars, float Dt)
	{
		const float Blend = 1.f - FMath::Exp(-Dt / ResponseTime);
		for (FCarCore* Me : Cars)
		{
			float Best = 0.f, Sum = 0.f;
			for (const FCarCore* Lead : Cars)
			{
				if (Lead == Me) continue;
				const float F = WakeStrength(*Me, *Lead);
				if (F > 0.f)
				{
					Sum += F;
					Best = FMath::Max(Best, F);
				}
			}
			const float Target = FMath::Min(MaxDraft, Best + TrainBonus * (Sum - Best));
			Me->SetDraftFactor(Me->GetDraftFactor() + (Target - Me->GetDraftFactor()) * Blend);
		}
	}
}

// ----------------------------------------------------------------------------- FLapTracker

void FLapTracker::NewRace(int32 Laps, int32 Stops)
{
	RequiredStops = Stops;
	RaceLaps = Laps;
	FinishedCount = 0;
	RaceStartTime = NAN;
}

int32 FLapTracker::AddEntry(const FString& Name, bool bPlayer, float StartS)
{
	FEntry E;
	E.Name = Name;
	E.bPlayer = bPlayer;
	ResetEntry(E, StartS);
	const int32 Index = Entries.Add(E);
	Order.Add(Index);
	return Index;
}

void FLapTracker::ResetEntry(FEntry& E, float StartS)
{
	const float S = FTrackSpline::WrapS(StartS);
	E.LastS = S;
	// positions in the second half of the lap are treated as "approaching the line"
	E.TotalDistance = S > FTrackSpline::Length / 2 ? S - FTrackSpline::Length : S;
	E.LapIndex = FMath::FloorToInt(E.TotalDistance / FTrackSpline::Length);
}

void FLapTracker::Reset(int32 Index, float StartS)
{
	FEntry& E = Entries[Index];
	ResetEntry(E, StartS);
	E.LapStartTime = E.LastLapTime = E.BestLapTime = E.FinishTime = NAN;
	E.bFinished = false;
	E.FinishOrder = 0;
	E.PitStops = 0;
}

void FLapTracker::ResetToLine(int32 Index)
{
	FEntry& E = Entries[Index];
	E.LapIndex = FMath::Max(E.LapIndex, 0);
	E.TotalDistance = E.LapIndex * FTrackSpline::Length + 0.01f;
	E.LastS = 0.01f;
	E.LapStartTime = Time;
}

void FLapTracker::Teleport(int32 Index, float NewS)
{
	FEntry& E = Entries[Index];
	E.TotalDistance += FTrackSpline::DeltaS(E.LastS, NewS);
	E.LastS = FTrackSpline::WrapS(NewS);
}

void FLapTracker::Update(int32 Index, float S)
{
	FEntry& E = Entries[Index];
	E.TotalDistance += FTrackSpline::DeltaS(E.LastS, S);
	E.LastS = S;
	const int32 NewLap = FMath::FloorToInt(E.TotalDistance / FTrackSpline::Length);
	if (NewLap > E.LapIndex)
	{
		if (E.LapIndex >= 0 && !FMath::IsNaN(E.LapStartTime))
		{
			E.LastLapTime = Time - E.LapStartTime;
			if (FMath::IsNaN(E.BestLapTime) || E.LastLapTime < E.BestLapTime) E.BestLapTime = E.LastLapTime;
		}
		if (NewLap == 0 && FMath::IsNaN(RaceStartTime)) RaceStartTime = Time;   // green flag
		E.LapStartTime = Time;
		E.LapIndex = NewLap;
		if (!E.bFinished && NewLap >= RaceLaps)                                  // checkered flag
		{
			E.bFinished = true;
			E.FinishOrder = ++FinishedCount;
			E.FinishTime = RaceTime();
		}
	}
	// Driving (or spinning) backward over the line leaves the lap count alone: crossing it forward
	// again is not a new lap. (Lowering it here made that crossing a bogus few-second lap.)
}

int32 FLapTracker::Group(const FEntry& E) const
{
	if (!E.bFinished) return 1;
	return StopsMet(E) ? 0 : 2;
}

void FLapTracker::UpdatePositions()
{
	// finishers who made their mandatory stops in finishing order, then the cars still running
	// by distance, then finishers who missed a stop
	Order.StableSort([this](int32 A, int32 B)
	{
		const FEntry& Ea = Entries[A];
		const FEntry& Eb = Entries[B];
		const int32 Ga = Group(Ea), Gb = Group(Eb);
		if (Ga != Gb) return Ga < Gb;
		if (Ea.bFinished) return Ea.FinishOrder < Eb.FinishOrder;
		return Ea.TotalDistance > Eb.TotalDistance;
	});
	for (int32 i = 0; i < Order.Num(); i++) Entries[Order[i]].Position = i + 1;
}

// -------------------------------------------------------------------------------- FPitStop

void FPitStop::Reset()
{
	State = EStatus::None;
	CountdownTime = 0.f;
	StatusClock = 0.f;
	bServed = false;
}

bool FPitStop::InBox(int32 InStall, float S, float D)
{
	if (!FTrackSpline::IsOval()) return false;
	const float R = PitLane::Rel(S), Start = PitLane::StallStart(InStall);
	return R > Start + 0.5f && R < Start + PitLane::StallLength - 0.5f
		&& D < PitLane::FastLaneInner - 0.5f && D > PitLane::BoxInner + 1.f;
}

bool FPitStop::Update(float Dt, float S, float D, float Speed)
{
	StatusClock += Dt;
	const bool bIn = InBox(Stall, S, D);
	if (!bIn) bServed = false;
	if (State == EStatus::Servicing)
	{
		if (!bIn)
		{
			SetStatus(EStatus::Aborted);
			return false;
		}
		CountdownTime -= Dt;
		if (CountdownTime <= 0.f)
		{
			CountdownTime = 0.f;
			bServed = true;
			SetStatus(EStatus::Complete);
			return true;
		}
		return false;
	}
	if (bIn && !bServed && FMath::Abs(Speed) < StoppedSpeed)
	{
		CountdownTime = ServiceTime;
		SetStatus(EStatus::Servicing);
	}
	return false;
}
