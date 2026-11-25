#include "Track/Scenery.h"
#include "Track/OvalTrack.h"
#include "Track/MeshBuilder.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"

namespace
{
	constexpr float Module = 40.f;
	constexpr float NearTreelineD = 330.f, NearTreelineH = 22.f;
	constexpr float FarTreelineD = 560.f, FarTreelineH = 30.f;
	enum { Oak = 0, Maple = 1, Spruce = 2, Young = 3, Shrub = 4, NumSpecies = 5 };
	constexpr int32 NumTints = 3;
	/** Leaf color multipliers: natural, darker / bluer, lighter / yellower. */
	const FLinearColor LeafTints[NumTints] = {
		FLinearColor(0.86f, 0.88f, 0.80f, 1.f), FLinearColor(0.70f, 0.80f, 0.76f, 1.f), FLinearColor(0.98f, 0.94f, 0.70f, 1.f) };
	const TCHAR* TreeFiles[NumSpecies] = { TEXT("tree_0"), TEXT("tree_1"), TEXT("tree_2"), TEXT("tree_3"), TEXT("shrub_0") };

	const TCHAR* Surf = TEXT("/Game/Materials/M_Surface.M_Surface");
	const TCHAR* SurfTwo = TEXT("/Game/Materials/M_SurfaceTwoSided.M_SurfaceTwoSided");
	const TCHAR* Masked = TEXT("/Game/Materials/M_Fence.M_Fence");

	FString Tex(const TCHAR* Folder, const TCHAR* Name) { return FString::Printf(TEXT("/Game/Textures/%s/%s.%s"), Folder, Name, Name); }
}

AScenery::AScenery()
{
	PrimaryActorTick.bCanEverTick = false;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Static);
	SetRootComponent(Root);
}

UStaticMesh* AScenery::LoadMesh(const TCHAR* Name)
{
	const FString Path = FString::Printf(TEXT("/Game/Models/Props/%s_mesh.%s_mesh"), Name, Name);
	UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, *Path);
	if (!M) UE_LOG(LogTemp, Warning, TEXT("Prop %s not found; run Scripts/setup_content.py"), *Path);
	return M;
}

UMaterialInterface* AScenery::MakeMid(const TCHAR* Parent, const TCHAR* Albedo, const TCHAR* Normal, const TCHAR* Rough,
	const FLinearColor& Tint, float Metallic, float Roughness, const FLinearColor& Emissive)
{
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, Parent);
	if (!Base) return nullptr;
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
	auto SetTex = [Mid](const TCHAR* Param, const TCHAR* Path)
	{
		if (!Path) return false;
		UTexture* T = LoadObject<UTexture>(nullptr, Path);
		if (T) Mid->SetTextureParameterValue(Param, T);
		return T != nullptr;
	};
	Mid->SetScalarParameterValue(TEXT("UseAlbedo"), SetTex(TEXT("Albedo"), Albedo) ? 1.f : 0.f);
	Mid->SetScalarParameterValue(TEXT("UseNormal"), SetTex(TEXT("Normal"), Normal) ? 1.f : 0.f);
	Mid->SetScalarParameterValue(TEXT("UseRoughTex"), SetTex(TEXT("RoughTex"), Rough) ? 1.f : 0.f);
	Mid->SetVectorParameterValue(TEXT("Tint"), Tint);
	Mid->SetScalarParameterValue(TEXT("Metallic"), Metallic);
	Mid->SetScalarParameterValue(TEXT("Roughness"), Roughness);
	Mid->SetVectorParameterValue(TEXT("Emissive"), Emissive);
	return Mid;
}

