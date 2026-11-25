#include "Game/RaceDirector.h"
#include "Car/RaceCar.h"
#include "Track/OvalTrack.h"

namespace
{
	/** Every oval race starts on the front straight, just off the last turn (rolling start). */
	constexpr float StraightStartS = FTrackSpline::OvalLength - FTrackSpline::Straight / 2.f;
	const float GridLanes[] = { -4.6f, -0.4f, 3.8f };
	/** Inside line used when the player resets - well clear of the AI groove on the straights. */
	float ResetLane() { return FTrackSpline::IsOval() ? -6.0f : -4.0f; }
	/** Standing start (road course): two by two, staggered, the pole 8 m behind the line. */
	constexpr float PoleGap = 8.f;
	constexpr float StaggerGap = 7.5f;
	constexpr float StaggerLane = 3.0f;
}

ARaceDirector::ARaceDirector()
{
	PrimaryActorTick.bCanEverTick = false;
	bAsyncPhysicsTickEnabled = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

void ARaceDirector::BeginPlay()
{
	Super::BeginPlay();
	if (!Track) Track = AOvalTrack::Get(GetWorld());
}

const FRacingLine* ARaceDirector::GetRacingLine()
{
	if (!Track) Track = AOvalTrack::Get(GetWorld());
	if (!Line && Track) Line = MakeUnique<FRacingLine>(&Track->GetSpline());
	return Line.Get();
}

void ARaceDirector::ClearField()
{
	FScopeLock Lock(&FieldLock);
	bFieldReady = false;
	bRunning = false;
	Cars.Reset();
	Field.Reset();
	Drivers.Reset();
	Pits.Reset();
	Entries.Reset();
	Laps = FLapTracker();
	FScopeLock SnapGuard(&SnapLock);
	Snap = FRaceSnapshot();
}

void ARaceDirector::FinishField()
{
	FScopeLock Lock(&FieldLock);
	bFieldReady = true;
	bGridPending = true;
}

void ARaceDirector::SetPlayerDownforce(float Level)
{
	FScopeLock Lock(&FieldLock);
	for (FCarCore* C : Field)
	{
		if (C->bPlayer) C->Sim->GetAero().SetDownforceLevel(Level);
	}
}

void ARaceDirector::RequestStop()
{
	bStopPending = true;
}

void ARaceDirector::AddCar(ARaceCar* Car, TUniquePtr<FAIDriver> Driver)
{
	FScopeLock Lock(&FieldLock);
	if (!Track) Track = AOvalTrack::Get(GetWorld());
	Cars.Add(Car);
	FCarCore* Core = &Car->Core();
	Field.Add(Core);
	Entries.Add(Laps.AddEntry(Core->Name, Core->bPlayer, Core->S));
	Pits.Add(FPitStop(Core->bPlayer ? PitLane::PlayerStall : NextAiStall()));
	Drivers.Add(MoveTemp(Driver));
	// the drivers keep pointers to their pit state: re-point them whenever the array grows
	for (int32 i = 0; i < Drivers.Num(); i++)
	{
		if (Drivers[i]) Drivers[i]->SetPit(&Pits[i]);
	}
}

int32 ARaceDirector::NextAiStall() const
{
	// every other box first (an empty box each side), then the ones in between; the middle box
	// is the player's
	int32 Used = 0;
	for (const FPitStop& P : Pits) if (P.GetStall() != PitLane::PlayerStall) Used++;
	int32 K = 0;
	for (int32 Pass = 0; Pass < 2; Pass++)
	{
		for (int32 St = Pass; St < PitLane::Stalls; St += 2)
		{
			if (St == PitLane::PlayerStall) continue;
			if (K++ == Used) return St;
		}
	}
	return Used % PitLane::Stalls;
}

void ARaceDirector::GridSlot(int32 Slot, int32 CarCount, float& OutS, float& OutD)
{
	if (FTrackSpline::IsRoadCourse())
	{
		// staggered: pole on the left (the outside line into the first turn, a right-hander),
		// each car half a row behind the one before it on the other side
		OutS = FTrackSpline::WrapS(-PoleGap - Slot * StaggerGap);
		OutD = Slot % 2 == 0 ? -StaggerLane : StaggerLane;
		return;
	}
	const int32 Rows = (CarCount + GridColumns - 1) / GridColumns;
	const float FrontS = StraightStartS + (Rows - 1) * GridRowGap + 5.f;
	const int32 Row = Slot / GridColumns, Col = Slot % GridColumns;
	OutS = FrontS - Row * GridRowGap;
	OutD = GridLanes[Col];
}

TArray<int32> ARaceDirector::PlanPitLaps(int32 LapCount, int32 Stops, int32 Index)
{
	TArray<int32> Plan;
	Plan.SetNum(Stops);
	const int32 Offset = Stops > 0 && LapCount >= 2 * (Stops + 1) ? Index % 3 - 1 : 0;
	for (int32 K = 0; K < Stops; K++)
	{
		int32 Lap = FMath::RoundToInt((K + 1) * LapCount / (float)(Stops + 1)) + Offset;
		Lap = FMath::Max(K > 0 ? Plan[K - 1] + 1 : 1, FMath::Min(LapCount, Lap));
		Plan[K] = Lap;
	}
	return Plan;
}

void ARaceDirector::RequestStart(int32 InLaps, int32 InStops)
{
	PendingLaps = InLaps;
	PendingStops = InStops;
	bStartPending = true;
}

FRaceSnapshot ARaceDirector::GetSnapshot() const
{
	FScopeLock Lock(&SnapLock);
	return Snap;
}

// ------------------------------------------------------------------------- physics thread

void ARaceDirector::AsyncPhysicsTickActor(float Dt, float SimTime)
{
	Super::AsyncPhysicsTickActor(Dt, SimTime);
	FScopeLock Lock(&FieldLock);
	if (!bFieldReady || Cars.Num() == 0 || !Track) return;

	const FTrackSpline& Spline = Track->GetSpline();
	bool bReady = true;
	for (ARaceCar* Car : Cars) bReady &= Car->PT_Begin(Spline);
	if (!bReady)
	{
		for (ARaceCar* Car : Cars) Car->PT_End(SimTime);
		return;
	}
	if (bStopPending.exchange(false))
	{
		bRunning = false;
		bGridPending = true;
	}
	if (bGridPending.exchange(false)) PT_PlaceGrid(0.f);
	if (bStartPending.exchange(false)) PT_StartRace();

	// standing start: the field waits on the grid while the five red lights come on, one a
	// second, and goes when they go out
	const bool bWaitingForLights = bRunning && LightsTime >= 0.f;
	if (bWaitingForLights)
	{
		LightsTime += Dt;
		if (LightsTime >= LightsOutAt)
		{
			LightsTime = -1.f;
			GreenTime = 0.f;
			Laps.StartClock();
		}
	}
	else if (GreenTime >= 0.f)
	{
		GreenTime += Dt;
		if (GreenTime > 3.f) GreenTime = -1.f;
	}
	if (bRunning && !bWaitingForLights)
	{
		for (int32 i = 0; i < Cars.Num(); i++)
		{
			if (!Drivers[i] && Cars[i]->ConsumeResetRequest()) PT_ResetPlayer(i);
		}
	}

	for (FCarCore* C : Field) C->UpdateTrackPosition(Spline, Dt);
	Drafting::Update(Field, Dt);

	for (int32 i = 0; i < Cars.Num(); i++)
	{
		if (!bRunning && Cars[i]->IsScripted())
		{
			Cars[i]->PT_ApplyPlayerInput(Dt);   // tests drive the car with a fixed throttle
			continue;
		}
		if (!bRunning || bWaitingForLights)
		{
			// lined up on the grid (behind the menu, or waiting for the lights): held on the brakes
			// (not hard enough to engage reverse)
			Field[i]->Input().Clear();
			Field[i]->Input().Brake = 0.45f;
			continue;
		}
		if (Drivers[i]) Drivers[i]->Update(Dt, Field);
		else Cars[i]->PT_ApplyPlayerInput(Dt);
	}

	for (int32 i = 0; i < Cars.Num(); i++)
	{
		FCarCore* C = Field[i];
		C->Input().bPitHold = Pits[i].IsServicing();
		C->Sim->Step(Dt, C->Body);
	}

	if (bRunning)
	{
		Laps.AdvanceTime(Dt);
		for (int32 i = 0; i < Cars.Num(); i++) Laps.Update(Entries[i], Field[i]->S);
		PT_UpdatePitStops(Dt);
		Laps.UpdatePositions();

		for (int32 i = 0; i < Cars.Num(); i++)
		{
			FCarCore* C = Field[i];
			if (!Drivers[i])
			{
				PT_CheckPlayerRecovery(i, Dt);
				continue;
			}
			const bool bOffTrack = C->D < Spline.MinD(C->S) - 1.f || C->D > Spline.MaxD(C->S) + 2.f || C->Body.Pos.Z < -3.f;
			if (Drivers[i]->NeedsRecovery() || C->Body.Rot.GetUpVector().Z < 0.3f || bOffTrack) PT_Recover(i);
		}
	}

	for (ARaceCar* Car : Cars) Car->PT_End(SimTime);
	PT_Publish();
}

void ARaceDirector::PT_PlaceGrid(float Speed)
{
	const FTrackSpline& Spline = Track->GetSpline();
	for (int32 i = 0; i < Cars.Num(); i++)
	{
		float S, D;
		GridSlot(i, Cars.Num(), S, D);
		Cars[i]->PT_Teleport(Spline, S, D, Speed);
	}
}

void ARaceDirector::PT_StartRace()
{
	// the oval: a rolling start, three abreast; a road course: a standing start
	const bool bStanding = FTrackSpline::IsRoadCourse();
	PT_PlaceGrid(bStanding ? 0.f : RollingStartSpeed);
	LightsTime = bStanding ? 0.f : -1.f;
	GreenTime = -1.f;
	Laps.NewRace(PendingLaps, PendingStops);
	for (int32 i = 0; i < Cars.Num(); i++)
	{
		Pits[i].Reset();
		Pits[i].SetPlannedLaps(PlanPitLaps(PendingLaps, PendingStops, i));
		Laps.Reset(Entries[i], Field[i]->S);
		if (Drivers[i])
		{
			// Each driver's own wing level: rookies run a heavy wing (stable, slower), veterans a
			// light one (fast, loose), +-1 at random. Tuning: -AiDownforce=<0..10> gives every AI
			// that level, -AiWingMax=<0..10> caps it.
			static int32 WingMax = -1;
			if (WingMax < 0) { WingMax = AiMaxDownforce; FParse::Value(FCommandLine::Get(), TEXT("AiWingMax="), WingMax); }
			int32 Wing = FMath::Min(Drivers[i]->PreferredWing(DownforceSteps, SetupRandom.NextInt(3) - 1), FMath::Clamp(WingMax, 0, DownforceSteps));
			int32 Forced = -1;
			if (FParse::Value(FCommandLine::Get(), TEXT("AiDownforce="), Forced) && Forced >= 0) Wing = FMath::Min(Forced, DownforceSteps);
			const float Level = Wing / (float)DownforceSteps;
			Drivers[i]->SetDownforceLevel(Level);
			UE_LOG(LogTemp, Display, TEXT("AI driver: %-9s %-7s (aggression %.2f) wing %d/%d"), *Field[i]->Name,
				FAIDriver::TypeName(Drivers[i]->GetAggression()), Drivers[i]->GetAggression(), Wing, DownforceSteps);
			Drivers[i]->ResetForRace();
		}
	}
	Laps.UpdatePositions();
	Recoveries = 0;
	bRunning = true;
}

void ARaceDirector::PT_UpdatePitStops(float Dt)
{
	for (int32 i = 0; i < Cars.Num(); i++)
	{
		FCarCore* C = Field[i];
		FLapTracker::FEntry& E = Laps.Entry(Entries[i]);
		FPitStop& Pit = Pits[i];
		if (Pit.Update(Dt, C->S, C->D, C->Speed())) Laps.PitStopCompleted(E);
		FAIDriver* Ai = Drivers[i].Get();
		if (!Ai || Ai->IsPitting() || E.bFinished || Laps.StopsMet(E)) continue;
		const float Rel = PitLane::Rel(C->S);
		if (Rel > FAIDriver::PitCommitRel - 60.f && Rel < FAIDriver::PitCommitRel
			&& E.Lap() >= Pit.NextPlannedLap(E.PitStops) && Laps.RaceTime() > 0.f)
		{
			Ai->RequestPit();
		}
	}
}

void ARaceDirector::PT_ResetPlayer(int32 Index)
{
	// restart on the inside lane, clear of the AI groove; move over if an AI car is right there
	float D = ResetLane();
	for (const FCarCore* C : Field)
	{
		if (C != Field[Index] && FMath::Abs(FTrackSpline::DeltaS(0.f, C->S)) < 12.f && FMath::Abs(C->D - D) < 3.f) D = C->D + 3.5f;
	}
	const float Edge = FTrackSpline::IsOval() ? 6.f : FTrackSpline::RoadOuter - 1.5f;
	Cars[Index]->PT_Teleport(Track->GetSpline(), 0.f, FMath::Clamp(D, -Edge, Edge), 0.f);
	Laps.ResetToLine(Entries[Index]);
}

void ARaceDirector::PT_CheckPlayerRecovery(int32 Index, float Dt)
{
	// a car below the world, upside down for 2 s, or stranded outside the retaining walls for
	// 3 s is put back on the track where it left it
	const FCarCore* C = Field[Index];
	PlayerUpsideDown = C->Body.Rot.GetUpVector().Z < 0.25f ? PlayerUpsideDown + Dt : 0.f;
	const FTrackSpline& Spline = Track->GetSpline();
	const bool bOutside = C->D > Spline.MaxD(C->S) + (FTrackSpline::IsOval() ? FTrackSpline::OuterWallThickness : 0.f) + 0.5f
		|| C->D < Spline.MinD(C->S) - 0.5f;
	PlayerOutside = bOutside ? PlayerOutside + Dt : 0.f;
	if (C->Body.Pos.Z > -3.f && PlayerUpsideDown < 2.0f && PlayerOutside < 3.0f) return;
	PlayerUpsideDown = PlayerOutside = 0.f;
	const float S = FTrackSpline::WrapS(C->S);
	float D = ResetLane();
	if (!RejoinClear(C, S, D)) D = FTrackSpline::RoadOuter - 1.5f;
	Cars[Index]->PT_Teleport(Track->GetSpline(), S, D, 25.f);
	Laps.Teleport(Entries[Index], S);
}

bool ARaceDirector::RejoinClear(const FCarCore* Self, float S, float D) const
{
	for (const FCarCore* O : Field)
	{
		if (O == Self) continue;
		const float Ds = FTrackSpline::DeltaS(S, O->S);
		if (Ds > -120.f && Ds < 25.f && FMath::Abs(O->D - D) < 3.5f) return false;
	}
	return true;
}

void ARaceDirector::PT_Recover(int32 Index)
{
	// put a crashed / stuck AI car back on the racing line (like a tow back onto the track),
	// or on the inside if that would drop it in front of someone; else wait for the traffic
	const FCarCore* C = Field[Index];
	const float S = FTrackSpline::WrapS(C->S + 5.f);
	float D = NAN;
	for (float Candidate : { Line->Offset(S), FTrackSpline::IsOval() ? -5.5f : 0.f })
	{
		if (RejoinClear(C, S, Candidate))
		{
			D = Candidate;
			break;
		}
	}
	if (FMath::IsNaN(D)) return;
	Recoveries++;
	Cars[Index]->PT_Teleport(Track->GetSpline(), S, D, 35.f);
	Laps.Teleport(Entries[Index], S);
	Drivers[Index]->ResetLane();
}

void ARaceDirector::PT_Publish()
{
	FScopeLock Lock(&SnapLock);
	Snap.Standings.SetNum(Cars.Num());
	const TArray<int32>& Order = Laps.GetOrder();
	for (int32 k = 0; k < Order.Num(); k++)
	{
		const FLapTracker::FEntry& E = Laps.GetEntries()[Order[k]];
		FRaceSnapshot::FRow& R = Snap.Standings[k];
		if (R.Name != E.Name) R.Name = E.Name;
		R.CarIndex = Entries.IndexOfByKey(Order[k]);
		R.bPlayer = E.bPlayer;
		R.Position = E.Position;
		R.Lap = E.Lap();
		R.LapsCompleted = E.LapsCompleted();
		R.LastLap = E.LastLapTime;
		R.BestLap = E.BestLapTime;
		R.bFinished = E.bFinished;
		R.FinishTime = E.FinishTime;
		R.PitStops = E.PitStops;
		R.bStopsMet = Laps.StopsMet(E);
		R.TotalDistance = E.TotalDistance;
		R.CurrentLapTime = E.CurrentLapTime(Laps.GetTime());
		const bool bAi = !E.bPlayer && Drivers.IsValidIndex(R.CarIndex) && Drivers[R.CarIndex];
		const float A = bAi ? Drivers[R.CarIndex]->GetAggression() : 0.f;
		R.DriverType = !bAi ? -1 : A < 0.34f ? 0 : A < 0.67f ? 1 : 2;
		R.Wing = Field.IsValidIndex(R.CarIndex) ? FMath::RoundToInt(Field[R.CarIndex]->Sim->GetAero().DownforceLevel * DownforceSteps) : 0;
	}
	Snap.Now = Laps.GetTime();
	for (int32 i = 0; i < Cars.Num(); i++)
	{
		if (!Field[i]->bPlayer) continue;
		const FPitStop& P = Pits[i];
		Snap.PitStatus = (int32)P.Status();
		Snap.PitCountdown = P.Countdown();
		Snap.PitStatusTime = P.StatusTime();
		Snap.PitStall = P.GetStall();
	}
	Snap.RaceTime = Laps.RaceTime();
	Snap.RaceLaps = Laps.GetRaceLaps();
	Snap.RequiredStops = Laps.GetRequiredStops();
	Snap.bRunning = bRunning;
	Snap.bLeaderFinished = Laps.LeaderFinished();
	Snap.Recoveries = Recoveries;
	Snap.StartLights = LightsTime >= 0.f ? FMath::Clamp(FMath::FloorToInt(LightsTime / LightInterval) + 1 - 1, 0, 5) : -1;
	Snap.GreenTime = GreenTime;
}
