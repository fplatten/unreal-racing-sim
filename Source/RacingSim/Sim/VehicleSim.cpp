#include "Sim/VehicleSim.h"

// ---------------------------------------------------------------------------- FTrackSurface

void FTrackSurface::Sample(const FVector& P, float& OutZ, FVector& OutNormal, bool& bOutOnTrack) const
{
	const FTrackSpline::FTrackPos Tp = Track->Project(P);
	const float S = Tp.S, D = Tp.D;
	const bool bLaneRow = PitLane::InExitLane(S);
	const bool bPitRow = PitLane::InPitRoad(S);
	const float InnerEdge = bLaneRow ? PitLane::LaneInnerAt(S) : bPitRow ? PitLane::BackDAt(S) : FTrackSpline::InnerWall;
	if (D > FTrackSpline::RoadOuter || D < InnerEdge)
	{
		// the walls are never road: past them the wheels only find the ground plane
		OutZ = GroundZ;
		OutNormal = FVector::UpVector;
		bOutOnTrack = false;
		return;
	}
	OutZ = Track->Height(S, D);
	OutNormal = Track->SurfaceNormal(S, D);
	const bool bInfieldGrass = !bLaneRow && !bPitRow && D < FTrackSpline::ApronInner;
	const bool bMedianGrass = bLaneRow && D > PitLane::LaneOuterAt(S) && D < PitLane::ApronInnerAt(S);
	bOutOnTrack = !(bInfieldGrass || bMedianGrass);
}

float FTrackSurface::Cast(const FVector& From, const FVector& Dir, float MaxDist, FVector& OutNormal, bool& bOutOnTrack) const
{
	// Intersect the ray with the local surface plane, then re-sample where it lands: the
	// surface is flat or gently banked, so this converges in a couple of iterations.
	float T = 0.f;
	for (int32 Iter = 0; Iter < 3; Iter++)
	{
		const FVector P = From + Dir * T;
		float Z;
		FVector N;
		Sample(P, Z, N, bOutOnTrack);
		const FVector OnPlane(P.X, P.Y, Z);
		const float Denom = FVector::DotProduct(N, Dir);
		if (Denom > -1e-3f) return -1.f;          // ray parallel to or leaving the surface
		T = FVector::DotProduct(N, OnPlane - From) / Denom;
		OutNormal = N;
	}
	if (T < 0.f || T > MaxDist) return -1.f;
	return T;
}

// ---------------------------------------------------------------------------- FVehicleSim

FVehicleSim::FVehicleSim(const FVehicleSpec& InSpec, const FTrackSurface* InSurface)
	: Spec(InSpec), Surface(InSurface), Gearbox(&Spec)
{
	FParse::Value(FCommandLine::Get(), TEXT("FinalDrive="), Spec.FinalDrive);   // tuning
	const float Mh = Spec.MountHeight;
	// Java mounts (+x = left) -> Unreal (+y = right)
	Wheels[FL].MountLocal = FVector(Spec.FrontAxleX, -Spec.FrontHalfTrack, Mh);
	Wheels[FR].MountLocal = FVector(Spec.FrontAxleX, Spec.FrontHalfTrack, Mh);
	Wheels[RL].MountLocal = FVector(Spec.RearAxleX, -Spec.RearHalfTrack, Mh);
	Wheels[RR].MountLocal = FVector(Spec.RearAxleX, Spec.RearHalfTrack, Mh);
	Wheels[FL].bFront = Wheels[FR].bFront = true;
	Wheels[RL].bDriven = Wheels[RR].bDriven = true;
	for (FWheel& W : Wheels) W.SuspensionLength = Spec.RestLength;
}

FVector FVehicleSim::BodyInvInertia(const FVehicleSpec& Spec)
{
	// Bullet derives a compound shape's inertia from its bounding box plus the 0.04 m collision
	// margin: the Java car's collider is a 2.0 x 0.5 x 5.0 m box (width, height, length).
	// Measured from Minie: pitch 1699.3, yaw 1958.6, roll 303.1 kg m^2.
	const float Margin = 0.04f;
	const float Lx = 5.0f + 2 * Margin, Ly = 2.0f + 2 * Margin, Lz = 0.5f + 2 * Margin;   // Unreal local: length, width, height
	const float K = Spec.Mass / 12.f;
	const FVector I(K * (Ly * Ly + Lz * Lz), K * (Lx * Lx + Lz * Lz), K * (Lx * Lx + Ly * Ly));
	return FVector(1.0 / I.X, 1.0 / I.Y, 1.0 / I.Z);
}

