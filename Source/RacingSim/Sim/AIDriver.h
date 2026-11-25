#pragma once

#include "CoreMinimal.h"
#include "Sim/RaceLogic.h"

/**
 * Port of com.racingsim.ai.AIDriver: throttle / brake / steering for a car that runs on exactly
 * the same vehicle model as the player.
 *  - Steering: pure pursuit toward a look-ahead point on a dynamic lateral target (racing line
 *    blended with a passing / avoidance lane), plus an integral loop on actual vs. desired path
 *    curvature that compensates for tire slip.
 *  - Speed: the pre-computed corner speed profile plus traffic speed caps.
 *  - Race craft, oval style: single file on the racing line; passes are set up in the tow and
 *    made with a late pull-out on the straights, only where they can be finished before the next
 *    turn and never beside a car that already has company (no three wide). A pass that stops
 *    gaining, or is not alongside by the turn, is abandoned and the car tucks back in. Cars
 *    rejoin the line only once clear of the car alongside, lift and brake behind slower traffic,
 *    and swerve only when braking cannot prevent a hit.
 *  - Also: keeping clear of cars alongside, a little more room for the human player, the
 *    rolling-start formation and pit stops.
 *  - On a road course the same race craft with the turns' own directions (the inside of a
 *    right-hander is the right), braking zones ahead of the turns, the speed profile of the
 *    lane in use when off the racing line, and a standing start.
 *  The race craft departs from the Java AIDriver, which passed on any free side and swerved
 *  rather than braking; the steering, speed control and pit stops are still its port.
 */
class RACINGSIM_API FAIDriver
{
public:
	/** Pit road speed limit (60 mph). */
	static constexpr float PitSpeed = 26.8f;
	/** Where the move down to the pit entry lane starts (rel s, turn 4). */
	static constexpr float PitCommitRel = -1150.f;

	FAIDriver(FCarCore* InCar, const FRacingLine* InLine, const FSpeedProfile& InProfile, float InAggression);

	FCarCore* GetCar() const { return Car; }
	/** Driver type, 0 = rookie (textbook passes only) .. 1 = veteran (passes in the chutes and turns). */
	float GetAggression() const { return Aggression; }
	static const TCHAR* TypeName(float Aggression) { return Aggression < 0.34f ? TEXT("rookie") : Aggression < 0.67f ? TEXT("regular") : TEXT("veteran"); }
	/** Wing level (0..Steps) this driver runs: rookies want a stable car, veterans a fast one. */
	int32 PreferredWing(int32 Steps, int32 Jitter) const;
	void SetDownforceLevel(float Level);
	bool IsPassing() const { return PassTarget != nullptr; }
	void ResetForRace();
	void ResetLane();
	bool NeedsRecovery() const { return StuckTime > 3.5f; }
	void Update(float Dt, const TArray<FCarCore*>& Field);

	/** Race-craft statistics over every AI car, racing laps only (SimCheck reports them). */
	struct FStats
	{
		int64 RacingTicks = 0;      // AI car-ticks racing (formation, pits and merging excluded)
		int64 OffLineTicks = 0;     // ... more than 1.5 m off the racing line
		int64 ThreeWideTicks = 0;   // ... overlapped with two or more other cars
		int64 LateTicks = 0, LateThreeWide = 0;
		// on the straights: speed held below target by following, setting up a run, the run,
		// backing out of a pass, being squeezed
		int64 StraightTicks = 0, StraightCapped[5] = {};
		int64 TurnTicks = 0, TurnCapped[5] = {};   // the same in and approaching the turns
		float CleanAirTop = 0.f;   // highest speed (m/s) with no tow at all
		// completed passes by driver type (rookie, regular, veteran), and those begun in a turn or
		// a short stretch (a chute, the end of a straight)
		int32 PassesByType[3] = {}, TightPassesByType[3] = {};
		// ticks braking hard in a turn: speed profile, following, setting up, draft run, backing out, squeezed
		int64 TurnBrakeTicks[6] = {};   // the same after 100 s (the rolling start long gone)
		int32 PassStarts = 0, DraftPassStarts = 0;
		int32 PassesDone = 0, DraftPassesDone = 0;
		int32 PassesAborted = 0, EmergencyMoves = 0;
		int32 AbortStall = 0, AbortLate = 0, AbortYield = 0, AbortBlocked = 0;
		double ClosingAtStart = 0.0;
		// why the off-line ticks were off the line
		int64 OffPassing = 0, OffEmergency = 0, OffHolding = 0, OffAvoiding = 0, OffReturning = 0, OffTracking = 0;
		// line-following error (d - racing line, m) by 50 m of track, while nothing asks for another lane
		double TrackErrSum[160] = {};
		int64 TrackErrCount[160] = {};
	};
	static FStats Stats;