UMaterialInterface* AScenery::MaterialFor(const FString& Key, int32 LeafTint)
{
	const FString CacheKey = Key == TEXT("Leaves") ? FString::Printf(TEXT("Leaves%d"), LeafTint) : Key;
	if (TObjectPtr<UMaterialInterface>* Found = Materials.Find(CacheKey)) return *Found;
	UMaterialInterface* M = nullptr;
	const FString Wall = Tex(TEXT("Track"), TEXT("wall_albedo")), WallN = Tex(TEXT("Track"), TEXT("wall_normal")), WallR = Tex(TEXT("Track"), TEXT("wall_rough"));
	if (Key == TEXT("Concrete")) M = MakeMid(SurfTwo, *Wall, *WallN, *WallR, FLinearColor::White, 0.f, 1.f);
	else if (Key == TEXT("Crowd")) M = MakeMid(Masked, *Tex(TEXT("Track"), TEXT("crowd_atlas")), nullptr, nullptr, FLinearColor::White, 0.f, 0.9f);
	else if (Key == TEXT("Steel")) M = MakeMid(Surf, nullptr, nullptr, nullptr, FLinearColor(0.62f, 0.63f, 0.65f), 0.9f, 0.35f);
	else if (Key == TEXT("Alu")) M = MakeMid(Surf, nullptr, nullptr, nullptr, FLinearColor(0.74f, 0.75f, 0.77f), 0.85f, 0.42f);
	else if (Key == TEXT("Banner")) M = MakeMid(SurfTwo, *Tex(TEXT("Generated"), TEXT("banner")), nullptr, nullptr, FLinearColor::White, 0.f, 0.45f);
	else if (Key == TEXT("Roof")) M = MakeMid(SurfTwo, nullptr, nullptr, nullptr, FLinearColor(0.25f, 0.26f, 0.28f), 0.4f, 0.55f);
	else if (Key == TEXT("Fascia")) M = MakeMid(Surf, nullptr, nullptr, nullptr, FLinearColor(0.07f, 0.20f, 0.58f), 0.2f, 0.35f);
	else if (Key == TEXT("Glass")) M = MakeMid(Surf, nullptr, nullptr, nullptr, FLinearColor(0.10f, 0.13f, 0.16f), 0.95f, 0.06f);
	else if (Key == TEXT("Screen")) M = MakeMid(Surf, nullptr, nullptr, nullptr, FLinearColor(0.1f, 0.1f, 0.12f), 0.f, 0.3f, FLinearColor(0.25f, 0.45f, 0.9f));
	else if (Key == TEXT("Lamp")) M = MakeMid(Surf, nullptr, nullptr, nullptr, FLinearColor(1.f, 0.95f, 0.85f), 0.f, 0.2f, FLinearColor(1.f, 0.92f, 0.75f));
	else if (Key == TEXT("Bark")) M = MakeMid(Surf, *Tex(TEXT("Props"), TEXT("bark_albedo")), *Tex(TEXT("Props"), TEXT("bark_normal")), nullptr, FLinearColor::White, 0.f, 0.92f);
	else if (Key == TEXT("Leaves")) M = MakeMid(Masked, *Tex(TEXT("Props"), TEXT("leaves_atlas")), nullptr, nullptr, LeafTints[LeafTint], 0.f, 0.82f);
	else if (Key == TEXT("Canvas")) M = MakeMid(SurfTwo, nullptr, nullptr, nullptr, FLinearColor(0.95f, 0.95f, 0.93f), 0.f, 0.8f);
	else M = MakeMid(Surf, nullptr, nullptr, nullptr, FLinearColor(0.92f, 0.92f, 0.92f), 0.f, 0.4f);   // Paint
	Materials.Add(CacheKey, M);
	return M;
}

void AScenery::ApplyMaterials(UStaticMeshComponent* Comp, int32 LeafTint)
{
	// the Blender material names are the slot names; "Concrete_x" -> "Concrete"
	const TArray<FName> Slots = Comp->GetMaterialSlotNames();
	for (int32 i = 0; i < Slots.Num(); i++)
	{
		FString Key = Slots[i].ToString();
		int32 Underscore;
		if (Key.FindChar(TEXT('_'), Underscore)) Key = Key.Left(Underscore);
		if (UMaterialInterface* M = MaterialFor(Key, LeafTint)) Comp->SetMaterial(i, M);
	}
}

