#include "Car/RaceCar.h"
#include "Car/RacingController.h"
#include "Track/OvalTrack.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CommandLine.h"

namespace
{
	/** Car collider: a 2.0 x 0.5 m box, FVehicleSpec::BodyLength long, 0.12 m above the center of
	 *  mass (Java: 2.0 x 0.5 x 5.0 m for the Indy car). */
	FVector BoxHalfCm(const FVehicleSpec& Spec) { return FVector(Spec.BodyLength * 50.f, 100.f, 25.f); }
	FVector ComInBoxCm(const FVehicleSpec& Spec) { return FVector(-Spec.BodyCenterAhead * 100.f, 0.f, -12.f); }

	/** Each car's model and wheels: the Java Indy car, and the Formula car (Scripts/make_f1_car.py). */
	struct FCarModel { const TCHAR* Body; const TCHAR* WheelFront; const TCHAR* WheelRear; };
	FCarModel ModelFor(ECarType Type)
	{
		if (Type == ECarType::Formula)
		{
			return { TEXT("/Game/Models/Car/f1car_mesh.f1car_mesh"), TEXT("/Game/Models/Car/f1_wheel_front_mesh.f1_wheel_front_mesh"),
				TEXT("/Game/Models/Car/f1_wheel_rear_mesh.f1_wheel_rear_mesh") };
		}
		return { TEXT("/Game/Models/Car/indycar_mesh.indycar_mesh"), TEXT("/Game/Models/Car/wheel_front_mesh.wheel_front_mesh"),
			TEXT("/Game/Models/Car/wheel_rear_mesh.wheel_rear_mesh") };
	}

	/**
	 * The glTF models are authored in the Java car frame (+Z forward, +Y up, +X left). Unreal's
	 * glTF importer maps (x, y, z) to (x, z, y), so the imported car faces +Y with its left side on
	 * +X: a -90 degree yaw turns it to Unreal's +X forward, +Y right.
	 */
	const FRotator ModelRotation(0.f, -90.f, 0.f);

	float Ramp(float Cur, float Target, float UpStep, float DownStep)
	{
		return Cur < Target ? FMath::Min(Target, Cur + UpStep) : FMath::Max(Target, Cur - DownStep);
	}

	float HorizontalFov(float VerticalFovDeg, float Aspect)
	{
		return FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(VerticalFovDeg) / 2.f) * Aspect));
	}
}

std::atomic<int32> ARaceCar::CarContacts{ 0 };

ARaceCar::ARaceCar()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Body = CreateDefaultSubobject<UBoxComponent>(TEXT("Body"));
	SetRootComponent(Body);
	Body->SetBoxExtent(BoxHalfCm(Spec));
	Body->SetCollisionProfileName(TEXT("PhysicsActor"));
	Body->SetSimulatePhysics(true);
	Body->BodyInstance.SetMassOverride(Spec.Mass, true);
	Body->BodyInstance.COMNudge = ComInBoxCm(Spec);
	// no CCD: Chaos' swept contacts against the thin wall triangles threw cars hundreds of meters in the wall test
	// wall contacts pass through FWallContactFilter (bodies must opt in to contact modification)
	Body->BodyInstance.bContactModification = true;
	Body->SetLinearDamping(0.f);
	Body->SetAngularDamping(0.05f);
	Body->SetEnableGravity(true);
	Body->SetHiddenInGame(true);

	ModelRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ModelRoot"));
	ModelRoot->SetupAttachment(Body);
	ModelRoot->SetRelativeLocation(ComInBoxCm(Spec));

	BodyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BodyMesh"));
	BodyMesh->SetupAttachment(ModelRoot);
	BodyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BodyMesh->SetRelativeLocation(FVector(0, 0, -Spec.RideHeight() * 100.f));
	BodyMesh->SetRelativeRotation(ModelRotation);

	WheelPivot.SetNum(4);
	WheelSpinner.SetNum(4);
	WheelMesh.SetNum(4);
	for (int32 i = 0; i < 4; i++)
	{
		WheelPivot[i] = CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("WheelPivot%d"), i));
		WheelPivot[i]->SetupAttachment(ModelRoot);
		WheelSpinner[i] = CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("WheelSpin%d"), i));
		WheelSpinner[i]->SetupAttachment(WheelPivot[i]);
		WheelMesh[i] = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("WheelMesh%d"), i));
		WheelMesh[i]->SetupAttachment(WheelSpinner[i]);
		WheelMesh[i]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// the model's outer face points to the car's left; mirror it for the right-hand wheels
		const bool bLeft = (i % 2) == 0;
		WheelMesh[i]->SetRelativeRotation(bLeft ? ModelRotation : (FRotator(0, 180, 0).Quaternion() * ModelRotation.Quaternion()).Rotator());
	}

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Body);
	Camera->SetUsingAbsoluteLocation(true);
	Camera->SetUsingAbsoluteRotation(true);

	AutoPossessPlayer = EAutoReceiveInput::Disabled;
}

