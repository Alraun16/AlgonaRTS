#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitSteering.h"

#include "Async/ParallelFor.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

/*
 * Движение Unit за один fixed step.
 *
 * Состояние Unit хранится в плоских массивах UnitState (индекс = UnitId - 1),
 * и это источник истины: копирования из Mass и обратно нет.
 *   1. SteerUnits     — L2: желаемая скорость, новая позиция, поворот;
 *   2. UpdateUnitGrid — обновление Unit Grid для Unit, сменивших ячейку.
 * Все Unit обрабатываются каждый тик: Unit всегда потенциально подвижен.
 *
 * Многопоточность: расчёт одного Unit пишет только в элементы этого Unit,
 * а Squad, раскладки и Unit Grid в это время только читаются. Поэтому
 * результат одинаков при любом числе потоков. Unit Grid хранит ячейки в TMap
 * и изменяется только последовательно, после расчёта.
 */

namespace
{
	TAutoConsoleVariable<int32> CVarAlgonaP2ParallelMovement(
		TEXT("algona.P2.ParallelMovement"),
		1,
		TEXT("1 = unit movement uses worker threads, 0 = single thread. Results are identical."),
		ECVF_Default);

	// Минимальная порция Unit на одну задачу SteerUnits: на более мелких
	// порциях раздача задач потокам стоит дороже самого расчёта.
	constexpr int32 SteerMinBatchSize = 1024;
}

bool UAlgonaSimulationSubsystem::IsParallelMovementEnabled() const
{
	return CVarAlgonaP2ParallelMovement.GetValueOnGameThread() != 0;
}

void UAlgonaSimulationSubsystem::InitializeUnitState(int32 UnitCount)
{
	FAlgonaUnitStateArrays& State = UnitState;

	State.Positions.Init(FVector::ZeroVector, UnitCount);
	State.FacingYaws.Init(0.0f, UnitCount);
	State.Velocities.Init(FVector2f::ZeroVector, UnitCount);
	State.SquadIds.Init(INDEX_NONE, UnitCount);
	State.SlotIndices.Init(INDEX_NONE, UnitCount);
	State.DesiredVelocities.Init(FVector2f::ZeroVector, UnitCount);
	State.ChangedFlags.Init(0, UnitCount);
	State.CellChangedFlags.Init(0, UnitCount);
}