void AScenery::Place(UStaticMesh* Mesh, float S, float D, bool bOutside)
{
	// the models' local frame is the Java one: +X along the track, +Z (Unreal +Y) toward it
	const FTrackSpline& Spline = Track->GetSpline();
	const FVector F = Spline.Forward(S), R = Spline.Right(S);
	FVector Pos = Spline.Centerline(S) + R * D;
	Pos.Z = GroundAt(Pos);
	Occupied.Add(Pos);
	if (!Mesh) return;
	const FMatrix Axes = bOutside ? FMatrix(-F, -R, FVector::UpVector, FVector::ZeroVector) : FMatrix(F, R, FVector::UpVector, FVector::ZeroVector);
	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(this);
	Comp->SetStaticMesh(Mesh);
	Comp->SetMobility(EComponentMobility::Static);
	Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Comp->SetWorldTransform(FTransform(Axes.ToQuat(), Pos * 100.0));
	Comp->SetupAttachment(GetRootComponent());
	ApplyMaterials(Comp, 0);
	Comp->RegisterComponent();
}

void AScenery::PlaceChord(UStaticMesh* Mesh, float SLo, float SHi, float D)
{
	// a straight outside-facing module along the chord between sLo and sHi (in a turn),
	// stretched a touch so neighboring modules close up at the back
	const FTrackSpline& Spline = Track->GetSpline();
	FVector Hi = Spline.Centerline(SHi) + Spline.Right(SHi) * D;
	FVector Lo = Spline.Centerline(SLo) + Spline.Right(SLo) * D;
	Hi.Z = Lo.Z = FMath::Min(GroundAt(Hi), GroundAt(Lo));
	FVector X = Lo - Hi;
	const float Len = X.Size();
	X /= Len;
	const FVector Y = -FVector::CrossProduct(X, FVector::UpVector);
	Occupied.Add((Hi + Lo) * 0.5);
	if (!Mesh) return;
	UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(this);
	Comp->SetStaticMesh(Mesh);
	Comp->SetMobility(EComponentMobility::Static);
	Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Comp->SetWorldTransform(FTransform(FMatrix(X, Y, FVector::UpVector, FVector::ZeroVector).ToQuat(), Hi * 100.0, FVector(Len / Module * 1.04f, 1, 1)));
	Comp->SetupAttachment(GetRootComponent());
	ApplyMaterials(Comp, 0);
	Comp->RegisterComponent();
}

float AScenery::GroundAt(const FVector& Pos) const
{
	return FTrackSpline::IsOval() ? 0.f : Track->GroundHeight(Pos.X, Pos.Y) - 0.1f;
}

void AScenery::MakeTreeComponents()
{
	// one instanced mesh per species and leaf tint
	for (int32 Sp = 0; Sp < NumSpecies; Sp++)
	{
		TreeMeshes.Add(LoadMesh(TreeFiles[Sp]));
		for (int32 Ti = 0; Ti < NumTints; Ti++)
		{
			UHierarchicalInstancedStaticMeshComponent* H = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
			H->SetStaticMesh(TreeMeshes.Last());
			H->SetMobility(EComponentMobility::Static);
			H->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			H->SetCullDistances(0, 120000);
			H->SetupAttachment(GetRootComponent());
			if (TreeMeshes.Last()) ApplyMaterials(H, Ti);
			H->RegisterComponent();
			TreeInstances.Add(H);
		}
	}
}

void AScenery::PlaceStands(UStaticMesh* Mesh, float S0, float S1, bool bRight, float Back)
{
	// grandstand modules along s0..s1 just behind the wall on one side, following the bends
	const FTrackSpline& Spline = Track->GetSpline();
	for (float S = S0; S + Module <= S1 + 1.f; S += Module)
	{
		float D = bRight ? -1e9f : 1e9f;
		for (float Q = S; Q <= S + Module; Q += 4.f) D = bRight ? FMath::Max(D, Spline.MaxD(Q)) : FMath::Min(D, Spline.MinD(Q));
		D += bRight ? Back : -Back;
		if (bRight) PlaceChord(Mesh, FTrackSpline::WrapS(S), FTrackSpline::WrapS(S + Module), D);
		else PlaceChord(Mesh, FTrackSpline::WrapS(S + Module), FTrackSpline::WrapS(S), D);
	}
}