void FVehicleSim::ResetState(float NewSpeed)
{
	for (FWheel& W : Wheels)
	{
		W.Omega = NewSpeed / Spec.WheelRadius;
		const float StaticComp = Spec.Mass * G / 4.f / (W.bFront ? Spec.SpringFront : Spec.SpringRear);
		W.Compression = W.PrevCompression = StaticComp;
		W.SuspensionLength = Spec.RestLength - StaticComp;
		W.SlipAngle = W.SlipRatio = 0.f;
	}
	Gearbox.SelectForSpeed(NewSpeed);
	bHasPrevVel = false;
	bHasPost = false;
	LateralG = LongitudinalG = 0.f;
}

void FVehicleSim::Step(float Dt, FRigidState& B)
{
	Pos = B.Pos;
	Vel = B.Vel;
	AngVel = B.AngVel;
	Fwd = B.Rot.GetForwardVector();
	Up = B.Rot.GetUpVector();
	Left = -B.Rot.GetRightVector();
	AbsorbImpact(Dt, B);

	Speed = Vel.Size();
	ForwardSpeed = FVector::DotProduct(Vel, Fwd);

	UpdateAccelerationTelemetry(Dt, Vel);

	// ---- steering (speed-sensitive lock) ----
	SteerAngle = FMath::Clamp(Input.Steer, -1.f, 1.f) * Spec.MaxSteerAngle(ForwardSpeed);
	Wheels[FL].Steer = Wheels[FR].Steer = SteerAngle;
	Wheels[RL].Steer = Wheels[RR].Steer = 0.f;

	// ---- powertrain ----
	const float RearOmega = (Wheels[RL].Omega + Wheels[RR].Omega) * 0.5f;
	float Ratio = Gearbox.TotalRatio();
	float RawRpm = FMath::Abs(RearOmega * Ratio) * 60.f / UE_TWO_PI;
	Gearbox.Update(Dt, Input, FMath::Max(RawRpm, Spec.IdleRpm), ForwardSpeed);
	Ratio = Gearbox.TotalRatio();
	RawRpm = FMath::Abs(RearOmega * Ratio) * 60.f / UE_TWO_PI;
	const bool bReverse = Gearbox.GetGear() == 0;
	float Throttle = FMath::Clamp(bReverse ? Input.Brake : Input.Throttle, 0.f, 1.f);
	float Brake = FMath::Clamp(bReverse ? Input.Throttle : Input.Brake, 0.f, 1.f);
	if (Input.bPitHold && Throttle < 0.05f) Brake = 1.f;
	if (Gearbox.IsShifting()) Throttle = 0.f;

	// the clutch slips below idle so the engine can always launch the car
	const float Rpm = FMath::Max(RawRpm, Spec.IdleRpm + Throttle * 2500.f * FMath::Clamp(1.f - RawRpm / Spec.IdleRpm, 0.f, 1.f));
	EngineRpm = EngineRpm + (Rpm - EngineRpm) * FMath::Min(1.f, Dt * 30.f);
	float EngineTorque = RawRpm > Spec.LimiterRpm ? 0.f : Spec.EngineTorque(Rpm) * Throttle;
	if (Throttle < 0.05f)
	{
		EngineTorque -= 0.07f * Spec.EngineTorque(Rpm) * FMath::Clamp(RawRpm / Spec.RedlineRpm, 0.f, 1.f);
	}
	const float DriveTorquePerWheel = EngineTorque * Ratio * Spec.DrivetrainEfficiency * 0.5f;

	// ---- aerodynamics ----
	const FAeroModel::FResult AeroOut = Aero.Compute(Speed, ForwardSpeed, DraftFactor);

	// ---- suspension geometry: ray per wheel ----
	const float RayLen = Spec.RestLength + Spec.WheelRadius + 0.05f;
	for (FWheel& W : Wheels)
	{
		const FVector From = Pos + B.Rot.RotateVector(W.MountLocal);
		W.PrevCompression = W.Compression;
		FVector N;
		bool bOnTrack;
		const float T = Surface->Cast(From, -Up, RayLen, N, bOnTrack);
		if (T >= 0.f)
		{
			const float Susp = T - Spec.WheelRadius;
			if (Susp <= Spec.RestLength && FVector::DotProduct(N, Up) > 0.5f)
			{
				W.bContact = true;
				W.Normal = N;
				W.bOnTrack = bOnTrack;
				W.SuspensionLength = FMath::Max(Susp, 0.f);
				W.Compression = Spec.RestLength - W.SuspensionLength;
				W.ContactPoint = From - Up * T;
				continue;
			}
		}
		W.bContact = false;
		W.SuspensionLength = Spec.RestLength;
		W.Compression = 0.f;
	}

	// ---- suspension forces ----
	float SpringForce[4];
	for (int32 i = 0; i < 4; i++)
	{
		const FWheel& W = Wheels[i];
		if (!W.bContact) { SpringForce[i] = 0.f; continue; }
		const float K = W.bFront ? Spec.SpringFront : Spec.SpringRear;
		const float CompVel = (W.Compression - W.PrevCompression) / Dt;
		const float C = CompVel > 0 ? Spec.DamperBump : Spec.DamperRebound;
		float F = K * W.Compression + C * CompVel;
		if (W.Compression > Spec.BumpStopStart) F += Spec.BumpStopRate * (W.Compression - Spec.BumpStopStart);
		SpringForce[i] = F;
	}
	// anti-roll bars
	const float ArbF = Spec.AntiRollFront * (Wheels[FL].Compression - Wheels[FR].Compression);
	const float ArbR = Spec.AntiRollRear * (Wheels[RL].Compression - Wheels[RR].Compression);
	if (Wheels[FL].bContact) SpringForce[FL] += ArbF;
	if (Wheels[FR].bContact) SpringForce[FR] -= ArbF;
	if (Wheels[RL].bContact) SpringForce[RL] += ArbR;
	if (Wheels[RR].bContact) SpringForce[RR] -= ArbR;

	for (int32 i = 0; i < 4; i++)
	{
		FWheel& W = Wheels[i];
		const float Fz = FMath::Clamp(SpringForce[i], 0.f, 40000.f);
		W.Load = Fz;
		if (W.bContact && Fz > 0.f)
		{
			B.ApplyImpulse(Up * (Fz * Dt), B.Rot.RotateVector(W.MountLocal));
		}
		const float DriveT = W.bDriven ? DriveTorquePerWheel : 0.f;
		const float BrakeT = Brake * (W.bFront ? Spec.BrakeTorqueFront : Spec.BrakeTorqueRear);
		UpdateTire(W, DriveT, BrakeT, Dt, B);
	}

	UpdateSqueal(Dt);

	// ---- aero forces ----
	if (Speed > 0.1f)
	{
		B.ApplyCentralImpulse(Vel * (-AeroOut.Drag * Dt / Speed));
	}
	B.ApplyImpulse(Up * (-AeroOut.DownforceFront * Dt), B.Rot.RotateVector(FVector(Spec.FrontAxleX, 0, 0)));
	B.ApplyImpulse(Up * (-AeroOut.DownforceRear * Dt), B.Rot.RotateVector(FVector(Spec.RearAxleX, 0, 0)));
	PostVel = B.Vel;
	PostAngVel = B.AngVel;
	bHasPost = true;
}

