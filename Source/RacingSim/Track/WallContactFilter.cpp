#include "Track/WallContactFilter.h"
#include "Sim/TrackSpline.h"
#include "Chaos/ContactModification.h"
#include "Chaos/ParticleHandle.h"

std::atomic<int32> FWallContactFilter::Calls{ 0 }, FWallContactFilter::Pairs{ 0 }, FWallContactFilter::Flattened{ 0 };

void FWallContactFilter::OnContactModification_Internal(Chaos::FCollisionContactModifier& Modifier)
{
	Calls++;
	if (!Track) return;
	for (Chaos::FContactPairModifier& Pair : Modifier)
	{
		const Chaos::TVec2<Chaos::FGeometryParticleHandle*> Particles = Pair.GetParticlePair();
		if (!Particles[0] || !Particles[1]) continue;
		const bool bStatic0 = Particles[0]->ObjectState() == Chaos::EObjectStateType::Static;
		const bool bStatic1 = Particles[1]->ObjectState() == Chaos::EObjectStateType::Static;
		if (bStatic0 == bStatic1) continue;   // only a car against the track / ground
		Pairs++;
		for (int32 i = 0; i < Pair.GetNumContacts(); i++)
		{
			Chaos::FVec3 N = Pair.GetWorldNormal(i);
			if (FMath::Abs(N.Z) > 0.85) continue;   // the top of a wall: a real surface
			Chaos::FVec3 L0, L1;
			Pair.GetWorldContactLocations(i, L0, L1);
			const FVector Pt = (L0 + L1) * 0.5 / 100.0;
			const FTrackSpline::FTrackPos Tp = Track->Project(Pt);
			const bool bBeyond = Tp.D > FTrackSpline::RoadOuter - 0.15f || Tp.D < FTrackSpline::ApronInner;
			// on the wall face itself, not the road surface at its foot
			const float AboveRoad = Pt.Z - Track->Height(Tp.S, FMath::Min(Tp.D, FTrackSpline::RoadOuter));
			if (!bBeyond || AboveRoad < 0.05f) continue;
			N.Z = 0;
			const Chaos::FReal Len = N.Size();
			if (Len < 1e-4) continue;
			Pair.ModifyWorldNormal(N / Len, i);
			Flattened++;
		}
	}
}
