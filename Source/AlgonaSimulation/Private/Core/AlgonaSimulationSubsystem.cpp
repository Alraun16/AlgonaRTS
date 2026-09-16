#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitFragments.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformTime.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "MassSpawnerSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Subsystems/SubsystemCollection.h"

namespace
{
	// Read when a world starts. Restart PIE after changing these values.
	TAutoConsoleVariable<int32> CVarAlgonaP0UnitCount(
		TEXT("algona.P0.UnitCount"),
		AlgonaSimulationDefaults::UnitCount,
		TEXT("Number of authoritative Mass units created at world begin play."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP0SquadSize(
		TEXT("algona.P0.SquadSize"),
		AlgonaSimulationDefaults::SquadSize,
		TEXT("Requested number of units in one P0/P1 squad."),
		ECVF_Default);

	// Период отчёта метрик в лог по реальному времени, а не по числу шагов:
	// при сильной перегрузке отчёт всё равно приходит. 0 — выключено.
	TAutoConsoleVariable<float> CVarAlgonaP2MetricsReportSeconds(
		TEXT("algona.P2.MetricsReportSeconds"),
		0.0f,
		TEXT("Wall-clock period in seconds of the simulation metrics log report. 0 disables it."),
		ECVF_Default);
}

DEFINE_LOG_CATEGORY(LogAlgonaSimulation);

#if !UE_BUILD_SHIPPING

namespace
{
	void StressMoveCommand(
		const TArray<FString>& Arguments,
		UWorld* World)
	{
		if (!World || Arguments.IsEmpty())
		{
			return;
		}

		UAlgonaSimulationSubsystem* Simulation =
			World->GetSubsystem<UAlgonaSimulationSubsystem>();

		if (Simulation)
		{
			Simulation->SetStressMoveEnabled(
				FCString::Atoi(*Arguments[0]) != 0);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GAlgonaP2StressMoveCommand(
		TEXT("algona.P2.StressMove"),
		TEXT("P2 measurement scenario: 1 keeps all squads moving through neighbouring rows, 0 stops issuing new orders."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&StressMoveCommand),
		ECVF_Cheat);

	// Отладочный приказ длины строки через обычную очередь команд.
	void SetRowLengthCommand(
		const TArray<FString>& Arguments,
		UWorld* World)
	{
		if (!World || Arguments.Num() < 2)
		{
			return;
		}

		UAlgonaSimulationSubsystem* Simulation =
			World->GetSubsystem<UAlgonaSimulationSubsystem>();

		if (!Simulation)
		{
			return;
		}

		const int32 SquadId = FCString::Atoi(*Arguments[0]);
		const int32 RowLength = FCString::Atoi(*Arguments[1]);

		if (SquadId >= 0)
		{
			Simulation->SubmitSetRowLengthCommand(SquadId, RowLength);
			return;
		}

		// SquadId < 0 — приказ всем Squad.
		for (int32 AnySquadId = 0;
			AnySquadId < Simulation->GetSquadCount();
			++AnySquadId)
		{
			Simulation->SubmitSetRowLengthCommand(AnySquadId, RowLength);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GAlgonaP2SetRowLengthCommand(
		TEXT("algona.P2.SetRowLength"),
		TEXT("Order squad row length through the command queue: SquadId (-1 = all squads) RowLength."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&SetRowLengthCommand),
		ECVF_Cheat);
}

#endif

bool UAlgonaSimulationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Cast<UWorld>(Outer);
	if (!World)
	{
		return false;
	}

	const bool bPlayableWorld =
		World->WorldType == EWorldType::Game
		|| World->WorldType == EWorldType::PIE;

	// Simulation runs in standalone and on the authoritative server, not on
	// ordinary network clients.
	return bPlayableWorld && World->GetNetMode() != NM_Client;
}

void UAlgonaSimulationSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	MassEntitySubsystem =
		Collection.InitializeDependency<UMassEntitySubsystem>();
	MassSpawnerSubsystem =
		Collection.InitializeDependency<UMassSpawnerSubsystem>();

	if (MassEntitySubsystem)
	{
		InitializeQueries();
	}
}

void UAlgonaSimulationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	(void)InWorld;

	FixedStepAccumulator.Reset();
	SimulationTick = 0;
	StateRevision = 0;
	Metrics = FAlgonaSimulationMetrics();
	MetricsReportWindow = FAlgonaMetricsReportWindow();
	bStressMoveEnabled = false;
	StressMoveDirections.Reset();

	if (!IsAuthoritativeSimulationWorld())
	{
		return;
	}

	if (!MassEntitySubsystem || !MassSpawnerSubsystem)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::MissingMassServices;
		return;
	}

	const int32 UnitCount = FMath::Clamp(
		CVarAlgonaP0UnitCount.GetValueOnGameThread(),
		1,
		500000);

	const int32 SquadSize = FMath::Clamp(
		CVarAlgonaP0SquadSize.GetValueOnGameThread(),
		1,
		1000);

	if (!CreateUnits(UnitCount, SquadSize))
	{
		DestroyUnits();
		return;
	}

#if !UE_BUILD_SHIPPING
	// Разовая полная проверка связи Squad <-> Unit после создания армии.
	if (!ValidateSquadMembership())
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::SquadInitializationFailed;
		DestroyUnits();
		return;
	}
#endif

	// First complete authoritative state is now available to Presentation.
	++StateRevision;

	Metrics.StartupState = EAlgonaSimulationStartupState::Ready;
	Metrics.EntityCount = UnitEntities.Num();
	Metrics.SquadCount = Squads.Num();
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	UnitUpdateQuery.Reset();
	UnitSnapshotQuery.Reset();

	// During world teardown Mass can already be deinitialized, so do not call
	// DestroyUnits() here. The world owns and tears down the entity manager.
	UnitEntities.Reset();
	Squads.Reset();
	SquadSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	UnitSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	PendingCommands.Reset();
	bStressMoveEnabled = false;
	StressMoveDirections.Reset();
	UnitEntityConfig = nullptr;

	Metrics.EntityCount = 0;
	Metrics.SquadCount = 0;

	MassSpawnerSubsystem = nullptr;
	MassEntitySubsystem = nullptr;
	FixedStepAccumulator.Reset();

	Super::Deinitialize();
}

void UAlgonaSimulationSubsystem::Tick(float DeltaTime)
{
	if (Metrics.StartupState != EAlgonaSimulationStartupState::Ready
		|| !IsAuthoritativeSimulationWorld()
		|| !MassEntitySubsystem
		|| UnitEntities.IsEmpty())
	{
		return;
	}

	Metrics.LastExecutedStepsThisFrame =
		FixedStepAccumulator.Advance(
			static_cast<double>(DeltaTime),
			[this](double StepSeconds)
			{
				RunSimulationStep(static_cast<float>(StepSeconds));
			});

	Metrics.BacklogSeconds = FixedStepAccumulator.GetBacklogSeconds();
	Metrics.OverloadedFrameCount =
		FixedStepAccumulator.GetOverloadedFrameCount();

	UpdateMetricsReport(Metrics.LastExecutedStepsThisFrame);
}

TStatId UAlgonaSimulationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UAlgonaSimulationSubsystem,
		STATGROUP_Tickables);
}

