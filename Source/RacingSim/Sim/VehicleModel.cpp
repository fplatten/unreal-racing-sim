#include "Sim/VehicleModel.h"
#include "Sim/TrackSpline.h"
#include "Misc/CommandLine.h"

namespace
{
	/** Tuning: -PowerScale=<x> multiplies the engine torque curve (read once). */
	float PowerScale()
	{
		static float Scale = -1.f;
		if (Scale < 0.f)
		{
			Scale = 1.f;
			FParse::Value(FCommandLine::Get(), TEXT("PowerScale="), Scale);
		}
		return Scale;
	}
}

FVehicleSpec FVehicleSpec::Formula()
{
	// 600 kg with the driver, 4.42 m (14.5 ft) long, 2.90 m wheelbase, 2.00 m wide over the tires,
	// ~700 hp, 215 mph flat out without a tow, 0-100 mph in about 3.5 s, 5-6 g on the brakes from
	// top speed (~110-125 m to stop)
	FVehicleSpec S;
	S.Type = ECarType::Formula;
	S.Mass = 600.f;
	// 2.90 m wheelbase, 55% of the weight on the rear axle; front track 1.69 m, rear 1.61 m: 2.00 m
	// over the 0.30 / 0.38 m wide tires
	S.FrontAxleX = 1.60f;
	S.RearAxleX = -1.30f;
	S.FrontHalfTrack = 0.845f;
	S.RearHalfTrack = 0.805f;
	// the collider runs from the front wing to the rear wing
	S.BodyLength = 4.42f;
	S.BodyCenterAhead = 0.24f;
	// carbon brakes: enough for all the grip at top speed with full downforce (~6 g), 42% on the
	// front like the grip (55% of the weight and 60% of the downforce on the wider rear tires)
	S.BrakeTorqueFront = 2900.f;
	S.BrakeTorqueRear = 4000.f;
	S.MaxBrakeDecel = 58.f;
	// slicks under downforce; the rear ones are wider (0.38 m against 0.30 m)
	S.RearGrip = 1.25f;
	S.TireMu = 1.90f;
	S.TireLoadSensitivity = 0.06f;
	// 1,000-1,500 kg of downforce at 165-200 mph; the drag sets the 215 mph top speed at minimum wing
	S.MinDragArea = 0.82f;
	S.MinLiftArea = 2.9f;
	S.MaxDragArea = 1.0f;
	S.MaxLiftArea = 3.4f;
	// ~700 hp at 12,000 rpm; peak power in top gear at 215 mph, the limiter ~220 mph in a tow
	S.TorqueNm = { 208, 237, 322, 378, 421, 445, 445, 416, 378 };
	S.FinalDrive = 3.92f;
	S.RoadFinalDrive = 3.92f;
	return S;
}

FVehicleSpec FVehicleSpec::ForTrack()
{
	FString Key;
	FParse::Value(FCommandLine::Get(), TEXT("Car="), Key);
	const bool bFormula = Key.IsEmpty() ? FTrackSpline::IsRoadCourse() : !Key.Equals(TEXT("indy"), ESearchCase::IgnoreCase);
	FVehicleSpec S = bFormula ? Formula() : Indy();
	if (FTrackSpline::IsRoadCourse()) S.FinalDrive = S.RoadFinalDrive;
	return S;
}

float FVehicleSpec::EngineTorque(float Rpm) const
{
	const float K = PowerScale();
	if (Rpm <= TorqueRpm[0]) return K * TorqueNm[0];
	for (int32 i = 1; i < TorqueRpm.Num(); i++)
	{
		if (Rpm <= TorqueRpm[i])
		{
			const float T = (Rpm - TorqueRpm[i - 1]) / (TorqueRpm[i] - TorqueRpm[i - 1]);
			return K * (TorqueNm[i - 1] + T * (TorqueNm[i] - TorqueNm[i - 1]));
		}
	}
	return K * TorqueNm.Last();
}

