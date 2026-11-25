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
	/** The human player is less predictable: plan gentler braking and keep a little more room,
	 *  but stay close enough (15 m) to draft them. */
	constexpr float HumanFollowDecel = 2.5f, HumanFollowGap = 15.f;
	/** Swerve only when a hit is this close and braking would need more than EmergencyDecel. */
	constexpr float EmergencyTtc = 1.5f;
	constexpr float EmergencyDecel = 8.f;
	/** Tow (drag reduction) at which a follower starts setting up a slingshot pass. */
	constexpr float DraftForPass = 0.12f;

	constexpr float LaneRate = 2.2f;
	constexpr float WeightRate = 0.9f;
	/** Like a real rolling start, the field holds station until it has strung out. */
	constexpr float NoPassingTime = 14.f;
	/** End of turn 2 relative to the start/finish line (where the opening-lap formation ends). */
	constexpr float Turn2ExitRel = FTrackSpline::Straight / 2 + 2 * FTrackSpline::Turn + FTrackSpline::Chute + 40.f;
	constexpr float CurvatureGain = 18.f;
	float LateralGain = 0.0025f, LateralDamping = 0.006f;
	float LookBase = 8.f, LookPerSpeed = 0.40f;
	/** Closing speed (m/s) the tow must have built before pulling out to pass. */
	float SlingshotClosing = 2.5f;
	/** A pass not yet alongside is given up after 1.5 s gaining less than this (m/s): only when
	 *  it is losing ground; a slow pass may run on down the straight. */
	float StallGain = 0.f;
	/** Pull-out distance behind the car being passed: base + 8 m x aggression. */
	float PullOutBase = 12.f;
	/** Following distance (m) on a draft run, closing up to slingshot past (+2 m behind the human). */
	float DraftRunGap = 7.f;
	/** Following distance (m) through the turns while setting up a run, less 4 m x aggression; at or
	 *  under the usual following distance it does nothing. Off: hanging back cost the whole train
	 *  its exit speed and let the leader escape. */
	float SetupGap = 0.f;
	/** Braking (m/s^2) planned behind the car we are running up on / passing (the brakes manage ~13). */
	float RunDecel = 6.f, PassDecel = 8.f;
	/** On a run, pull out once within DraftRunGap + this (m) with any closing speed. */
	float RunPullOut = 1.5f;
	/** Closing speed (m/s) assumed for a pass from the tow when judging if it fits before the turn. */
	float RoomClosing = 3.f;
	/** Corner speed given up per unit of draft factor (dirty air). */
	float DirtyAirSlowdown = 0.08f;
	/** Tuning overrides (-AiLatGain= -AiLatDamp= -AiLookBase= -AiLookK=), read once. */
	void ReadTuning()
	{
		static bool bRead = false;
		if (bRead) return;
		bRead = true;
		const TCHAR* Cmd = FCommandLine::Get();
		FParse::Value(Cmd, TEXT("AiLatGain="), LateralGain);
		FParse::Value(Cmd, TEXT("AiLatDamp="), LateralDamping);
		FParse::Value(Cmd, TEXT("AiLookBase="), LookBase);
		FParse::Value(Cmd, TEXT("AiLookK="), LookPerSpeed);
		FParse::Value(Cmd, TEXT("AiSling="), SlingshotClosing);
		FParse::Value(Cmd, TEXT("AiStallGain="), StallGain);
		FParse::Value(Cmd, TEXT("AiPullOut="), PullOutBase);
		FParse::Value(Cmd, TEXT("AiDirtyAir="), DirtyAirSlowdown);
		FParse::Value(Cmd, TEXT("AiRunGap="), DraftRunGap);
		FParse::Value(Cmd, TEXT("AiSetupGap="), SetupGap);
		FParse::Value(Cmd, TEXT("AiRunPull="), RunPullOut);
		FParse::Value(Cmd, TEXT("AiRunDecel="), RunDecel);
		FParse::Value(Cmd, TEXT("AiRoomClose="), RoomClosing);
	}

	// pit path
	constexpr float EntryD = -12.3f, FastD = -21.9f, BoxD = -28.5f;
	constexpr float PitMoveEndRel = -950.f;
	constexpr float PitDecel = 4.5f;
	/** Brake used to hold a stopped car without the gearbox engaging reverse (threshold 0.5). */
	constexpr float HoldBrake = 0.45f;

	float Smooth(float X) { return FTrackSpline::Smooth(X); }
	float Lerp(float T, float A, float B) { return A + (B - A) * T; }   // jME interpolateLinear(t, a, b)
}

