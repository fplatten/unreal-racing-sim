#pragma once

#include "CoreMinimal.h"

/**
 * Ports of the Java vehicle parameter and force models: VehicleSpec, TireModel, AeroModel,
 * Gearbox and VehicleInput. All SI units.
 *
 * Car local frame (Unreal): +X forward, +Y right, +Z up, origin = center of mass.
 * (The Java frame is +Z forward, +Y up, +X left: Java (x, y, z) -> Unreal (z, -x, y).)
 */

/** Physical parameters of the open-wheel car (com.racingsim.vehicle.VehicleSpec). */
struct RACINGSIM_API FVehicleSpec
{
	// ---- Mass & geometry ----
	float Mass = 780.f;
	float WheelRadius = 0.33f;
	float FrontAxleX = 1.65f;
	float RearAxleX = -1.35f;
	float FrontHalfTrack = 0.86f;
	float RearHalfTrack = 0.82f;
	/** Height of the suspension mount points above the center of mass. */
	float MountHeight = 0.10f;

	// ---- Suspension ----
	float RestLength = 0.12f;
	float SpringFront = 110000.f;
	float SpringRear = 120000.f;
	float DamperBump = 5500.f;
	float DamperRebound = 6500.f;
	float AntiRollFront = 28000.f;
	float AntiRollRear = 20000.f;
	float BumpStopStart = 0.09f;
	float BumpStopRate = 600000.f;
	/** Height above the contact patch at which tire forces are applied (reduces jacking). */
	float TireForceLift = 0.08f;

	// ---- Wheels / brakes ----
	float WheelInertia = 1.4f;
	float BrakeTorqueFront = 3200.f;
	float BrakeTorqueRear = 2200.f;
	float RollingResistance = 0.012f;
	float FrontGrip = 1.0f;
	float RearGrip = 1.10f;

	// ---- Steering ----
	float MaxSteerLow = 0.40f;
	float MaxSteerHigh = 0.08f;
	float SteerSpeedFalloff = 22.f;

	// ---- Engine & gearbox ----
	TArray<float> TorqueRpm = { 0, 2000, 4000, 6000, 8000, 10000, 11000, 12000, 12500 };
	TArray<float> TorqueNm = { 220, 250, 340, 400, 445, 470, 470, 440, 400 };
	float IdleRpm = 3000.f;
	float RedlineRpm = 12000.f;
	float LimiterRpm = 12300.f;
	TArray<float> GearRatios = { 2.80f, 2.15f, 1.75f, 1.48f, 1.27f, 1.10f };
	float ReverseRatio = 2.9f;
	float FinalDrive = 3.2f;
	float DrivetrainEfficiency = 0.92f;
	float ShiftTime = 0.06f;
	float AutoUpshiftRpm = 11800.f;
	float AutoDownshiftRpm = 7200.f;

	/** Speed-sensitive steering lock (rad). */
	float MaxSteerAngle(float Speed) const
	{
		return FMath::Max(MaxSteerHigh, MaxSteerLow / (1.f + FMath::Abs(Speed) / SteerSpeedFalloff));
	}

	float EngineTorque(float Rpm) const;

	/** Height of the center of mass above the ground at static ride height. */
	float RideHeight() const
	{
		const float StaticComp = Mass * 9.81f / 4.f / SpringFront;
		return WheelRadius + RestLength - StaticComp - MountHeight;
	}
};

/** Driver commands (com.racingsim.vehicle.VehicleInput), written by the player or an AI. */
struct RACINGSIM_API FVehicleInput
{
	float Throttle = 0.f;   // 0..1
	float Brake = 0.f;      // 0..1
	float Steer = 0.f;      // -1..1, positive = steer LEFT
	bool bShiftUpRequest = false;
	bool bShiftDownRequest = false;
	bool bPitHold = false;

