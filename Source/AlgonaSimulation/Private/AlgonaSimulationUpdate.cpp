#include "AlgonaSimulationSubsystem.h"

#include "AlgonaSoldierFragments.h"

#include "HAL/PlatformTime.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassExecutionContext.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

void UAlgonaSimulationSubsystem::RunSimulationStep(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);
	const double StartSeconds = FPlatformTime::Seconds();

	(void)DeltaTime;
	++SimulationTick;

	const int32 VisitedEntities = UpdateSoldiersForP0();

	const double StepMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	Metrics.SimulationTick = static_cast<int64>(SimulationTick);
	Metrics.EntityCount = SoldierEntities.Num();
	Metrics.SquadCount = Squads.Num();
	Metrics.LastVisitedEntities = VisitedEntities;
	Metrics.LastMovedEntities = 0;
	Metrics.LastStepMilliseconds = StepMilliseconds;
}

int32 UAlgonaSimulationSubsystem::UpdateSoldiersForP0()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_UpdateSoldiersP0);

	if (!MassEntitySubsystem || !SoldierUpdateQuery)
	{
		return 0;
	}

	int32 VisitedEntities = 0;

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(
			static_cast<float>(
				FixedStepAccumulator.GetFixedStepSeconds()));

	SoldierUpdateQuery->ForEachEntityChunk(
		ExecutionContext,
		[this, &VisitedEntities](FMassExecutionContext& Context)
		{
			TArrayView<FAlgonaSoldierMovementFragment> Movement =
				Context.GetMutableFragmentView<
					FAlgonaSoldierMovementFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				Movement[Index].LastProcessedSimulationTick =
					SimulationTick;
				Movement[Index].Velocity = FVector::ZeroVector;
				Movement[Index].State =
					EAlgonaSoldierMovementState::Idle;
				++VisitedEntities;
			}
		});

	return VisitedEntities;
}