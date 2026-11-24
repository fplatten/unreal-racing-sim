#include "Track/OvalTrack.h"
#include "Track/MeshBuilder.h"
#include "Track/WallContactFilter.h"
#include "Misc/CommandLine.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PBDRigidsSolver.h"
#include "Components/BoxComponent.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "ProceduralMeshComponent.h"

namespace
{
	constexpr float RowStep = 2.0f;
	constexpr float AsphaltTile = 3.f, GrassTile = 4.f, WallTile = 4.f;
	constexpr float FenceHeight = 4.2f;
	constexpr float AsphaltShade = 0.80f;
	constexpr float GroundZ = FTrackSurface::GroundZ;

	const FLinearColor White(1, 1, 1, 1);
	const FLinearColor PaintCol(0.92f, 0.92f, 0.90f, 1);
	const FLinearColor Steel(0.35f, 0.35f, 0.37f, 1);
	const FVector Up = FVector::UpVector;

	FLinearColor Gray(float G) { return FLinearColor(G, G, G, 1); }

	UPhysicalMaterial* MakePhysMat(UObject* Outer, float Friction, float Restitution)
	{
		UPhysicalMaterial* M = NewObject<UPhysicalMaterial>(Outer);
		M->Friction = Friction;
		M->StaticFriction = Friction;
		M->Restitution = Restitution;
		return M;
	}
}

AOvalTrack::AOvalTrack()
	: Surface(&Spline)
{
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Static);
	SetRootComponent(Root);

	TrackMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TrackMesh"));
	TrackMesh->SetupAttachment(Root);
	TrackMesh->bUseComplexAsSimpleCollision = true;
	TrackMesh->BodyInstance.bContactModification = true;
	TrackMesh->SetCollisionProfileName(TEXT("BlockAll"));
	TrackMesh->SetMobility(EComponentMobility::Static);

	DecoMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("DecoMesh"));
	DecoMesh->SetupAttachment(Root);
	DecoMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DecoMesh->SetMobility(EComponentMobility::Static);

	GroundMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GroundMesh"));
	GroundMesh->SetupAttachment(Root);
	GroundMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GroundMesh->SetCastShadow(false);
	GroundMesh->SetMobility(EComponentMobility::Static);

	// an "infinite" ground plane under everything (grass), so a car that leaves the track can
	// never fall out of the world
	GroundCollider = CreateDefaultSubobject<UBoxComponent>(TEXT("GroundCollider"));
	GroundCollider->SetupAttachment(Root);
	GroundCollider->SetCollisionProfileName(TEXT("BlockAll"));
	GroundCollider->SetMobility(EComponentMobility::Static);
	GroundCollider->SetHiddenInGame(true);
	// static components cannot move once registered: place it here (2 km square, top at GroundZ)
	const FVector C = Spline.GetCenter();
	GroundCollider->SetBoxExtent(FVector(2000.f * 100.f, 2000.f * 100.f, 100.f));
	GroundCollider->SetRelativeLocation(FVector(C.X * 100.f, C.Y * 100.f, GroundZ * 100.f - 100.f));
}

AOvalTrack* AOvalTrack::Get(const UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<AOvalTrack> It(World); It; ++It) return *It;
	return nullptr;
}

void AOvalTrack::BeginPlay()
{
	Super::BeginPlay();
	Build();
	// walls push cars sideways only (see FWallContactFilter)
	if (FPhysScene* Scene = FParse::Param(FCommandLine::Get(), TEXT("NoWallFilter")) ? nullptr : GetWorld()->GetPhysicsScene())
	{
		if (Chaos::FPhysicsSolver* Solver = Scene->GetSolver())
		{
			WallFilter = Solver->CreateAndRegisterSimCallbackObject_External<FWallContactFilter>();
			WallFilter->Track = &Spline;
		}
	}
}

void AOvalTrack::EndPlay(const EEndPlayReason::Type Reason)
{
	if (WallFilter)
	{
		if (FPhysScene* Scene = GetWorld()->GetPhysicsScene())
		{
			if (Chaos::FPhysicsSolver* Solver = Scene->GetSolver()) Solver->UnregisterAndFreeSimCallbackObject_External(WallFilter);
		}
		WallFilter = nullptr;
	}
	Super::EndPlay(Reason);
}

FVector AOvalTrack::P(float S, float D, float Above) const
{
	FVector V = Spline.Point(S, D);
	V.Z += Above;
	return V;
}

FLinearColor AOvalTrack::GroundColor(const FVector& Pos) const
{
	return Shade->Color(Pos.X, Pos.Y);
}