void FVehicleSim::AbsorbImpact(float Dt, FRigidState& B)
{
	// The car body is a rigid box, so a wall or car contact hands the whole impact straight
	// back as rotation and lift. Real tires, suspension and crushable bodywork soak most of that
	// up, so when the physics step changed the velocity by an impact-sized amount only part of
	// the collision's roll / pitch kick and of its upward kick is kept.
	if (!bHasPost) return;
	FVector Dv = Vel - PostVel;
	Dv.Z += G * Dt;                                           // remove gravity
	const FVector Dw = AngVel - PostAngVel;
	if (Dv.Size() < ImpactDv && Dw.Size() < ImpactDw) return;
	const float DRoll = FVector::DotProduct(Dw, Fwd);
	const float DPitch = FVector::DotProduct(Dw, Left);
	const float DUp = FVector::DotProduct(Dv, Up);
	AngVel -= Fwd * (DRoll * (1 - ImpactRollKeep));
	AngVel -= Left * (DPitch * (1 - ImpactRollKeep));
	if (DUp > 0.f) Vel -= Up * (DUp * (1 - ImpactLiftKeep));
	B.Vel = Vel;
	B.AngVel = AngVel;
}

void FVehicleSim::UpdateTire(FWheel& W, float DriveTorque, float BrakeTorque, float Dt, FRigidState& B)
{
	const float R = Spec.WheelRadius;
	const float H = Dt / WheelSubsteps;

	if (!W.bContact || W.Load <= 0.f)
	{
		for (int32 k = 0; k < WheelSubsteps; k++)
		{
			W.Omega += DriveTorque / Spec.WheelInertia * H;
			W.Omega = ApplyBrake(W.Omega, BrakeTorque + 2.f, H);
		}
		W.SpinAngle = FMath::Fmod(W.SpinAngle + W.Omega * Dt, UE_TWO_PI);
		W.SlipAngle = 0.f;
		W.SlipRatio = 0.f;
		return;
	}

	// wheel heading projected onto the ground plane
	const FVector N = W.Normal;
	FVector WheelFwd = Fwd * FMath::Cos(W.Steer) + Left * FMath::Sin(W.Steer);
	WheelFwd = (WheelFwd - N * FVector::DotProduct(WheelFwd, N)).GetSafeNormal();
	// Unreal is left-handed: forward x up points left
	const FVector WheelLeft = FVector::CrossProduct(WheelFwd, N);

	// velocity of the contact patch
	FVector Arm = W.ContactPoint - Pos;
	const FVector VPoint = FVector::CrossProduct(AngVel, Arm) + Vel;
	const float Vx = FVector::DotProduct(VPoint, WheelFwd);
	const float Vy = FVector::DotProduct(VPoint, WheelLeft);

	W.SlipAngle = FMath::Atan2(Vy, FMath::Max(FMath::Abs(Vx), 3.f));
	const float Denom = FMath::Max(FMath::Abs(Vx), 5.f);
	const float SurfaceGrip = W.bOnTrack ? 1.f : GrassGrip;
	const float RollTorque = Spec.RollingResistance * W.Load * R * (W.bOnTrack ? 1.f : GrassRolling);

	float FxSum = 0.f, FySum = 0.f;
	for (int32 k = 0; k < WheelSubsteps; k++)
	{
		const float SlipRatio = FMath::Clamp((W.Omega * R - Vx) / Denom, -1.f, 2.f);
		const FTireModel::FResult T = Tire.Compute(W.SlipAngle, SlipRatio, W.Load, (W.bFront ? Spec.FrontGrip : Spec.RearGrip) * SurfaceGrip);
		FxSum += T.Fx;
		FySum += T.Fy;
		W.Omega += (DriveTorque - T.Fx * R) / Spec.WheelInertia * H;
		W.Omega = ApplyBrake(W.Omega, BrakeTorque + RollTorque, H);
		W.SlipRatio = SlipRatio;
	}
	const float Fx = FxSum / WheelSubsteps;
	const float Fy = FySum / WheelSubsteps;
	W.SpinAngle = FMath::Fmod(W.SpinAngle + W.Omega * Dt, UE_TWO_PI);

	const FVector Force = WheelFwd * Fx + WheelLeft * Fy;
	Arm += Up * Spec.TireForceLift;
	B.ApplyImpulse(Force * Dt, Arm);
}

