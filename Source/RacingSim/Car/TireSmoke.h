#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TireSmoke.generated.h"

class ARaceCar;
class UInstancedStaticMeshComponent;

/**
 * Port of com.racingsim.vehicle.TireSmoke: when a tire slides sideways past its grip peak (or
 * locks up / spins hard), puffs of smoke billow from its contact patch, then grow, drift up and
 * fade. One shared world-space pool of puffs serves every car; fractional emission carries over
 * between frames, so a light slide gives a thin wisp and a big slide a thick cloud. Puffs are
 * camera-facing instances of one quad (the 2 x 2 puff atlas baked from the Java painter).
 */
UCLASS()
class RACINGSIM_API ATireSmoke : public AActor
{
	GENERATED_BODY()

public:
	ATireSmoke();
	virtual void Tick(float DeltaSeconds) override;

	void SetCars(const TArray<TObjectPtr<ARaceCar>>& InCars) { Cars = InCars; }
	/** Clears all smoke (race restart). */
	void Clear();

private:
	struct FPuff
	{
		FVector Pos;   // m
		FVector Vel;   // m/s
		float Age = 0.f;
		float Life = 1.f;
		float Angle = 0.f;
		float Spin = 0.f;
		int32 Cell = 0;
	};

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> Quads;

	UPROPERTY()
	TArray<TObjectPtr<ARaceCar>> Cars;

	TArray<FPuff> Puffs;
	TMap<const ARaceCar*, FVector4f> Carry;
	FRandomStream Rnd{ 7 };
};
