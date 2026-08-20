#include "AlgonaSimulationSubsystem.h"

#include "AlgonaSoldierFragments.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "MassSpawnerSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Subsystems/SubsystemCollection.h"

namespace
{
	// Эти значения читаются при старте мира.
	// После изменения нужно перезапустить PIE.
	TAutoConsoleVariable<int32> CVarAlgonaP0SoldierCount(
		TEXT("algona.P0.SoldierCount"),
		AlgonaSimulationDefaults::SoldierCount,
		TEXT("Number of authoritative Mass soldiers created at world begin play."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP0SquadSize(
		TEXT("algona.P0.SquadSize"),
		AlgonaSimulationDefaults::SquadSize,
		TEXT("Requested number of soldiers in one P0/P1 squad."),
		ECVF_Default);
}

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

	// Simulation работает в одиночной игре и на сервере,
	// но не создаётся на обычном сетевом клиенте.
	return bPlayableWorld && World->GetNetMode() != NM_Client;
}

void UAlgonaSimulationSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Явно получаем Mass-системы, без которых наша Simulation не работает.
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

	// ShouldCreateSubsystem уже отсеивает обычные клиенты.
	// Эта проверка остаётся как дополнительная защита.
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

	const int32 SoldierCount = FMath::Clamp(
		CVarAlgonaP0SoldierCount.GetValueOnGameThread(),
		1,
		500000);

	const int32 SquadSize = FMath::Clamp(
		CVarAlgonaP0SquadSize.GetValueOnGameThread(),
		1,
		1000);

	if (!CreateSoldiers(
		SoldierCount,
		SquadSize))
	{
		DestroySoldiers();
		return;
	}

	// Сообщает Presentation, что появился первый полный набор солдат.
	++StateRevision;

	Metrics.StartupState =
		EAlgonaSimulationStartupState::Ready;

	Metrics.EntityCount = SoldierEntities.Num();
	Metrics.SquadCount = Squads.Num();
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	SoldierUpdateQuery.Reset();
	SoldierSnapshotQuery.Reset();

	/*
	 * При teardown мира Mass уже сам уничтожает своё состояние.
	 * Здесь больше не вызываем DestroySoldiers(), потому что
	 * EntityManager к этому моменту уже может быть deinitialized.
	 */
	SoldierEntities.Reset();
	Squads.Reset();
	SoldierEntityConfig = nullptr;

	Metrics.EntityCount = 0;
	Metrics.SquadCount = 0;

	MassSpawnerSubsystem = nullptr;
	MassEntitySubsystem = nullptr;

	FixedStepAccumulator.Reset();

	Super::Deinitialize();
}

void UAlgonaSimulationSubsystem::Tick(float DeltaTime)
{
	if (Metrics.StartupState
			!= EAlgonaSimulationStartupState::Ready
		|| !IsAuthoritativeSimulationWorld()
		|| !MassEntitySubsystem
		|| SoldierEntities.IsEmpty())
	{
		return;
	}

	Metrics.LastExecutedStepsThisFrame =
		FixedStepAccumulator.Advance(
			static_cast<double>(DeltaTime),
			[this](double StepSeconds)
			{
				RunSimulationStep(
					static_cast<float>(StepSeconds));
			});

	Metrics.BacklogSeconds =
		FixedStepAccumulator.GetBacklogSeconds();

	Metrics.OverloadedFrameCount =
		FixedStepAccumulator.GetOverloadedFrameCount();
}

TStatId UAlgonaSimulationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UAlgonaSimulationSubsystem,
		STATGROUP_Tickables);
}

FAlgonaSimulationMetrics
UAlgonaSimulationSubsystem::GetSimulationMetrics() const
{
	FAlgonaSimulationMetrics Snapshot = Metrics;

	Snapshot.SimulationTick = SimulationTick;
	Snapshot.EntityCount = SoldierEntities.Num();
	Snapshot.SquadCount = Squads.Num();

	Snapshot.BacklogSeconds =
		FixedStepAccumulator.GetBacklogSeconds();

	Snapshot.OverloadedFrameCount =
		FixedStepAccumulator.GetOverloadedFrameCount();

	return Snapshot;
}

int32 UAlgonaSimulationSubsystem::ExportSoldierSnapshots(
	TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSimulation_ExportSnapshots);

	OutSnapshots.Reset();

	if (!MassEntitySubsystem
		|| !SoldierSnapshotQuery
		|| MaxEntities <= 0)
	{
		return 0;
	}

	const int32 SafeMaxEntities = FMath::Min(
		MaxEntities,
		SoldierEntities.Num());

	if (SafeMaxEntities <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SafeMaxEntities);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	SoldierSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[&OutSnapshots, SafeMaxEntities](
			FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaSoldierIdFragment> Ids =
				Context.GetFragmentView<
					FAlgonaSoldierIdFragment>();

			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities()
					&& OutSnapshots.Num() < SafeMaxEntities;
				++Index)
			{
				const FTransform& Transform =
					Transforms[Index].GetTransform();

				FAlgonaSoldierSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();

				Snapshot.EntityId = Ids[Index].Value;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		});

	return OutSnapshots.Num();
}

void UAlgonaSimulationSubsystem::InitializeQueries()
{
	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	// P0 проходит по игровому состоянию каждого солдата.
	SoldierUpdateQuery =
		MakeUnique<FMassEntityQuery>(
			EntityManager.AsShared());

	SoldierUpdateQuery
		->AddRequirement<FAlgonaSoldierMovementFragment>(
			EMassFragmentAccess::ReadWrite);

	SoldierUpdateQuery
		->AddTagRequirement<FAlgonaSoldierTag>(
			EMassFragmentPresence::All);

	// Этот запрос отдаёт Presentation только ID и положение солдата.
	SoldierSnapshotQuery =
		MakeUnique<FMassEntityQuery>(
			EntityManager.AsShared());

	SoldierSnapshotQuery
		->AddRequirement<FAlgonaSoldierIdFragment>(
			EMassFragmentAccess::ReadOnly);

	SoldierSnapshotQuery
		->AddRequirement<FTransformFragment>(
			EMassFragmentAccess::ReadOnly);

	SoldierSnapshotQuery
		->AddTagRequirement<FAlgonaSoldierTag>(
			EMassFragmentPresence::All);
}

bool UAlgonaSimulationSubsystem::IsAuthoritativeSimulationWorld() const
{
	const UWorld* World = GetWorld();

	return World
		&& World->GetNetMode() != NM_Client;
}