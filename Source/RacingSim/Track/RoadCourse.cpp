// The road course: the parts of AOvalTrack that build its road, kerbs,
// run-off, barriers and the hills around it. The oval's own builders are in OvalTrack.cpp.

#include "Track/OvalTrack.h"
#include "Track/MeshBuilder.h"
#include "Game/RaceDirector.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"

namespace
{
	constexpr float RowStep = 2.0f;
	constexpr float AsphaltTile = 3.f, GrassTile = 4.f, WallTile = 4.f;
	constexpr float AsphaltShade = 0.80f;
	const FLinearColor White(1, 1, 1, 1);
	const FLinearColor PaintCol(0.92f, 0.92f, 0.90f, 1);
	const FLinearColor KerbRed(0.72f, 0.05f, 0.04f, 1);
	const FLinearColor Steel(0.35f, 0.35f, 0.37f, 1);
	const FVector Up = FVector::UpVector;
	FLinearColor Gray(float G) { return FLinearColor(G, G, G, 1); }

	/** Terrain: cell size (m), how far it reaches past the track, and the hills' size. */
	constexpr float TerrainCellSize = 12.f;
	constexpr float TerrainMargin = 900.f;
	constexpr float HillHeight = 55.f;
	/** Beyond this distance from a wall the land is forest seen from afar (darker canopy). */
	constexpr float ForestFrom = 300.f;
}

// ----------------------------------------------------------------------------- road surface