void UAlgonaSimulationSubsystem::SteerUnits(
	float DeltaTime,
	bool bParallel)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_SteerUnits);

	if (DeltaTime <= 0.0f)
	{
		return;
	}

	// Подготовка Squad: направления, скорость центра и максимальная скорость
	// Unit считаются один раз на Squad, а не для каждого Unit.
	SquadMovementFrames.SetNum(Squads.Num(), EAllowShrinking::No);

	for (int32 SquadIndex = 0; SquadIndex < Squads.Num(); ++SquadIndex)
	{
		const FAlgonaSquad& Squad = Squads[SquadIndex];
		FAlgonaSquadMovementFrame& Frame = SquadMovementFrames[SquadIndex];

		Frame.Forward = Squad.GetForwardDirection2D();
		Frame.Right = FVector::CrossProduct(
			FVector::UpVector,
			Frame.Forward).GetSafeNormal();
		Frame.FacingYaw = static_cast<float>(
			FMath::Atan2(Frame.Forward.Y, Frame.Forward.X));
		Frame.CenterVelocity = FVector2f(
			static_cast<float>(Squad.CenterVelocity.X),
			static_cast<float>(Squad.CenterVelocity.Y));
		Frame.YawRate = Squad.YawRate;
		Frame.UnitMaxSpeed =
			Squad.CenterMoveSpeed
			* (Squad.YawRate != 0.0f
				? Squad.TurningUnitSpeedFactor
				: Squad.UnitSpeedFactor);
	}

	FAlgonaUnitStateArrays& State = UnitState;

	const float MaxTurnStep =
		FMath::DegreesToRadians(AlgonaUnitSteering::MaxTurnRateDegrees)
		* DeltaTime;
	const float FaceMovementMinDistanceSquared =
		FMath::Square(AlgonaUnitSteering::FaceMovementMinDistance);
	const float IdleSpeedSquared =
		FMath::Square(AlgonaUnitSteering::IdleSpeed);

	// Расчёт одного Unit. Пишет только в элементы UnitIndex, Squad, раскладки
	// и Unit Grid только читает — поэтому безопасен для параллельного вызова.
	auto SteerUnit =
		[this, &State, DeltaTime, MaxTurnStep,
			FaceMovementMinDistanceSquared, IdleSpeedSquared](int32 UnitIndex)
		{
			FVector2f& Velocity = State.Velocities[UnitIndex];
			uint8& bChanged = State.ChangedFlags[UnitIndex];
			bChanged = 0;
			State.CellChangedFlags[UnitIndex] = 0;

			// Unit без Squad или без слота стоит на месте.
			const int32 SquadId = State.SquadIds[UnitIndex];
			const int32 SlotIndex = State.SlotIndices[UnitIndex];

			if (!Squads.IsValidIndex(SquadId)
				|| !Squads[SquadId].FormationLayout.Slots.IsValidIndex(SlotIndex))
			{
				State.DesiredVelocities[UnitIndex] = FVector2f::ZeroVector;
				Velocity = FVector2f::ZeroVector;
				return;
			}

			const FAlgonaSquad& Squad = Squads[SquadId];
			const FAlgonaSquadMovementFrame& Frame =
				SquadMovementFrames[SquadId];
			const FVector2f& LocalOffset =
				Squad.FormationLayout.Slots[SlotIndex].LocalOffset;

			const FVector SlotPosition = Squad.CenterLocation
				+ Frame.Forward * static_cast<double>(LocalOffset.X)
				+ Frame.Right * static_cast<double>(LocalOffset.Y);

			FVector& Position = State.Positions[UnitIndex];

			// Скорость слота = скорость центра + вклад поворота строя.
			// При повороте на YawRate вектор «вперёд» движется вдоль «вправо»,
			// а «вправо» — против «вперёд», поэтому вклад поворота равен
			// YawRate * (X * Right - Y * Forward). Дальние слоты быстрее.
			const FVector2f SlotVelocity(
				Frame.CenterVelocity.X + Frame.YawRate * static_cast<float>(
					LocalOffset.X * Frame.Right.X - LocalOffset.Y * Frame.Forward.X),
				Frame.CenterVelocity.Y + Frame.YawRate * static_cast<float>(
					LocalOffset.X * Frame.Right.Y - LocalOffset.Y * Frame.Forward.Y));

			// L2: желаемая скорость. Squad в этом тике уже сдвинут и повёрнут,
			// поэтому отставание считается от положения слота в начале тика.
			const FVector2f ToSlotAtTickStart(
				static_cast<float>(SlotPosition.X - Position.X)
					- SlotVelocity.X * DeltaTime,
				static_cast<float>(SlotPosition.Y - Position.Y)
					- SlotVelocity.Y * DeltaTime);

			const FVector2f DesiredVelocity =
				AlgonaUnitSteering::ComputeDesiredVelocity(
					ToSlotAtTickStart,
					SlotVelocity,
					Frame.UnitMaxSpeed,
					DeltaTime);

			State.DesiredVelocities[UnitIndex] = DesiredVelocity;

			// Фактическая скорость пока равна желаемой. На шаге инерции здесь
			// появится ограничение ускорения.
			Velocity = DesiredVelocity;

			// Новая позиция.
			const FVector2f MoveDelta = Velocity * DeltaTime;
			if (MoveDelta.SizeSquared() > UE_KINDA_SMALL_NUMBER)
			{
				Position.X += MoveDelta.X;
				Position.Y += MoveDelta.Y;
				bChanged = 1;
			}

			if (Position.Z != SlotPosition.Z)
			{
				Position.Z = SlotPosition.Z;
				bChanged = 1;
			}

			// Поворот: далеко от слота и в движении — по ходу движения,
			// в слоте или рядом — по направлению Squad. Поворот плавный.
			const float DistanceToSlotSquared = FVector2f(
				static_cast<float>(SlotPosition.X - Position.X),
				static_cast<float>(SlotPosition.Y - Position.Y)).SizeSquared();

			const bool bFaceMovement =
				DistanceToSlotSquared > FaceMovementMinDistanceSquared
				&& Velocity.SizeSquared() > IdleSpeedSquared;

			const float TargetYaw = bFaceMovement
				? static_cast<float>(FMath::Atan2(Velocity.Y, Velocity.X))
				: Frame.FacingYaw;

			float& FacingYaw = State.FacingYaws[UnitIndex];
			const float NewFacingYaw = AlgonaUnitSteering::StepYawTowards(
				FacingYaw,
				TargetYaw,
				MaxTurnStep);

			if (NewFacingYaw != FacingYaw)
			{
				FacingYaw = NewFacingYaw;
				bChanged = 1;
			}

			// Unit Grid здесь только читается: помечаются Unit, которым нужна
			// другая ячейка. Сама сетка обновляется последовательно.
			if (bChanged != 0
				&& UnitSpatialGrid.NeedsCellUpdate(
					static_cast<uint32>(UnitIndex + 1),
					Position))
			{
				State.CellChangedFlags[UnitIndex] = 1;
			}
		};

	const int32 UnitCount = State.Positions.Num();

	if (bParallel)
	{
		ParallelFor(
			TEXT("AlgonaSimulation_SteerUnits"),
			UnitCount,
			SteerMinBatchSize,
			SteerUnit);
	}
	else
	{
		for (int32 UnitIndex = 0; UnitIndex < UnitCount; ++UnitIndex)
		{
			SteerUnit(UnitIndex);
		}
	}
}

int32 UAlgonaSimulationSubsystem::UpdateUnitGrid()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateUnitGrid);

	const FAlgonaUnitStateArrays& State = UnitState;
	int32 ChangedEntities = 0;

	// Последовательно: запись в TMap сетки из нескольких потоков небезопасна.
	for (int32 UnitIndex = 0;
		UnitIndex < State.ChangedFlags.Num();
		++UnitIndex)
	{
		ChangedEntities += State.ChangedFlags[UnitIndex];

		if (State.CellChangedFlags[UnitIndex] != 0)
		{
			UnitSpatialGrid.UpdateUnit(
				static_cast<uint32>(UnitIndex + 1),
				State.Positions[UnitIndex]);
		}
	}

	return ChangedEntities;
}