int32 AScenery::BuildRoadCourse()
{
	// stands along the start / finish straight and round the first-turn hairpin, the paddock and
	// timing tower on the left of the grid, stands beside the valley sweep and at the last
	// chicane, and forest everywhere else
	const FTrackSpline& Spline = Track->GetSpline();
	UStaticMesh* Grandstand = LoadMesh(TEXT("grandstand"));
	PlaceStands(Grandstand, -240.f, 40.f, true, 4.f);       // opposite the pits
	PlaceStands(Grandstand, 120.f, 280.f, false, 5.f);      // round the outside of the first-turn hairpin
	PlaceStands(Grandstand, 700.f, 860.f, false, 6.f);      // the valley sweep
	PlaceStands(Grandstand, 6580.f, 6700.f, true, 5.f);     // the last chicane
	{
		float D = 1e9f;
		for (float Q = -160.f; Q <= 40.f; Q += 5.f) D = FMath::Min(D, Spline.MinD(Q));
		Place(LoadMesh(TEXT("timing_tower")), FTrackSpline::WrapS(-60.f), D - 30.f, false);
		UStaticMesh* Tent = LoadMesh(TEXT("tent"));
		for (int32 i = 0; i < 6; i++) Place(Tent, FTrackSpline::WrapS(-200.f + i * 22.f), D - 22.f - (i % 2) * 14.f, false);
	}
	MakeTreeComponents();
	const int32 Planted = PlantForest();
	for (UHierarchicalInstancedStaticMeshComponent* H : TreeInstances) H->BuildTreeIfOutdated(false, true);
	UE_LOG(LogTemp, Display, TEXT("Scenery: %d trees and shrubs"), Planted);
	return Planted;
}

int32 AScenery::PlantForest()
{
	// spruce and broadleaf woods in patches on the hills, from a little way past the barriers to
	// where the terrain turns to distant forest
	const FTrackSpline& Spline = Track->GetSpline();
	float MinX = TNumericLimits<float>::Max(), MaxX = -MinX, MinY = MinX, MaxY = -MinX;
	for (float S = 0; S < FTrackSpline::Length; S += 10.f)
	{
		const FVector Pc = Spline.Centerline(S);
		MinX = FMath::Min(MinX, (float)Pc.X); MaxX = FMath::Max(MaxX, (float)Pc.X);
		MinY = FMath::Min(MinY, (float)Pc.Y); MaxY = FMath::Max(MaxY, (float)Pc.Y);
	}
	int32 Planted = 0;
	const float Step = 15.f, Reach = 330.f;
	for (float X = MinX - Reach; X < MaxX + Reach; X += Step)
	{
		for (float Y = MinY - Reach; Y < MaxY + Reach; Y += Step)
		{
			const float Px = X + (Rnd.NextFloat() - 0.5f) * Step * 0.8f;
			const float Py = Y + (Rnd.NextFloat() - 0.5f) * Step * 0.8f;
			float S;
			const float Beyond = Track->BeyondWall(Px, Py, S);
			if (Beyond < 14.f || Beyond > Reach) continue;
			const float Patches = FGroundShade::Fbm(Px / 170.f, Py / 170.f, 3, 51);
			const float Density = FTrackSpline::Smooth((Patches + 0.3f * FTrackSpline::Smooth((Beyond - 14.f) / 90.f) - 0.42f) / 0.2f);
			if (Rnd.NextFloat() > Density * 0.85f) continue;
			FVector Pos(Px, Py, 0);
			if (TooClose(Pos, 18.f)) continue;
			Pos.Z = GroundAt(Pos);
			const float U = Rnd.NextFloat();
			const int32 Species = U < 0.45f ? Spruce : U < 0.68f ? Oak : U < 0.88f ? Maple : Young;
			const float Scale = 0.85f + Rnd.NextFloat() * 0.45f;
			Planted += Plant(Species, Pos, Scale);
		}
	}
	return Planted;
}