FTireModel::FResult FTireModel::Compute(float SlipAngle, float SlipRatio, float Fz, float GripScale) const
{
	FResult Out;
	if (Fz <= 0.f) return Out;
	const float M = EffectiveMu(Fz) * GripScale;
	const float DMax = M * Fz;
	float Fx = DMax * Magic(SlipRatio, LongB, LongC, LongE);
	float Fy = -DMax * Magic(SlipAngle, LatB, LatC, LatE);
	// friction ellipse: the combined force can never exceed the available grip
	const float Mag = FMath::Sqrt(Fx * Fx + Fy * Fy);
	if (Mag > DMax)
	{
		const float K = DMax / Mag;
		Fx *= K;
		Fy *= K;
	}
	Out.Fx = Fx;
	Out.Fy = Fy;
	Out.MuEff = M;
	Out.Usage = Mag / DMax;
	return Out;
}

FAeroModel::FResult FAeroModel::Compute(float Speed, float ForwardSpeed, float DraftFactor) const
{
	FResult Out;
	const float Q = 0.5f * AirDensity;
	Out.CdEffective = DragArea * (1.f - DraftDragGain * DraftFactor);
	Out.Drag = Q * Speed * Speed * Out.CdEffective;
	const float V2 = ForwardSpeed * ForwardSpeed;
	Out.DownforceFront = Q * V2 * LiftArea * FrontBalance * (1.f - DraftFrontLiftGain * DraftFactor);
	Out.DownforceRear = Q * V2 * LiftArea * (1.f - FrontBalance) * (1.f - DraftRearLiftGain * DraftFactor);
	return Out;
}

float FGearbox::TotalRatio() const
{
	if (Gear == 0) return -Spec->ReverseRatio * Spec->FinalDrive;
	return Spec->GearRatios[Gear - 1] * Spec->FinalDrive;
}

float FGearbox::RpmAt(float Speed, int32 G) const
{
	const float WheelOmega = Speed / Spec->WheelRadius;
	return WheelOmega * Spec->GearRatios[G - 1] * Spec->FinalDrive * 60.f / UE_TWO_PI;
}

void FGearbox::SelectForSpeed(float Speed)
{
	for (int32 G = 1; G <= TopGear(); G++)
	{
		if (RpmAt(Speed, G) < Spec->AutoUpshiftRpm * 0.93f)
		{
			Gear = G;
			return;
		}
	}
	Gear = TopGear();
}

void FGearbox::ShiftTo(int32 G)
{
	if (G == Gear) return;
	Gear = G;
	ShiftTimer = Spec->ShiftTime;
	ShiftCooldown = 0.35f;
}

void FGearbox::Update(float Dt, FVehicleInput& In, float Rpm, float ForwardSpeed)
{
	ShiftTimer = FMath::Max(0.f, ShiftTimer - Dt);
	ShiftCooldown = FMath::Max(0.f, ShiftCooldown - Dt);

	if (In.bShiftUpRequest)
	{
		In.bShiftUpRequest = false;
		if (Gear < TopGear()) ShiftTo(Gear + 1);
	}
	if (In.bShiftDownRequest)
	{
		In.bShiftDownRequest = false;
		if (Gear > 1 || (Gear == 1 && FMath::Abs(ForwardSpeed) < 2.f)) ShiftTo(Gear - 1);
	}

	// Reverse (both modes): hold the brake at a standstill to engage reverse, press the
	// throttle while in reverse to go back to first.
	if (Gear >= 1 && !In.bPitHold && FMath::Abs(ForwardSpeed) < 0.6f && In.Brake > 0.5f && In.Throttle < 0.05f)
	{
		ReverseHold += Dt;
		if (ReverseHold > 0.6f)
		{
			ShiftTo(0);
			ReverseHold = 0.f;
		}
	}
	else
	{
		ReverseHold = 0.f;
	}
	if (Gear == 0 && In.Throttle > 0.05f && ForwardSpeed > -2.f) ShiftTo(1);

	if (!bAutomatic || Gear == 0 || ShiftCooldown > 0.f) return;
	if (Rpm > Spec->AutoUpshiftRpm && Gear < TopGear())
	{
		ShiftTo(Gear + 1);
	}
	else if (Gear > 1)
	{
		const float DownRpm = In.Brake > 0.1f ? Spec->AutoDownshiftRpm + 1200.f : Spec->AutoDownshiftRpm;
		// only downshift if the lower gear would not over-rev
		const float LowerRpm = Rpm * Spec->GearRatios[Gear - 2] / Spec->GearRatios[Gear - 1];
		if (Rpm < DownRpm && LowerRpm < Spec->AutoUpshiftRpm - 600.f) ShiftTo(Gear - 1);
	}
}
