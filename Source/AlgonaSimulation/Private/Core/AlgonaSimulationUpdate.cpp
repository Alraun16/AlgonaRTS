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

	// Стадии 3-4: движение Unit (AlgonaSimulationMovement.cpp).
	// Режим читается один раз, чтобы весь шаг шёл в одном режиме.
	const bool bParallelMovement = IsParallelMovementEnabled();

	// L2: желаемая скорость, новая позиция и поворот каждого Unit.
	SteerUnits(DeltaTime, bParallelMovement);

	const double SteerEndSeconds =
		FPlatformTime::Seconds();

	// Обновление Unit Grid для Unit, сменивших ячейку.
	const int32 ChangedEntities = UpdateUnitGrid();

	const double UnitGridEndSeconds =
		FPlatformTime::Seconds();

	if (bSquadCentersChanged || ChangedEntities > 0)
	{
		++StateRevision;
	}

	Metrics.SimulationTick = SimulationTick;
	Metrics.EntityCount = UnitEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = UnitState.Positions.Num();
	Metrics.LastMovedEntities = ChangedEntities;
	Metrics.LastCommandsMilliseconds =
		(CommandsEndSeconds - StepStartSeconds) * 1000.0;
	Metrics.LastSquadsMilliseconds =
		(SquadsEndSeconds - CommandsEndSeconds) * 1000.0;
	Metrics.LastSteerMilliseconds =
		(SteerEndSeconds - SquadsEndSeconds) * 1000.0;
	Metrics.LastUnitGridMilliseconds =
		(UnitGridEndSeconds - SteerEndSeconds) * 1000.0;
	Metrics.bLastParallelMovement = bParallelMovement;
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
		if (Command.Type == EAlgonaSquadCommandType::MoveGroup)
		{
			ApplyMoveGroupCommand(Command);
			continue;
		}

		if (!Squads.IsValidIndex(Command.SquadId)
			|| Squads[Command.SquadId].SquadId != Command.SquadId)
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[Command.SquadId];

		switch (Command.Type)
		{
		case EAlgonaSquadCommandType::Move:
			ApplyMoveCommand(
				Squad,
				Command.TargetLocation,
				Command.FinalDirection,
				Command.RowLength);
			break;

		case EAlgonaSquadCommandType::SetRowLength:
			// Меняются только позиции слотов, назначение Unit по слотам то же,
			// поэтому состояние Unit обновлять не нужно.
			Squad.ApplyRowLengthOrder(Command.RowLength);
			break;

		case EAlgonaSquadCommandType::MoveGroup:
			break;
		}
	}

	PendingCommands.Reset();
}

void UAlgonaSimulationSubsystem::ApplyMoveCommand(
	FAlgonaSquad& Squad,
	const FVector& TargetLocation,
	const FVector& FinalDirection,
	int32 RowLength)
{
	// Новый приказ движения полностью заменяет текущий, включая конечное
	// направление и отложенную длину строки.
	Squad.TargetCenterLocation = TargetLocation;
	Squad.TargetCenterLocation.Z = Squad.CenterLocation.Z;
	Squad.bHasMoveTarget = true;

	// Без явного направления — направление последнего отрезка пути.
	// Пока путь прямой, это вектор от центра к цели; если цель совпадает
	// с центром, направление не меняется.
	FVector Direction = FinalDirection.GetSafeNormal2D();
	if (Direction.IsNearlyZero())
	{
		Direction = (Squad.TargetCenterLocation - Squad.CenterLocation).GetSafeNormal2D();
	}
	Squad.FinalFacingDirection = Direction.IsNearlyZero()
		? Squad.GetForwardDirection2D()
		: Direction;

	Squad.PendingRowLength = FMath::Max(RowLength, 0);
}