int32 AScenery::Build(const AOvalTrack* InTrack)
{
	Track = InTrack;
	using T = FTrackSpline;
	const FTrackSpline& Spline = Track->GetSpline();
	Center = Spline.GetCenter();
	if (FTrackSpline::IsRoadCourse()) return BuildRoadCourse();

	UStaticMesh* Grandstand = LoadMesh(TEXT("grandstand"));
	// front straight: the whole straight, so the starting grid always has stands behind it
	for (float S = -T::Straight / 2; S + Module <= T::Straight / 2 + 1.f; S += Module)
	{
		Place(Grandstand, T::WrapS(S + Module), 14.f, true);
	}
	// ...and a little way round the turns at either end, so the stands curve away with the wall
	for (int32 i = 1; i <= 3; i++)
	{
		const float SIn = -T::Straight / 2 - i * Module;
		PlaceChord(Grandstand, T::WrapS(SIn), T::WrapS(SIn + Module), 14.f);
		const float SOut = T::Straight / 2 + (i - 1) * Module;
		PlaceChord(Grandstand, SOut, SOut + Module, 14.f);
	}
	const float BackStart = T::Straight / 2 + 2 * T::Turn + T::Chute;
	for (float S = BackStart + 160.f; S < BackStart + T::Straight - 160.f; S += Module)
	{
		Place(Grandstand, S + Module, 14.f, true);
	}
	Place(LoadMesh(TEXT("timing_tower")), 70.f, -62.f, false);
	UStaticMesh* Tent = LoadMesh(TEXT("tent"));
	for (int32 i = 0; i < 6; i++) Place(Tent, 180.f + i * 18.f, -78.f - (i % 2) * 14.f, false);
	UStaticMesh* Tower = LoadMesh(TEXT("light_tower"));
	for (float S = 250.f; S < T::Length; S += 420.f)
	{
		Place(Tower, S, Spline.InTurn(S) ? 34.f : 32.f, true);
		const float SIn = S + 210.f;
		Place(Tower, SIn, FMath::Min(-32.f, PitLane::InnerLimit(SIn) - 12.f), false);
	}

	MakeTreeComponents();
	const int32 Planted = PlantTrees();
	for (UHierarchicalInstancedStaticMeshComponent* H : TreeInstances) H->BuildTreeIfOutdated(false, true);

	Treeline(NearTreelineD, NearTreelineH, nullptr, 0.f);
	const FLinearColor Haze(0.035f, 0.045f, 0.06f, 1.f);
	Treeline(FarTreelineD, FarTreelineH, &Haze, 0.37f);
	UE_LOG(LogTemp, Display, TEXT("Scenery: %d trees and shrubs"), Planted);
	return Planted;
}

// ---------------------------------------------------------------------------------- trees

int32 AScenery::Plant(int32 Species, const FVector& Pos, float Scale)
{
	// the random draws happen in the Java order: tint, yaw, height variation
	const int32 Tint = Rnd.NextInt(NumTints);
	const float Yaw = Rnd.NextFloat() * 360.f;
	const float Tall = 0.9f + Rnd.NextFloat() * 0.2f;
	UHierarchicalInstancedStaticMeshComponent* H = TreeInstances[Species * NumTints + Tint];
	const float Z = FTrackSpline::IsOval() ? -0.05f : Pos.Z;
	H->AddInstance(FTransform(FRotator(0, Yaw, 0), FVector(Pos.X, Pos.Y, Z) * 100.0, FVector(Scale, Scale, Scale * Tall)), true);
	return 1;
}

bool AScenery::OnServiceRoad(const FVector& P) const
{
	// the infield service roads: a cross through the center
	const float Dx = FMath::Abs(P.X - Center.X), Dy = FMath::Abs(P.Y - Center.Y);
	return (Dy < 12.f && Dx < 610.f) || (Dx < 12.f && Dy < 290.f);
}

bool AScenery::TooClose(const FVector& P, float Min) const
{
	for (const FVector& O : Occupied)
	{
		if (FVector::DistSquared(O, P) < Min * Min) return true;
	}
	return false;
}