FAlgonaSimulationMetrics UAlgonaSimulationSubsystem::GetSimulationMetrics() const
{
	FAlgonaSimulationMetrics Snapshot = Metrics;
	Snapshot.SimulationTick = SimulationTick;
	Snapshot.EntityCount = UnitEntities.Num();
	Snapshot.SquadCount = Squads.Num();
	Snapshot.BacklogSeconds = FixedStepAccumulator.GetBacklogSeconds();
	Snapshot.OverloadedFrameCount =
		FixedStepAccumulator.GetOverloadedFrameCount();
	return Snapshot;
}

int32 UAlgonaSimulationSubsystem::ExportUnitSnapshots(
	TArray<FAlgonaUnitSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSnapshots);

	OutSnapshots.Reset();

	if (!MassEntitySubsystem
		|| !UnitSnapshotQuery
		|| MaxEntities <= 0)
	{
		return 0;
	}

	const int32 SafeMaxEntities = FMath::Min(
		MaxEntities,
		UnitEntities.Num());

	if (SafeMaxEntities <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SafeMaxEntities);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	UnitSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[&OutSnapshots, SafeMaxEntities](FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaUnitIdFragment> Ids =
				Context.GetFragmentView<FAlgonaUnitIdFragment>();
			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities()
					&& OutSnapshots.Num() < SafeMaxEntities;
				++Index)
			{
				const FTransform& Transform =
					Transforms[Index].GetTransform();

				FAlgonaUnitSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();
				Snapshot.EntityId = Ids[Index].Value;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		});

	return OutSnapshots.Num();
}

bool UAlgonaSimulationSubsystem::SubmitMoveSquadCommand(
	int32 SquadId,
	const FVector& TargetLocation)
{
	if (!IsAuthoritativeSimulationWorld()
		|| !Squads.IsValidIndex(SquadId)
		|| Squads[SquadId].SquadId != SquadId)
	{
		return false;
	}

	FAlgonaSquadCommand& Command =
		PendingCommands.AddDefaulted_GetRef();
	Command.Type = EAlgonaSquadCommandType::Move;
	Command.SquadId = SquadId;
	Command.TargetLocation = TargetLocation;
	return true;
}