void UAlgonaSimulationSubsystem::ApplyMoveGroupCommand(
	const FAlgonaSquadCommand& Command)
{
	// Действительные Squad группы без повторов.
	TArray<FAlgonaSquad*> GroupSquads;
	GroupSquads.Reserve(Command.SquadIds.Num());

	for (const int32 SquadId : Command.SquadIds)
	{
		if (Squads.IsValidIndex(SquadId)
			&& Squads[SquadId].SquadId == SquadId)
		{
			GroupSquads.AddUnique(&Squads[SquadId]);
		}
	}

	if (GroupSquads.IsEmpty())
	{
		return;
	}

	const FVector GroupTarget = Command.TargetLocation;

	if (GroupSquads.Num() == 1)
	{
		ApplyMoveCommand(*GroupSquads[0], GroupTarget, FVector::ZeroVector, 0);
		return;
	}

	// Направление группы одно для всех: от центра группы (среднего центров
	// Squad) к цели. Временное правило до путей по миру.
	FVector GroupCenter = FVector::ZeroVector;

	for (const FAlgonaSquad* Squad : GroupSquads)
	{
		GroupCenter += Squad->CenterLocation;
	}
	GroupCenter /= static_cast<double>(GroupSquads.Num());

	FVector GroupForward = (GroupTarget - GroupCenter).GetSafeNormal2D();
	if (GroupForward.IsNearlyZero())
	{
		GroupForward = GroupSquads[0]->GetForwardDirection2D();
	}
	const FVector GroupRight =
		FVector::CrossProduct(FVector::UpVector, GroupForward).GetSafeNormal();

	// Расстояние между точками = размер самого крупного Squad (наибольшее
	// из ширины и глубины раскладки слотов) * 1.1. Небольшое пересечение
	// допустимо.
	double MaxSquadExtent = 0.0;

	for (const FAlgonaSquad* Squad : GroupSquads)
	{
		double Extent = 0.0;

		if (!Squad->FormationLayout.Slots.IsEmpty())
		{
			FVector2f Min = Squad->FormationLayout.Slots[0].LocalOffset;
			FVector2f Max = Min;

			for (const FAlgonaFormationSlot& Slot : Squad->FormationLayout.Slots)
			{
				Min = FVector2f::Min(Min, Slot.LocalOffset);
				Max = FVector2f::Max(Max, Slot.LocalOffset);
			}

			Extent = FMath::Max(Max.X - Min.X, Max.Y - Min.Y);
		}

		// Squad из одного Unit не должен давать нулевой интервал.
		MaxSquadExtent = FMath::Max(
			MaxSquadExtent,
			FMath::Max(Extent, static_cast<double>(Squad->FormationParams.SlotSpacing)));
	}

	// Точки центров строит тот же генератор формы: длина строки —
	// округлённый вверх корень из N, неполная строка по центру.
	FAlgonaFormationParams PointParams;
	PointParams.RowLength = FMath::CeilToInt32(
		FMath::Sqrt(static_cast<double>(GroupSquads.Num())));
	PointParams.SlotSpacing = static_cast<float>(
		MaxSquadExtent * AlgonaSimulationDefaults::GroupSpacingFactor);
	PointParams.RowSpacing = PointParams.SlotSpacing;

	FAlgonaFormationLayout PointLayout;
	BuildAlgonaFormationLayout(PointParams, GroupSquads.Num(), PointLayout);

	// Назначение по текущему положению, чтобы пути не перекрещивались:
	// Squad упорядочиваются спереди назад и режутся на строки, внутри
	// строки — слева направо. Точки упорядочиваются так же и
	// сопоставляются по порядку.
	GroupSquads.Sort([&GroupTarget, &GroupForward](const FAlgonaSquad& A, const FAlgonaSquad& B)
	{
		return FVector::DotProduct(A.CenterLocation - GroupTarget, GroupForward)
			> FVector::DotProduct(B.CenterLocation - GroupTarget, GroupForward);
	});

	const int32 RowLength = PointLayout.Slots.IsEmpty() ? 1 : FMath::Max(PointParams.RowLength, 1);

	for (int32 RowStart = 0; RowStart < GroupSquads.Num(); RowStart += RowLength)
	{
		const int32 RowCount = FMath::Min(RowLength, GroupSquads.Num() - RowStart);
		TArrayView<FAlgonaSquad*>(GroupSquads.GetData() + RowStart, RowCount)
			.Sort([&GroupRight](const FAlgonaSquad& A, const FAlgonaSquad& B)
			{
				return FVector::DotProduct(A.CenterLocation, GroupRight)
					< FVector::DotProduct(B.CenterLocation, GroupRight);
			});
	}

	TArray<int32> PointOrder;
	PointOrder.Reserve(PointLayout.Slots.Num());
	for (int32 SlotIndex = 0; SlotIndex < PointLayout.Slots.Num(); ++SlotIndex)
	{
		PointOrder.Add(SlotIndex);
	}

	PointOrder.Sort([&PointLayout](int32 A, int32 B)
	{
		const FAlgonaFormationSlot& SlotA = PointLayout.Slots[A];
		const FAlgonaFormationSlot& SlotB = PointLayout.Slots[B];
		return SlotA.RowIndex != SlotB.RowIndex
			? SlotA.RowIndex < SlotB.RowIndex
			: SlotA.LocalOffset.Y < SlotB.LocalOffset.Y;
	});

	const int32 AssignedCount = FMath::Min(GroupSquads.Num(), PointOrder.Num());

	for (int32 Index = 0; Index < AssignedCount; ++Index)
	{
		const FVector PointLocation = GetAlgonaSlotWorldLocation(
			GroupTarget,
			GroupForward,
			PointLayout.Slots[PointOrder[Index]].LocalOffset);

		ApplyMoveCommand(*GroupSquads[Index], PointLocation, GroupForward, 0);
	}
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

		// Составной приказ: ширина строя применяется за 10 м до цели
		// (или сразу, если путь короче). Reform сохраняет центр.
		if (Squad.PendingRowLength > 0
			&& DistanceToTarget <= FAlgonaSquad::RowLengthApplyDistanceCm)
		{
			Squad.ApplyRowLengthOrder(Squad.PendingRowLength);
			Squad.PendingRowLength = 0;
		}

		if (DistanceToTarget <= KINDA_SMALL_NUMBER)
		{
			// Центр уже в цели: приказ завершён, Squad принимает конечное
			// направление (пока мгновенно; плавно — на шаге поворота).
			Squad.CenterLocation = Squad.TargetCenterLocation;
			Squad.FacingDirection = Squad.FinalFacingDirection;
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
				Squad.FacingDirection = Squad.FinalFacingDirection;
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