UMaterialInterface* AOvalTrack::MakeMaterial(const TCHAR* Parent, const TCHAR* Albedo, const TCHAR* Normal, const TCHAR* Rough,
	float Roughness, float Metallic, const FLinearColor& Tint)
{
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, Parent);
	if (!Base)
	{
		UE_LOG(LogTemp, Warning, TEXT("Track material %s not found; run Scripts/setup_content.py"), Parent);
		return nullptr;
	}
	UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
	auto SetTex = [Mid](const TCHAR* Param, const TCHAR* Path)
	{
		if (!Path) return;
		if (UTexture* T = LoadObject<UTexture>(nullptr, Path)) Mid->SetTextureParameterValue(Param, T);
		else UE_LOG(LogTemp, Warning, TEXT("Texture %s not found"), Path);
	};
	SetTex(TEXT("Albedo"), Albedo);
	SetTex(TEXT("Normal"), Normal);
	SetTex(TEXT("RoughTex"), Rough);
	Mid->SetScalarParameterValue(TEXT("UseAlbedo"), Albedo ? 1.f : 0.f);
	Mid->SetScalarParameterValue(TEXT("UseNormal"), Normal ? 1.f : 0.f);
	Mid->SetScalarParameterValue(TEXT("UseRoughTex"), Rough ? 1.f : 0.f);
	Mid->SetScalarParameterValue(TEXT("Roughness"), Roughness);
	Mid->SetScalarParameterValue(TEXT("Metallic"), Metallic);
	Mid->SetVectorParameterValue(TEXT("Tint"), Tint);
	return Mid;
}

void AOvalTrack::Build()
{
	if (bBuilt) return;
	bBuilt = true;
	Shade = MakeUnique<FGroundShade>(&Spline);
	Line = MakeUnique<FRacingLine>(&Spline);

	FMeshBuilder Asphalt, AsphaltVis, PaintMb, GrassMb, WallMb, FenceMb, Deco;
	BuildSurface(Asphalt, AsphaltVis, PaintMb, GrassMb, WallMb);
	BuildFence(FenceMb);
	BuildDecorations(Deco);

	// the surfaces double as the static collider, as in the Java version
	// the collider keeps the coarse road; the visible road is finely subdivided for the rubber groove
	Asphalt.AddTo(TrackMesh, SecAsphalt, true);
	TrackMesh->SetMeshSectionVisible(SecAsphalt, false);
	AsphaltVis.AddTo(TrackMesh, SecAsphaltVisible, false);
	PaintMb.AddTo(TrackMesh, SecPaint, true);
	GrassMb.AddTo(TrackMesh, SecGrass, true);
	WallMb.AddTo(TrackMesh, SecWalls, true);
	FenceMb.AddTo(TrackMesh, SecFence, true);
	Deco.AddTo(DecoMesh, 0, false);

	const TCHAR* Surf = TEXT("/Game/Materials/M_Surface.M_Surface");
	TrackMesh->SetMaterial(SecAsphaltVisible, MakeMaterial(Surf, TEXT("/Game/Textures/Track/asphalt_albedo.asphalt_albedo"),
		TEXT("/Game/Textures/Track/asphalt_normal.asphalt_normal"), TEXT("/Game/Textures/Track/asphalt_rough.asphalt_rough"), 1.f, 0.f, White));
	TrackMesh->SetMaterial(SecPaint, MakeMaterial(Surf, nullptr, TEXT("/Game/Textures/Track/asphalt_normal.asphalt_normal"), nullptr, 0.55f, 0.f, White));
	TrackMesh->SetMaterial(SecGrass, MakeMaterial(Surf, TEXT("/Game/Textures/Track/grass_albedo.grass_albedo"),
		TEXT("/Game/Textures/Track/grass_normal.grass_normal"), nullptr, 0.92f, 0.f, White));
	TrackMesh->SetMaterial(SecWalls, MakeMaterial(Surf, TEXT("/Game/Textures/Track/wall_albedo.wall_albedo"),
		TEXT("/Game/Textures/Track/wall_normal.wall_normal"), TEXT("/Game/Textures/Track/wall_rough.wall_rough"), 1.f, 0.f, White));
	TrackMesh->SetMaterial(SecFence, MakeMaterial(TEXT("/Game/Materials/M_Fence.M_Fence"), TEXT("/Game/Textures/Track/chainlink.chainlink"),
		nullptr, nullptr, 0.4f, 0.8f, FLinearColor(0.75f, 0.76f, 0.78f, 1)));
	DecoMesh->SetMaterial(0, MakeMaterial(Surf, nullptr, nullptr, nullptr, 0.45f, 0.6f, White));
	TrackMesh->SetPhysMaterialOverride(MakePhysMat(this, 0.6f, 0.1f));

	BuildGround();
	GroundCollider->SetPhysMaterialOverride(MakePhysMat(this, 0.5f, 0.05f));
}