bool AScenery::InfieldClear(const FVector& Pos, float Margin) const
{
	const FTrackSpline::FTrackPos Tp = Track->GetSpline().Project(Pos);
	return Tp.D < PitLane::InnerLimit(Tp.S) - Margin && !TooClose(Pos, 20.f) && !OnServiceRoad(Pos);
}

bool AScenery::InfieldPoint(float Margin, FVector& Out)
{
	const FTrackSpline& Spline = Track->GetSpline();
	for (int32 Attempt = 0; Attempt < 40; Attempt++)
	{
		const float S = Rnd.NextFloat() * FTrackSpline::Length;
		const float D = -(Margin + Rnd.NextFloat() * 330.f);
		FVector Pos = Spline.Centerline(S) + Spline.Right(S) * D;
		Pos.Z = 0;
		if (InfieldClear(Pos, Margin))
		{
			Out = Pos;
			return true;
		}
	}
	return false;
}

int32 AScenery::PlantTrees()
{
	// Java evaluates a call's arguments (and their random draws) before the call's own draws;
	// the order of every Rnd call below follows the Java source exactly
	const FTrackSpline& Spline = Track->GetSpline();
	const FGroundShade& Ground = Track->GetGroundShade();
	int32 Planted = 0;
	float MinX = TNumericLimits<float>::Max(), MaxX = -MinX, MinY = MinX, MaxY = -MinX;
	for (float S = 0; S < FTrackSpline::Length; S += 10.f)
	{
		const FVector Pc = Spline.Centerline(S);
		MinX = FMath::Min(MinX, (float)Pc.X); MaxX = FMath::Max(MaxX, (float)Pc.X);
		MinY = FMath::Min(MinY, (float)Pc.Y); MaxY = FMath::Max(MaxY, (float)Pc.Y);
	}
	// ---- woods outside the track (jittered grid) ----
	const float Step = 12.5f;
	for (float X = MinX - 300.f; X < MaxX + 300.f; X += Step)
	{
		for (float Y = MinY - 300.f; Y < MaxY + 300.f; Y += Step)
		{
			const float Px = X + (Rnd.NextFloat() - 0.5f) * Step * 0.8f;
			const float Py = Y + (Rnd.NextFloat() - 0.5f) * Step * 0.8f;
			const float D = Ground.OffsetAt(Px, Py);
			if (D < 90.f || D > NearTreelineD - 25.f) continue;
			const float Density = Ground.WoodDensity(Px, Py, D);
			if (Rnd.NextFloat() > Density * 0.75f) continue;
			const FVector Pos(Px, Py, 0);
			if (TooClose(Pos, 18.f) || OnServiceRoad(Pos)) continue;
			const float U = Rnd.NextFloat();
			const int32 Species = U < 0.32f ? Oak : U < 0.60f ? Maple : U < 0.86f ? Spruce : Young;
			const float Scale = 0.8f + Rnd.NextFloat() * 0.45f;
			Planted += Plant(Species, Pos, Scale);
			// undergrowth where the wood thins out toward the lawn
			if (Density < 0.6f && Rnd.NextFloat() < 0.55f)
			{
				FVector ToTrack = Spline.Centerline(Spline.Project(Pos).S) - Pos;
				ToTrack.Z = 0;
				ToTrack = ToTrack.GetSafeNormal() * (4.f + Rnd.NextFloat() * 3.f);
				const float Jx = Rnd.NextFloat() * 3 - 1.5f;
				const float Jy = Rnd.NextFloat() * 3 - 1.5f;
				const float ShrubScale = 0.8f + Rnd.NextFloat() * 0.6f;
				Planted += Plant(Shrub, Pos + ToTrack + FVector(Jx, Jy, 0), ShrubScale);
			}
		}
	}
	// ---- lone trees on the outer lawns ----
	for (int32 i = 0; i < 70; i++)
	{
		const float S = Rnd.NextFloat() * FTrackSpline::Length, D = 60.f + Rnd.NextFloat() * 50.f;
		FVector Pos = Spline.Centerline(S) + Spline.Right(S) * D;
		Pos.Z = 0;
		if (TooClose(Pos, 22.f) || OnServiceRoad(Pos)) continue;
		const int32 Species = Rnd.NextFloat() < 0.6f ? Oak : Maple;
		const float Scale = 1.0f + Rnd.NextFloat() * 0.35f;
		Planted += Plant(Species, Pos, Scale);
	}
	// ---- infield parkland: clumps, lone oaks, shrub groups ----
	for (int32 K = 0; K < 34; K++)
	{
		FVector C;
		if (!InfieldPoint(45.f, C)) continue;
		const int32 N = 4 + Rnd.NextInt(12);
		const float Radius = 10.f + N * 1.6f;
		for (int32 i = 0; i < N; i++)
		{
			const float A = Rnd.NextFloat() * UE_TWO_PI, Rr = Radius * FMath::Sqrt(Rnd.NextFloat());
			const FVector Pos = C + FVector(FMath::Cos(A) * Rr, FMath::Sin(A) * Rr, 0);
			if (!InfieldClear(Pos, 40.f)) continue;
			const float U = Rnd.NextFloat();
			const int32 Species = U < 0.45f ? Oak : U < 0.8f ? Maple : U < 0.9f ? Young : Spruce;
			const float Scale = 0.85f + Rnd.NextFloat() * 0.4f;
			Planted += Plant(Species, Pos, Scale);
		}
		for (int32 i = 0; i < 3 + Rnd.NextInt(5); i++)   // shrubs around the clump (the bound is redrawn each pass, as in Java)
		{
			const float A = Rnd.NextFloat() * UE_TWO_PI;
			const FVector Pos = C + FVector(FMath::Cos(A) * (Radius + 4.f), FMath::Sin(A) * (Radius + 4.f), 0);
			if (InfieldClear(Pos, 35.f))
			{
				const float Scale = 0.8f + Rnd.NextFloat() * 0.6f;
				Planted += Plant(Shrub, Pos, Scale);
			}
		}
	}
	for (int32 i = 0; i < 45; i++)
	{
		FVector Pos;
		if (!InfieldPoint(45.f, Pos)) continue;
		const int32 Species = Rnd.NextFloat() < 0.7f ? Oak : Maple;
		const float Scale = 1.1f + Rnd.NextFloat() * 0.35f;
		Planted += Plant(Species, Pos, Scale);
	}
	return Planted;
}

