#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"

/**
 * Port of com.racingsim.track.MeshBuilder: flat-shaded, vertex-colored quads and boxes.
 * Positions are given in meters and stored in centimeters. Each quad's normal is oriented
 * with a hint direction, and its triangles are wound so their front face agrees with it.
 */
class FMeshBuilder
{
public:
	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FLinearColor> Colors;
	TArray<FProcMeshTangent> Tangents;

	/** UVs in {ua, va, ub, vb, uc, vc, ud, vd} order (nullptr = all zero). */
	void Quad(const FVector& A, const FVector& B, const FVector& C, const FVector& D,
		const FLinearColor& Ca, const FLinearColor& Cb, const FLinearColor& Cc, const FLinearColor& Cd,
		const FVector& Hint, const float* UV = nullptr)
	{
		static const float NoUV[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		if (!UV) UV = NoUV;
		const FVector E1 = B - A;
		FVector E2 = D - A;
		FVector N = FVector::CrossProduct(E1, E2);
		if (N.SizeSquared() < 1e-12)
		{
			E2 = C - A;
			N = FVector::CrossProduct(E1, E2);
		}
		N = N.GetSafeNormal();
		if (FVector::DotProduct(N, Hint) < 0) N = -N;

		// tangent: the direction in which u grows along the a-b edge
		const float Du = UV[2] - UV[0];
		FVector T = (FMath::Abs(Du) > 1e-6f ? E1 * FMath::Sign(Du) : E1).GetSafeNormal();
		T = (T - N * FVector::DotProduct(T, N)).GetSafeNormal();

		const int32 Base = Verts.Num();
		const FVector* P[4] = { &A, &B, &C, &D };
		const FLinearColor* Col[4] = { &Ca, &Cb, &Cc, &Cd };
		for (int32 i = 0; i < 4; i++)
		{
			Verts.Add(*P[i] * 100.0);
			Normals.Add(N);
			UV0.Add(FVector2D(UV[i * 2], UV[i * 2 + 1]));
			Colors.Add(*Col[i]);
			Tangents.Add(FProcMeshTangent(T, false));
		}
		// Unreal's front face for (p0, p1, p2) has normal -(p1 - p0) x (p2 - p0)
		const bool bFlip = FVector::DotProduct(FVector::CrossProduct(E1, E2), N) > 0;
		if (bFlip)
		{
			Tris.Append({ Base, Base + 2, Base + 1, Base, Base + 3, Base + 2 });
		}
		else
		{
			Tris.Append({ Base, Base + 1, Base + 2, Base, Base + 2, Base + 3 });
		}
	}

	void Quad(const FVector& A, const FVector& B, const FVector& C, const FVector& D,
		const FLinearColor& Color, const FVector& Hint, const float* UV = nullptr)
	{
		Quad(A, B, C, D, Color, Color, Color, Color, Hint, UV);
	}

	/** Oriented box from a center (m), three orthonormal axes and half-extents. */
	void Box(const FVector& C, const FVector& Ax, const FVector& Ay, const FVector& Az,
		float Hx, float Hy, float Hz, const FLinearColor& Color)
	{
		const FVector X = Ax * Hx, Y = Ay * Hy, Z = Az * Hz;
		FVector V[8];
		for (int32 i = 0; i < 8; i++)
		{
			V[i] = C + ((i & 1) ? X : -X) + ((i & 2) ? Y : -Y) + ((i & 4) ? Z : -Z);
		}
		Quad(V[1], V[3], V[7], V[5], Color, Ax);
		Quad(V[0], V[4], V[6], V[2], Color, -Ax);
		Quad(V[2], V[6], V[7], V[3], Color, Ay);
		Quad(V[0], V[1], V[5], V[4], Color, -Ay);
		Quad(V[4], V[5], V[7], V[6], Color, Az);
		Quad(V[0], V[2], V[3], V[1], Color, -Az);
	}

	void AddTo(UProceduralMeshComponent* Pmc, int32 Section, bool bCollision) const
	{
		Pmc->CreateMeshSection_LinearColor(Section, Verts, Tris, Normals, UV0, Colors, Tangents, bCollision);
	}
};

/** {u0, v0, u1, v0, u1, v1, u0, v1} */
struct FUVRect
{
	float V[8];
	FUVRect(float U0, float V0, float U1, float V1) : V{ U0, V0, U1, V0, U1, V1, U0, V1 } {}
	operator const float*() const { return V; }
};
