#pragma once

#include "CoreMinimal.h"
#include "Sim/TrackSpline.h"
#include "Sim/VehicleSim.h"

/**
 * Ports of the Java race logic that is independent of rendering: RacingLine, SpeedProfile,
 * DraftingSystem, LapTracker, PitStop, plus FCarCore (the simulation half of
 * com.racingsim.vehicle.Car) and java.util.Random (so a field set up from the same seeds gets
 * the same grip, aggression and wing levels as in the Java version).
 */

/** java.util.Random, bit for bit. */
class RACINGSIM_API FJavaRandom
{
public:
	explicit FJavaRandom(int64 InSeed) { SetSeed(InSeed); }
	void SetSeed(int64 InSeed) { Seed = (InSeed ^ 0x5DEECE66DLL) & ((1LL << 48) - 1); }
	int32 Next(int32 Bits)
	{
		Seed = (Seed * 0x5DEECE66DLL + 0xBLL) & ((1LL << 48) - 1);
		return (int32)(Seed >> (48 - Bits));
	}
	float NextFloat() { return Next(24) / (float)(1 << 24); }
	double NextDouble() { return (double)(((int64)Next(26) << 27) + Next(27)) * (1.0 / (double)(1LL << 53)); }
	int32 NextInt(int32 Bound)
	{
		if ((Bound & -Bound) == Bound) return (int32)((Bound * (int64)Next(31)) >> 31);
		int32 Bits, Val;
		do
		{
			Bits = Next(31);
			Val = Bits % Bound;
		} while (Bits - Val + (Bound - 1) < 0);
		return Val;
	}
	bool NextBoolean() { return Next(1) != 0; }

private:
	int64 Seed = 0;
};

/**
 * The racing line d(s). On the oval the classic line (RacingLine): high on the straights, low at
 * the apex. On a road course the minimum-curvature line within the road (less the car's width):
 * out wide, clip the apex, out wide again, worked out once when the track is loaded.
 */
class RACINGSIM_API FRacingLine
{
public:
	static constexpr float High = 4.3f;
	static constexpr float Low = -5.0f;
	/** How close (m) the road-course line comes to the road edge (car center). */
	static constexpr float EdgeMargin = 1.3f;

	explicit FRacingLine(const FTrackSpline* InTrack);
	const FTrackSpline& GetTrack() const { return *Track; }
	float Offset(float S) const;
	/** Signed curvature of the line itself (+ = left); road course only. */
	float PathCurvature(float S) const;

private:
	void BuildRoadLine();

	const FTrackSpline* Track;
	TArray<float> Centers;
	float HalfWidth;
	// road course: offsets and path curvature every Step metres
	float Step = 2.f;
	TArray<float> Offsets, Curv;
};

/** Target speed along the racing line with a backward braking pass (SpeedProfile). */
class RACINGSIM_API FSpeedProfile
{
public:
	/** BrakeGrip: road course, how much of the grip the braking points plan on (negative: the
	 *  usual, -AiBrakeGrip=). */
	FSpeedProfile(const FRacingLine* Line, const FVehicleSpec& Spec, const FAeroModel& Aero, float GripFactor, float BrakingDecel, float BrakeGrip = -1.f);
	FSpeedProfile WithAero(const FAeroModel& Aero) const;
	/** The same profile braking on BrakeGrip of the grip (a road course's late-braking pass). */
	FSpeedProfile WithBrakeGrip(const FAeroModel& Aero, float BrakeGrip) const;
	float At(float S) const;
	/** Road course: the speed for a fixed lane (constant d) rather than the racing line. */
	float LaneAt(float S, float D) const;
	static float CornerSpeed(float M, float R, float Bank, float Mu, float K);

private:
	/** Road course: corner speeds for curvatures K, then the braking pass (grip, downforce, the
	 *  grip the turn already uses, and the slope). */
	void BuildRoad(TArray<float>& Out, TFunctionRef<float(float)> CurvatureAt, float K) const;

	const FRacingLine* Line;
	FVehicleSpec Spec;
	float GripFactor, BrakingDecel, BrakeGrip;
	TArray<float> Speeds;
	static constexpr int32 NumLanes = 5;
	static constexpr float LaneSpacing = 2.5f;   // lanes at d = -5, -2.5, 0, 2.5, 5
	TArray<float> LaneSpeeds[NumLanes];
};

/**
 * The simulation half of a race car (com.racingsim.vehicle.Car): its vehicle model, the rigid
 * body state of the current physics step and its track-relative position.
 */
struct RACINGSIM_API FCarCore
{
	FString Name;
	int32 Number = 0;
	bool bPlayer = false;
	TUniquePtr<FVehicleSim> Sim;
	FRigidState Body;
	float S = 0.f;
	float D = 0.f;
	float LateralRate = 0.f;

	float TrackS() const { return S; }
	float TrackD() const { return D; }
	float Speed() const { return Sim->GetSpeed(); }
	float ForwardSpeed() const { return Sim->GetForwardSpeed(); }
	float GetDraftFactor() const { return Sim->GetDraftFactor(); }
	void SetDraftFactor(float F) { Sim->SetDraftFactor(F); }
	const FVehicleSpec& Spec() const { return Sim->GetSpec(); }
	FVehicleInput& Input() { return Sim->Input; }