void AOvalTrack::BuildSurface(FMeshBuilder& Asphalt, FMeshBuilder& AsphaltVis, FMeshBuilder& PaintMb, FMeshBuilder& GrassMb, FMeshBuilder& Wall) const
{
	// the same quads into the collider and the visible road (only the road band differs)
	auto Both = [&Asphalt, &AsphaltVis](const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FLinearColor& Col, const FVector& Hint, const float* Uv)
	{
		Asphalt.Quad(A, B, C, D, Col, Hint, Uv);
		AsphaltVis.Quad(A, B, C, D, Col, Hint, Uv);
	};
	using T = FTrackSpline;
	const int32 Rows = FMath::CeilToInt(T::Length / RowStep);
	const float Step = T::Length / Rows;
	const float WallOut = T::RoadOuter + T::OuterWallThickness;
	const float InnerWallBack = T::InnerWall - 0.4f;
	const float LineIn0 = T::RoadInner - 0.3f, LineOut0 = T::RoadOuter - 0.45f, LineOut1 = T::RoadOuter - 0.2f;
	const float RoadD[] = { T::RoadInner, -5.5f, -3.5f, -1.5f, 0.5f, 2.5f, 4.5f, 6.5f, LineOut0 };

	for (int32 i = 0; i < Rows; i++)
	{
		const float S0 = i * Step, S1 = (i + 1) * Step;
		const FVector Inward = -(Spline.Right(S0) + Spline.Right(S1)).GetSafeNormal();
		const FVector Outward = -Inward;
		const float Us0 = S0 / AsphaltTile, Us1 = S1 / AsphaltTile;

		// ---- asphalt: banked road (coarse, for the collider) ----
		for (int32 k = 0; k < UE_ARRAY_COUNT(RoadD) - 1; k++)
		{
			const float D0 = RoadD[k], D1 = RoadD[k + 1];
			Asphalt.Quad(P(S0, D0), P(S1, D0), P(S1, D1), P(S0, D1), Gray(AsphaltShade), Up,
				FUVRect(Us0, D0 / AsphaltTile, Us1, D1 / AsphaltTile));
		}
		// ---- visible road: fine strips carrying the rubber laid down along the racing line ----
		const int32 Strips = FMath::RoundToInt((LineOut0 - T::RoadInner) / 0.4f);
		for (int32 k = 0; k < Strips; k++)
		{
			const float D0 = T::RoadInner + (LineOut0 - T::RoadInner) * k / Strips;
			const float D1 = T::RoadInner + (LineOut0 - T::RoadInner) * (k + 1) / Strips;
			AsphaltVis.Quad(P(S0, D0), P(S1, D0), P(S1, D1), P(S0, D1),
				Gray(GrooveShade(S0, D0)), Gray(GrooveShade(S1, D0)), Gray(GrooveShade(S1, D1)), Gray(GrooveShade(S0, D1)),
				Up, FUVRect(Us0, D0 / AsphaltTile, Us1, D1 / AsphaltTile));
		}
		Both(P(S0, LineOut1), P(S1, LineOut1), P(S1, T::RoadOuter), P(S0, T::RoadOuter), Gray(0.7f), Up,
			FUVRect(Us0, LineOut1 / AsphaltTile, Us1, T::RoadOuter / AsphaltTile));
		const float Ai0 = PitLane::ApronInnerAt(S0), Ai1 = PitLane::ApronInnerAt(S1);
		{
			const float Uv[8] = { Us0, Ai0 / AsphaltTile, Us1, Ai1 / AsphaltTile, Us1, LineIn0 / AsphaltTile, Us0, LineIn0 / AsphaltTile };
			Both(P(S0, Ai0), P(S1, Ai1), P(S1, LineIn0), P(S0, LineIn0), Gray(1.0f), Up, Uv);
		}

		// ---- painted lines ----
		PaintMb.Quad(P(S0, LineIn0), P(S1, LineIn0), P(S1, T::RoadInner), P(S0, T::RoadInner), PaintCol, Up,
			FUVRect(Us0, LineIn0 / AsphaltTile, Us1, T::RoadInner / AsphaltTile));
		PaintMb.Quad(P(S0, LineOut0), P(S1, LineOut0), P(S1, LineOut1), P(S0, LineOut1), PaintCol, Up,
			FUVRect(Us0, LineOut0 / AsphaltTile, Us1, LineOut1 / AsphaltTile));

		// ---- walls: texture v = 0 at the top of the wall, 1 at the base ----
		const float U0 = S0 / WallTile, U1 = S1 / WallTile;
		const float VTopInner = 1.f - T::InnerWallHeight / T::OuterWallHeight;
		const float SMid = (S0 + S1) / 2.f;
		const bool bPitRow = PitLane::InPitRoad(SMid);
		const bool bLaneRow = PitLane::InExitLane(SMid);
		auto WallBlock = [&](float A0, float A1, float Back0, float Back1, const FLinearColor& BackCol)
		{
			// face toward the track, top, back
			Wall.Quad(AtZ(P(S0, A0), 0), AtZ(P(S1, A1), 0), AtZ(P(S1, A1), T::InnerWallHeight), AtZ(P(S0, A0), T::InnerWallHeight),
				White, Outward, FUVRect(U0, 1, U1, VTopInner));
			Wall.Quad(AtZ(P(S0, Back0), T::InnerWallHeight), AtZ(P(S1, Back1), T::InnerWallHeight),
				AtZ(P(S1, A1), T::InnerWallHeight), AtZ(P(S0, A0), T::InnerWallHeight), Gray(0.9f), Up, FUVRect(U0, 0.02f, U1, 0.06f));
			Wall.Quad(AtZ(P(S0, Back0), 0), AtZ(P(S1, Back1), 0), AtZ(P(S1, Back1), T::InnerWallHeight), AtZ(P(S0, Back0), T::InnerWallHeight),
				BackCol, Inward, FUVRect(U0, 1, U1, VTopInner));
		};
		if (bLaneRow)
		{
			// ---- pit exit lane: separate paved lane, grass median between it and the track ----
			const float Li0 = PitLane::LaneInnerAt(S0), Li1 = PitLane::LaneInnerAt(S1);
			const float Lo0 = PitLane::LaneOuterAt(S0), Lo1 = PitLane::LaneOuterAt(S1);
			{
				const float Uv[8] = { Us0, Li0 / AsphaltTile, Us1, Li1 / AsphaltTile, Us1, Lo1 / AsphaltTile, Us0, Lo0 / AsphaltTile };
				Both(P(S0, Li0), P(S1, Li1), P(S1, Lo1), P(S0, Lo0), Gray(0.9f), Up, Uv);
			}
			if (Ai0 - Lo0 > 0.02f || Ai1 - Lo1 > 0.02f)
			{
				const float Uv[8] = { S0 / GrassTile, Lo0 / GrassTile, S1 / GrassTile, Lo1 / GrassTile,
					S1 / GrassTile, Ai1 / GrassTile, S0 / GrassTile, Ai0 / GrassTile };
				GrassMb.Quad(P(S0, Lo0), P(S1, Lo1), P(S1, Ai1), P(S0, Ai0),
					GroundColor(P(S0, Lo0)), GroundColor(P(S1, Lo1)), GroundColor(P(S1, Ai1)), GroundColor(P(S0, Ai0)), Up, Uv);
			}
			if (PitLane::Rel(SMid) < PitLane::End)
			{
				// the pit exit road keeps its wall on the infield side until it reaches the lane
				WallBlock(Li0, Li1, Li0 - 0.4f, Li1 - 0.4f, Gray(0.85f));
			}
		}
		else if (bPitRow)
		{
			// ---- pit complex: pit road surface from the apron in to the pit back wall ----
			const float Pb0 = PitLane::BackDAt(S0), Pb1 = PitLane::BackDAt(S1);
			{
				const float Uv[8] = { Us0, Pb0 / AsphaltTile, Us1, Pb1 / AsphaltTile,
					Us1, T::ApronInner / AsphaltTile, Us0, T::ApronInner / AsphaltTile };
				Both(P(S0, Pb0), P(S1, Pb1), P(S1, T::ApronInner), P(S0, T::ApronInner), Gray(0.92f), Up, Uv);
			}
			WallBlock(Pb0, Pb1, Pb0 - 0.4f, Pb1 - 0.4f, Gray(0.85f));
		}
		else
		{
			// ---- infield grass strip ----
			GrassMb.Quad(P(S0, T::InnerWall), P(S1, T::InnerWall), P(S1, T::ApronInner), P(S0, T::ApronInner),
				GroundColor(P(S0, T::InnerWall)), GroundColor(P(S1, T::InnerWall)), GroundColor(P(S1, T::ApronInner)),
				GroundColor(P(S0, T::ApronInner)), Up,
				FUVRect(S0 / GrassTile, T::InnerWall / GrassTile, S1 / GrassTile, T::ApronInner / GrassTile));
		}
		if ((!bPitRow && !bLaneRow) || (bPitRow && PitLane::PitWallAt(SMid)))
		{
			// inner retaining wall - along the pits this is the pit wall
			WallBlock(T::InnerWall, T::InnerWall, InnerWallBack, InnerWallBack, bPitRow ? White : Gray(0.85f));
		}

		// outer wall: face, top, back
		Wall.Quad(P(S0, T::RoadOuter), P(S1, T::RoadOuter), P(S1, T::RoadOuter, T::OuterWallHeight), P(S0, T::RoadOuter, T::OuterWallHeight),
			White, Inward, FUVRect(U0, 1, U1, 0));
		const FVector T0 = P(S0, T::RoadOuter, T::OuterWallHeight), T1 = P(S1, T::RoadOuter, T::OuterWallHeight);
		FVector W0 = Spline.Point(S0, WallOut), W1 = Spline.Point(S1, WallOut);
		W0.Z = T0.Z; W1.Z = T1.Z;
		Wall.Quad(T0, T1, W1, W0, Gray(0.9f), Up, FUVRect(U0, 0.02f, U1, 0.08f));
		Wall.Quad(W0, W1, AtZ(W1, 0), AtZ(W0, 0), Gray(0.85f), Outward, FUVRect(U0, 0, U1, 1));
	}

	// ---- start/finish line: painted checkered band across the asphalt (centered on s = 0) ----
	const FLinearColor CheckBlack(0.035f, 0.035f, 0.04f, 1);
	const int32 CheckCols = 31, CheckRows = 3;
	const float Square = (T::RoadOuter - T::RoadInner) / CheckCols;
	const float SStart = -CheckRows * Square / 2;
	for (int32 r = 0; r < CheckRows; r++)
	{
		const float S0 = SStart + r * Square, S1 = S0 + Square;
		for (int32 c = 0; c < CheckCols; c++)
		{
			const float D0 = T::RoadInner + c * Square, D1 = D0 + Square;
			const FLinearColor& Col = ((r + c) % 2 == 0) ? PaintCol : CheckBlack;
			PaintMb.Quad(P(S0, D0, 0.005f), P(S1, D0, 0.005f), P(S1, D1, 0.005f), P(S0, D1, 0.005f), Col, Up,
				FUVRect(S0 / AsphaltTile, D0 / AsphaltTile, S1 / AsphaltTile, D1 / AsphaltTile));
		}
	}
	for (float SEdge : { SStart - 0.12f, -SStart })
	{
		PaintMb.Quad(P(SEdge, T::RoadInner, 0.005f), P(SEdge + 0.12f, T::RoadInner, 0.005f),
			P(SEdge + 0.12f, T::RoadOuter, 0.005f), P(SEdge, T::RoadOuter, 0.005f), PaintCol, Up,
			FUVRect(SEdge / AsphaltTile, T::RoadInner / AsphaltTile, (SEdge + 0.12f) / AsphaltTile, T::RoadOuter / AsphaltTile));
	}
	PaintPitLane(PaintMb, Step);
}