void ARaceCar::PreRegisterAllComponents()
{
	// the car for the selected track (the constructor ran with the defaults, the Indy car), set
	// before the components register so the body is created with this car's size and mass
	Spec = FVehicleSpec::ForTrack();
	Body->SetBoxExtent(BoxHalfCm(Spec), false);
	Body->BodyInstance.SetMassOverride(Spec.Mass, true);
	Body->BodyInstance.COMNudge = ComInBoxCm(Spec);
	ModelRoot->SetRelativeLocation(ComInBoxCm(Spec));
	BodyMesh->SetRelativeLocation(FVector(0, 0, -Spec.RideHeight() * 100.f));
	Super::PreRegisterAllComponents();
}

void ARaceCar::SetIdentity(const FString& Name, int32 Number, bool bHuman, const FLinearColor& Primary, const FLinearColor& Secondary, const FLinearColor& Accent)
{
	if (!CoreData.Sim)
	{
		const AOvalTrack* Track = AOvalTrack::Get(GetWorld());
		check(Track);
		CoreData.Sim = MakeUnique<FVehicleSim>(Spec, &Track->GetSurface());
		CoreData.Sim->ResetState(0.f);
	}
	CoreData.Name = Name;
	CoreData.Number = Number;
	CoreData.bPlayer = bHuman;
	LiveryPrimary = Primary;
	LiverySecondary = Secondary;
	LiveryAccent = Accent;
	if (BodyMesh->GetStaticMesh()) ApplyLivery();
}

void ARaceCar::BeginPlay()
{
	Super::BeginPlay();

	const FCarModel Model = ModelFor(Spec.Type);
	if (UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, Model.Body)) BodyMesh->SetStaticMesh(M);
	else UE_LOG(LogTemp, Warning, TEXT("Car model %s not found; run Scripts/setup_content.py"), Model.Body);
	UStaticMesh* Front = LoadObject<UStaticMesh>(nullptr, Model.WheelFront);
	UStaticMesh* Rear = LoadObject<UStaticMesh>(nullptr, Model.WheelRear);
	for (int32 i = 0; i < 4; i++) WheelMesh[i]->SetStaticMesh(i < 2 ? Front : Rear);
	ApplyLivery();

	UPhysicalMaterial* Mat = NewObject<UPhysicalMaterial>(this);
	Mat->Friction = Mat->StaticFriction = 0.35f;
	Mat->Restitution = 0.15f;
	// Bullet multiplies the two bodies' friction and restitution
	Mat->bOverrideFrictionCombineMode = true;
	Mat->FrictionCombineMode = EFrictionCombineMode::Multiply;
	Mat->bOverrideRestitutionCombineMode = true;
	Mat->RestitutionCombineMode = EFrictionCombineMode::Multiply;
	Body->SetPhysMaterialOverride(Mat);
	Body->SetNotifyRigidBodyCollision(true);
	Body->OnComponentHit.AddDynamic(this, &ARaceCar::OnBodyHit);

	if (!CoreData.Sim) SetIdentity(TEXT("Car"), 0, false, LiveryPrimary, LiverySecondary, LiveryAccent);
}