void AOvalTrack::BuildRoadCourse(FMeshBuilder& Asphalt, FMeshBuilder& AsphaltVis, FMeshBuilder& PaintMb, FMeshBuilder& GrassMb,
	FMeshBuilder& Wall, FMeshBuilder& Deco) const
{
	using T = FTrackSpline;
	const int32 Rows = FMath::CeilToInt(T::Length / RowStep);
	const float Step = T::Length / Rows;
	const float Half = T::RoadOuter;
	const float LineIn = Half - 0.45f, LineOut = Half - 0.2f;     // white edge lines
	const float RoadD[] = { -Half, -3.25f, 0.f, 3.25f, Half };
	const float H = T::BarrierHeight;

	for (int32 i = 0; i < Rows; i++)
	{
		const float S0 = i * Step, S1 = (i + 1) * Step, SMid = (S0 + S1) / 2;
		const FVector Inward0 = -Spline.Right(S0);
		const float Us0 = S0 / AsphaltTile, Us1 = S1 / AsphaltTile;

		// ---- the road: coarse for the collider, fine strips (rubber, wear) for the eye ----
		for (int32 k = 0; k < UE_ARRAY_COUNT(RoadD) - 1; k++)
		{
			const float D0 = RoadD[k], D1 = RoadD[k + 1];
			Asphalt.Quad(P(S0, D0), P(S1, D0), P(S1, D1), P(S0, D1), Gray(AsphaltShade), Up, FUVRect(Us0, D0 / AsphaltTile, Us1, D1 / AsphaltTile));
		}
		const int32 Strips = FMath::RoundToInt(2 * Half / 0.4f);
		for (int32 k = 0; k < Strips; k++)
		{
			const float D0 = -Half + 2 * Half * k / Strips, D1 = -Half + 2 * Half * (k + 1) / Strips;
			AsphaltVis.Quad(P(S0, D0), P(S1, D0), P(S1, D1), P(S0, D1),
				Gray(GrooveShade(S0, D0)), Gray(GrooveShade(S1, D0)), Gray(GrooveShade(S1, D1)), Gray(GrooveShade(S0, D1)),
				Up, FUVRect(Us0, D0 / AsphaltTile, Us1, D1 / AsphaltTile));
		}
		// white lines along both edges
		for (float Sign : { -1.f, 1.f })
		{
			const float A = Sign * LineIn, B = Sign * LineOut;
			PaintMb.Quad(P(S0, A, 0.004f), P(S1, A, 0.004f), P(S1, B, 0.004f), P(S0, B, 0.004f), PaintCol, Up,
				FUVRect(Us0, A / AsphaltTile, Us1, B / AsphaltTile));
		}

		// ---- each side: kerb (through the turns), grass run-off, the barrier ----
		for (int32 Side = 0; Side < 2; Side++)
		{
			const bool bRight = Side == 1;
			const float Sg = bRight ? 1.f : -1.f;
			const bool bKerb = Spline.KerbAt(SMid, bRight);
			const float Edge = Half + (bKerb ? T::KerbWidth : 0.f);
			if (bKerb)
			{
				// red and white blocks, 2 m each, a touch above the road
				const bool bRed = (i / 1) % 2 == 0;
				const float A = Sg * Half, B = Sg * Edge;
				PaintMb.Quad(P(S0, A, 0.01f), P(S1, A, 0.01f), P(S1, B, 0.03f), P(S0, B, 0.03f), bRed ? KerbRed : PaintCol, Up,
					FUVRect(Us0, A / AsphaltTile, Us1, B / AsphaltTile));
			}
			const float W0 = bRight ? Spline.MaxD(S0) : Spline.MinD(S0);
			const float W1 = bRight ? Spline.MaxD(S1) : Spline.MinD(S1);
			const float E = Sg * Edge;
			{
				const float Uv[8] = { S0 / GrassTile, E / GrassTile, S1 / GrassTile, E / GrassTile, S1 / GrassTile, W1 / GrassTile, S0 / GrassTile, W0 / GrassTile };
				GrassMb.Quad(P(S0, E), P(S1, E), P(S1, W1), P(S0, W0),
					GroundColor(P(S0, E)), GroundColor(P(S1, E)), GroundColor(P(S1, W1)), GroundColor(P(S0, W0)), Up, Uv);
			}
			// barrier: face toward the track, top, back (texture v = 0 at the top)
			const FVector In = bRight ? Inward0 : -Inward0;
			const float U0 = S0 / WallTile, U1 = S1 / WallTile;
			const float Back0 = W0 + Sg * 0.5f, Back1 = W1 + Sg * 0.5f;
			Wall.Quad(P(S0, W0), P(S1, W1), P(S1, W1, H), P(S0, W0, H), White, In, FUVRect(U0, 1, U1, 0));
			Wall.Quad(P(S0, W0, H), P(S1, W1, H), P(S1, Back1, H), P(S0, Back0, H), Gray(0.9f), Up, FUVRect(U0, 0.02f, U1, 0.08f));
			Wall.Quad(P(S0, Back0, H), P(S1, Back1, H), P(S1, Back1, -1.f), P(S0, Back0, -1.f), Gray(0.85f), -In, FUVRect(U0, 0, U1, 1));
		}
	}

	// ---- start / finish line: a checkered band across the road at s = 0 ----
	const FLinearColor CheckBlack(0.035f, 0.035f, 0.04f, 1);
	const int32 CheckCols = 26, CheckRows = 3;
	const float Square = 2 * Half / CheckCols;
	const float SStart = -CheckRows * Square / 2;
	for (int32 r = 0; r < CheckRows; r++)
	{
		const float S0 = SStart + r * Square, S1 = S0 + Square;
		for (int32 c = 0; c < CheckCols; c++)
		{
			const float D0 = -Half + c * Square, D1 = D0 + Square;
			PaintMb.Quad(P(S0, D0, 0.006f), P(S1, D0, 0.006f), P(S1, D1, 0.006f), P(S0, D1, 0.006f), ((r + c) % 2 == 0) ? PaintCol : CheckBlack, Up,
				FUVRect(S0 / AsphaltTile, D0 / AsphaltTile, S1 / AsphaltTile, D1 / AsphaltTile));
		}
	}
	// ---- the grid: a white bracket in front of each starting place ----
	for (int32 Slot = 0; Slot < 33; Slot++)
	{
		float S, D;
		ARaceDirector::GridSlot(Slot, 33, S, D);
		const float Front = S + 2.9f;
		StripeAcross(PaintMb, Front, Front + 0.25f, D - 1.4f, D + 1.4f, PaintCol);
		StripeAlong(PaintMb, Front - 1.5f, Front, D - 1.4f, 0.2f, PaintCol);
		StripeAlong(PaintMb, Front - 1.5f, Front, D + 1.4f, 0.2f, PaintCol);
	}

	// ---- braking boards (300 / 200 / 100 m) before the slow turns, behind the outside barrier ----
	const FLinearColor Yellow(0.95f, 0.75f, 0.08f, 1), Red(0.75f, 0.06f, 0.06f, 1);
	const FLinearColor BoardCols[] = { PaintCol, Yellow, Red };
	for (const float Ts : Spline.TurnStarts())
	{
		float KMax = 0.f;
		for (float S = Ts; S < Ts + 120.f; S += 4.f) KMax = FMath::Max(KMax, FMath::Abs(Spline.Curvature(S)));
		if (KMax < 1.f / 90.f) continue;                      // a fast turn: no braking to speak of
		const bool bOutsideRight = Spline.TurnDirection(Ts) > 0.f;
		for (int32 k = 0; k < 3; k++)
		{
			const float S = FTrackSpline::WrapS(Ts - 300 + k * 100);
			const float Wd = bOutsideRight ? Spline.MaxD(S) + 1.5f : Spline.MinD(S) - 1.5f;
			const FVector Base = P(S, Wd, 0.f);
			const FVector Fwd = Spline.Forward(S), Right = Spline.Right(S);
			Deco.Box(Base + FVector(0, 0, 2.4f), Fwd, Up, Right, 0.12f, 0.8f, 0.8f, BoardCols[k]);
			Deco.Box(Base + FVector(0, 0, 0.8f), Fwd, Up, Right, 0.08f, 0.8f, 0.08f, Steel);
		}
	}
}

