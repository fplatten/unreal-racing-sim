#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/AIDriver.h"
#include <atomic>
#include "RaceDirector.generated.h"

class ARaceCar;
class AOvalTrack;

/** Race state for the game thread (HUD, logs), copied from the physics thread every step. */
struct FRaceSnapshot
{
	struct FRow
	{
		FString Name;
		int32 CarIndex = 0;
		bool bPlayer = false;
		int32 Position = 0;
		int32 Lap = 1;
		int32 LapsCompleted = 0;
		float LastLap = NAN;
		float BestLap = NAN;
		bool bFinished = false;
		float FinishTime = NAN;
		int32 PitStops = 0;
		bool bStopsMet = true;
		float TotalDistance = 0.f;
		float CurrentLapTime = 0.f;
		/** AI driver type: 0 rookie, 1 regular, 2 veteran; -1 for the human player. */
		int32 DriverType = -1;
		/** Wing (downforce) level, 0..DownforceSteps. */
		int32 Wing = 0;
	};
	/** In race order. */
	TArray<FRow> Standings;
	float RaceTime = 0.f;
	int32 RaceLaps = 0;
	int32 RequiredStops = 0;
	bool bRunning = false;
	bool bLeaderFinished = false;
	int32 Recoveries = 0;
	int32 CarContacts = 0;
	/** Lap-tracker clock (s). */
	float Now = 0.f;
	/** The human player's pit stop (Status: 0 none, 1 servicing, 2 complete, 3 aborted). */
	int32 PitStatus = 0;
	float PitCountdown = 0.f;
	float PitStatusTime = 0.f;
	int32 PitStall = 0;
};

/**
 * Port of com.racingsim.app.RaceDirector: the per-step orchestration of the whole field, run in
 * Unreal's fixed 240 Hz async physics tick so every car sees the others at the same instant:
 *  1. read every car's body and project it onto the track (s, d)
 *  2. slipstreams (draft factors), before any aero force is computed
 *  3. drivers (player keys / AI)
 *  4. each car's vehicle model
 *  5. lap and position bookkeeping, pit stops, recovery of crashed cars
 */
UCLASS()
class RACINGSIM_API ARaceDirector : public AActor
{
	GENERATED_BODY()

public:
	static constexpr int32 GridColumns = 3;
	static constexpr float GridRowGap = 18.f;
	static constexpr float RollingStartSpeed = 58.f;   // m/s (~130 mph)
	static constexpr int32 DownforceSteps = 10;
	/** Highest wing level an AI car runs (of DownforceSteps). */
	static constexpr int32 AiMaxDownforce = DownforceSteps;

	ARaceDirector();

	virtual void BeginPlay() override;
	virtual void AsyncPhysicsTickActor(float DeltaTime, float SimTime) override;

	/** Adds a car in grid order (index 0 = pole). Driver null = driven by the keyboard. Game thread, before the race. */
	void AddCar(ARaceCar* Car, TUniquePtr<FAIDriver> Driver);
	/** Grid position (s, d) of a slot: rows of three abreast, pole on the inside. */
	static void GridSlot(int32 Slot, int32 CarCount, float& OutS, float& OutD);
	/** Lines the field up on the grid and starts a race (applied on the next physics step). */
	void RequestStart(int32 Laps, int32 Stops);
	/** Stops the race and lines the field up on the grid, standing (menu). */
	void RequestStop();
	/** Removes every car (the field is about to be rebuilt). Game thread. */
	void ClearField();
	/** Done adding cars: lines them up on the grid, standing, and lets the physics step them. */
	void FinishField();
	/** Wing level of the human player's car (0..1). */
	void SetPlayerDownforce(float Level);

	/** The racing line every AI driver follows (built on first use). */
	const FRacingLine* GetRacingLine();
	FRaceSnapshot GetSnapshot() const;
	const TArray<TObjectPtr<ARaceCar>>& GetCars() const { return Cars; }

private:
	void PT_StartRace();
	void PT_PlaceGrid(float Speed);
	void PT_UpdatePitStops(float Dt);
	void PT_ResetPlayer(int32 Index);
	void PT_CheckPlayerRecovery(int32 Index, float Dt);
	void PT_Recover(int32 Index);
	bool RejoinClear(const FCarCore* Self, float S, float D) const;
	int32 NextAiStall() const;
	static TArray<int32> PlanPitLaps(int32 Laps, int32 Stops, int32 Index);
	void PT_Publish();

	UPROPERTY()
	TObjectPtr<AOvalTrack> Track;

	UPROPERTY()
	TArray<TObjectPtr<ARaceCar>> Cars;

	TArray<FCarCore*> Field;
	TArray<TUniquePtr<FAIDriver>> Drivers;
	TArray<FPitStop> Pits;
	TArray<int32> Entries;
	FLapTracker Laps;
	TUniquePtr<FRacingLine> Line;
	FJavaRandom SetupRandom{ 2024 };

	std::atomic<bool> bStartPending{ false };
	std::atomic<bool> bStopPending{ false };
	std::atomic<bool> bGridPending{ false };
	bool bFieldReady = false;
	/** Held by the physics step; the game thread takes it to change the field. */
	FCriticalSection FieldLock;
	int32 PendingLaps = 5, PendingStops = 0;
	bool bRunning = false;
	int32 Recoveries = 0;
	float PlayerUpsideDown = 0.f, PlayerOutside = 0.f;

	mutable FCriticalSection SnapLock;
	FRaceSnapshot Snap;
};