FAIDriver::FStats FAIDriver::Stats;

FAIDriver::FAIDriver(FCarCore* InCar, const FRacingLine* InLine, const FSpeedProfile& InProfile, float InAggression)
	: Car(InCar), Track(&InLine->GetTrack()), Line(InLine), Profile(InProfile), Aggression(FMath::Clamp(InAggression, 0.f, 1.f))
{
	ReadTuning();
	// Rookie (0): textbook passes only, from the tow with plenty of closing speed and done well
	// before the turn, given up as soon as they stall. Veteran (1): pulls out with less, will
	// pass in the short chutes and into the turns and go side by side through them.
	const float A = Aggression;
	SlingClosing = SlingshotClosing + (0.5f - A) * 2.f;   // 3.5 .. 1.5 m/s
	RoomMargin = 120.f - 80.f * A;                          // 120 .. 40 m
	YieldDs = -6.f + 6.f * A;                               // well clear .. merely alongside
	StallLimit = StallGain + 0.4f * (1.f - A);              // 0.4 .. 0 m/s
	RunGap = DraftRunGap + 2.f * (1.f - A);                 // 9 .. 7 m
	bTurnPasser = A >= 0.67f;
}

int32 FAIDriver::PreferredWing(int32 Steps, int32 Jitter) const
{
	// rookies run more wing (stable, slower), veterans less (fast, loose): 5 .. 1 of 10, +-1. (A
	// rookie on 8-9 was ~1.5 s a lap off the pace: a full field swarmed them, three wide.)
	return FMath::Clamp(FMath::RoundToInt((0.5f - 0.4f * Aggression) * Steps) + Jitter, 0, Steps);
}

float FAIDriver::TurnExitDistance(float S) const
{
	// distance to the exit of the turn we are in, or else of the next one
	for (float Start : Track->TurnStarts())
	{
		const float Into = FTrackSpline::DeltaS(Start, S);
		if (Into >= 0.f && Into < FTrackSpline::Turn) return FTrackSpline::Turn - Into;
	}
	return Track->DistanceToNextTurn(S) + FTrackSpline::Turn;
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
	PassCooldown = 0.f;
	PassStall = 0.f;
	LaneD = Car->TrackD();
	LaneWeight = 1.f;
	StuckTime = 0.f;
	SteerIntegral = 0.f;
	SteerCmd = 0.f;
	PitPhase = EPitPhase::None;
}

bool FAIDriver::HasCompany(const FCarCore* Other, const TArray<FCarCore*>& Field) const
{
	// someone alongside that car already: passing it now would make it three wide
	for (const FCarCore* C : Field)
	{
		if (C == Car || C == Other || C->TrackD() < DMin - 1.5f) continue;
		if (FMath::Abs(FTrackSpline::DeltaS(Other->TrackS(), C->TrackS())) < 9.f) return true;
	}
	return false;
}

