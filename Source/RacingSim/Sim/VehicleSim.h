#pragma once

#include "CoreMinimal.h"
#include "Sim/VehicleModel.h"
#include "Sim/TrackSpline.h"

/**
 * What the wheels stand on. The Java version ray-tests Bullet's track mesh; here the same
 * surface is evaluated from the track's own geometry (TrackSpline / PitLane), which matches the
 * mesh, needs no scene queries, and is safe to call on the physics thread.
 *
 *  - between the inner edge (inner wall, pit back wall or pit exit lane) and the outer wall:
 *    the banked road / flat apron / pit lane, with the infield grass strips and the pit-exit
 *    median marked as grass;
 *  - anywhere else (infield, outside the outer wall, on top of a wall): the ground plane
 *    just below the apron, which is grass.
 * On a road course: the road (with its slopes), the kerbs through the turns, and grass run-off.
 */
class RACINGSIM_API FTrackSurface
{
public:
	static constexpr float GroundZ = -0.05f;

	explicit FTrackSurface(const FTrackSpline* InTrack = nullptr) : Track(InTrack) {}

	/**
	 * Casts a ray from From (m) along unit direction Dir up to MaxDist.
	 * Returns the hit distance, or -1 on a miss.
	 */
	float Cast(const FVector& From, const FVector& Dir, float MaxDist, FVector& OutNormal, bool& bOutOnTrack) const;

	/** Surface height, normal and type straight below/above a world point. */
	void Sample(const FVector& P, float& OutZ, FVector& OutNormal, bool& bOutOnTrack) const;

	const FTrackSpline* GetTrack() const { return Track; }

private:
	const FTrackSpline* Track;
};

/** Rigid body state the vehicle model reads and writes (SI units, world frame, Z up). */
struct FRigidState
{
	FVector Pos = FVector::ZeroVector;      // center of mass (m)
	FQuat Rot = FQuat::Identity;
	FVector Vel = FVector::ZeroVector;      // m/s
	FVector AngVel = FVector::ZeroVector;   // rad/s
	float Mass = 1.f;
	/** Inverse principal inertia in the car's local frame (1 / kg m^2). */
	FVector InvInertiaLocal = FVector::OneVector;

	/** Same as Bullet's applyImpulse: Rel is the offset of the application point from the COM. */
	void ApplyImpulse(const FVector& J, const FVector& Rel)
	{
		Vel += J / Mass;
		AngVel += InvInertiaWorld(FVector::CrossProduct(Rel, J));
	}

	void ApplyCentralImpulse(const FVector& J) { Vel += J / Mass; }

	FVector InvInertiaWorld(const FVector& WorldTorqueImpulse) const
	{
		const FVector Local = Rot.UnrotateVector(WorldTorqueImpulse);
		return Rot.RotateVector(Local * InvInertiaLocal);
	}
};

/**
 * Port of com.racingsim.vehicle.VehiclePhysics: 4-wheel raycast suspension, Pacejka tires with
 * sub-stepped wheel spin, aero with drafting, RWD powertrain and gearbox, brakes, and the crash
 * energy absorption. Advanced once per fixed 240 Hz physics step.
 */
class RACINGSIM_API FVehicleSim
{
public:
	enum { FL = 0, FR = 1, RL = 2, RR = 3 };
	static constexpr float G = 9.81f;
	static constexpr int32 WheelSubsteps = 16;
	static constexpr float GrassGrip = 0.55f;
	static constexpr float GrassRolling = 6.f;
	static constexpr float ImpactRollKeep = 0.25f;
	static constexpr float ImpactLiftKeep = 0.2f;
	static constexpr float ImpactDv = 0.6f;
	static constexpr float ImpactDw = 0.4f;
	/** Final drive on a road course (the oval's is FVehicleSpec::FinalDrive). */
	static constexpr float RoadFinalDrive = 4.2f;