void AScenery::Treeline(float D, float Height, const FLinearColor* Haze, float UShift)
{
	// a distant wood edge: a vertical band following the track at offset d, textured with the
	// alpha-tested tree-line silhouette (the texture is 4:1)
	const FTrackSpline& Spline = Track->GetSpline();
	FMeshBuilder Mb;
	const float Step = 6.f, TexWidth = Height * 4.f;
	float Along = 0.f;
	bool bHavePrev = false;
	FVector Prev;
	for (float S = 0; S <= FTrackSpline::Length + 0.01f; S += Step)
	{
		FVector P = Spline.Centerline(S) + Spline.Right(S) * D;
		P.Z = -0.3f;
		if (bHavePrev)
		{
			const float A0 = Along / TexWidth + UShift;
			Along += FVector::Dist(Prev, P);
			const float A1 = Along / TexWidth + UShift;
			const float Uv[8] = { A0, 1.f, A1, 1.f, A1, 0.f, A0, 0.f };   // PNG row 0 (top) is v = 0
			Mb.Quad(Prev, P, P + FVector(0, 0, Height), Prev + FVector(0, 0, Height), FLinearColor::White, -Spline.Right(S), Uv);
		}
		Prev = P;
		bHavePrev = true;
	}
	UProceduralMeshComponent* Pmc = NewObject<UProceduralMeshComponent>(this);
	Pmc->SetupAttachment(GetRootComponent());
	Pmc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Pmc->SetCastShadow(false);
	Pmc->RegisterComponent();
	Mb.AddTo(Pmc, 0, false);
	Pmc->SetMaterial(0, MakeMid(Masked, *Tex(TEXT("Props"), TEXT("treeline")), nullptr, nullptr, FLinearColor::White, 0.f, 0.95f,
		Haze ? *Haze : FLinearColor::Black));
}