float FVehicleSim::ApplyBrake(float Omega, float Torque, float H) const
{
	const float Delta = Torque / Spec.WheelInertia * H;
	if (FMath::Abs(Omega) <= Delta) return 0.f;
	return Omega - FMath::Sign(Omega) * Delta;
}

void FVehicleSim::UpdateSqueal(float Dt)
{
	float Target = 0.f;
	for (const FWheel& W : Wheels)
	{
		if (!W.bContact || !W.bOnTrack || W.Load < 300.f) continue;
		const float Lat = (FMath::Abs(W.SlipAngle) - 0.075f) / 0.12f;
		const float Lon = (FMath::Abs(W.SlipRatio) - 0.10f) / 0.25f;
		const float LoadFactor = FMath::Min(1.f, W.Load / 2500.f);
		Target = FMath::Max(Target, FMath::Clamp(FMath::Max(Lat, Lon), 0.f, 1.f) * LoadFactor);
	}
	if (Speed < 3.f) Target = 0.f;
	Squeal += (Target - Squeal) * FMath::Min(1.f, Dt / 0.06f);
}

void FVehicleSim::UpdateAccelerationTelemetry(float Dt, const FVector& V)
{
	if (bHasPrevVel)
	{
		const FVector Acc = (V - PrevVel) / Dt;
		const float LatRaw = FVector::DotProduct(Acc, Left) / G;
		const float LonRaw = FVector::DotProduct(Acc, Fwd) / G;
		const float A = FMath::Min(1.f, Dt / 0.15f);
		LateralG += (LatRaw - LateralG) * A;
		LongitudinalG += (LonRaw - LongitudinalG) * A;
	}
	PrevVel = V;
	bHasPrevVel = true;
}