// ----------------------------------------------------------------------------------- terrain

float AOvalTrack::GroundHeight(float X, float Y) const
{
	if (FTrackSpline::IsOval()) return FTrackSurface::GroundZ;
	if (Terrain.Num() == 0) return Spline.Elevation(Spline.Project(FVector(X, Y, 0)).S);
	const float Fx = FMath::Clamp((X - TerrainX0) / TerrainCell, 0.f, TerrainNx - 1.001f);
	const float Fy = FMath::Clamp((Y - TerrainY0) / TerrainCell, 0.f, TerrainNy - 1.001f);
	const int32 Ix = (int32)Fx, Iy = (int32)Fy;
	const float Tx = Fx - Ix, Ty = Fy - Iy;
	auto Z = [this](int32 I, int32 J) { return Terrain[J * TerrainNx + I]; };
	return FMath::Lerp(FMath::Lerp(Z(Ix, Iy), Z(Ix + 1, Iy), Tx), FMath::Lerp(Z(Ix, Iy + 1), Z(Ix + 1, Iy + 1), Tx), Ty);
}

float AOvalTrack::BeyondWall(float X, float Y, float& OutS) const
{
	const FTrackSpline::FTrackPos Tp = Spline.Project(FVector(X, Y, 0));
	OutS = Tp.S;
	return Tp.D >= 0.f ? Tp.D - Spline.MaxD(Tp.S) : Spline.MinD(Tp.S) - Tp.D;
}

