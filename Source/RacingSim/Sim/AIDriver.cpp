#include "Sim/AIDriver.h"

namespace
{
	constexpr float DMin = -5.6f;
	constexpr float DMax = 5.7f;
	constexpr float CarWidthClearance = 2.9f;
	constexpr float PassOffset = 3.6f;
	/** Lateral distance below which a car ahead is considered "in our lane" (2 m wide cars). */
	constexpr float Overlap = 2.4f;
	/** Deceleration assumed when planning how fast we may close on a car ahead (m/s^2). */
	constexpr float FollowDecel = 3.0f;
	constexpr float HumanFollowDecel = 2.5f, HumanFollowGap = 20.f;
	constexpr float EmergencyTtc = 2.5f;
	constexpr float LaneRate = 2.2f;
	constexpr float WeightRate = 0.9f;
	/** Like a real rolling start, the field holds station until it has strung out. */
	constexpr float NoPassingTime = 14.f;
	/** End of turn 2 relative to the start/finish line (where the opening-lap formation ends). */
	constexpr float Turn2ExitRel = FTrackSpline::Straight / 2 + 2 * FTrackSpline::Turn + FTrackSpline::Chute + 40.f;
	constexpr float CurvatureGain = 18.f;
	constexpr float LateralGain = 0.0025f, LateralDamping = 0.006f;

	// pit path
	constexpr float EntryD = -12.3f, FastD = -21.9f, BoxD = -28.5f;
	constexpr float PitMoveEndRel = -950.f;
	constexpr float PitDecel = 4.5f;
	/** Brake used to hold a stopped car without the gearbox engaging reverse (threshold 0.5). */
	constexpr float HoldBrake = 0.45f;

	float Smooth(float X) { return FTrackSpline::Smooth(X); }
	float Lerp(float T, float A, float B) { return A + (B - A) * T; }   // jME interpolateLinear(t, a, b)
}

FAIDriver::FAIDriver(FCarCore* InCar, const FRacingLine* InLine, const FSpeedProfile& InProfile, float InAggression)
	: Car(InCar), Track(&InLine->GetTrack()), Line(InLine), Profile(InProfile), Aggression(InAggression)
{
}

void FAIDriver::SetDownforceLevel(float Level)
{
	Car->Sim->GetAero().SetDownforceLevel(Level);
	Profile = Profile.WithAero(Car->Sim->GetAero());
}

void FAIDriver::ResetForRace()
{
	ResetLane();
	RaceTime = 0.f;
	GridLane = Car->TrackD();
	bFormationDone = false;
	bReadyToRelease = false;
	bMergeHold = false;
}

void FAIDriver::ResetLane()
{
	PassTarget = nullptr;
	bPassYield = false;
	LaneD = Car->TrackD();
	LaneWeight = 1.f;
	StuckTime = 0.f;
	SteerIntegral = 0.f;
	SteerCmd = 0.f;
	PitPhase = EPitPhase::None;
}

bool FAIDriver::RacingLineClear(const TArray<FCarCore*>& Field, float S, float D) const
{
	const float LineD = Line->Offset(S);
	if (FMath::Abs(D - LineD) < 1.2f) return true;             // already on the line
	for (const FCarCore* C : Field)
	{
		if (C == Car) continue;
		const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
		if (Ds <= -30.f || Ds >= 40.f || FMath::Abs(C->TrackD() - D) <= 1.2f) continue;   // own lane: fine
		// anything on the line, or in the corridor we would cross to reach it, blocks the merge
		const float Lo = FMath::Min(D, LineD) - 1.8f, Hi = FMath::Max(D, LineD) + 1.8f;
		if (C->TrackD() > Lo && C->TrackD() < Hi) return false;
	}
	return true;
}

