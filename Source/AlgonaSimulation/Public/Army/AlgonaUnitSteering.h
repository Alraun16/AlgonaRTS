#pragma once

#include "CoreMinimal.h"

/**
 * L2 — управление отдельным Unit на пути к его слоту.
 * Чистые функции без состояния: не зависят от Mass, Squad и мира, поэтому
 * покрываются автотестами и безопасны для многопоточного вызова.
 */
namespace AlgonaUnitSteering
{
	// Замедление при подходе к слоту, см/с². Unit сбрасывает скорость так,
	// будто тормозит с этим замедлением, и останавливается ровно в слоте.
	inline constexpr float ArrivalDeceleration = 800.0f;

	// Дальше этого расстояния от слота движущийся Unit смотрит по ходу
	// движения, ближе — по направлению Squad, см.
	inline constexpr float FaceMovementMinDistance = 100.0f;

	// Максимальная скорость поворота Unit, градусов в секунду.
	inline constexpr float MaxTurnRateDegrees = 360.0f;

	// Ниже этой скорости Unit считается стоящим, см/с.
	inline constexpr float IdleSpeed = 1.0f;

	/**
	 * Желаемая скорость Unit на плоскости: скорость слота (упреждение) плюс
	 * поправка к слоту, не больше MaxSpeed.
	 * ToSlotAtTickStart — вектор от Unit до положения слота в начале тика.
	 */
	ALGONASIMULATION_API FVector2f ComputeDesiredVelocity(
		const FVector2f& ToSlotAtTickStart,
		const FVector2f& SlotVelocity,
		float MaxSpeed,
		float DeltaTime);

	/**
	 * Ограничение ускорения: приближает текущую скорость к желаемой не
	 * быстрее MaxAcceleration (когда скорость растёт) или MaxDeceleration
	 * (когда падает), см/с².
	 */
	ALGONASIMULATION_API FVector2f StepVelocityTowards(
		const FVector2f& CurrentVelocity,
		const FVector2f& DesiredVelocity,
		float MaxAcceleration,
		float MaxDeceleration,
		float DeltaTime);

	/** То же для скалярной величины (скорость центра Squad, угловая скорость). */
	ALGONASIMULATION_API float StepValueTowards(
		float CurrentValue,
		float TargetValue,
		float MaxIncreaseRate,
		float MaxDecreaseRate,
		float DeltaTime);

	/**
	 * Наибольшая скорость, с которой ещё можно пройти Distance и
	 * остановиться ровно в конце при замедлении Deceleration: sqrt(2 a s).
	 */
	ALGONASIMULATION_API float GetArrivalSpeedLimit(
		float Distance,
		float Deceleration);

	/**
	 * Поворачивает угол CurrentYaw к TargetYaw кратчайшим путём,
	 * не больше чем на MaxStep. Все углы в радианах.
	 */
	ALGONASIMULATION_API float StepYawTowards(
		float CurrentYaw,
		float TargetYaw,
		float MaxStep);
}