void AOvalTrack::PaintPitLane(FMeshBuilder& PaintMb, float Step) const
{
	const float Z = 0.005f;
	const int32 Rows = FMath::CeilToInt(FTrackSpline::Length / Step);
	for (int32 i = 0; i < Rows; i++)
	{
		const float S0 = i * Step, S1 = S0 + Step;
		const float A = PitLane::DividerD(S0), B = PitLane::DividerD(S1);
		if (FMath::IsNaN(A) || FMath::IsNaN(B)) continue;
		PaintMb.Quad(P(S0, A - 0.11f, Z), P(S1, B - 0.11f, Z), P(S1, B + 0.11f, Z), P(S0, A + 0.11f, Z), PaintCol, Up,
			FUVRect(S0 / AsphaltTile, A / AsphaltTile, S1 / AsphaltTile, B / AsphaltTile));
	}
	// pit exit lane edge lines through turns 1 and 2
	for (float S = PitLane::WallEnd + 50.f; S < PitLane::LaneEnd; S += 2.f)
	{
		const float E = FMath::Min(S + 2.f, PitLane::LaneEnd);
		const float A = PitLane::LaneOuterAt(S) - 0.15f, B = PitLane::LaneOuterAt(E) - 0.15f;
		PaintMb.Quad(P(S, A - 0.1f, Z), P(E, B - 0.1f, Z), P(E, B + 0.1f, Z), P(S, A + 0.1f, Z), PaintCol, Up,
			FUVRect(S / AsphaltTile, A / AsphaltTile, E / AsphaltTile, B / AsphaltTile));
	}
	for (float S = PitLane::End; S < PitLane::LaneEnd - 30.f; S += 2.f)
	{
		const float E = S + 2.f;
		const float A = PitLane::LaneInnerAt(S) + 0.25f, B = PitLane::LaneInnerAt(E) + 0.25f;
		PaintMb.Quad(P(S, A - 0.1f, Z), P(E, B - 0.1f, Z), P(E, B + 0.1f, Z), P(S, A + 0.1f, Z), PaintCol, Up,
			FUVRect(S / AsphaltTile, A / AsphaltTile, E / AsphaltTile, B / AsphaltTile));
	}
	const float First = PitLane::StallStart(0), Last = PitLane::StallStart(PitLane::Stalls);
	for (float D : { PitLane::FastLaneInner, PitLane::BoxInner }) StripeAlong(PaintMb, First, Last, D, 0.24f, PaintCol);
	for (int32 k = 0; k <= PitLane::Stalls; k++)
	{
		const float S = PitLane::StallStart(k);
		StripeAcross(PaintMb, S - 0.08f, S + 0.08f, PitLane::BoxInner, PitLane::FastLaneInner, PaintCol);
	}
	// the player's box: solid yellow paint
	const FLinearColor Yellow(0.98f, 0.78f, 0.05f, 1.f);
	const float B0 = PitLane::StallStart(PitLane::PlayerStall) + 0.25f, B1 = PitLane::StallStart(PitLane::PlayerStall + 1) - 0.25f;
	for (float S = B0; S < B1; S += 2.f)
	{
		StripeAcross(PaintMb, S, FMath::Min(S + 2.f, B1), PitLane::BoxInner + 0.25f, PitLane::FastLaneInner - 0.25f, Yellow);
	}
	// pit-lane speed-limit lines across the lane at both ends of the pit wall
	for (float Rel : { PitLane::WallStart + 15.f, PitLane::WallEnd - 15.f })
	{
		StripeAcross(PaintMb, Rel - 0.3f, Rel + 0.3f, PitLane::BackD + 0.5f, PitLane::WallD - 0.5f, PaintCol);
	}
}

