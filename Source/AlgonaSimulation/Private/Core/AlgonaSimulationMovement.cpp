#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitFragments.h"
#include "Army/AlgonaUnitSteering.h"

#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

/*
 * Конвейер движения Unit за один fixed step:
 *   1. GatherUnitMovementState  — сбор состояния Unit из Mass в плоские массивы;
 *   2. SteerUnits               — L2: желаемая скорость, новая позиция, поворот;
 *   3. ScatterUnitMovementState — запись результата в Mass и Unit Grid.
 * Все Unit обрабатываются каждый тик: Unit всегда потенциально подвижен.
 * Индекс в массивах — UnitId - 1.
 */

int32 UAlgonaSimulationSubsystem::GatherUnitMovementState()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_GatherUnits);

	FAlgonaUnitMovementBuffers& Buffers = UnitMovementBuffers;
	const int32 UnitCount = UnitEntities.Num();

	// Массивы переиспользуются между тиками и меняют размер только вместе
	// с числом Unit.
	if (Buffers.Positions.Num() != UnitCount)
	{
		Buffers.Positions.SetNum(UnitCount);
		Buffers.FacingYaws.SetNum(UnitCount);
		Buffers.SquadIds.SetNum(UnitCount);
		Buffers.SlotIndices.SetNum(UnitCount);
		Buffers.DesiredVelocities.SetNum(UnitCount);
		Buffers.Velocities.SetNum(UnitCount);
		Buffers.ChangedFlags.SetNum(UnitCount);
	}

	if (!MassEntitySubsystem || !UnitUpdateQuery)
	{
		return 0;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	int32 VisitedEntities = 0;

	UnitUpdateQuery->ForEachEntityChunk(
		ExecutionContext,
		[&Buffers, &VisitedEntities](FMassExecutionContext& Context)
		{
			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();
			const TConstArrayView<FAlgonaUnitIdFragment> Ids =
				Context.GetFragmentView<FAlgonaUnitIdFragment>();
			const TConstArrayView<FAlgonaSquadMemberFragment> Members =
				Context.GetFragmentView<FAlgonaSquadMemberFragment>();
			const TConstArrayView<FAlgonaUnitMovementFragment> Movement =
				Context.GetFragmentView<FAlgonaUnitMovementFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				const int32 UnitIndex =
					static_cast<int32>(Ids[Index].Value) - 1;

				if (!Buffers.Positions.IsValidIndex(UnitIndex))
				{
					continue;
				}

				Buffers.Positions[UnitIndex] =
					Transforms[Index].GetTransform().GetLocation();
				Buffers.FacingYaws[UnitIndex] =
					Movement[Index].FacingYawRadians;
				Buffers.SquadIds[UnitIndex] = Members[Index].SquadId;
				Buffers.SlotIndices[UnitIndex] = Members[Index].SlotIndex;

				++VisitedEntities;
			}
		});

	return VisitedEntities;
}