void FAIDriver::Update(float Dt, const TArray<FCarCore*>& Field)
{
	if (PitPhase != EPitPhase::None)
	{
		RaceTime += Dt;
		UpdatePit(Dt, Field);
		if (PitPhase != EPitPhase::None) return;
	}
	const float S = Car->TrackS(), D = Car->TrackD();
	const float V = Car->ForwardSpeed();

	// ---------------- traffic analysis ----------------
	const FCarCore* Blocker = nullptr;
	float BlockerGap = TNumericLimits<float>::Max();
	for (const FCarCore* C : Field)
	{
		if (C == Car) continue;
		const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
		if (Ds > 0 && Ds < 50.f && FMath::Abs(C->TrackD() - D) < 2.6f && Ds < BlockerGap)
		{
			Blocker = C;
			BlockerGap = Ds;
		}
	}

	float DesiredLane = Line->Offset(S);
	float WeightTarget = 0.f;
	float SpeedCap = TNumericLimits<float>::Max();
	// Rolling start, Indianapolis style: hold the grid lane (three abreast) through the green
	// flag and turns 1 and 2; the field races freely from the back straight onwards.
	const float RelS = PitLane::Rel(S);
	if (RaceTime > 3.f && RelS > Turn2ExitRel && RelS < Turn2ExitRel + 400.f) bReadyToRelease = true;
	if (!bFormationDone && (RaceTime > 70.f || (bReadyToRelease && RacingLineClear(Field, S, D)))) bFormationDone = true;
	if (!bFormationDone)
	{
		DesiredLane = GridLane;
		WeightTarget = 1.f;
	}
	// just out of the pits: run low on the apron until the racing line beside us is clear
	if (bMergeHold)
	{
		const bool bOnTrack = D > DMin - 1.2f;
		if ((bOnTrack && RacingLineClear(Field, S, D)) || RelS > PitLane::LaneEnd + 700.f)
		{
			bMergeHold = false;
		}
		else
		{
			DesiredLane = DMin;
			WeightTarget = 1.f;
		}
	}

	// start a pass when closing on a slower car or riding its draft
	RaceTime += Dt;
	const bool bPassingAllowed = bFormationDone && !bMergeHold && RaceTime > NoPassingTime;
	if (bPassingAllowed && !PassTarget && Blocker && BlockerGap < 30.f + 15.f * Aggression)
	{
		// a pass on the human player needs a clear speed advantage, not just a tow
		const bool bClosing = V > Blocker->ForwardSpeed() + (Blocker->bPlayer ? 1.5f : 0.3f);
		const bool bTowing = Car->GetDraftFactor() > 0.12f;
		// only start a pass that will be done before the next turn
		bool bRoom = true;
		if (!Track->InTurn(S))
		{
			const float ClosingSpeed = FMath::Max(V - Blocker->ForwardSpeed(), bTowing ? 3.f : 0.3f);
			const float PassDistance = (BlockerGap + 10.f) / ClosingSpeed * FMath::Max(V, 1.f);
			bRoom = PassDistance < Track->DistanceToNextTurn(S) - 40.f;
		}
		if ((bClosing || bTowing) && bRoom)
		{
			const float Lane = ChoosePassLane(Blocker, Field);
			if (!FMath::IsNaN(Lane))
			{
				PassTarget = Blocker;
				bPassYield = false;
				PassLane = Lane;
			}
		}
	}
	if (PassTarget)
	{
		const float Ds = FTrackSpline::DeltaS(S, PassTarget->TrackS());
		const bool bOutside = PassLane > PassTarget->TrackD();
		const bool bTurnNear = Track->InTurn(S) || Track->DistanceToNextTurn(S) < 80.f;
		// still not ahead on the outside as the turn arrives: back out and tuck in behind
		if (bOutside && bTurnNear && Ds > -3.f) bPassYield = true;
		if (Ds < -8.f || Ds > 70.f || (bPassYield && Ds > 10.f) || LaneBlocked(PassLane, PassTarget, Field, -6.f, 8.f))
		{
			PassTarget = nullptr;   // completed, abandoned or the lane closed
			bPassYield = false;
		}
		else
		{
			DesiredLane = PassLane;
			WeightTarget = 1.f;
			if (bPassYield) SpeedCap = FMath::Min(SpeedCap, PassTarget->ForwardSpeed() - 2.f);
		}
	}

	// Never run into the back of a car that is in our path, now or in the lane we are steering
	// into. The human player gets a much wider berth.
	const FCarCore* Threat = nullptr;
	float ThreatTtc = TNumericLimits<float>::Max();
	for (const FCarCore* C : Field)
	{
		if (C == Car) continue;
		const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
		const float Vc = C->ForwardSpeed();
		const float Closing = V - Vc;
		const bool bHuman = C->bPlayer;
		const float Decel = bHuman ? HumanFollowDecel : FollowDecel;
		const float Range = FMath::Max(70.f, Closing > 0.f ? Closing * Closing / (2.f * Decel) + 40.f : 0.f);
		if (Ds <= 0.f || Ds > Range) continue;
		const float Width = bHuman ? Overlap + 0.8f : Overlap;
		const float LaneThere = Lerp(LaneWeight, Line->Offset(S + Ds), LaneD);
		// where will that car be laterally by the time we reach it? (people cut across)
		const float Reach = Closing > 0.5f ? FMath::Min(Ds / Closing, 3.f) : 0.f;
		const float DPred = C->TrackD() + FMath::Clamp(C->LateralRate * Reach, -6.f, 6.f);
		const bool bInPath = FMath::Abs(C->TrackD() - D) < Width || FMath::Abs(C->TrackD() - LaneThere) < Width
			|| FMath::Abs(DPred - D) < Width || FMath::Abs(DPred - LaneThere) < Width;
		if (!bInPath) continue;
		const float Gap = (bHuman ? HumanFollowGap : 12.f) - 3.f * Aggression;
		const float Margin = FMath::Max(Ds - Gap, 0.f);
		const float Cap = Vc + FMath::Sqrt(2.f * Decel * Margin) - (bHuman ? 1.5f : 0.5f);
		SpeedCap = FMath::Min(SpeedCap, Cap);
		if (Closing > 0.5f)
		{
			const float Ttc = FMath::Max(Ds - 5.f, 0.f) / Closing;
			if (Ttc < ThreatTtc)
			{
				ThreatTtc = Ttc;
				Threat = C;
			}
		}
	}
	// emergency: about to hit a car we are still lined up with -> take any free lane
	if (Threat && ThreatTtc < EmergencyTtc && PassTarget != Threat)
	{
		const float Lane = ChoosePassLane(Threat, Field);
		if (!FMath::IsNaN(Lane))
		{
			PassTarget = Threat;
			bPassYield = false;
			PassLane = Lane;
			DesiredLane = Lane;
			WeightTarget = 1.f;
		}
	}

	// side-by-side avoidance
	for (const FCarCore* C : Field)
	{
		if (C == Car) continue;
		const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
		const float Dd = C->TrackD() - D;
		const float Clearance = C->bPlayer ? CarWidthClearance + 0.6f : CarWidthClearance;
		if (FMath::Abs(Ds) < 7.5f && FMath::Abs(Dd) < Clearance + 0.4f)
		{
			if (Dd > 0) DesiredLane = FMath::Min(DesiredLane, C->TrackD() - Clearance);
			else DesiredLane = FMath::Max(DesiredLane, C->TrackD() + Clearance);
			WeightTarget = 1.f;
			if (DesiredLane < DMin + 0.4f || DesiredLane > DMax - 0.4f)
			{
				SpeedCap = FMath::Min(SpeedCap, V - 1.f);   // squeezed: lift, don't brake
			}
		}
	}
	// inside the turns stay off the wall: understeer in traffic pushes cars up the banking
	const float DMaxHere = (Track->InTurn(S) || Track->DistanceToNextTurn(S) < 60.f) ? DMax - 0.8f : DMax;
	DesiredLane = FMath::Clamp(DesiredLane, DMin, DMaxHere);

	// smooth lateral target
	const float MaxStep = LaneRate * Dt * (WeightTarget > 0 ? 1.5f : 1.f);
	if (LaneWeight < 0.01f) LaneD = Line->Offset(S);
	LaneD += FMath::Clamp(DesiredLane - LaneD, -MaxStep, MaxStep);
	LaneWeight += FMath::Clamp(WeightTarget - LaneWeight, -WeightRate * Dt, WeightRate * Dt);

	// ---------------- steering: pure pursuit ----------------
	const float Lookahead = FMath::Clamp(10.f + 0.55f * FMath::Abs(V), 12.f, 75.f);
	const float SAhead = S + Lookahead;
	const float DTarget = Lerp(LaneWeight, Line->Offset(SAhead), LaneD);
	const float IntendedNow = Lerp(LaneWeight, Line->Offset(S), LaneD);
	Steer(Dt, D, V, SAhead, DTarget, IntendedNow);

	// ---------------- speed control ----------------
	float VTarget = Profile.At(S + 4.f);
	const bool bCornering = Track->InTurn(S) || Track->DistanceToNextTurn(S) < 150.f;
	if (bCornering)
	{
		// dirty air costs front downforce, and an off-line lane is a tighter radius
		VTarget *= 1.f - 0.25f * Car->GetDraftFactor();
		if (LaneWeight > 0.5f) VTarget *= 0.985f;
		if (!bFormationDone) VTarget *= 0.90f;
	}
	VTarget = FMath::Min(VTarget, SpeedCap);
	ApplySpeed(VTarget, S, D, IntendedNow);

	// stuck detection (crashed / facing the wall)
	if (FMath::Abs(V) < 3.f) StuckTime += Dt; else StuckTime = 0.f;
}