void AOvalTrack::StripeAlong(FMeshBuilder& PaintMb, float S0, float S1, float D, float Width, const FLinearColor& Color) const
{
	for (float S = S0; S < S1; S += 2.f)
	{
		const float E = FMath::Min(S + 2.f, S1);
		PaintMb.Quad(P(S, D - Width / 2, 0.005f), P(E, D - Width / 2, 0.005f), P(E, D + Width / 2, 0.005f),
			P(S, D + Width / 2, 0.005f), Color, Up,
			FUVRect(S / AsphaltTile, D / AsphaltTile, E / AsphaltTile, (D + Width) / AsphaltTile));
	}
}

void AOvalTrack::StripeAcross(FMeshBuilder& PaintMb, float S0, float S1, float D0, float D1, const FLinearColor& Color) const
{
	PaintMb.Quad(P(S0, D0, 0.005f), P(S1, D0, 0.005f), P(S1, D1, 0.005f), P(S0, D1, 0.005f), Color, Up,
		FUVRect(S0 / AsphaltTile, D0 / AsphaltTile, S1 / AsphaltTile, D1 / AsphaltTile));
}

void AOvalTrack::BuildFence(FMeshBuilder& FenceMb) const
{
	const float FenceD = FTrackSpline::RoadOuter + 0.3f;
	const int32 Rows = FMath::CeilToInt(FTrackSpline::Length / 4.f);
	const float Step = FTrackSpline::Length / Rows;
	const float H0 = FTrackSpline::OuterWallHeight;
	for (int32 i = 0; i < Rows; i++)
	{
		const float S0 = i * Step, S1 = (i + 1) * Step;
		const FVector Inward = -Spline.Right(S0);
		FenceMb.Quad(P(S0, FenceD, H0), P(S1, FenceD, H0), P(S1, FenceD, H0 + FenceHeight), P(S0, FenceD, H0 + FenceHeight),
			White, Inward, FUVRect(S0 / 0.5f, FenceHeight / 0.5f, S1 / 0.5f, 0));
	}
}