	struct FWheel
	{
		FVector MountLocal = FVector::ZeroVector;
		bool bFront = false;
		bool bDriven = false;
		float Omega = 0.f;
		float SpinAngle = 0.f;
		float Steer = 0.f;
		float SuspensionLength = 0.f;
		float Compression = 0.f;
		float PrevCompression = 0.f;
		float Load = 0.f;
		float SlipAngle = 0.f;
		float SlipRatio = 0.f;
		bool bContact = false;
		bool bOnTrack = true;
		FVector ContactPoint = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
	};

	FVehicleSim(const FVehicleSpec& InSpec, const FTrackSurface* InSurface);

	/** One fixed step: reads the body state, applies this tick's impulses to it. */
	void Step(float Dt, FRigidState& Body);
	/** Re-initializes wheel state after a teleport (reset / rolling start). */
	void ResetState(float NewSpeed);

	FVehicleInput Input;

	const FVehicleSpec& GetSpec() const { return Spec; }
	FGearbox& GetGearbox() { return Gearbox; }
	const FGearbox& GetGearbox() const { return Gearbox; }
	FAeroModel& GetAero() { return Aero; }
	const FWheel* GetWheels() const { return Wheels; }
	float GetDraftFactor() const { return DraftFactor; }
	void SetDraftFactor(float F) { DraftFactor = F; }
	float GetEngineRpm() const { return EngineRpm; }
	float GetSpeed() const { return Speed; }
	float GetForwardSpeed() const { return ForwardSpeed; }
	float GetLateralG() const { return LateralG; }
	float GetLongitudinalG() const { return LongitudinalG; }
	float GetSteerAngle() const { return SteerAngle; }
	float GetSqueal() const { return Squeal; }
	float GetDragReduction() const { return Aero.DraftDragGain * DraftFactor; }
	/** Average rear slip ratio magnitude. */
	float GetRearSlipRatio() const { return (FMath::Abs(Wheels[RL].SlipRatio) + FMath::Abs(Wheels[RR].SlipRatio)) * 0.5f; }
	/** Average front slip angle magnitude (rad). */
	float GetFrontSlipAngle() const { return (FMath::Abs(Wheels[FL].SlipAngle) + FMath::Abs(Wheels[FR].SlipAngle)) * 0.5f; }
	/** Average rear slip angle magnitude (rad). */
	float GetRearSlipAngle() const { return (FMath::Abs(Wheels[RL].SlipAngle) + FMath::Abs(Wheels[RR].SlipAngle)) * 0.5f; }

	/** Inverse principal inertia of the Java car's box collider (as Bullet computes it). */
	static FVector BodyInvInertia(const FVehicleSpec& Spec);

private:
	void AbsorbImpact(float Dt, FRigidState& B);
	void UpdateTire(FWheel& W, float DriveTorque, float BrakeTorque, float Dt, FRigidState& B);
	float ApplyBrake(float Omega, float Torque, float H) const;
	void UpdateSqueal(float Dt);
	void UpdateAccelerationTelemetry(float Dt, const FVector& Vel);

	FVehicleSpec Spec;
	const FTrackSurface* Surface;
	FTireModel Tire;
	FAeroModel Aero;
	FGearbox Gearbox;
	FWheel Wheels[4];

	float DraftFactor = 0.f;
	float EngineRpm = 0.f;
	float Speed = 0.f;
	float ForwardSpeed = 0.f;
	float LateralG = 0.f;
	float LongitudinalG = 0.f;
	float SteerAngle = 0.f;
	float Squeal = 0.f;

	// per-step cached frame (read at the start of the step, as in the Java version)
	FVector Pos, Vel, AngVel, Fwd, Up, Left;

	FVector PrevVel = FVector::ZeroVector;
	bool bHasPrevVel = false;
	FVector PostVel = FVector::ZeroVector, PostAngVel = FVector::ZeroVector;
	bool bHasPost = false;
};