bool FAIDriver::RacingLineClear(const TArray<FCarCore*>& Field, float S, float D, float Behind, float Ahead) const
{
	const float LineD = Line->Offset(S);
	if (FMath::Abs(D - LineD) < 1.2f) return true;             // already on the line
	for (const FCarCore* C : Field)
	{
		if (C == Car) continue;
		const float Ds = FTrackSpline::DeltaS(S, C->TrackS());
		if (Ds <= -Behind || Ds >= Ahead || FMath::Abs(C->TrackD() - D) <= 1.2f) continue;   // own lane: fine
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
	int32 CapCause = -1;   // statistics: 0 following, 1 setting up a run, 2 draft run, 3 backing out, 4 squeezed
	// Rolling start, Indianapolis style: hold the grid lane (three abreast) through the green
	// flag and turns 1 and 2; the field races freely from the back straight onwards.
	const float RelS = PitLane::Rel(S);
	if (!bReadyToRelease && RaceTime > 3.f && RelS > Turn2ExitRel && RelS < Turn2ExitRel + 400.f)
	{
		bReadyToRelease = true;
		ReleaseTime = RaceTime;
	}
	// Past turn 2 the whole field races, still in its lanes until each car can merge into line
	// (the pole sitter used to escape while the rows beside and behind it waited at formation
	// pace); 20 s after that everyone is racing.
	if (!bFormationDone && (RaceTime > 70.f || (bReadyToRelease && (RacingLineClear(Field, S, D) || RaceTime > ReleaseTime + 20.f)))) bFormationDone = true;
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

	// Passing, oval style: run nose to tail on the racing line, build speed in the leader's tow,
	// then pull out late and slingshot by on the straight. A pass starts only where it can be
	// finished before the next turn, never beside a car that already has company (no three
	// wide), and a car that is simply faster may go by without a tow.
	RaceTime += Dt;
	PassCooldown = FMath::Max(0.f, PassCooldown - Dt);
	const bool bPassingAllowed = bFormationDone && !bMergeHold && RaceTime > NoPassingTime && PassCooldown <= 0.f;
	const FCarCore* DraftRunTarget = nullptr;
	const FCarCore* SetupTarget = nullptr;
	// Every driver wants the place ahead. Through the turns, hang back a little (out of the dirty
	// air, still in the tow) to get a run at the car ahead on the next straight.
	if (bPassingAllowed && !PassTarget && Blocker && BlockerGap < 40.f && Track->InTurn(S)) SetupTarget = Blocker;
	// (veterans will also start one on the exit of a turn)
	const bool bTurnExit = Track->InTurn(S) && TurnExitDistance(S) < FTrackSpline::Turn / 2.f;
	if (bPassingAllowed && !PassTarget && Blocker && (!Track->InTurn(S) || (bTurnPasser && bTurnExit)))
	{
		const float Closing = V - Blocker->ForwardSpeed();
		const bool bTowing = Car->GetDraftFactor() > DraftForPass;
		// pull out late, from the tow, once the run has built real closing speed: out of the tow
		// that speed fades within seconds (12-20 m back; the human gets more room). Right on its
		// gearbox, any closing speed will do.
		const float PullOut = PullOutBase + (Blocker->bPlayer ? 4.f : 0.f) + 8.f * Aggression;
		const bool bSlingshot = bTowing && ((BlockerGap < PullOut && Closing > SlingClosing)
			|| (BlockerGap < RunGap + RunPullOut && Closing > 0.5f));
		// a pass on the human player needs a clear speed advantage, not just a tow
		const bool bFaster = BlockerGap < 30.f && Closing > (Blocker->bPlayer ? 2.5f : 1.5f);
		// Only start a pass that will be done in time (a run from the tow arrives at around
		// 3 m/s): before the next turn, with a margin a rookie makes generous; a veteran will
		// carry it side by side through the next turn instead (the short chutes, the turns).
		const float ClosingSpeed = FMath::Max(Closing, bTowing ? RoomClosing : 0.3f);
		const float PassDistance = (BlockerGap + 10.f) / ClosingSpeed * FMath::Max(V, 1.f);
		const float RoomLeft = bTurnPasser ? TurnExitDistance(S) - 40.f
			: (Track->InTurn(S) ? 0.f : Track->DistanceToNextTurn(S) - RoomMargin);
		const bool bRoom = PassDistance < RoomLeft;
		const float Lane = (bRoom && !HasCompany(Blocker, Field)) ? ChoosePassLane(Blocker, Field) : NAN;
		if (!FMath::IsNaN(Lane))
		{
			// A pass is on, but sitting at the usual following distance the tow builds no speed:
			// close right up in it (a "run"), and the slingshot follows once we are closing.
			if (bTowing && !bSlingshot && !bFaster && !Track->InTurn(S)) DraftRunTarget = Blocker;
			else if (bSlingshot || bFaster)
			{
				PassTarget = Blocker;
				bPassYield = false;
				PassLane = Lane;
				bPassFromDraft = bTowing;
				bEmergencyMove = false;
				// a pass begun in a turn or with less than a long straight's worth of road left
				bPassTight = Track->InTurn(S) || Track->DistanceToNextTurn(S) < 300.f;
				Stats.PassStarts++;
				Stats.ClosingAtStart += Closing;
				if (bTowing) Stats.DraftPassStarts++;
			}
		}
	}
	if (PassTarget)
	{
		const float Ds = FTrackSpline::DeltaS(S, PassTarget->TrackS());
		const bool bOutside = PassLane > PassTarget->TrackD();
		const bool bTurnNear = Track->InTurn(S) || Track->DistanceToNextTurn(S) < 80.f;
		// not far enough ahead on the outside as the turn arrives (a rookie wants to be well clear,
		// a veteran stays out there once alongside): back out and tuck in behind
		// A rookie does not go side by side into a turn on either side: clear ahead, or back out.
		const bool bRookie = Aggression < 0.34f;
		if (!bEmergencyMove && (bOutside || bRookie) && bTurnNear && Ds > YieldDs) bPassYield = true;
		// an avoiding move ends once we have dropped back behind that car (or cleared it)
		const bool bAvoided = bEmergencyMove && Ds > 6.f && V < PassTarget->ForwardSpeed();
		// not alongside yet as the turn arrives: no dive-bomb on the inside, from anyone (a veteran
		// alongside by then may carry the pass side by side through the turn; only a veteran
		// keeps a pass going in a turn at all)
		const bool bTurnAhead = !Track->InTurn(S) && Track->DistanceToNextTurn(S) < 80.f;
		const bool bTooLate = !bEmergencyMove && Ds > 4.f && (bTurnAhead || (Track->InTurn(S) && !bTurnPasser));
		// out of the tow and not gaining before even drawing alongside: give up and tuck back in
		// (a rookie as soon as it stalls, a veteran only once losing ground)
		const float Gain = V - PassTarget->ForwardSpeed();
		PassStall = (!bEmergencyMove && Ds > 3.f && Gain < StallLimit) ? PassStall + Dt : 0.f;
		if (Ds < -8.f || Ds > 70.f || (bPassYield && Ds > 10.f) || bTooLate || bAvoided || PassStall > 1.5f || LaneBlocked(PassLane, PassTarget, Field, -6.f, 8.f))
		{
			if (Ds >= -8.f) PassCooldown = 3.f;   // abandoned: settle in behind before trying again
			if (!bEmergencyMove)
			{
				if (Ds < -8.f)
				{
					Stats.PassesDone++;
					if (bPassFromDraft) Stats.DraftPassesDone++;
					const int32 Type = Aggression < 0.34f ? 0 : Aggression < 0.67f ? 1 : 2;
					Stats.PassesByType[Type]++;
					// finished inside a turn: carried side by side through it
					if (Track->InTurn(S)) Stats.TightPassesByType[Type]++;
				}
				else
				{
					Stats.PassesAborted++;
					if (PassStall > 1.5f) Stats.AbortStall++;
					else if (bTooLate) Stats.AbortLate++;
					else if (bPassYield) Stats.AbortYield++;
					else Stats.AbortBlocked++;
				}
			}
			PassTarget = nullptr;   // completed, abandoned or the lane closed
			bPassYield = false;
			PassStall = 0.f;
		}
		else
		{
			DesiredLane = PassLane;
			WeightTarget = 1.f;
			// backing out: lift (never more than 1.5 m/s under our speed) until it is ahead of us,
			// rather than braking hard in the turn
			const float YieldCap = FMath::Max(PassTarget->ForwardSpeed() - 2.f, V - 1.5f);
			if (bPassYield && YieldCap < SpeedCap)
			{
				SpeedCap = YieldCap;
				CapCause = 3;
			}
		}
	}

	// Back to the racing line after a pass or an avoiding move only once it is clear: never chop
	// across a car we are alongside or have only just passed. (Merging in behind a car further up
	// the road is fine: the following distance takes care of it.)
	bool bHolding = false, bAvoiding = false;
	if (!PassTarget && bFormationDone && !bMergeHold && LaneWeight > 0.05f && !RacingLineClear(Field, S, D, 8.f, 8.f))
	{
		bHolding = true;
		DesiredLane = LaneD;
		WeightTarget = 1.f;
	}

	// Never run into the back of a car that is in our path, now or in the lane we are steering
	// into. The human player gets a much wider berth.
	const FCarCore* Threat = nullptr;
	float ThreatTtc = TNumericLimits<float>::Max();
	float ThreatDecel = 0.f;
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
		// On a draft run, close right up on the car we mean to pass, planning firmer braking so the
		// tow can build closing speed; once pulling out to pass it, that car only limits us as a
		// collision risk (otherwise the run dies while we move across). Setting a run up in the
		// turn: hang back.
		const bool bPassing = C == PassTarget && !bEmergencyMove;
		const float Gap = bPassing ? 2.f
			: C == DraftRunTarget ? (bHuman ? RunGap + 2.f : RunGap)
			: C == SetupTarget ? FMath::Max(SetupGap - 4.f * Aggression, bHuman ? HumanFollowGap : 12.f)
			: (bHuman ? HumanFollowGap : 12.f) - 3.f * Aggression;
		const float PlanDecel = bPassing ? PassDecel : C == DraftRunTarget ? RunDecel : Decel;
		const float Margin = FMath::Max(Ds - Gap, 0.f);
		const float Cap = Vc + FMath::Sqrt(2.f * PlanDecel * Margin) - (bHuman ? 1.5f : 0.5f);
		if (Cap < SpeedCap)
		{
			SpeedCap = Cap;
			CapCause = C == SetupTarget ? 1 : C == DraftRunTarget ? 2 : 0;
		}
		if (Closing > 0.5f)
		{
			const float Ttc = FMath::Max(Ds - 5.f, 0.f) / Closing;
			if (Ttc < ThreatTtc)
			{
				ThreatTtc = Ttc;
				Threat = C;
				ThreatDecel = Closing * Closing / (2.f * FMath::Max(Ds - 5.f, 0.5f));
			}
		}
	}
	// emergency: about to hit a car we are still lined up with, faster than braking can fix ->
	// take any free lane. Otherwise lift and brake behind it (the speed cap above).
	// Beside a car that already has company the only free lane makes it three wide: brake.
	if (Threat && ThreatTtc < EmergencyTtc && ThreatDecel > EmergencyDecel && PassTarget != Threat && !HasCompany(Threat, Field))
	{
		const float Lane = ChoosePassLane(Threat, Field);
		if (!FMath::IsNaN(Lane))
		{
			PassTarget = Threat;
			bPassYield = false;
			PassLane = Lane;
			DesiredLane = Lane;
			WeightTarget = 1.f;
			bEmergencyMove = true;
			if (bFormationDone) Stats.EmergencyMoves++;
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
			bAvoiding = true;
			if (Dd > 0) DesiredLane = FMath::Min(DesiredLane, C->TrackD() - Clearance);
			else DesiredLane = FMath::Max(DesiredLane, C->TrackD() + Clearance);
			WeightTarget = 1.f;
			if (DesiredLane < DMin + 0.4f || DesiredLane > DMax - 0.4f)
			{
				if (V - 1.f < SpeedCap)
				{
					SpeedCap = V - 1.f;   // squeezed: lift, don't brake
					CapCause = 4;
				}
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
	const float Lookahead = FMath::Clamp(LookBase + LookPerSpeed * FMath::Abs(V), 12.f, 75.f);
	const float SAhead = S + Lookahead;
	const float DTarget = Lerp(LaneWeight, Line->Offset(SAhead), LaneD);
	const float IntendedNow = Lerp(LaneWeight, Line->Offset(S), LaneD);
	Steer(Dt, D, V, SAhead, DTarget, IntendedNow);

	// ---------------- speed control ----------------
	float VTarget = Profile.At(S + 4.f);
	const bool bCornering = Track->InTurn(S) || Track->DistanceToNextTurn(S) < 150.f;
	if (bCornering)
	{
		// dirty air costs front downforce (the aero model: up to 14% front, 5% rear, about 3% of
		// corner speed in a full tow), and an off-line lane is a tighter radius
		VTarget *= 1.f - DirtyAirSlowdown * Car->GetDraftFactor();
		if (LaneWeight > 0.5f) VTarget *= 0.985f;
		if (!bReadyToRelease) VTarget *= 0.90f;   // formation pace through turns 1 and 2
	}
	VTarget = FMath::Min(VTarget, SpeedCap);
	ApplySpeed(VTarget, S, D, IntendedNow);
	// statistics: hard braking in a turn, by what asked for it (0 the speed profile, 1+ CapCause)
	if (bFormationDone && Track->InTurn(S) && Car->Input().Brake > 0.3f) Stats.TurnBrakeTicks[VTarget < SpeedCap - 0.01f || CapCause < 0 ? 0 : CapCause + 1]++;

	// stuck detection (crashed / facing the wall)
	if (FMath::Abs(V) < 3.f) StuckTime += Dt; else StuckTime = 0.f;

	// statistics (racing laps only: the rolling start runs three abreast on purpose)
	if (bFormationDone && !bMergeHold)
	{
		Stats.RacingTicks++;
		if (Car->GetDraftFactor() < 0.01f) Stats.CleanAirTop = FMath::Max(Stats.CleanAirTop, V);
		if (!PassTarget && !bAvoiding && !bHolding && LaneWeight <= 0.05f)
		{
			const int32 Bin = FMath::Clamp((int32)(FTrackSpline::WrapS(S) / 50.f), 0, 95);
			Stats.TrackErrSum[Bin] += D - Line->Offset(S);
			Stats.TrackErrCount[Bin]++;
		}
		if (FMath::Abs(D - Line->Offset(S)) > 1.5f)
		{
			Stats.OffLineTicks++;
			if (PassTarget && bEmergencyMove) Stats.OffEmergency++;
			else if (PassTarget) Stats.OffPassing++;
			else if (bAvoiding) Stats.OffAvoiding++;
			else if (bHolding) Stats.OffHolding++;
			else if (LaneWeight > 0.05f) Stats.OffReturning++;
			else Stats.OffTracking++;
		}
		int32 Alongside = 0;
		for (const FCarCore* C : Field)
		{
			if (C != Car && FMath::Abs(FTrackSpline::DeltaS(S, C->TrackS())) < 5.f && C->TrackD() > DMin - 1.5f) Alongside++;
		}
		if (Alongside >= 2) Stats.ThreeWideTicks++;
		if (!Track->InTurn(S) && Track->DistanceToNextTurn(S) > 100.f)
		{
			Stats.StraightTicks++;
			if (CapCause >= 0 && SpeedCap < Profile.At(S + 4.f) - 0.5f) Stats.StraightCapped[CapCause]++;
		}
		else
		{
			Stats.TurnTicks++;
			if (CapCause >= 0 && SpeedCap < Profile.At(S + 4.f) - 0.5f) Stats.TurnCapped[CapCause]++;
		}
		if (RaceTime > 100.f)
		{
			Stats.LateTicks++;
			if (Alongside >= 2) Stats.LateThreeWide++;
		}
	}
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