bool UAlgonaSimulationSubsystem::SubmitSetRowLengthCommand(
	int32 SquadId,
	int32 RowLength)
{
	if (!IsAuthoritativeSimulationWorld()
		|| !Squads.IsValidIndex(SquadId)
		|| Squads[SquadId].SquadId != SquadId)
	{
		return false;
	}

	FAlgonaSquadCommand& Command =
		PendingCommands.AddDefaulted_GetRef();
	Command.Type = EAlgonaSquadCommandType::SetRowLength;
	Command.SquadId = SquadId;
	Command.RowLength = RowLength;
	return true;
}

int32 UAlgonaSimulationSubsystem::SubmitMoveAllSquadsByOffset(
	const FVector& Offset)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		return 0;
	}

	int32 SubmittedCommands = 0;

	for (const FAlgonaSquad& Squad : Squads)
	{
		if (SubmitMoveSquadCommand(
			Squad.SquadId,
			Squad.CenterLocation + Offset))
		{
			++SubmittedCommands;
		}
	}

	return SubmittedCommands;
}

void UAlgonaSimulationSubsystem::SetStressMoveEnabled(bool bEnabled)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		return;
	}

	bStressMoveEnabled = bEnabled && !Squads.IsEmpty();
	StressMoveDirections.Reset();

	if (bStressMoveEnabled)
	{
		// Чётные ряды отрядов идут в +Y, нечётные в -Y, поэтому соседние
		// ряды проходят друг сквозь друга.
		StressMoveDirections.SetNum(Squads.Num());

		for (int32 SquadIndex = 0;
			SquadIndex < Squads.Num();
			++SquadIndex)
		{
			const int32 Row =
				SquadIndex / AlgonaSimulationDefaults::SquadsPerRow;

			StressMoveDirections[SquadIndex] =
				(Row % 2 == 0) ? 1 : -1;
		}
	}

	UE_LOG(
		LogAlgonaSimulation,
		Display,
		TEXT("[P2 StressMove] %s squads=%d distance=%.0f cm"),
		bStressMoveEnabled ? TEXT("ON") : TEXT("OFF"),
		Squads.Num(),
		AlgonaSimulationDefaults::StressMoveDistanceCm);
}

void UAlgonaSimulationSubsystem::SubmitStressMoveCommands()
{
	// Отряд, завершивший проход, получает приказ идти обратно.
	// Команда разбирается в этом же шаге, поэтому повторной подачи не будет.
	const int32 SquadCount = FMath::Min(
		Squads.Num(),
		StressMoveDirections.Num());

	for (int32 SquadIndex = 0;
		SquadIndex < SquadCount;
		++SquadIndex)
	{
		const FAlgonaSquad& Squad = Squads[SquadIndex];

		if (Squad.bHasMoveTarget)
		{
			continue;
		}

		int8& Direction = StressMoveDirections[SquadIndex];

		SubmitMoveSquadCommand(
			Squad.SquadId,
			Squad.CenterLocation
				+ FVector(
					0.0,
					Direction * AlgonaSimulationDefaults::StressMoveDistanceCm,
					0.0));

		Direction = -Direction;
	}
}

void UAlgonaSimulationSubsystem::AccumulateMetricsReportStep()
{
	FAlgonaMetricsReportWindow& Window = MetricsReportWindow;

	++Window.StepCount;
	Window.StepMillisecondsSum += Metrics.LastStepMilliseconds;
	Window.StepMillisecondsMax = FMath::Max(
		Window.StepMillisecondsMax,
		Metrics.LastStepMilliseconds);
	Window.CommandsMillisecondsSum += Metrics.LastCommandsMilliseconds;
	Window.SquadsMillisecondsSum += Metrics.LastSquadsMilliseconds;
	Window.GatherMillisecondsSum += Metrics.LastGatherMilliseconds;
	Window.SteerMillisecondsSum += Metrics.LastSteerMilliseconds;
	Window.ScatterMillisecondsSum += Metrics.LastScatterMilliseconds;
	Window.MovedEntitiesSum += Metrics.LastMovedEntities;
}