void FAIDriver::Steer(float Dt, float D, float V, float SAhead, float DAhead, float IntendedNow)
{
	const FRigidState& B = Car->Body;
	FVector Target = Track->Point(SAhead, DAhead);
	FVector Fwd = B.Rot.GetForwardVector();
	const FVector Up = B.Rot.GetUpVector();
	const FVector Vel = B.Vel;
	// pure pursuit in the *velocity* frame, so body slip angle does not bias the target
	const float SpeedAbs = Vel.Size();
	if (SpeedAbs > 5.f && V > 0.f) Fwd = (Vel - Up * FVector::DotProduct(Vel, Up)).GetSafeNormal();
	// Unreal is left-handed: forward x up points left
	const FVector Left = FVector::CrossProduct(Fwd, Up);
	Target -= B.Pos;
	const float LocalX = FVector::DotProduct(Target, Left);
	const float LocalZ = FVector::DotProduct(Target, Fwd);
	const float DistSq = FMath::Max(LocalX * LocalX + LocalZ * LocalZ, 1.f);
	const float DesiredCurvature = 2.f * LocalX / DistSq;
	const FVehicleSpec& Spec = Car->Spec();
	const float Wheelbase = Spec.FrontAxleX - Spec.RearAxleX;
	const float FeedForward = FMath::Atan(Wheelbase * DesiredCurvature);

	// integral loop on path curvature: finds the extra lock the tires' slip requires.
	// (A positive angular velocity about up turns Unreal's +X toward +Y, i.e. to the right,
	// so the leftward yaw rate is its negative.)
	const float YawRateLeft = -FVector::DotProduct(B.AngVel, Up);
	const float ActualCurvature = YawRateLeft / FMath::Max(SpeedAbs, 5.f);
	if (SpeedAbs > 8.f) SteerIntegral += CurvatureGain * (DesiredCurvature - ActualCurvature) * Dt;
	else SteerIntegral *= 1.f - FMath::Min(1.f, 2.f * Dt);
	const float MaxSteer = Spec.MaxSteerAngle(V);
	SteerIntegral = FMath::Clamp(SteerIntegral, -MaxSteer, MaxSteer);
	// lane-keeping damping (d grows outwards = to the right -> steer left, i.e. positive)
	float LateralCorrection = LateralGain * (D - IntendedNow) + LateralDamping * Car->LateralRate;
	LateralCorrection = FMath::Clamp(LateralCorrection, -0.4f * MaxSteer, 0.4f * MaxSteer);
	const float Delta = FeedForward + SteerIntegral + LateralCorrection;
	const float SteerTarget = FMath::Clamp(Delta / MaxSteer, -1.f, 1.f);
	const float SteerRate = 4.f * Dt;
	SteerCmd += FMath::Clamp(SteerTarget - SteerCmd, -SteerRate, SteerRate);
	Car->Input().Steer = SteerCmd;
}