void ARaceCar::ApplyLivery()
{
	// The Indy car: the livery and helmet are painted by the Java LiveryPainter / HelmetPainter at
	// build time (gradlew bakeLiveries) and imported as T_Livery_<number> / T_Helmet_<number>; a car
	// without a baked texture gets its primary (livery) and secondary (helmet) color.
	// The Formula car is painted at run time: the model's Livery parts in its primary color,
	// LiverySecondary in the secondary, LiveryAccent (the pinstripe, the wing tips) and the helmet
	// in the accent.
	// The imported glTF materials have their colors baked in, so these parts get the
	// parameterized surface material.
	const bool bIndy = Spec.Type == ECarType::Indy;
	UMaterialInterface* Surface = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Surface.M_Surface"));
	if (!Surface) return;
	// Java BodyDetail, shared by every livery: panel seams, fasteners, stone chips and rubber
	// marbles (normal map), and a glossy clear coat that is rougher where the car is dirty
	// (roughness in R) with the seams darkened (ambient occlusion in G).
	UTexture* BodyNormal = LoadObject<UTexture>(nullptr, TEXT("/Game/Textures/Generated/body_normal.body_normal"));
	UTexture* BodyRough = LoadObject<UTexture>(nullptr, TEXT("/Game/Textures/Generated/body_rough.body_rough"));
	const TArray<FName> Slots = BodyMesh->GetMaterialSlotNames();
	for (int32 i = 0; i < Slots.Num(); i++)
	{
		const FString SlotName = Slots[i].ToString();
		const bool bLivery = SlotName.StartsWith(TEXT("Livery"));
		const bool bHelmet = SlotName.StartsWith(TEXT("Helmet"));
		if (!bLivery && !bHelmet) continue;
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Surface, this);
		UTexture* Painted = nullptr;
		if (bIndy)
		{
			const FString Kind = bLivery ? TEXT("Livery") : TEXT("Helmet");
			const FString TexPath = FString::Printf(TEXT("/Game/Textures/Liveries/T_%s_%d.T_%s_%d"), *Kind, CoreData.Number, *Kind, CoreData.Number);
			Painted = LoadObject<UTexture>(nullptr, *TexPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
		}
		if (Painted)
		{
			Mid->SetTextureParameterValue(TEXT("Albedo"), Painted);
			Mid->SetScalarParameterValue(TEXT("UseAlbedo"), 1.f);
			Mid->SetVectorParameterValue(TEXT("Tint"), FLinearColor::White);
		}
		else
		{
			const FLinearColor Color = bIndy ? (bLivery ? LiveryPrimary : LiverySecondary)
				: bHelmet ? LiveryAccent
				: SlotName.StartsWith(TEXT("LiverySecondary")) ? LiverySecondary
				: SlotName.StartsWith(TEXT("LiveryAccent")) ? LiveryAccent : LiveryPrimary;
			Mid->SetVectorParameterValue(TEXT("Tint"), Color);
		}
		// clear-coated paint, glossy helmet
		Mid->SetScalarParameterValue(TEXT("Roughness"), bLivery ? 0.25f : 0.10f);
		Mid->SetScalarParameterValue(TEXT("Metallic"), 0.f);
		if (bLivery && BodyNormal && BodyRough)
		{
			Mid->SetTextureParameterValue(TEXT("Normal"), BodyNormal);
			Mid->SetTextureParameterValue(TEXT("RoughTex"), BodyRough);
			Mid->SetScalarParameterValue(TEXT("UseNormal"), 1.f);
			Mid->SetScalarParameterValue(TEXT("UseRoughTex"), 1.f);
			Mid->SetScalarParameterValue(TEXT("UseAO"), 1.f);
			Mid->SetScalarParameterValue(TEXT("Roughness"), 1.f);   // the map holds the roughness
		}
		BodyMesh->SetMaterial(i, Mid);
	}

	// Tires: the Java TireTexture worn racing slick (scrubbed gray tread, rubber pick-up at the
	// shoulders, molded sidewall lettering), baked by Scripts/BakeTextures.java. The lathe-turned
	// tire surface is single sided, so it is drawn from both sides like the Java material.
	UMaterialInterface* TwoSided = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_SurfaceTwoSided.M_SurfaceTwoSided"));
	UTexture* TireAlbedo = LoadObject<UTexture>(nullptr, TEXT("/Game/Textures/Generated/tire_albedo.tire_albedo"));
	UTexture* TireNormal = LoadObject<UTexture>(nullptr, TEXT("/Game/Textures/Generated/tire_normal.tire_normal"));
	UTexture* TireRough = LoadObject<UTexture>(nullptr, TEXT("/Game/Textures/Generated/tire_rough.tire_rough"));
	if (!TwoSided || !TireAlbedo || !TireNormal || !TireRough) return;
	UMaterialInstanceDynamic* Tire = UMaterialInstanceDynamic::Create(TwoSided, this);
	Tire->SetTextureParameterValue(TEXT("Albedo"), TireAlbedo);
	Tire->SetTextureParameterValue(TEXT("Normal"), TireNormal);
	Tire->SetTextureParameterValue(TEXT("RoughTex"), TireRough);
	Tire->SetScalarParameterValue(TEXT("UseAlbedo"), 1.f);
	Tire->SetScalarParameterValue(TEXT("UseNormal"), 1.f);
	Tire->SetScalarParameterValue(TEXT("UseRoughTex"), 1.f);
	Tire->SetVectorParameterValue(TEXT("Tint"), FLinearColor::White);
	Tire->SetScalarParameterValue(TEXT("Roughness"), 1.f);
	Tire->SetScalarParameterValue(TEXT("Metallic"), 0.f);
	for (UStaticMeshComponent* W : WheelMesh)
	{
		const int32 Slot = W->GetMaterialIndex(TEXT("Tire"));
		if (Slot != INDEX_NONE) W->SetMaterial(Slot, Tire);
	}
}