void UAlgonaSimulationSubsystem::UpdateMetricsReport(
	int32 ExecutedStepsThisFrame)
{
	const double ReportSeconds = static_cast<double>(
		CVarAlgonaP2MetricsReportSeconds.GetValueOnGameThread());

	FAlgonaMetricsReportWindow& Window = MetricsReportWindow;

	if (ReportSeconds <= 0.0)
	{
		Window = FAlgonaMetricsReportWindow();
		return;
	}

	const double NowSeconds = FPlatformTime::Seconds();

	// Начало нового окна. Шаги, выполненные до этого момента, отбрасываются,
	// чтобы окно содержало только полные кадры.
	auto StartWindow = [this, &Window, NowSeconds]()
	{
		Window = FAlgonaMetricsReportWindow();
		Window.StartWallSeconds = NowSeconds;
		Window.StartOverloadedFrameCount =
			FixedStepAccumulator.GetOverloadedFrameCount();
	};

	if (Window.StartWallSeconds <= 0.0)
	{
		StartWindow();
		return;
	}

	Window.MaxStepsPerFrame = FMath::Max(
		Window.MaxStepsPerFrame,
		ExecutedStepsThisFrame);

	const double WindowSeconds = NowSeconds - Window.StartWallSeconds;
	if (WindowSeconds < ReportSeconds)
	{
		return;
	}

	// Одна строка сводки за окно: частота шагов, средняя и худшая стоимость
	// одного шага, разбивка по стадиям и состояние накопителя шагов.
	const double InverseStepCount = Window.StepCount > 0
		? 1.0 / static_cast<double>(Window.StepCount)
		: 0.0;

	UE_LOG(
		LogAlgonaSimulation,
		Display,
		TEXT("[P2 Metrics] units=%d window=%.2fs steps=%d (%.1f Hz) maxSteps/frame=%d | step avg=%.2f max=%.2f ms | commands=%.2f squads=%.2f gather=%.2f steer=%.2f scatter=%.2f ms | changed avg=%lld | backlog=%.3fs overloaded+=%llu | parallel=%s workers=%d | stress=%s"),
		UnitEntities.Num(),
		WindowSeconds,
		Window.StepCount,
		static_cast<double>(Window.StepCount) / WindowSeconds,
		Window.MaxStepsPerFrame,
		Window.StepMillisecondsSum * InverseStepCount,
		Window.StepMillisecondsMax,
		Window.CommandsMillisecondsSum * InverseStepCount,
		Window.SquadsMillisecondsSum * InverseStepCount,
		Window.GatherMillisecondsSum * InverseStepCount,
		Window.SteerMillisecondsSum * InverseStepCount,
		Window.ScatterMillisecondsSum * InverseStepCount,
		static_cast<long long>(
			static_cast<double>(Window.MovedEntitiesSum) * InverseStepCount),
		FixedStepAccumulator.GetBacklogSeconds(),
		static_cast<unsigned long long>(
			FixedStepAccumulator.GetOverloadedFrameCount()
				- Window.StartOverloadedFrameCount),
		Metrics.bLastParallelMovement ? TEXT("ON") : TEXT("OFF"),
		FTaskGraphInterface::Get().GetNumWorkerThreads(),
		bStressMoveEnabled ? TEXT("ON") : TEXT("OFF"));

	StartWindow();
}

void UAlgonaSimulationSubsystem::InitializeQueries()
{
	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	UnitUpdateQuery =
		MakeUnique<FMassEntityQuery>(EntityManager.AsShared());
	UnitUpdateQuery->AddRequirement<FTransformFragment>(
		EMassFragmentAccess::ReadWrite);
	UnitUpdateQuery->AddRequirement<FAlgonaUnitIdFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitUpdateQuery->AddRequirement<FAlgonaSquadMemberFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitUpdateQuery->AddRequirement<FAlgonaUnitMovementFragment>(
		EMassFragmentAccess::ReadWrite);
	UnitUpdateQuery->AddTagRequirement<FAlgonaUnitTag>(
		EMassFragmentPresence::All);

	// Конвейер движения не отправляет отложенные команды Mass, поэтому
	// параллельным задачам не нужны собственные буферы команд.
	UnitUpdateQuery->SetParallelCommandBufferEnabled(false);

	UnitSnapshotQuery =
		MakeUnique<FMassEntityQuery>(EntityManager.AsShared());
	UnitSnapshotQuery->AddRequirement<FAlgonaUnitIdFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitSnapshotQuery->AddRequirement<FTransformFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitSnapshotQuery->AddRequirement<FAlgonaSquadMemberFragment>(
		EMassFragmentAccess::ReadOnly);
	UnitSnapshotQuery->AddTagRequirement<FAlgonaUnitTag>(
		EMassFragmentPresence::All);
}

bool UAlgonaSimulationSubsystem::IsAuthoritativeSimulationWorld() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_Client;
}