void FAIDriver::ApplySpeed(float VTarget, float S, float D, float Intended)
{
	const FVehicleSim& Phys = *Car->Sim;
	FVehicleInput& In = Car->Input();
	const float V = Phys.GetForwardSpeed();
	const float Err = VTarget - V;
	float Throttle, Brake;
	if (Err > 0.f)
	{
		Throttle = FMath::Clamp(0.5f + Err * 0.4f, 0.f, 1.f);
		Brake = 0.f;
	}
	else if (Err > -2.f)
	{
		Throttle = FMath::Clamp(0.45f + Err * 0.25f, 0.f, 1.f);
		Brake = 0.f;
	}
	else
	{
		Throttle = 0.f;
		Brake = FMath::Clamp(-Err * 0.12f, 0.f, 1.f);
		if (FMath::Abs(V) < 1.5f) Brake = FMath::Min(Brake, HoldBrake);   // never engage reverse
	}
	// oversteer: the rear is sliding more than the front -> feed throttle in gently
	const float RearSlip = Phys.GetRearSlipAngle();
	const float FrontSlip = Phys.GetFrontSlipAngle();
	if (RearSlip > 0.05f && RearSlip > FrontSlip) Throttle *= FMath::Clamp(1.f - (RearSlip - 0.05f) * 6.f, 0.35f, 1.f);
	if (Phys.GetRearSlipRatio() > 0.12f) Throttle *= 0.6f;
	if (Track->InTurn(S) && RearSlip > 0.08f) Brake = 0.f;
	// understeer management: pushing wide of the intended line or front tires at the limit
	if (Track->InTurn(S) || Track->DistanceToNextTurn(S) < 60.f)
	{
		const float Wide = D - Intended;
		if (Wide > 0.5f) Throttle *= FMath::Clamp(1.f - (Wide - 0.5f) * 0.7f, 0.15f, 1.f);
		if (FrontSlip > 0.085f) Throttle *= FMath::Clamp(1.f - (FrontSlip - 0.085f) * 12.f, 0.3f, 1.f);
	}
	In.Throttle = Throttle;
	In.Brake = Brake;
}