void ARaceCar::OnBodyHit(UPrimitiveComponent* HitComp, AActor* Other, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
	const ARaceCar* O = Cast<ARaceCar>(Other);
	if (!O || O < this) return;                  // each pair counted by one of its two cars
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now - LastCarHit.FindRef(O, -10.0) > 0.5)
	{
		CarContacts++;
		UE_LOG(LogTemp, Display, TEXT("CARCONTACT t=%.2f %s (s=%.0f d=%.2f) x %s (s=%.0f d=%.2f) impulse=%.0f"), Now,
			*CoreData.Name, CoreData.S, CoreData.D, *O->CoreData.Name, O->CoreData.S, O->CoreData.D, NormalImpulse.Size() / 100.0);
	}
	LastCarHit.Add(O, Now);
}

FTransform ARaceCar::GetComTransform() const
{
	return ModelRoot->GetComponentTransform();
}

void ARaceCar::PlaceCustom(const FVector& ComPos, const FQuat& Rot, const FVector& Vel, float WheelSpeed)
{
	FScopeLock Lock(&ResetLock);
	bCustomPending = true;
	CustomPos = ComPos;
	CustomRot = Rot;
	CustomVel = Vel;
	CustomWheelSpeed = WheelSpeed;
}

void ARaceCar::PlaceOnTrack(float S, float D, float Speed)
{
	FScopeLock Lock(&ResetLock);
	bResetPending = true;
	ResetS = S;
	ResetD = D;
	ResetSpeed = Speed;
}

FCarSnapshot ARaceCar::GetSnapshot() const
{
	FScopeLock Lock(&SnapLock);
	return Snap;
}

// ------------------------------------------------------------------------- physics thread

bool ARaceCar::PT_Begin(const FTrackSpline& Spline)
{
	Rb = nullptr;
	if (!CoreData.Sim || !Body) return false;
	FBodyInstanceAsyncPhysicsTickHandle Handle = Body->GetBodyInstanceAsyncPhysicsTickHandle();
	if (!Handle) return false;
	Rb = Handle.operator->();
	if (!Rb) return false;

	if (!bPhysicsInit)
	{
		// match the Java body's inertia (Chaos works in kg cm^2)
		const FVector Inv = FVehicleSim::BodyInvInertia(Spec);
		const Chaos::TVec3<Chaos::FRealSingle> I(1e4f / Inv.X, 1e4f / Inv.Y, 1e4f / Inv.Z);
		Rb->SetI(I);
		Rb->SetInvI(Chaos::TVec3<Chaos::FRealSingle>(1.f / I.X, 1.f / I.Y, 1.f / I.Z));
		Rb->SetSleepType(Chaos::ESleepType::NeverSleep);
		bPhysicsInit = true;
	}

	FRigidState& B = CoreData.Body;
	B.Rot = Rb->R();
	B.Pos = (Rb->X() + B.Rot.RotateVector(Rb->CenterOfMass())) / 100.0;
	B.Vel = Rb->V() / 100.0;
	B.AngVel = Rb->W();
	B.Mass = Spec.Mass;
	B.InvInertiaLocal = FVehicleSim::BodyInvInertia(Spec);

	if (ToggleAutoRequest.exchange(false)) CoreData.Sim->GetGearbox().SetAutomatic(!CoreData.Sim->GetGearbox().IsAutomatic());

	FScopeLock Lock(&ResetLock);
	if (bResetPending)
	{
		bResetPending = false;
		PT_Teleport(Spline, ResetS, ResetD, ResetSpeed);
	}
	if (bCustomPending)
	{
		bCustomPending = false;
		Rb->SetX(CustomPos * 100.0 - CustomRot.RotateVector(Rb->CenterOfMass()));
		Rb->SetR(CustomRot);
		Rb->SetV(CustomVel * 100.0);
		Rb->SetW(FVector::ZeroVector);
		B.Pos = CustomPos;
		B.Rot = CustomRot;
		B.Vel = CustomVel;
		B.AngVel = FVector::ZeroVector;
		CoreData.Sim->ResetState(CustomWheelSpeed);
		CoreData.Sim->Input.Clear();
		const FTrackSpline::FTrackPos Tp = Spline.Project(CustomPos);
		CoreData.S = Tp.S;
		CoreData.D = Tp.D;
		CoreData.LateralRate = 0.f;
		PlacementCount++;
	}
	return true;
}

