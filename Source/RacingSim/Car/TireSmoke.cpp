#include "Car/TireSmoke.h"
#include "Car/RaceCar.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"

namespace
{
	constexpr int32 MaxParticles = 900;
	/** Puffs per second from one wheel at full slide. */
	constexpr float MaxRate = 45.f;
	constexpr float StartSize = 0.6f, EndSize = 3.0f;
	constexpr float StartAlpha = 0.30f;
	constexpr float LowLife = 1.2f, HighLife = 2.4f;
	/** Negative gravity: warm smoke keeps rising (m/s^2). */
	constexpr float Rise = 0.45f;
}

ATireSmoke::ATireSmoke()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	Quads = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Puffs"));
	SetRootComponent(Quads);
	Quads->SetMobility(EComponentMobility::Movable);
	Quads->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Quads->SetCastShadow(false);
	Quads->NumCustomDataFloats = 2;   // opacity, atlas cell
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (Plane.Succeeded()) Quads->SetStaticMesh(Plane.Object);
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Mat(TEXT("/Game/Materials/M_Smoke.M_Smoke"));
	if (Mat.Succeeded()) Quads->SetMaterial(0, Mat.Object);
}

void ATireSmoke::Clear()
{
	Puffs.Reset();
	Carry.Reset();
	Quads->ClearInstances();
}

void ATireSmoke::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.1f);

	// ---- emit from every sliding wheel ----
	for (ARaceCar* Car : Cars)
	{
		if (!IsValid(Car)) continue;
		const FCarSnapshot S = Car->GetSnapshot();
		FVector4f& Acc = Carry.FindOrAdd(Car);
		const FTransform Com = Car->GetComTransform();
		const FVector CarVel = Car->GetVelocity() / 100.0;
		for (int32 i = 0; i < 4; i++)
		{
			const float Amount = S.WheelSmoke[i];
			if (Amount <= 0.f) { Acc[i] = 0.f; continue; }
			Acc[i] += Amount * MaxRate * Dt;
			const int32 N = (int32)Acc[i];
			if (N == 0) continue;
			Acc[i] -= N;
			const FVector Patch = Com.TransformPosition(S.WheelPatch[i]) / 100.0;
			// shed slowly: a little of the car's motion, a lift, and random spread
			const FVector Base = CarVel * 0.04f + FVector(0, 0, 1.3f + 0.5f * Amount);
			for (int32 k = 0; k < N && Puffs.Num() < MaxParticles; k++)
			{
				FPuff P;
				// anywhere in a small box above the contact patch, so the cloud has a ragged outline
				P.Pos = Patch + FVector(Rnd.FRandRange(-0.4f, 0.4f), Rnd.FRandRange(-0.4f, 0.4f), Rnd.FRandRange(0.f, 0.45f));
				P.Vel = Base + Rnd.GetUnitVector() * Base.Size() * 0.35f;
				P.Life = Rnd.FRandRange(LowLife, HighLife);
				P.Angle = Rnd.FRandRange(0.f, 360.f);
				P.Spin = Rnd.FRandRange(-0.6f, 0.6f) * 57.3f;
				P.Cell = Rnd.RandRange(0, 3);
				Puffs.Add(P);
			}
		}
	}

	// ---- age, move ----
	for (int32 i = Puffs.Num() - 1; i >= 0; i--)
	{
		FPuff& P = Puffs[i];
		P.Age += Dt;
		if (P.Age >= P.Life)
		{
			Puffs.RemoveAtSwap(i);
			continue;
		}
		P.Vel.Z += Rise * Dt;
		P.Pos += P.Vel * Dt;
		P.Angle += P.Spin * Dt;
	}

	// ---- draw: camera-facing quads ----
	FVector CamPos = FVector::ZeroVector;
	if (APlayerController* Pc = UGameplayStatics::GetPlayerController(this, 0))
	{
		if (Pc->PlayerCameraManager) CamPos = Pc->PlayerCameraManager->GetCameraLocation();
	}
	if (Quads->GetInstanceCount() != Puffs.Num())
	{
		Quads->ClearInstances();
		TArray<FTransform> Init;
		Init.SetNum(Puffs.Num());
		if (Puffs.Num() > 0) Quads->AddInstances(Init, false, true);
	}
	TArray<FTransform> Xf;
	Xf.SetNum(Puffs.Num());
	for (int32 i = 0; i < Puffs.Num(); i++)
	{
		const FPuff& P = Puffs[i];
		const float T = P.Age / P.Life;
		const float Size = FMath::Lerp(StartSize, EndSize, T);
		const FVector Pos = P.Pos * 100.0;
		// the engine plane faces +Z: turn +Z toward the camera, then roll it
		const FVector ToCam = (CamPos - Pos).GetSafeNormal();
		const FQuat Face = FRotationMatrix::MakeFromZ(ToCam).ToQuat() * FQuat(FVector::UpVector, FMath::DegreesToRadians(P.Angle));
		Xf[i] = FTransform(Face, Pos, FVector(Size, Size, 1.f));   // the plane is 1 m across
		Quads->SetCustomDataValue(i, 0, StartAlpha * (1.f - T), false);
		Quads->SetCustomDataValue(i, 1, (float)P.Cell, false);
	}
	if (Puffs.Num() > 0) Quads->BatchUpdateInstancesTransforms(0, Xf, true, true, true);
}
