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
 *  - Pack awareness: passing on a free side, keeping clear of cars alongside, swerving when a
 *    collision is imminent, a wider berth for the human player; the rolling-start formation;
 *    pit stops.
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
	void SetDownforceLevel(float Level);
	bool IsPassing() const { return PassTarget != nullptr; }
	void ResetForRace();
	void ResetLane();
	bool NeedsRecovery() const { return StuckTime > 3.5f; }
	void Update(float Dt, const TArray<FCarCore*>& Field);

	void SetPit(FPitStop* InPit) { Pit = InPit; }
	bool IsPitting() const { return PitPhase != EPitPhase::None; }
	void RequestPit();

private:
	enum class EPitPhase : uint8 { None, In, Service, Out };

	bool RacingLineClear(const TArray<FCarCore*>& Field, float S, float D) const;
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
	float PassLane = 0.f;
	float LaneD = 0.f;
	float LaneWeight = 0.f;
	float SteerCmd = 0.f;
	float StuckTime = 0.f;
	float RaceTime = 0.f;
	float SteerIntegral = 0.f;
	bool bReadyToRelease = false;
	bool bFormationDone = false;
	float GridLane = 0.f;

	EPitPhase PitPhase = EPitPhase::None;
	float PitStuck = 0.f;
	FPitStop* Pit = nullptr;
	float StopRel = 0.f;
	bool bMergeHold = false;
};