void ARaceCar::PT_Teleport(const FTrackSpline& Spline, float S, float D, float Speed)
{
	if (!Rb) return;
	const FVector Normal = Spline.SurfaceNormal(S, D);
	// along the road, up or down its slope (the road course's hills)
	FVector Fwd = Spline.Forward(S);
	Fwd = (Fwd - Normal * FVector::DotProduct(Fwd, Normal)).GetSafeNormal();
	const FVector ComPos = Spline.Point(S, D) + Normal * (Spec.RideHeight() + 0.03f);
	const FQuat Q = FRotationMatrix::MakeFromXZ(Fwd, Normal).ToQuat();
	Rb->SetX(ComPos * 100.0 - Q.RotateVector(Rb->CenterOfMass()));
	Rb->SetR(Q);
	Rb->SetV(Fwd * Speed * 100.0);
	Rb->SetW(FVector::ZeroVector);

	FRigidState& B = CoreData.Body;
	B.Pos = ComPos;
	B.Rot = Q;
	B.Vel = Fwd * Speed;
	B.AngVel = FVector::ZeroVector;
	CoreData.Sim->ResetState(Speed);
	CoreData.Sim->SetDraftFactor(0.f);
	CoreData.Sim->Input.Clear();
	CoreData.S = FTrackSpline::WrapS(S);
	CoreData.D = D;
	CoreData.LateralRate = 0.f;
	ThrottleCmd = BrakeCmd = SteerCmd = 0.f;
}

void ARaceCar::PT_ApplyPlayerInput(float Dt)
{
	FVehicleInput& In = CoreData.Sim->Input;
	const float Speed = CoreData.Sim->GetForwardSpeed();
	if (ClearCommandsRequest.exchange(false)) ThrottleCmd = BrakeCmd = SteerCmd = 0.f;
	if (ScriptedThrottle.load() >= 0.f)
	{
		float T = ScriptedThrottle.load(), Bk = ScriptedBrake.load();
		if (bScriptedLimits.load())
		{
			// a good driver: throttle fed in against wheelspin, brakes on the edge of locking
			const FVehicleSim& Phys = *CoreData.Sim;
			float Lock = 0.f;
			for (int32 i = 0; i < 4; i++) Lock = FMath::Min(Lock, Phys.GetWheels()[i].SlipRatio);
			// hold the slip just under the tires' peak (about 10%): integral control on the slip
			ThrottleCmd = FMath::Clamp(ThrottleCmd + (0.08f - Phys.GetRearSlipRatio()) * 40.f * Dt, 0.15f, T);
			BrakeCmd = FMath::Clamp(BrakeCmd + (Lock + 0.085f) * 60.f * Dt, 0.05f, Bk);
			T = T > 0.f ? ThrottleCmd : 0.f;
			Bk = Bk > 0.f ? BrakeCmd : 0.f;
		}
		In.Throttle = T;
		In.Brake = Bk;
		In.Steer = 0.f;
		return;
	}
	ThrottleCmd = Ramp(ThrottleCmd, KeyThrottle ? 1.f : 0.f, 6.f * Dt, 10.f * Dt);
	BrakeCmd = Ramp(BrakeCmd, KeyBrake ? 1.f : 0.f, 7.f * Dt, 10.f * Dt);
	const float SteerTarget = (KeyLeft ? 1.f : 0.f) - (KeyRight ? 1.f : 0.f);
	const float SpeedFactor = 1.f / (1.f + FMath::Abs(Speed) / 50.f);
	float Rate = (SteerTarget != 0.f ? 3.0f : 5.0f) * SpeedFactor * Dt;
	// returning through center is quicker than winding on lock
	if (SteerTarget != 0.f && FMath::Sign(SteerTarget) != FMath::Sign(SteerCmd)) Rate *= 1.8f;
	const bool bWheel = bPadConnected;
	if (bMouseSteering)
	{
		// follow the mouse wheel position with a tiny smoothing lag
		SteerCmd += (MouseSteer.load() - SteerCmd) * FMath::Min(1.f, Dt / 0.03f);
	}
	else if (bWheel && SteerTarget == 0.f)
	{
		// racing wheel / stick: follow its position, with just enough smoothing to hide sensor jitter
		SteerCmd += (PadSteer.load() - SteerCmd) * FMath::Min(1.f, Dt / 0.015f);
	}
	else
	{
		SteerCmd += FMath::Clamp(SteerTarget - SteerCmd, -Rate, Rate);
	}

	In.Throttle = bWheel ? FMath::Max(ThrottleCmd, PadThrottle.load()) : ThrottleCmd;
	In.Brake = bWheel ? FMath::Max(BrakeCmd, PadBrake.load()) : BrakeCmd;
	In.Steer = SteerCmd;
	const int32 Up = ShiftUpCount.load(), Down = ShiftDownCount.load();
	if (Up != ShiftUpSeen) { In.bShiftUpRequest = true; ShiftUpSeen = Up; }
	if (Down != ShiftDownSeen) { In.bShiftDownRequest = true; ShiftDownSeen = Down; }
}