// ================================================================================ pit stops

void FAIDriver::RequestPit()
{
	if (!Pit || PitPhase != EPitPhase::None) return;
	PitPhase = EPitPhase::In;
	PassTarget = nullptr;
}

float FAIDriver::BoxCenter() const
{
	return PitLane::StallStart(Pit->GetStall()) + PitLane::StallLength / 2.f;
}

float FAIDriver::PitPathD(float Sq) const
{
	const float R = PitLane::Rel(Sq);
	if (PitPhase == EPitPhase::In)
	{
		const float BoxC = BoxCenter();
		if (R < PitCommitRel) return Line->Offset(Sq);
		if (R < PitMoveEndRel) return Lerp(Smooth((R - PitCommitRel) / (PitMoveEndRel - PitCommitRel)), Line->Offset(Sq), EntryD);
		if (R < PitLane::Start) return EntryD;
		if (R < PitLane::WallStart) return Lerp(Smooth((R - PitLane::Start) / (PitLane::WallStart - PitLane::Start)), EntryD, FastD);
		return Lerp(Smooth((R - (BoxC - 26.f)) / 24.f), FastD, BoxD);
	}
	// leaving the box, down the fast lane and along the pit exit lane through turns 1 and 2
	if (R < PitLane::WallEnd) return Lerp(Smooth((R - StopRel - 1.f) / 23.f), BoxD, FastD);
	const float Rq = FMath::Min(R, PitLane::LaneEnd);
	const float Sq2 = Sq + (Rq - R);
	const float Outer = PitLane::LaneOuterAt(Sq2);
	const float Inner = FMath::Max(PitLane::LaneInnerAt(Sq2), PitLane::LaneInner);
	return 0.5f * (Outer + Inner);
}

