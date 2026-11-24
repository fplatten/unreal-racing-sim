#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Sim/TrackSpline.h"
#include "Sim/VehicleSim.h"
#include "Sim/RaceLogic.h"
#include "Track/GroundShade.h"
#include "OvalTrack.generated.h"

class UProceduralMeshComponent;
class UBoxComponent;
class UMaterialInterface;

/**
 * The oval: geometry (TrackSpline), what the wheels stand on (FTrackSurface) and the
 * procedurally built meshes and colliders (port of com.racingsim.track.TrackBuilder).
 */
UCLASS()
class RACINGSIM_API AOvalTrack : public AActor
{
	GENERATED_BODY()

public:
	AOvalTrack();

	const FTrackSpline& GetSpline() const { return Spline; }
	const FTrackSurface& GetSurface() const { return Surface; }

	/** The track in this world (spawned by the game mode). */
	static AOvalTrack* Get(const UWorld* World);

	/** Sub-mesh (section) indices of the track collider. */
	enum ESection { SecAsphalt = 0, SecPaint = 1, SecGrass = 2, SecWalls = 3, SecFence = 4, SecAsphaltVisible = 5 };

	const FGroundShade& GetGroundShade() const { return *Shade; }

	/** Builds meshes and colliders (called once by BeginPlay). */
	void Build();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	FVector P(float S, float D, float Above = 0.f) const;
	static FVector AtZ(const FVector& V, float Z) { return FVector(V.X, V.Y, Z); }

	void BuildSurface(class FMeshBuilder& Asphalt, FMeshBuilder& AsphaltVis, FMeshBuilder& Paint, FMeshBuilder& Grass, FMeshBuilder& Wall) const;
	void BuildGround();
	float Groove(float S, float D) const;
	float SurfaceVariation(float S, float D) const;
	float GrooveShade(float S, float D) const;
	void PaintPitLane(FMeshBuilder& Paint, float Step) const;
	void StripeAlong(FMeshBuilder& Paint, float S0, float S1, float D, float Width, const FLinearColor& Color) const;
	void StripeAcross(FMeshBuilder& Paint, float S0, float S1, float D0, float D1, const FLinearColor& Color) const;
	void BuildFence(FMeshBuilder& Fence) const;
	void BuildDecorations(FMeshBuilder& Deco) const;
	FLinearColor GroundColor(const FVector& P) const;

	UMaterialInterface* MakeMaterial(const TCHAR* Parent, const TCHAR* Albedo, const TCHAR* Normal, const TCHAR* Rough,
		float Roughness, float Metallic, const FLinearColor& Tint);

	FTrackSpline Spline;
	FTrackSurface Surface;
	TUniquePtr<FGroundShade> Shade;
	TUniquePtr<FRacingLine> Line;
	bool bBuilt = false;
	class FWallContactFilter* WallFilter = nullptr;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> TrackMesh;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> DecoMesh;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> GroundMesh;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UBoxComponent> GroundCollider;
};
