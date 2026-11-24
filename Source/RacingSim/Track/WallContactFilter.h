#pragma once

#include "CoreMinimal.h"
#include "Chaos/SimCallbackObject.h"
#include "Chaos/SimCallbackInput.h"
#include <atomic>

class FTrackSpline;

/**
 * Port of com.racingsim.physics.WallContactFilter: keeps the retaining walls from launching cars.
 * The walls are vertical, but a box hitting a triangle mesh at speed often gets contact normals
 * from triangle edges / corners (the wall's bottom and top edges, the seams between segments)
 * that point well up or down; at 100 m/s the wall stops ~20,000 N s of sideways momentum, so even
 * a 10 degree tilt throws the car meters into the air. Contacts with the side faces of the walls
 * and the catch fence are made horizontal here, so the wall only pushes the car sideways, as a
 * real concrete / SAFER barrier does.
 *
 * Bullet told the walls apart by sub-mesh index; here a car-vs-static contact counts as a wall
 * contact when it lies outside the edge of the road or inside the inner walls.
 */
class FWallContactFilter : public Chaos::TSimCallbackObject<Chaos::FSimCallbackNoInput, Chaos::FSimCallbackNoOutput,
	Chaos::ESimCallbackOptions::ContactModification>
{
public:
	const FTrackSpline* Track = nullptr;
	/** Diagnostics: callbacks, car-vs-static pairs seen, normals flattened. */
	static std::atomic<int32> Calls, Pairs, Flattened;

private:
	virtual void OnContactModification_Internal(Chaos::FCollisionContactModifier& Modifier) override;
};
