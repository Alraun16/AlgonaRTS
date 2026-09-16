#include "Army/AlgonaUnitSteering.h"

FVector2f AlgonaUnitSteering::ComputeDesiredVelocity(
	const FVector2f& ToSlotAtTickStart,
	const FVector2f& SlotVelocity,
	float MaxSpeed,
	float DeltaTime)
{
	if (MaxSpeed <= 0.0f || DeltaTime <= 0.0f)
	{
		return FVector2f::ZeroVector;
	}

	FVector2f Correction = FVector2f::ZeroVector;
	const float Distance = ToSlotAtTickStart.Size();

	if (Distance > UE_KINDA_SMALL_NUMBER)
	{
		// Скорость поправки ограничена тремя условиями:
		// - не больше максимальной скорости Unit;
		// - не больше скорости, с которой Unit успеет остановиться у слота
		//   с замедлением ArrivalDeceleration;
		// - не больше, чем нужно, чтобы дойти до слота ровно за один тик.
		const float CorrectionSpeed = FMath::Min3(
			MaxSpeed,
			FMath::Sqrt(2.0f * ArrivalDeceleration * Distance),
			Distance / DeltaTime);

		Correction = ToSlotAtTickStart * (CorrectionSpeed / Distance);
	}

	// Итоговая скорость тоже не превышает максимальную: при отставании на
	// марше запас на догон равен MaxSpeed минус скорость слота.
	FVector2f Desired = SlotVelocity + Correction;

	const float SpeedSquared = Desired.SizeSquared();
	if (SpeedSquared > FMath::Square(MaxSpeed))
	{
		Desired *= MaxSpeed / FMath::Sqrt(SpeedSquared);
	}

	return Desired;
}

float AlgonaUnitSteering::StepYawTowards(
	float CurrentYaw,
	float TargetYaw,
	float MaxStep)
{
	// Разница углов в диапазоне [-PI, PI] — кратчайшее направление поворота.
	const float Delta = FMath::FindDeltaAngleRadians(CurrentYaw, TargetYaw);

	if (FMath::Abs(Delta) <= UE_KINDA_SMALL_NUMBER)
	{
		return CurrentYaw;
	}

	if (FMath::Abs(Delta) <= MaxStep)
	{
		return FMath::UnwindRadians(CurrentYaw + Delta);
	}

	return FMath::UnwindRadians(
		CurrentYaw + FMath::Sign(Delta) * MaxStep);
}