void AOvalTrack::BuildDecorations(FMeshBuilder& Mb) const
{
	using T = FTrackSpline;
	const float FenceD = T::RoadOuter + 0.3f;
	const float H0 = T::OuterWallHeight;

	// catch-fence posts every 4 m + overhang arms + cables
	for (float S = 0; S < T::Length - 1; S += 4.f)
	{
		const FVector Base = P(S, FenceD + 0.08f, H0);
		const FVector Fwd = Spline.Forward(S), Right = Spline.Right(S);
		Mb.Box(Base + FVector(0, 0, FenceHeight / 2), Fwd, Up, Right, 0.05f, FenceHeight / 2, 0.05f, Steel);
		Mb.Box(Base + FVector(0, 0, FenceHeight) - Right * 0.45f, Fwd, Up, Right, 0.03f, 0.03f, 0.5f, Steel);
	}
	{
		const int32 Rows = FMath::CeilToInt(T::Length / 4.f);
		const float Step = T::Length / Rows;
		for (int32 i = 0; i < Rows; i++)
		{
			const float S0 = i * Step, S1 = (i + 1) * Step;
			const FVector Inward = -Spline.Right(S0);
			for (float H : { 0.9f, 2.0f, 3.1f, FenceHeight - 0.05f })
			{
				Mb.Quad(P(S0, FenceD - 0.06f, H0 + H), P(S1, FenceD - 0.06f, H0 + H),
					P(S1, FenceD - 0.06f, H0 + H + 0.025f), P(S0, FenceD - 0.06f, H0 + H + 0.025f), Steel, Inward);
			}
		}
	}

	// infield pylons every 50 m (taller red/white ones every 200 m)
	const FLinearColor Yellow(0.95f, 0.75f, 0.08f, 1), Black(0.06f, 0.06f, 0.06f, 1), Red(0.75f, 0.06f, 0.06f, 1);
	for (float S = 0; S < T::Length - 1; S += 50.f)
	{
		if (PitLane::InRange(S) || PitLane::InExitLane(S)) continue;
		const FVector Base = P(S, -16.5f, 0);
		const FVector Fwd = Spline.Forward(S), Right = Spline.Right(S);
		const int32 Idx = (int32)(S / 50.f);
		const bool bMajor = Idx % 4 == 0;
		const float H = bMajor ? 2.6f : 1.2f;
		Mb.Box(Base + FVector(0, 0, H / 2), Fwd, Up, Right, 0.3f, H / 2, 0.3f, bMajor ? Red : (Idx % 2 == 0 ? Yellow : Black));
		if (bMajor) Mb.Box(Base + FVector(0, 0, H + 0.3f), Fwd, Up, Right, 0.32f, 0.3f, 0.32f, PaintCol);
	}

	// pit stands (timing stands with a canopy) behind each of the 33 boxes; the player's is yellow
	const FLinearColor TeamColors[] = {
		FLinearColor(0.75f, 0.05f, 0.06f, 1), FLinearColor(0.06f, 0.18f, 0.60f, 1),
		FLinearColor(0.90f, 0.42f, 0.04f, 1), FLinearColor(0.05f, 0.40f, 0.15f, 1),
		FLinearColor(0.10f, 0.10f, 0.12f, 1), FLinearColor(0.75f, 0.05f, 0.45f, 1),
		FLinearColor(0.00f, 0.45f, 0.50f, 1), FLinearColor(0.85f, 0.85f, 0.87f, 1) };
	const FLinearColor StandGray(0.30f, 0.31f, 0.33f, 1);
	for (int32 k = 0; k < PitLane::Stalls; k++)
	{
		const float Sc = (PitLane::StallStart(k) + PitLane::StallStart(k + 1)) / 2.f;
		const FVector Fwd = Spline.Forward(Sc), Right = Spline.Right(Sc);
		const FVector Base = P(Sc, -37.2f, 0);
		const bool bMine = k == PitLane::PlayerStall;
		Mb.Box(Base + FVector(0, 0, 1.1f), Fwd, Up, Right, 1.6f, 1.1f, 0.6f, StandGray);
		Mb.Box(Base + FVector(0, 0, 2.75f), Fwd, Up, Right, 2.0f, 0.08f, 1.1f,
			bMine ? FLinearColor(0.98f, 0.78f, 0.05f, 1) : TeamColors[k % UE_ARRAY_COUNT(TeamColors)]);
		for (float Side : { -1.8f, 1.8f })
		{
			Mb.Box(Base + Fwd * Side + FVector(0, 0, 1.35f), Fwd, Up, Right, 0.05f, 1.35f, 0.05f, Steel);
		}
	}

	// turn-in boards behind the outer wall: 300 m (white), 200 m (yellow), 100 m (red)
	const FLinearColor BoardCols[] = { PaintCol, Yellow, Red };
	for (float Ts : Spline.TurnStarts())
	{
		for (int32 k = 0; k < 3; k++)
		{
			const float S = FTrackSpline::WrapS(Ts - 300 + k * 100);
			FVector Base = P(S, T::RoadOuter + 4.f, 0);
			Base.Z = Spline.Height(S, T::RoadOuter) + 0.2f;
			const FVector Fwd = Spline.Forward(S), Right = Spline.Right(S);
			Mb.Box(Base + FVector(0, 0, 6.0f), Fwd, Up, Right, 0.15f, 1.6f, 1.6f, BoardCols[k]);
			Mb.Box(Base + FVector(0, 0, 2.5f), Fwd, Up, Right, 0.1f, 2.5f, 0.1f, Steel);
		}
	}
}