void UAlgonaSimulationSubsystem::SteerUnits(float DeltaTime)
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
		Frame.UnitMaxSpeed =
			Squad.CenterMoveSpeed * Squad.UnitSpeedFactor;
	}

	FAlgonaUnitMovementBuffers& Buffers = UnitMovementBuffers;

	const float MaxTurnStep =
		FMath::DegreesToRadians(AlgonaUnitSteering::MaxTurnRateDegrees)
		* DeltaTime;
	const float FaceMovementMinDistanceSquared =
		FMath::Square(AlgonaUnitSteering::FaceMovementMinDistance);
	const float IdleSpeedSquared =
		FMath::Square(AlgonaUnitSteering::IdleSpeed);

	for (int32 UnitIndex = 0;
		UnitIndex < Buffers.Positions.Num();
		++UnitIndex)
	{
		FVector2f& Velocity = Buffers.Velocities[UnitIndex];
		uint8& bChanged = Buffers.ChangedFlags[UnitIndex];
		bChanged = 0;

		// Unit без Squad или без слота стоит на месте.
		const int32 SquadId = Buffers.SquadIds[UnitIndex];
		const int32 SlotIndex = Buffers.SlotIndices[UnitIndex];

		if (!Squads.IsValidIndex(SquadId)
			|| !Squads[SquadId].FormationLayout.Slots.IsValidIndex(SlotIndex))
		{
			Buffers.DesiredVelocities[UnitIndex] = FVector2f::ZeroVector;
			Velocity = FVector2f::ZeroVector;
			continue;
		}

		const FAlgonaSquad& Squad = Squads[SquadId];
		const FAlgonaSquadMovementFrame& Frame = SquadMovementFrames[SquadId];
		const FVector2f& LocalOffset =
			Squad.FormationLayout.Slots[SlotIndex].LocalOffset;

		const FVector SlotPosition = Squad.CenterLocation
			+ Frame.Forward * static_cast<double>(LocalOffset.X)
			+ Frame.Right * static_cast<double>(LocalOffset.Y);

		FVector& Position = Buffers.Positions[UnitIndex];

		// L2: желаемая скорость. Центр Squad в этом тике уже сдвинут, поэтому
		// отставание считается от положения слота в начале тика.
		const FVector2f ToSlotAtTickStart(
			static_cast<float>(SlotPosition.X - Position.X)
				- Frame.CenterVelocity.X * DeltaTime,
			static_cast<float>(SlotPosition.Y - Position.Y)
				- Frame.CenterVelocity.Y * DeltaTime);

		const FVector2f DesiredVelocity =
			AlgonaUnitSteering::ComputeDesiredVelocity(
				ToSlotAtTickStart,
				Frame.CenterVelocity,
				Frame.UnitMaxSpeed,
				DeltaTime);

		Buffers.DesiredVelocities[UnitIndex] = DesiredVelocity;

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

		float& FacingYaw = Buffers.FacingYaws[UnitIndex];
		const float NewFacingYaw = AlgonaUnitSteering::StepYawTowards(
			FacingYaw,
			TargetYaw,
			MaxTurnStep);

		if (NewFacingYaw != FacingYaw)
		{
			FacingYaw = NewFacingYaw;
			bChanged = 1;
		}
	}
}

int32 UAlgonaSimulationSubsystem::ScatterUnitMovementState()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ScatterUnits);

	if (!MassEntitySubsystem || !UnitUpdateQuery)
	{
		return 0;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	const FAlgonaUnitMovementBuffers& Buffers = UnitMovementBuffers;
	const float IdleSpeedSquared =
		FMath::Square(AlgonaUnitSteering::IdleSpeed);

	int32 ChangedEntities = 0;

	UnitUpdateQuery->ForEachEntityChunk(
		ExecutionContext,
		[this, &Buffers, IdleSpeedSquared, &ChangedEntities](
			FMassExecutionContext& Context)
		{
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();
			const TConstArrayView<FAlgonaUnitIdFragment> Ids =
				Context.GetFragmentView<FAlgonaUnitIdFragment>();
			TArrayView<FAlgonaUnitMovementFragment> Movement =
				Context.GetMutableFragmentView<FAlgonaUnitMovementFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				const uint32 UnitId = Ids[Index].Value;
				const int32 UnitIndex = static_cast<int32>(UnitId) - 1;

				if (!Buffers.Positions.IsValidIndex(UnitIndex))
				{
					continue;
				}

				FAlgonaUnitMovementFragment& UnitMovement = Movement[Index];
				const FVector2f& Velocity = Buffers.Velocities[UnitIndex];

				UnitMovement.LastProcessedSimulationTick = SimulationTick;
				UnitMovement.Velocity = FVector(Velocity.X, Velocity.Y, 0.0f);
				UnitMovement.State =
					Velocity.SizeSquared() > IdleSpeedSquared
						? EAlgonaUnitMovementState::Moving
						: EAlgonaUnitMovementState::Idle;

				// Transform и Unit Grid обновляются только у Unit, у которых
				// изменились позиция или поворот.
				if (Buffers.ChangedFlags[UnitIndex] == 0)
				{
					continue;
				}

				const FVector& NewPosition = Buffers.Positions[UnitIndex];
				const float FacingYaw = Buffers.FacingYaws[UnitIndex];

				UnitMovement.FacingYawRadians = FacingYaw;

				FTransform& Transform = Transforms[Index].GetMutableTransform();
				Transform.SetLocation(NewPosition);
				Transform.SetRotation(FQuat(FVector::UpVector, FacingYaw));

				UnitSpatialGrid.UpdateUnit(UnitId, NewPosition);

				++ChangedEntities;
			}
		});

	return ChangedEntities;
}
