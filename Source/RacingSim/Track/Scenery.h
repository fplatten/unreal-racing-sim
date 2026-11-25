#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/RaceLogic.h"
#include "Scenery.generated.h"

class AOvalTrack;
class UStaticMesh;
class UMaterialInterface;
class UHierarchicalInstancedStaticMeshComponent;
class UProceduralMeshComponent;

/**
 * Port of com.racingsim.track.Scenery: the Blender-made trackside props (blender/make_props.py)
 * - covered grandstands along both straights, the timing tower, hospitality tents, light towers -
 * and the woods: about 3,500 trees and shrubs placed exactly as in the Java version (same
 * Random(1990) sequence), plus two rings of tree-line silhouettes that close off the horizon.
 * Round the road course: stands at the start, the first-turn hairpin, the valley sweep and the
 * last chicane, the paddock,
 * and woods on the hills.
 */
UCLASS()
class RACINGSIM_API AScenery : public AActor
{
	GENERATED_BODY()

public:
	AScenery();

	/** Places everything around the given track. @return trees and shrubs planted */
	int32 Build(const AOvalTrack* Track);

private:
	UStaticMesh* LoadMesh(const TCHAR* Name);
	void ApplyMaterials(UStaticMeshComponent* Comp, int32 LeafTint);
	UMaterialInterface* MaterialFor(const FString& Key, int32 LeafTint);
	UMaterialInterface* MakeMid(const TCHAR* Parent, const TCHAR* Albedo, const TCHAR* Normal, const TCHAR* Rough,
		const FLinearColor& Tint, float Metallic, float Roughness, const FLinearColor& Emissive = FLinearColor::Black);
	void Place(UStaticMesh* Mesh, float S, float D, bool bOutside);
	void PlaceChord(UStaticMesh* Mesh, float SLo, float SHi, float D);
	int32 PlantTrees();
	int32 Plant(int32 Species, const FVector& Pos, float Scale);
	bool InfieldPoint(float Margin, FVector& Out);
	bool InfieldClear(const FVector& Pos, float Margin) const;
	bool OnServiceRoad(const FVector& P) const;
	bool TooClose(const FVector& P, float Min) const;
	void Treeline(float D, float Height, const FLinearColor* Haze, float UShift);
	void MakeTreeComponents();
	int32 BuildRoadCourse();
	int32 PlantForest();
	void PlaceStands(UStaticMesh* Mesh, float S0, float S1, bool bRight, float Back);
	/** Ground height (m) for a prop at a world position. */
	float GroundAt(const FVector& Pos) const;

	const AOvalTrack* Track = nullptr;
	FJavaRandom Rnd{ 1990 };
	TArray<FVector> Occupied;
	FVector Center = FVector::ZeroVector;

	UPROPERTY()
	TMap<FString, TObjectPtr<UMaterialInterface>> Materials;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> TreeMeshes;

	/** [species * 3 + tint] */
	UPROPERTY()
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> TreeInstances;
};