	/** Refreshes s, d and the filtered lateral velocity (m/s, + = outwards); call once per tick. */
	void UpdateTrackPosition(const FTrackSpline& Track, float Dt)
	{
		const float PrevD = D;
		const FTrackSpline::FTrackPos P = Track.Project(Body.Pos);
		S = P.S;
		D = P.D;
		float Raw = (D - PrevD) / Dt;
		if (FMath::Abs(Raw) > 30.f) Raw = 0.f;                 // teleport / reset
		LateralRate += (Raw - LateralRate) * FMath::Min(1.f, Dt / 0.2f);
	}
};

/** Slipstream evaluation, once per tick before any aero force (DraftingSystem). */
namespace Drafting
{
	constexpr float WakeLength = 60.f;
	constexpr float WakeHalfWidth = 3.5f;
	constexpr float MaxDraft = 0.35f;

	RACINGSIM_API float WakeStrength(const FCarCore& Me, const FCarCore& Lead);
	RACINGSIM_API void Update(const TArray<FCarCore*>& Cars, float Dt);
}

/** Lap counting, timing, race order and the checkered flag (LapTracker). */
class RACINGSIM_API FLapTracker
{
public:
	struct FEntry
	{
		FString Name;
		bool bPlayer = false;
		float TotalDistance = 0.f;
		float LastS = 0.f;
		int32 LapIndex = 0;
		float LapStartTime = NAN;
		float LastLapTime = NAN;
		float BestLapTime = NAN;
		int32 Position = 0;
		bool bFinished = false;
		int32 FinishOrder = 0;
		float FinishTime = NAN;
		int32 PitStops = 0;

		int32 Lap() const { return FMath::Max(1, LapIndex + 1); }
		int32 LapsCompleted() const { return FMath::Max(0, LapIndex); }
		float CurrentLapTime(float Now) const { return FMath::IsNaN(LapStartTime) ? 0.f : Now - LapStartTime; }
	};

	void NewRace(int32 Laps, int32 Stops);
	/** A standing start: the race clock starts as the lights go out, not at the line. */
	void StartClock() { RaceStartTime = Time; }
	int32 GetRaceLaps() const { return RaceLaps; }
	int32 GetRequiredStops() const { return RequiredStops; }
	bool StopsMet(const FEntry& E) const { return E.PitStops >= RequiredStops; }
	void PitStopCompleted(FEntry& E) { E.PitStops++; }
	float RaceTime() const { return FMath::IsNaN(RaceStartTime) ? 0.f : Time - RaceStartTime; }
	bool LeaderFinished() const { return FinishedCount > 0; }

	int32 AddEntry(const FString& Name, bool bPlayer, float StartS);
	void Reset(int32 Index, float StartS);
	void ResetToLine(int32 Index);
	void Teleport(int32 Index, float NewS);
	void Update(int32 Index, float S);
	void AdvanceTime(float Dt) { Time += Dt; }
	void UpdatePositions();

	float GetTime() const { return Time; }
	const TArray<FEntry>& GetEntries() const { return Entries; }
	FEntry& Entry(int32 Index) { return Entries[Index]; }
	/** Entry indices in race order. */
	const TArray<int32>& GetOrder() const { return Order; }

private:
	void ResetEntry(FEntry& E, float StartS);
	int32 Group(const FEntry& E) const;

	TArray<FEntry> Entries;
	TArray<int32> Order;
	float Time = 0.f;
	int32 RaceLaps = MAX_int32;
	int32 FinishedCount = 0;
	float RaceStartTime = NAN;
	int32 RequiredStops = 0;
};

/** Pit-stop state of one car: an 8 s stop in its own box (PitStop). */
class RACINGSIM_API FPitStop
{
public:
	static constexpr float ServiceTime = 8.f;
	static constexpr float StoppedSpeed = 0.5f;
	enum class EStatus : uint8 { None, Servicing, Complete, Aborted };

	explicit FPitStop(int32 InStall = 0) : Stall(InStall) {}

	int32 GetStall() const { return Stall; }
	EStatus Status() const { return State; }
	float Countdown() const { return CountdownTime; }
	float StatusTime() const { return StatusClock; }
	bool IsServicing() const { return State == EStatus::Servicing; }
	void SetPlannedLaps(const TArray<int32>& Laps) { PlannedLaps = Laps; }
	int32 NextPlannedLap(int32 Done) const { return Done < PlannedLaps.Num() ? PlannedLaps[Done] : MAX_int32; }
	void Reset();
	static bool InBox(int32 Stall, float S, float D);
	/** @return true exactly when a stop has just been completed */
	bool Update(float Dt, float S, float D, float Speed);

private:
	void SetStatus(EStatus S) { State = S; StatusClock = 0.f; }

	int32 Stall;
	EStatus State = EStatus::None;
	float CountdownTime = 0.f;
	float StatusClock = 0.f;
	bool bServed = false;
	TArray<int32> PlannedLaps;
};