void ARaceCar::PT_End(double SimTime)
{
	if (!Rb) return;
	const FRigidState& B = CoreData.Body;
	Rb->SetV(B.Vel * 100.0);
	Rb->SetW(B.AngVel);

	const FVehicleSim& Sim = *CoreData.Sim;
	FScopeLock Lock(&SnapLock);
	const FVehicleSim::FWheel* W = Sim.GetWheels();
	Snap.Contacts = 0;
	for (int32 i = 0; i < 4; i++)
	{
		Snap.WheelSteer[i] = W[i].Steer;
		Snap.WheelSpin[i] = W[i].SpinAngle;
		Snap.WheelSusp[i] = W[i].SuspensionLength;
		Snap.Contacts += W[i].bContact ? 1 : 0;
	}
	Snap.Speed = Sim.GetSpeed();
	Snap.ForwardSpeed = Sim.GetForwardSpeed();
	Snap.Rpm = Sim.GetEngineRpm();
	Snap.Gear = Sim.GetGearbox().GetGear();
	Snap.bAutomatic = Sim.GetGearbox().IsAutomatic();
	Snap.LateralG = Sim.GetLateralG();
	Snap.Squeal = Sim.GetSqueal();
	Snap.DraftFactor = Sim.GetDraftFactor();
	Snap.TrackS = CoreData.S;
	Snap.TrackD = CoreData.D;
	Snap.SimTime = SimTime;
	Snap.ComZ = B.Pos.Z;
	Snap.UpZ = B.Rot.GetUpVector().Z;
	Snap.SteerInput = Sim.Input.Steer;
	Snap.ComPos = B.Pos;
	Snap.Up = B.Rot.GetUpVector();
	Snap.Fwd = B.Rot.GetForwardVector();
	Snap.Vel = B.Vel;
	Snap.PlacementCount = PlacementCount;
	Snap.LongitudinalG = Sim.GetLongitudinalG();
	Snap.RearSlipRatio = Sim.GetRearSlipRatio();
	Snap.FrontSlipAngle = Sim.GetFrontSlipAngle();
	Snap.DownforceLevel = CoreData.Sim->GetAero().DownforceLevel;
	Snap.DragReduction = Sim.GetDragReduction();
	Snap.Throttle = Sim.Input.Throttle;
	Snap.Brake = Sim.Input.Brake;
	Snap.bShifting = Sim.GetGearbox().IsShifting();
	for (int32 i = 0; i < 4; i++)
	{
		// port of TireSmoke.intensity: sideways slide past the grip peak, big lock-ups / wheelspin
		const FVehicleSim::FWheel& Wh = W[i];
		float Smoke = 0.f;
		if (Wh.bContact && Wh.bOnTrack && Wh.Load >= 400.f && Snap.Speed >= 4.f)
		{
			const float Lateral = (FMath::Abs(Wh.SlipAngle) - 0.10f) / 0.22f;
			const float Longitudinal = (FMath::Abs(Wh.SlipRatio) - 0.25f) / 0.45f;
			const float S = FMath::Max(Lateral, 0.6f * Longitudinal);
			const float LoadF = FMath::Min(1.f, Wh.Load / 3000.f);
			Smoke = FMath::Clamp(S, 0.f, 1.f) * (0.4f + 0.6f * LoadF) * FMath::Min(1.f, Snap.Speed / 15.f);
		}
		Snap.WheelSmoke[i] = Smoke;
		Snap.WheelPatch[i] = FVector(Wh.MountLocal.X, Wh.MountLocal.Y, Wh.MountLocal.Z - Wh.SuspensionLength - Spec.WheelRadius + 0.30f) * 100.0;
	}
	Rb = nullptr;
}

// ---------------------------------------------------------------------------- game thread

