#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Sim/RaceLogic.h"
#include <atomic>
#include "RaceCar.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UCameraComponent;
class AOvalTrack;

namespace Chaos { class FRigidBodyHandle_Internal; }

/** What the game thread shows of the car (copied from the physics thread every step). */
struct FCarSnapshot
{
	float WheelSteer[4] = {};
	float WheelSpin[4] = {};
	float WheelSusp[4] = {};
	float Speed = 0.f;
	float ForwardSpeed = 0.f;
	float Rpm = 0.f;
	int32 Gear = 1;
	bool bAutomatic = true;
	float LateralG = 0.f;
	float Squeal = 0.f;
	float DraftFactor = 0.f;
	float TrackS = 0.f;
	float TrackD = 0.f;
	double SimTime = 0.0;
	float ComZ = 0.f, UpZ = 0.f;
	int32 Contacts = 0;
	// HUD telemetry
	float SteerInput = 0.f;
	float LongitudinalG = 0.f;
	float RearSlipRatio = 0.f;
	float FrontSlipAngle = 0.f;
	float DownforceLevel = 0.f;
	float DragReduction = 0.f;
	// sound
	float Throttle = 0.f, Brake = 0.f;
	bool bShifting = false;
	// tire smoke: per-wheel contact patch (car local, cm) and smoke intensity 0..1
	FVector WheelPatch[4];
	float WheelSmoke[4] = {};
	// wall check: body state (m, m/s) and how many custom placements have been applied
	FVector ComPos = FVector::ZeroVector, Up = FVector::UpVector, Fwd = FVector::ForwardVector, Vel = FVector::ZeroVector;
	int32 PlacementCount = 0;
};

/**
 * One race car (port of com.racingsim.vehicle.Car + the player's input and camera): a Chaos
 * rigid body, its simulation state (FCarCore) and the Blender car model. The race director
 * steps every car's vehicle model in one fixed 240 Hz loop on the physics thread (the PT_*
 * functions); this actor reads the keyboard and moves the wheels and the camera.
 */
UCLASS()
class RACINGSIM_API ARaceCar : public APawn
{
	GENERATED_BODY()

public:
	ARaceCar();

	virtual void Tick(float DeltaSeconds) override;

	/** Name, race number, livery color and whether a human drives it. Call right after spawning. */
	void SetIdentity(const FString& Name, int32 Number, bool bHuman, const FLinearColor& Primary, const FLinearColor& Secondary,
		const FLinearColor& Accent = FLinearColor(0.8f, 0.05f, 0.05f));

	FCarCore& Core() { return CoreData; }
	const FCarCore& Core() const { return CoreData; }

	/** Game thread: teleport onto the track at (s, d) moving forward at Speed (m/s), applied on the next physics step. */
	void PlaceOnTrack(float S, float D, float Speed);

	/** The R key (consumed by the race director). */
	bool ConsumeResetRequest() { return ResetRequest.exchange(false); }

	void SetTCam(bool bOn) { bTCam = bOn; bSnapCamera = true; }
	/** Screenshot camera: low, ahead of the car, looking back at it and the pack. */
	void SetShowcase(bool bOn) { bShowcase = bOn; }
	/** World transform of the center of mass (the model root). */
	FTransform GetComTransform() const;
	/** Test placement: center of mass (m), orientation, velocity (m/s), wheel speed (m/s); applied on the next physics step. */
	void PlaceCustom(const FVector& ComPos, const FQuat& Rot, const FVector& Vel, float WheelSpeed);
	/** Fixed throttle instead of the keys (tests); negative = off. */
	void SetScriptedThrottle(float T) { ScriptedThrottle = T; }
	/** Scripted runs (tests): the brake pedal, and whether the "driver" feeds the throttle in
	 *  against wheelspin and keeps the brakes just short of locking. */
	void SetScriptedBrake(float B) { ScriptedBrake = B; }
	void SetScriptedLimits(bool bOn) { bScriptedLimits = bOn; }
	bool IsScripted() const { return ScriptedThrottle.load() >= 0.f; }
	bool IsTCam() const { return bTCam; }
	bool IsMouseSteering() const { return bMouseSteering; }
	void SetMouseSteering(bool bOn);
	/** Driving controls are read only while racing (the menus use the same keys). */
	void SetDrivingEnabled(bool bOn);
	/** True while a gamepad / racing wheel is plugged in. */
	static bool IsControllerConnected();
	FCarSnapshot GetSnapshot() const;