void FAIDriver::UpdatePit(float Dt, const TArray<FCarCore*>& Field)
{
	FVehicleInput& In = Car->Input();
	const float S = Car->TrackS(), D = Car->TrackD();
	const float V = Car->ForwardSpeed();
	const float R = PitLane::Rel(S);
	StuckTime = 0.f;

	// ---- phase transitions ----
	if (PitPhase == EPitPhase::In && Pit->IsServicing()) PitPhase = EPitPhase::Service;
	if (PitPhase == EPitPhase::Service && !Pit->IsServicing())
	{
		PitPhase = EPitPhase::Out;
		StopRel = R;
	}
	const bool bMissedEntry = R > -760.f && R < -480.f && D > PitLane::LaneDividerD + 0.5f;
	// squeezed onto the race-track side of the pit wall
	const bool bWrongSide = R >= PitLane::WallStart - 10.f && R < PitLane::WallStart + 200.f && D > FTrackSpline::InnerWall - 0.5f;
	if (PitPhase == EPitPhase::In && (bMissedEntry || bWrongSide))
	{
		ResetLane();   // missed the pit entry: stay out, try again next lap
		return;
	}
	// stuck in the pit lane (blocked, spun): give up the stop and let the recovery tow us out
	PitStuck = PitPhase != EPitPhase::Service && FMath::Abs(V) < 1.f && !FPitStop::InBox(Pit->GetStall(), S, D) ? PitStuck + Dt : 0.f;
	if (PitStuck > 10.f)
	{
		PitStuck = 0.f;
		ResetLane();
		StuckTime = 10.f;
		return;
	}
	if (PitPhase == EPitPhase::In && R > BoxCenter() + 6.f)   // overshot the box: drive on out
	{
		PitPhase = EPitPhase::Out;
		StopRel = R;
	}
	if (PitPhase == EPitPhase::Out && R > PitLane::LaneEnd)
	{
		ResetLane();            // back to racing, but merge only when the line is clear
		bMergeHold = true;
		return;
	}
	if (PitPhase == EPitPhase::Service)   // on the jacks
	{
		In.Throttle = 0.f;
		In.Brake = 1.f;
		SteerCmd *= 0.9f;
		In.Steer = SteerCmd;
		return;
	}

	// ---- path ----
	const float Lookahead = FMath::Clamp(4.f + 0.5f * FMath::Abs(V), 7.f, 60.f);
	float DNow = PitPathD(S), DAhead = PitPathD(S + Lookahead);
	float YieldCap = TNumericLimits<float>::Max();
	if (PitPhase == EPitPhase::In && R < PitLane::Start - 100.f)
	{
		// never move down onto a car on our inside: hold our line and drop in behind it
		for (const FCarCore* C : Field)
		{
			if (C == Car) continue;
			const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
			if (FMath::Abs(Ds) < 9.f && C->TrackD() < D && D - C->TrackD() < CarWidthClearance + 0.8f)
			{
				const float Floor = C->TrackD() + CarWidthClearance + 0.3f;
				DNow = FMath::Max(DNow, Floor);
				DAhead = FMath::Max(DAhead, Floor);
				if (Ds > -4.f) YieldCap = FMath::Min(YieldCap, C->ForwardSpeed() - 2.f);
			}
		}
	}
	Steer(Dt, D, V, S + Lookahead, DAhead, DNow);

	// ---- speed ----
	float VTarget = Profile.At(S + 4.f);
	if (D < FTrackSpline::RoadInner && Track->InTurn(S)) VTarget *= 0.85f;   // flat apron: less grip
	const bool bOnPitRoad = R >= PitLane::WallStart && R <= PitLane::WallEnd;
	if (PitPhase == EPitPhase::In)
	{
		const float ToLimit = FMath::Max(0.f, PitLane::WallStart - R);
		VTarget = FMath::Min(VTarget, FMath::Sqrt(PitSpeed * PitSpeed + 2.f * PitDecel * ToLimit));
		const float ToStop = BoxCenter() - 2.f - R;
		VTarget = FMath::Min(VTarget, 1.0f + FMath::Sqrt(2.f * 3.f * FMath::Max(0.f, ToStop)));
	}
	else
	{
		if (bOnPitRoad) VTarget = FMath::Min(VTarget, PitSpeed);
		else if (R < PitLane::BlendStart) VTarget = FMath::Min(VTarget, 45.f);
	}
	VTarget = FMath::Min(VTarget, FMath::Max(YieldCap, 5.f));
	// follow traffic on the pit path
	for (const FCarCore* C : Field)
	{
		if (C == Car) continue;
		const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
		if (Ds <= 0.f || Ds > 90.f) continue;
		const float PathThere = PitPathD(C->TrackS());
		if (FMath::Abs(C->TrackD() - PathThere) > 2.6f && FMath::Abs(C->TrackD() - D) > 2.6f) continue;
		VTarget = FMath::Min(VTarget, C->ForwardSpeed() + FMath::Sqrt(2.f * 2.5f * FMath::Max(Ds - 13.f, 0.f)) - 1.f);
	}
	// pulling out of the box: wait for cars coming down the fast lane
	bool bWaiting = false;
	if (PitPhase == EPitPhase::Out && R < StopRel + 4.f)
	{
		for (const FCarCore* C : Field)
		{
			if (C == Car) continue;
			const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
			if (Ds > -70.f && Ds < 6.f && C->TrackD() > PitLane::FastLaneInner - 0.5f && C->TrackD() < FTrackSpline::InnerWall && C->Speed() > 2.f)
			{
				bWaiting = true;
			}
		}
	}
	ApplySpeed(FMath::Max(VTarget, 0.f), S, D, DNow);
	if (PitPhase == EPitPhase::Out && R < StopRel + 25.f) In.Throttle = FMath::Min(In.Throttle, 0.5f);
	if (bWaiting || (PitPhase == EPitPhase::In && BoxCenter() - R < 3.f && FMath::Abs(R - BoxCenter()) < 10.f))
	{
		In.Throttle = 0.f;
		In.Brake = FMath::Abs(V) < 1.5f ? HoldBrake : 1.f;
	}
}