// --------------------------------------------------------------------- rubber and wear

float AOvalTrack::Groove(float S, float D) const
{
	// rubber laid down along the racing line: a darker band about a car wide with soft edges,
	// darkest and narrowest through the turns, with two darker wheel tracks there; wider and
	// fainter on the straights; the amount varies a little along the lap
	if (D < FTrackSpline::RoadInner - 0.01f || D > FTrackSpline::RoadOuter) return 0.f;
	const float Turn = Spline.Bank(S) / FTrackSpline::BankAngle;   // 0 on the straights, 1 in the turns
	const float X = FMath::Abs(D - Line->Offset(S));
	const float Core = 1.0f + 0.9f * (1.f - Turn);
	const float Feather = 1.5f + 0.8f * (1.f - Turn);
	const float Band = 1.f - FMath::SmoothStep(Core, Core + Feather, X);
	const float Wheels = FMath::Exp(-FMath::Square((X - 0.8f) / 0.32f)) * Turn;
	const float Amount = (0.5f + 0.5f * Turn) * (0.88f + 0.24f * FGroundShade::Fbm(S / 37.f, D / 11.f, 2, 5));
	return Band * (Amount + 0.25f * Wheels);
}

float AOvalTrack::SurfaceVariation(float S, float D) const
{
	// non-repeating wear: broad lighter / darker patches and the odd small dark stain (more of
	// them in the groove), from noise over the whole lap rather than the 3 m texture
	const float Patches = FGroundShade::Fbm(S / 55.f, D / 9.f, 3, 71) - 0.5f;
	const float StainNoise = FGroundShade::Fbm(S / 7.f, D / 2.2f, 2, 83);
	const float StainChance = 0.74f - 0.05f * FMath::Min(1.f, Groove(S, D));
	const float Stain = FMath::SmoothStep(StainChance, StainChance + 0.08f, StainNoise);
	return (1.f + 0.09f * Patches) * (1.f - 0.12f * Stain);
}