void AOvalTrack::BuildTerrain()
{
	// The wooded hills around the circuit: the land meets each barrier at the track's height,
	// then eases toward the lie of the land around it (the track's heights, averaged over a few
	// hundred metres) plus rolling hills that grow away from the track; far out it is forest.
	float MinX = TNumericLimits<float>::Max(), MaxX = -MinX, MinY = MinX, MaxY = -MinX;
	TArray<FVector> Coarse;   // track points every 25 m with their elevation
	for (float S = 0; S < FTrackSpline::Length; S += 25.f)
	{
		const FVector C = Spline.Centerline(S);
		MinX = FMath::Min(MinX, (float)C.X); MaxX = FMath::Max(MaxX, (float)C.X);
		MinY = FMath::Min(MinY, (float)C.Y); MaxY = FMath::Max(MaxY, (float)C.Y);
		Coarse.Add(FVector(C.X, C.Y, Spline.Elevation(S)));
	}
	float MeanZ = 0.f;
	for (const FVector& C : Coarse) MeanZ += C.Z;
	MeanZ /= Coarse.Num();
	TerrainCell = TerrainCellSize;
	TerrainX0 = MinX - TerrainMargin;
	TerrainY0 = MinY - TerrainMargin;
	TerrainNx = FMath::CeilToInt((MaxX - MinX + 2 * TerrainMargin) / TerrainCell) + 1;
	TerrainNy = FMath::CeilToInt((MaxY - MinY + 2 * TerrainMargin) / TerrainCell) + 1;
	Terrain.SetNumUninitialized(TerrainNx * TerrainNy);
	TArray<FLinearColor> Colors;
	Colors.SetNumUninitialized(TerrainNx * TerrainNy);
	const float EdgeFade = 250.f;
	for (int32 j = 0; j < TerrainNy; j++)
	{
		for (int32 i = 0; i < TerrainNx; i++)
		{
			const float X = TerrainX0 + i * TerrainCell, Y = TerrainY0 + j * TerrainCell;
			float S;
			const float Beyond = BeyondWall(X, Y, S);
			const float E = Spline.Elevation(S);
			// the lie of the land: inverse-distance average of the track's heights
			float Wsum = 0.f, Zsum = 0.f;
			for (const FVector& C : Coarse)
			{
				const float W = 1.f / FMath::Square(FMath::Square((float)C.X - X) + FMath::Square((float)C.Y - Y) + 120.f * 120.f);
				Wsum += W;
				Zsum += W * C.Z;
			}
			const float Regional = Zsum / Wsum;
			const float Hills = (FGroundShade::Fbm(X / 420.f, Y / 420.f, 4, 7) - 0.5f) * 2.f * HillHeight;
			float Z;
			if (Beyond < 0.f)
			{
				Z = E - 0.5f;   // under the run-off (hidden)
			}
			else
			{
				const float Blend = FTrackSpline::Smooth(Beyond / 260.f);
				Z = FMath::Lerp(E, Regional, Blend) + Hills * FTrackSpline::Smooth((Beyond - 15.f) / 300.f) - 0.05f;
			}
			// fade to a level horizon at the edge of the terrain
			const float ToEdge = FMath::Min(FMath::Min(i, TerrainNx - 1 - i), FMath::Min(j, TerrainNy - 1 - j)) * TerrainCell;
			Z = FMath::Lerp(MeanZ - 10.f, Z, FTrackSpline::Smooth(ToEdge / EdgeFade));
			Terrain[j * TerrainNx + i] = Z;

			FLinearColor Col = Shade->Color(X, Y);
			// forest canopy far from the track (darker, bluer with distance), patchy
			const float Canopy = FTrackSpline::Smooth((Beyond - ForestFrom) / 120.f);
			const float Patch = 0.85f + 0.3f * FGroundShade::Fbm(X / 60.f, Y / 60.f, 3, 91);
			const FLinearColor Forest(0.20f * Patch, 0.27f * Patch, 0.15f * Patch, 1.f);
			Col = FMath::Lerp(Col, Forest, Canopy);
			Colors[j * TerrainNx + i] = Col;
		}
	}

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UV;
	TArray<FProcMeshTangent> Tangents;
	Verts.Reserve(TerrainNx * TerrainNy);
	auto Zat = [this](int32 I, int32 J) { return Terrain[FMath::Clamp(J, 0, TerrainNy - 1) * TerrainNx + FMath::Clamp(I, 0, TerrainNx - 1)]; };
	const float Tile = 6.f;
	for (int32 j = 0; j < TerrainNy; j++)
	{
		for (int32 i = 0; i < TerrainNx; i++)
		{
			const float X = TerrainX0 + i * TerrainCell, Y = TerrainY0 + j * TerrainCell;
			Verts.Add(FVector(X, Y, Zat(i, j)) * 100.0);
			const FVector Nrm = FVector(Zat(i - 1, j) - Zat(i + 1, j), Zat(i, j - 1) - Zat(i, j + 1), 2.f * TerrainCell).GetSafeNormal();
			Normals.Add(Nrm);
			UV.Add(FVector2D(X / Tile, Y / Tile));
			Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));
		}
	}
	// Unreal's front face for (a, b, c) has normal -(b - a) x (c - a): wound to face up
	auto Tri = [&Tris](int32 A, int32 B, int32 C) { Tris.Append({ A, C, B }); };
	for (int32 j = 0; j < TerrainNy - 1; j++)
	{
		for (int32 i = 0; i < TerrainNx - 1; i++)
		{
			const int32 A = j * TerrainNx + i, B = A + 1, D = A + TerrainNx, E = D + 1;
			Tri(A, B, D);
			Tri(B, E, D);
		}
	}
	// a wide skirt of forest out to the horizon, level with the faded edge
	const float Far = 6000.f, Zs = (MeanZ - 10.f) * 100.f;
	const FLinearColor FarCol(0.17f, 0.23f, 0.13f, 1.f);
	const float Xa = TerrainX0, Xb = TerrainX0 + (TerrainNx - 1) * TerrainCell, Ya = TerrainY0, Yb = TerrainY0 + (TerrainNy - 1) * TerrainCell;
	const FVector Cn((Xa + Xb) / 2, (Ya + Yb) / 2, 0);
	auto Skirt = [&](float X0, float Y0, float X1, float Y1, float X2, float Y2, float X3, float Y3)
	{
		const int32 Base = Verts.Num();
		for (const FVector2D V : { FVector2D(X0, Y0), FVector2D(X1, Y1), FVector2D(X2, Y2), FVector2D(X3, Y3) })
		{
			Verts.Add(FVector(V.X * 100.0, V.Y * 100.0, Zs));
			Normals.Add(FVector::UpVector);
			UV.Add(V / Tile);
			Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));
			Colors.Add(FarCol);
		}
		Tri(Base, Base + 1, Base + 2);
		Tri(Base, Base + 2, Base + 3);
	};
	const float Fx0 = Cn.X - Far, Fx1 = Cn.X + Far, Fy0 = Cn.Y - Far, Fy1 = Cn.Y + Far;
	// four trapezoids around the terrain rectangle (wound like the grid: +x then +y)
	Skirt(Fx0, Fy0, Fx1, Fy0, Xb, Ya, Xa, Ya);
	Skirt(Xb, Ya, Fx1, Fy0, Fx1, Fy1, Xb, Yb);
	Skirt(Xa, Yb, Xb, Yb, Fx1, Fy1, Fx0, Fy1);
	Skirt(Fx0, Fy0, Xa, Ya, Xa, Yb, Fx0, Fy1);

	GroundMesh->CreateMeshSection_LinearColor(0, Verts, Tris, Normals, UV, Colors, Tangents, false);
	GroundMesh->SetMaterial(0, MakeMaterial(TEXT("/Game/Materials/M_Surface.M_Surface"), TEXT("/Game/Textures/Track/grass_albedo.grass_albedo"),
		TEXT("/Game/Textures/Track/grass_normal.grass_normal"), nullptr, 0.95f, 0.f, FLinearColor::White));
	UE_LOG(LogTemp, Display, TEXT("Terrain: %d x %d cells, %.0f .. %.0f m"), TerrainNx, TerrainNy, FMath::Min(Terrain), FMath::Max(Terrain));
}