float FAIDriver::ChoosePassLane(const FCarCore* Blocker, const TArray<FCarCore*>& Field) const
{
	float Inside = Blocker->TrackD() - PassOffset;
	float Outside = Blocker->TrackD() + PassOffset;
	// a car moving across the track: go behind it, i.e. to the side it is leaving
	const float Drift = Blocker->LateralRate;
	if (FMath::Abs(Drift) > 1.0f)
	{
		if (Drift > 0) Inside = FMath::Min(Inside, Blocker->TrackD() - PassOffset - 1.f);
		else Outside = FMath::Max(Outside, Blocker->TrackD() + PassOffset + 1.f);
		const bool bPreferInside = Drift > 0;
		const float First = bPreferInside ? Inside : Outside, Second = bPreferInside ? Outside : Inside;
		if (First >= DMin && First <= DMax && !LaneBlocked(First, Blocker, Field, -12.f, 20.f)) return First;
		if (Second >= DMin && Second <= DMax && !LaneBlocked(Second, Blocker, Field, -12.f, 20.f)) return Second;
		return NAN;
	}
	const bool bInsideOk = Inside >= DMin && !LaneBlocked(Inside, Blocker, Field, -12.f, 20.f);
	const bool bOutsideOk = Outside <= DMax && !LaneBlocked(Outside, Blocker, Field, -12.f, 20.f);
	if (bInsideOk && bOutsideOk)
	{
		const float S = Car->TrackS();
		const bool bTurnSoon = Track->DistanceToNextTurn(S) < 350.f || Track->InTurn(S);
		if (bTurnSoon) return Inside;
		return FMath::Abs(Inside - Car->TrackD()) <= FMath::Abs(Outside - Car->TrackD()) ? Inside : Outside;
	}
	if (bInsideOk) return Inside;
	if (bOutsideOk) return Outside;
	return NAN;
}

bool FAIDriver::LaneBlocked(float Lane, const FCarCore* Ignore, const TArray<FCarCore*>& Field, float DsMin, float DsMax) const
{
	const float S = Car->TrackS();
	for (const FCarCore* C : Field)
	{
		if (C == Car || C == Ignore) continue;
		const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
		const float Clearance = C->bPlayer ? CarWidthClearance + 0.6f : CarWidthClearance;
		if (Ds > DsMin && Ds < DsMax && FMath::Abs(C->TrackD() - Lane) < Clearance) return true;
	}
	return false;
}