float AOvalTrack::GrooveShade(float S, float D) const
{
	return AsphaltShade * (1.f - 0.30f * Groove(S, D)) * SurfaceVariation(S, D);
}

// ------------------------------------------------------------------------------ ground

void AOvalTrack::BuildGround()
{
	// the land around the track: a flat grid colored by GroundShade (mown lawns with stripes,
	// rough meadow, darker ground under the woods); 5 m cells around the track and the woods,
	// 100 m cells out to 2 km
	const FVector C = Spline.GetCenter();
	const float Z = GroundZ, Tile = 6.f, Half = 2000.f, Coarse = 100.f, Fine = 5.f;
	float MinX = TNumericLimits<float>::Max(), MaxX = -MinX, MinY = MinX, MaxY = -MinX;
	for (float S = 0; S < FTrackSpline::Length; S += 10.f)
	{
		const FVector Pc = Spline.Centerline(S);
		MinX = FMath::Min(MinX, (float)Pc.X); MaxX = FMath::Max(MaxX, (float)Pc.X);
		MinY = FMath::Min(MinY, (float)Pc.Y); MaxY = FMath::Max(MaxY, (float)Pc.Y);
	}
	auto Snap = [](float V, float Step, bool bUp) { return (bUp ? FMath::CeilToFloat(V / Step) : FMath::FloorToFloat(V / Step)) * Step; };
	const float X0 = Snap(MinX - 450.f - C.X, Coarse, false) + C.X, X1 = Snap(MaxX + 450.f - C.X, Coarse, true) + C.X;
	const float Y0 = Snap(MinY - 450.f - C.Y, Coarse, false) + C.Y, Y1 = Snap(MaxY + 450.f - C.Y, Coarse, true) + C.Y;
	const int32 Nx = FMath::RoundToInt((X1 - X0) / Fine), Ny = FMath::RoundToInt((Y1 - Y0) / Fine);

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UV;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	auto Vertex = [&](float X, float Y)
	{
		Verts.Add(FVector(X, Y, Z) * 100.0);
		Normals.Add(FVector::UpVector);
		UV.Add(FVector2D(X / Tile, Y / Tile));
		Colors.Add(Shade->Color(X, Y));
		Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));
	};
	// Unreal's front face for (a, b, c) has normal -(b - a) x (c - a): wound to face up
	auto Tri = [&Tris](int32 A, int32 B, int32 Cc) { Tris.Append({ A, Cc, B }); };
	for (int32 j = 0; j <= Ny; j++)
	{
		for (int32 i = 0; i <= Nx; i++) Vertex(X0 + i * Fine, Y0 + j * Fine);
	}
	for (int32 j = 0; j < Ny; j++)
	{
		for (int32 i = 0; i < Nx; i++)
		{
			const int32 A = j * (Nx + 1) + i, B = A + 1, D = A + Nx + 1, E = D + 1;
			Tri(A, B, D);
			Tri(B, E, D);
		}
	}
	const int32 Cn = FMath::RoundToInt(2 * Half / Coarse);
	for (int32 i = 0; i < Cn; i++)
	{
		for (int32 j = 0; j < Cn; j++)
		{
			const float Cx0 = C.X - Half + i * Coarse, Cy0 = C.Y - Half + j * Coarse;
			const bool bInside = Cx0 >= X0 - 0.1f && Cx0 + Coarse <= X1 + 0.1f && Cy0 >= Y0 - 0.1f && Cy0 + Coarse <= Y1 + 0.1f;
			if (bInside) continue;
			const int32 A = Verts.Num();
			Vertex(Cx0, Cy0);
			Vertex(Cx0 + Coarse, Cy0);
			Vertex(Cx0, Cy0 + Coarse);
			Vertex(Cx0 + Coarse, Cy0 + Coarse);
			Tri(A, A + 1, A + 2);
			Tri(A + 1, A + 3, A + 2);
		}
	}
	GroundMesh->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UV, Colors, Tangents, false);
	GroundMesh->SetMaterial(0, MakeMaterial(TEXT("/Game/Materials/M_Surface.M_Surface"), TEXT("/Game/Textures/Track/grass_albedo.grass_albedo"),
		TEXT("/Game/Textures/Track/grass_normal.grass_normal"), nullptr, 0.95f, 0.f, FLinearColor::White));
}
