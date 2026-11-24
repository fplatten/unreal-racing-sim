#include "Sim/VehicleModel.h"

float FVehicleSpec::EngineTorque(float Rpm) const
{
	if (Rpm <= TorqueRpm[0]) return TorqueNm[0];
	for (int32 i = 1; i < TorqueRpm.Num(); i++)
	{
		if (Rpm <= TorqueRpm[i])
		{
			const float T = (Rpm - TorqueRpm[i - 1]) / (TorqueRpm[i] - TorqueRpm[i - 1]);
			return TorqueNm[i - 1] + T * (TorqueNm[i] - TorqueNm[i - 1]);
		}
	}
	return TorqueNm.Last();
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
