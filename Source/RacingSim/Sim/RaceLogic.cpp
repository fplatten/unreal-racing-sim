#include "Sim/RaceLogic.h"

// ----------------------------------------------------------------------------- FRacingLine

namespace
{
	constexpr float Spread = 140.f;
	constexpr float Resolution = 2.f;
	constexpr float MaxSpeed = 150.f;
}

FRacingLine::FRacingLine(const FTrackSpline* InTrack)
	: Track(InTrack)
{
	for (float Start : Track->TurnStarts()) Centers.Add(Start + FTrackSpline::Turn / 2.f);
	HalfWidth = FTrackSpline::Turn / 2.f + Spread;
}

float FRacingLine::Offset(float S) const
{
	float W = 0.f;
	for (float C : Centers)
	{
		const float X = FMath::Abs(FTrackSpline::DeltaS(C, S)) / HalfWidth;
		if (X < 1.f) W = FMath::Max(W, 0.5f * (1.f + FMath::Cos(UE_PI * X)));
	}
	return High + (Low - High) * W;
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
	else if (NewLap < E.LapIndex)
	{
		E.LapIndex = NewLap;   // driving backward over the line
	}
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