	void Clear()
	{
		Throttle = Brake = Steer = 0.f;
		bShiftUpRequest = bShiftDownRequest = false;
	}
};

/** Pacejka Magic Formula tire with load sensitivity and a friction ellipse (TireModel). */
struct RACINGSIM_API FTireModel
{
	float LatB = 18.f, LatC = 1.6f, LatE = 0.6f;
	float LongB = 20.f, LongC = 1.6f, LongE = 0.6f;
	float Mu = 1.55f;
	float NominalLoad = 2500.f;
	float LoadSensitivity = 0.07f;

	struct FResult
	{
		float Fx = 0.f;     // longitudinal (N), + = forward along the wheel
		float Fy = 0.f;     // lateral (N), + = toward the wheel's LEFT
		float MuEff = 0.f;
		float Usage = 0.f;
	};

	static float Magic(float X, float B, float C, float E)
	{
		const float Bx = B * X;
		return FMath::Sin(C * FMath::Atan(Bx - E * (Bx - FMath::Atan(Bx))));
	}

	float EffectiveMu(float Fz) const
	{
		const float M = Mu * (1.f - LoadSensitivity * (Fz / NominalLoad - 1.f));
		return FMath::Clamp(M, Mu * 0.70f, Mu * 1.1f);
	}

	FResult Compute(float SlipAngle, float SlipRatio, float Fz, float GripScale) const;
};

/** Speed-squared drag and downforce, modulated by drafting (AeroModel). */
struct RACINGSIM_API FAeroModel
{
	static constexpr float AirDensity = 1.225f;
	static constexpr float MinDragArea = 0.75f;
	static constexpr float MinLiftArea = 2.2f;
	static constexpr float MaxDragArea = 0.98f;
	static constexpr float MaxLiftArea = 4.0f;

	float DragArea = MinDragArea;
	float LiftArea = MinLiftArea;
	float DownforceLevel = 0.f;
	float FrontBalance = 0.40f;
	float DraftDragGain = 1.0f;
	float DraftFrontLiftGain = 0.4f;
	float DraftRearLiftGain = 0.15f;

	struct FResult
	{
		float Drag = 0.f;
		float DownforceFront = 0.f;
		float DownforceRear = 0.f;
		float CdEffective = 0.f;
	};

	/** 0 = minimum downforce (max speed) .. 1 = maximum downforce (max control). */
	void SetDownforceLevel(float Level)
	{
		DownforceLevel = FMath::Clamp(Level, 0.f, 1.f);
		DragArea = MinDragArea + (MaxDragArea - MinDragArea) * DownforceLevel;
		LiftArea = MinLiftArea + (MaxLiftArea - MinLiftArea) * DownforceLevel;
	}

	FResult Compute(float Speed, float ForwardSpeed, float DraftFactor) const;
};

/** 6-speed sequential gearbox, gear 0 = reverse (Gearbox). */
class RACINGSIM_API FGearbox
{
public:
	explicit FGearbox(const FVehicleSpec* InSpec) : Spec(InSpec) {}

	int32 GetGear() const { return Gear; }
	bool IsAutomatic() const { return bAutomatic; }
	void SetAutomatic(bool bAuto) { bAutomatic = bAuto; }
	bool IsShifting() const { return ShiftTimer > 0.f; }
	int32 TopGear() const { return Spec->GearRatios.Num(); }
	float TotalRatio() const;
	void SetGear(int32 G) { Gear = FMath::Clamp(G, 0, TopGear()); }
	void SelectForSpeed(float Speed);
	void Update(float Dt, FVehicleInput& In, float Rpm, float ForwardSpeed);

private:
	float RpmAt(float Speed, int32 G) const;
	void ShiftTo(int32 G);

	const FVehicleSpec* Spec;
	int32 Gear = 1;
	bool bAutomatic = true;
	float ShiftTimer = 0.f;
	float ShiftCooldown = 0.f;
	float ReverseHold = 0.f;
};
