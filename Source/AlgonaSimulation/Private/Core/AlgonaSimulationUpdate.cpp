#include "Core/AlgonaSimulationSubsystem.h"

#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

void UAlgonaSimulationSubsystem::RunSimulationStep(
	float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);

	const double StepStartSeconds =
		FPlatformTime::Seconds();

	++SimulationTick;

	// Стадия 1: команды. Приказы стресс-сценария замеров подаются в ту же
	// очередь непосредственно перед её разбором.
	if (bStressMoveEnabled)
	{
		SubmitStressMoveCommands();
	}

	ProcessPendingCommands();

	const double CommandsEndSeconds =
		FPlatformTime::Seconds();

	// Стадия 2: движение центров Squad.
	const bool bSquadCentersChanged =
		UpdateSquadCenters(DeltaTime);

	const double SquadsEndSeconds =
		FPlatformTime::Seconds();

	// Стадии 3-5: конвейер движения Unit (AlgonaSimulationMovement.cpp).
	// Сбор состояния Unit из Mass в плоские массивы.
	const int32 VisitedEntities =
		GatherUnitMovementState();

	const double GatherEndSeconds =
		FPlatformTime::Seconds();

	// L2: желаемая скорость, новая позиция и поворот каждого Unit.
	SteerUnits(DeltaTime);

	const double SteerEndSeconds =
		FPlatformTime::Seconds();

	// Запись результата в Mass и Unit Grid.
	const int32 ChangedEntities =
		ScatterUnitMovementState();

	const double ScatterEndSeconds =
		FPlatformTime::Seconds();

	if (bSquadCentersChanged || ChangedEntities > 0)
	{
		++StateRevision;
	}

	Metrics.SimulationTick = SimulationTick;
	Metrics.EntityCount = UnitEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = VisitedEntities;
	Metrics.LastMovedEntities = ChangedEntities;
	Metrics.LastCommandsMilliseconds =
		(CommandsEndSeconds - StepStartSeconds) * 1000.0;
	Metrics.LastSquadsMilliseconds =
		(SquadsEndSeconds - CommandsEndSeconds) * 1000.0;
	Metrics.LastGatherMilliseconds =
		(GatherEndSeconds - SquadsEndSeconds) * 1000.0;
	Metrics.LastSteerMilliseconds =
		(SteerEndSeconds - GatherEndSeconds) * 1000.0;
	Metrics.LastScatterMilliseconds =
		(ScatterEndSeconds - SteerEndSeconds) * 1000.0;
	Metrics.LastStepMilliseconds =
		(FPlatformTime::Seconds() - StepStartSeconds) * 1000.0;

	AccumulateMetricsReportStep();
}

void UAlgonaSimulationSubsystem::ProcessPendingCommands()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ProcessCommands);

	// Команды применяются строго в порядке поступления.
	for (const FAlgonaSquadCommand& Command : PendingCommands)
	{
		if (!Squads.IsValidIndex(Command.SquadId)
			|| Squads[Command.SquadId].SquadId != Command.SquadId)
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[Command.SquadId];

		switch (Command.Type)
		{
		case EAlgonaSquadCommandType::Move:
			// Новый приказ движения полностью заменяет текущий.
			Squad.TargetCenterLocation = Command.TargetLocation;
			Squad.TargetCenterLocation.Z = Squad.CenterLocation.Z;
			Squad.bHasMoveTarget = true;
			break;

		case EAlgonaSquadCommandType::SetRowLength:
			// Меняются только позиции слотов, назначение Unit по слотам то же,
			// поэтому фрагменты Unit обновлять не нужно.
			Squad.ApplyRowLengthOrder(Command.RowLength);
			break;
		}
	}

	PendingCommands.Reset();
}

bool UAlgonaSimulationSubsystem::UpdateSquadCenters(
	float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateSquads);

	bool bAnyCenterChanged = false;

	for (FAlgonaSquad& Squad : Squads)
	{
		if (!Squad.bHasMoveTarget)
		{
			Squad.CenterVelocity = FVector::ZeroVector;
			continue;
		}

		const FVector OldCenterLocation = Squad.CenterLocation;

		FVector ToTarget =
			Squad.TargetCenterLocation - Squad.CenterLocation;
		ToTarget.Z = 0.0;

		const double DistanceToTarget = ToTarget.Length();

		if (DistanceToTarget <= KINDA_SMALL_NUMBER)
		{
			// Центр уже в цели: приказ завершён без изменения направления.
			Squad.CenterLocation = Squad.TargetCenterLocation;
			Squad.bHasMoveTarget = false;

			bAnyCenterChanged |=
				!OldCenterLocation.Equals(
					Squad.CenterLocation,
					KINDA_SMALL_NUMBER);
		}
		else
		{
			// Пока направление меняется мгновенно; плавный поворот — задача L1.
			const FVector MoveDirection = ToTarget / DistanceToTarget;
			Squad.FacingDirection = MoveDirection;

			const double MaxMoveDistance =
				static_cast<double>(Squad.CenterMoveSpeed) * DeltaTime;

			if (DistanceToTarget <= MaxMoveDistance)
			{
				Squad.CenterLocation = Squad.TargetCenterLocation;
				Squad.bHasMoveTarget = false;
			}
			else
			{
				Squad.CenterLocation += MoveDirection * MaxMoveDistance;
			}

			bAnyCenterChanged = true;
		}

		// Скорость центра за этот тик — упреждение для L2: Unit сразу идут
		// вместе с Squad, а не догоняют свои слоты.
		Squad.CenterVelocity = DeltaTime > 0.0f
			? (Squad.CenterLocation - OldCenterLocation)
				/ static_cast<double>(DeltaTime)
			: FVector::ZeroVector;
		Squad.CenterVelocity.Z = 0.0;

		if (IsSquadSpatialGridEnabled())
		{
			SquadSpatialGrid.UpdateSquad(
				Squad.SquadId,
				OldCenterLocation,
				Squad.CenterLocation);
		}
	}

	return bAnyCenterChanged;
}