bool ARaceCar::IsControllerConnected()
{
	return FRacingController::Get().IsConnected() || (FSlateApplication::IsInitialized() && FSlateApplication::Get().IsGamepadAttached());
}

void ARaceCar::SetMouseSteering(bool bOn)
{
	// the wheel picks up from its current position
	if (bOn && !bMouseSteering) MouseSteer = GetSnapshot().SteerInput;
	bMouseSteering = bOn;
}

void ARaceCar::SetDrivingEnabled(bool bOn)
{
	bDrivingEnabled = bOn;
	if (!bOn)
	{
		KeyThrottle = KeyBrake = KeyLeft = KeyRight = false;
		PadSteer = PadThrottle = PadBrake = 0.f;
		SetMouseSteering(false);
		ClearCommandsRequest = true;
	}
}

void ARaceCar::ReadPlayerKeys()
{
	APlayerController* Pc = Cast<APlayerController>(GetController());
	if (!Pc || !bDrivingEnabled) return;
	// racing wheel / gamepad (XInput): wheel or left stick steers (left = positive), pedals or
	// triggers, paddles or shoulder buttons shift
	bPadConnected = IsControllerConnected();
	const FRacingController& Wheel = FRacingController::Get();
	if (Wheel.IsConnected())
	{
		// racing wheel and pedals over DirectInput (see RacingController.h for the mapping)
		using EB = FRacingController::EButton;
		PadSteer = Wheel.GetSteer();
		PadThrottle = Wheel.GetThrottle();
		PadBrake = Wheel.GetBrake();
		if (Wheel.WasPressed(EB::ShiftUp)) ShiftUpCount++;
		if (Wheel.WasPressed(EB::ShiftDown)) ShiftDownCount++;
		if (Wheel.WasPressed(EB::Auto)) ToggleAutoRequest = true;
		if (Wheel.WasPressed(EB::Camera)) { bTCam = !bTCam; bSnapCamera = true; }
		if (Wheel.WasPressed(EB::Reset)) { ResetRequest = true; bSnapCamera = true; }
	}
	else if (bPadConnected)
	{
		PadSteer = FMath::Clamp(-Pc->GetInputAnalogKeyState(EKeys::Gamepad_LeftX), -1.f, 1.f);
		PadThrottle = FMath::Clamp(Pc->GetInputAnalogKeyState(EKeys::Gamepad_RightTriggerAxis), 0.f, 1.f);
		PadBrake = FMath::Clamp(Pc->GetInputAnalogKeyState(EKeys::Gamepad_LeftTriggerAxis), 0.f, 1.f);
		if (Pc->WasInputKeyJustPressed(EKeys::Gamepad_RightShoulder)) ShiftUpCount++;
		if (Pc->WasInputKeyJustPressed(EKeys::Gamepad_LeftShoulder)) ShiftDownCount++;
		if (Pc->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Left)) ToggleAutoRequest = true;
		if (Pc->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Top)) { bTCam = !bTCam; bSnapCamera = true; }
		if (Pc->WasInputKeyJustPressed(EKeys::Gamepad_Special_Left)) { ResetRequest = true; bSnapCamera = true; }
	}
	// mouse steering: click the mouse wheel; the mouse then works like a steering wheel whose
	// position stays where it is left (full lock in about 850 pixels)
	if (Pc->WasInputKeyJustPressed(EKeys::MiddleMouseButton)) SetMouseSteering(!bMouseSteering);
	if (bMouseSteering)
	{
		// Unreal reports mouse movement scaled by the MouseX axis sensitivity (0.07 by default):
		// undo that to get pixels. -MouseSensitivity=2 makes the wheel twice as quick.
		static const float Sensitivity = []
		{
			float S = 1.f;
			FParse::Value(FCommandLine::Get(), TEXT("MouseSensitivity="), S);
			return FMath::Clamp(S, 0.1f, 10.f);
		}();
		float Dx = 0.f, Dy = 0.f;
		Pc->GetInputMouseDelta(Dx, Dy);
		const float Pixels = Dx / 0.07f;
		MouseSteer = FMath::Clamp(MouseSteer.load() - Pixels * Sensitivity / 850.f, -1.f, 1.f);
	}
	KeyThrottle = Pc->IsInputKeyDown(EKeys::W) || Pc->IsInputKeyDown(EKeys::Up);
	KeyBrake = Pc->IsInputKeyDown(EKeys::S) || Pc->IsInputKeyDown(EKeys::Down);
	KeyLeft = Pc->IsInputKeyDown(EKeys::A) || Pc->IsInputKeyDown(EKeys::Left);
	KeyRight = Pc->IsInputKeyDown(EKeys::D) || Pc->IsInputKeyDown(EKeys::Right);
	if (Pc->WasInputKeyJustPressed(EKeys::E)) ShiftUpCount++;
	if (Pc->WasInputKeyJustPressed(EKeys::Q)) ShiftDownCount++;
	if (Pc->WasInputKeyJustPressed(EKeys::T)) ToggleAutoRequest = true;
	if (Pc->WasInputKeyJustPressed(EKeys::C))
	{
		bTCam = !bTCam;
		bSnapCamera = true;
	}
	if (Pc->WasInputKeyJustPressed(EKeys::R))
	{
		ResetRequest = true;
		bSnapCamera = true;
	}
}