	void SetPit(FPitStop* InPit) { Pit = InPit; }
	bool IsPitting() const { return PitPhase != EPitPhase::None; }
	void RequestPit();

private:
	enum class EPitPhase : uint8 { None, In, Service, Out };

	/** No car in the corridor between d and the racing line, from Behind metres back to Ahead metres ahead. */
	bool RacingLineClear(const TArray<FCarCore*>& Field, float S, float D, float Behind = 30.f, float Ahead = 40.f) const;
	bool HasCompany(const FCarCore* Other, const TArray<FCarCore*>& Field) const;
	void Steer(float Dt, float D, float V, float SAhead, float DAhead, float IntendedNow);
	void ApplySpeed(float VTarget, float S, float D, float Intended);
	float ChoosePassLane(const FCarCore* Blocker, const TArray<FCarCore*>& Field) const;
	bool LaneBlocked(float Lane, const FCarCore* Ignore, const TArray<FCarCore*>& Field, float DsMin, float DsMax) const;
	float BoxCenter() const;
	float PitPathD(float Sq) const;
	void UpdatePit(float Dt, const TArray<FCarCore*>& Field);

	FCarCore* Car;
	const FTrackSpline* Track;
	const FRacingLine* Line;
	FSpeedProfile Profile;
	float Aggression;

	const FCarCore* PassTarget = nullptr;
	bool bPassYield = false;
	bool bPassFromDraft = false;
	bool bEmergencyMove = false;
	float PassCooldown = 0.f;
	float PassStall = 0.f;

	// Personality, from Aggression (rookie 0 .. veteran 1)
	float SlingClosing = 2.5f;   // closing speed (m/s) needed to pull out from the tow
	float RoomMargin = 40.f;     // a pass must be finished this far (m) before the turn
	float YieldDs = -3.f;        // on the outside at the turn, back out unless at least this far ahead (m, negative = ahead)
	float StallLimit = 0.f;      // give a pass up when gaining less than this (m/s)
	float RunGap = 7.f;          // how close (m) a draft run closes up
	bool bTurnPasser = false;    // veterans: passes in the chutes and turns, side by side through a turn
	bool bPassTight = false;     // statistics: the current pass began in a turn or a short stretch
	float TurnExitDistance(float S) const;
	/** How far before a turn counts as "the turn is near" at speed V. */
	float TurnApproach(float V) const;
	/** For passing: in a turn worth racing for, and the distance to the next one (a road course:
	 *  to the next braking zone). */
	bool PassTurn(float S) const;
	float PassTurnDist(float S) const;
	mutable float CachedBrakeS = -1.f, CachedBrakeDist = 0.f;
	/** The lanes this car may use (d of its center). */
	float DMin = -5.6f, DMax = 5.7f;
	float PassLane = 0.f;
	float LaneD = 0.f;
	float LaneWeight = 0.f;
	float SteerCmd = 0.f;
	float StuckTime = 0.f;
	float RaceTime = 0.f;
	float SteerIntegral = 0.f;
	bool bReadyToRelease = false;
	float ReleaseTime = 0.f;
	bool bFormationDone = false;
	float GridLane = 0.f;

	EPitPhase PitPhase = EPitPhase::None;
	float PitStuck = 0.f;
	FPitStop* Pit = nullptr;
	float StopRel = 0.f;
	bool bMergeHold = false;
};