	// ---- physics thread (called by ARaceDirector each fixed step) ----
	/** Reads the body state into Core().Body; false while the physics body is not ready. */
	bool PT_Begin(const FTrackSpline& Track);
	/** Immediate teleport (s, d, speed) during a physics step. */
	void PT_Teleport(const FTrackSpline& Track, float S, float D, float Speed);
	/** Keyboard -> smooth analog commands (port of PlayerInput.apply). */
	void PT_ApplyPlayerInput(float Dt);
	/** Writes the velocities back to Chaos and publishes the snapshot. */
	void PT_End(double SimTime);
	float PT_UpZ() const { return CoreData.Body.Rot.GetUpVector().Z; }

	/** Car-to-car contacts so far (a pair touching again within 0.5 s counts once). */
	static std::atomic<int32> CarContacts;

protected:
	virtual void BeginPlay() override;
	virtual void PreRegisterAllComponents() override;

private:
	void ReadPlayerKeys();
	void UpdateVisuals();
	void UpdateCamera(float DeltaSeconds);
	void ApplyLivery();

	UFUNCTION()
	void OnBodyHit(UPrimitiveComponent* HitComp, AActor* Other, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);
	TMap<const AActor*, double> LastCarHit;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UBoxComponent> Body;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> ModelRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BodyMesh;

	UPROPERTY(VisibleAnywhere)
	TArray<TObjectPtr<USceneComponent>> WheelPivot;

	UPROPERTY(VisibleAnywhere)
	TArray<TObjectPtr<USceneComponent>> WheelSpinner;

	UPROPERTY(VisibleAnywhere)
	TArray<TObjectPtr<UStaticMeshComponent>> WheelMesh;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCameraComponent> Camera;

	FVehicleSpec Spec;
	FCarCore CoreData;
	FLinearColor LiveryPrimary = FLinearColor(0.9f, 0.9f, 0.9f);
	FLinearColor LiverySecondary = FLinearColor(0.9f, 0.9f, 0.9f);
	FLinearColor LiveryAccent = FLinearColor(0.8f, 0.05f, 0.05f);

	// ---- game thread -> physics thread ----
	std::atomic<bool> KeyThrottle{ false }, KeyBrake{ false }, KeyLeft{ false }, KeyRight{ false };
	std::atomic<int32> ShiftUpCount{ 0 }, ShiftDownCount{ 0 };
	std::atomic<bool> ToggleAutoRequest{ false };
	std::atomic<bool> ResetRequest{ false };
	// gamepad / racing wheel (XInput) and mouse steering
	std::atomic<float> PadSteer{ 0.f }, PadThrottle{ 0.f }, PadBrake{ 0.f }, MouseSteer{ 0.f };
	std::atomic<bool> bPadConnected{ false }, bMouseSteering{ false }, bDrivingEnabled{ true };
	std::atomic<bool> ClearCommandsRequest{ false };
	FCriticalSection ResetLock;
	bool bResetPending = false;
	bool bCustomPending = false;
	FVector CustomPos, CustomVel;
	FQuat CustomRot;
	float CustomWheelSpeed = 0.f;
	int32 PlacementCount = 0;
	std::atomic<float> ScriptedThrottle{ -1.f };
	std::atomic<float> ScriptedBrake{ 0.f };
	std::atomic<bool> bScriptedLimits{ false };
	float ResetS = 0.f, ResetD = 0.f, ResetSpeed = 0.f;

	// ---- physics thread state ----
	Chaos::FRigidBodyHandle_Internal* Rb = nullptr;
	bool bPhysicsInit = false;
	float ThrottleCmd = 0.f, BrakeCmd = 0.f, SteerCmd = 0.f;
	int32 ShiftUpSeen = 0, ShiftDownSeen = 0;

	// ---- physics thread -> game thread ----
	mutable FCriticalSection SnapLock;
	FCarSnapshot Snap;

	// ---- camera ----
	bool bTCam = false;
	bool bShowcase = false;
	bool bSnapCamera = true;
	FVector SmoothedOffset = FVector(-850, 0, 260);
};