void ARaceCar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ReadPlayerKeys();
	UpdateVisuals();
	if (Cast<APlayerController>(GetController())) UpdateCamera(DeltaSeconds);
}

void ARaceCar::UpdateVisuals()
{
	if (!CoreData.Sim) return;
	const FCarSnapshot S = GetSnapshot();
	const FVehicleSim::FWheel* W = CoreData.Sim->GetWheels();
	for (int32 i = 0; i < 4; i++)
	{
		const FVector& M = W[i].MountLocal;
		WheelPivot[i]->SetRelativeLocation(FVector(M.X, M.Y, M.Z - S.WheelSusp[i]) * 100.f);
		// steer is positive to the left: a negative yaw in Unreal
		WheelPivot[i]->SetRelativeRotation(FRotator(0.f, -FMath::RadiansToDegrees(S.WheelSteer[i]), 0.f));
		// rolling forward turns the wheel top toward +X: a negative pitch about +Y
		WheelSpinner[i]->SetRelativeRotation(FRotator(-FMath::RadiansToDegrees(S.WheelSpin[i]), 0.f, 0.f));
	}
}

void ARaceCar::UpdateCamera(float DeltaSeconds)
{
	const FTransform Com = ModelRoot->GetComponentTransform();
	const FVector CarPos = Com.GetLocation();
	const FQuat CarRot = Com.GetRotation();
	FVector FlatFwd = CarRot.GetForwardVector();
	FlatFwd.Z = 0;
	if (FlatFwd.SizeSquared() < 1e-4) FlatFwd = FVector::ForwardVector;
	FlatFwd.Normalize();

	float Aspect = 16.f / 9.f;
	if (GEngine && GEngine->GameViewport)
	{
		FVector2D Size;
		GEngine->GameViewport->GetViewportSize(Size);
		if (Size.Y > 0) Aspect = Size.X / Size.Y;
	}

	if (bShowcase)
	{
		// port of CameraController.SHOWCASE: 7.5 m ahead, 1.2 m to the left, 1 m up, looking back
		const FVector Right = FVector::CrossProduct(FVector::UpVector, FlatFwd);
		const FVector CamPos = CarPos + FlatFwd * 750.f - Right * 120.f + FVector(0, 0, 100.f);
		const FVector LookAt = CarPos - FlatFwd * 250.f + FVector(0, 0, 35.f);
		Camera->SetWorldLocationAndRotation(CamPos, (LookAt - CamPos).Rotation());
		Camera->SetFieldOfView(HorizontalFov(50.f, Aspect));
	}
	else if (!bTCam)
	{
		const FVector Desired = FlatFwd * -850.f + FVector(0, 0, 260.f);
		const float A = bSnapCamera ? 1.f : 1.f - FMath::Exp(-DeltaSeconds * 7.f);
		SmoothedOffset = FMath::Lerp(SmoothedOffset, Desired, A);
		const FVector CamPos = CarPos + SmoothedOffset;
		const FVector LookAt = CarPos + FlatFwd * 400.f + FVector(0, 0, 90.f);
		Camera->SetWorldLocationAndRotation(CamPos, (LookAt - CamPos).Rotation());
		Camera->SetFieldOfView(HorizontalFov(62.f, Aspect));
	}
	else
	{
		// locked to the chassis above the driver's head, pitch and roll attenuated
		const FQuat YawOnly = FlatFwd.Rotation().Quaternion();
		FQuat CamRot = FQuat::Slerp(YawOnly, CarRot, 0.45f);
		CamRot = CamRot * FRotator(-4.f, 0.f, 0.f).Quaternion();
		Camera->SetWorldLocationAndRotation(CarPos + CarRot.RotateVector(FVector(-45.f, 0.f, 95.f)), CamRot);
		Camera->SetFieldOfView(HorizontalFov(72.f, Aspect));
	}
	bSnapCamera = false;
}
