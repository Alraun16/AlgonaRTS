#include "Core/AlgonaSimulationSubsystem.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

int32 UAlgonaSimulationSubsystem::QueryUnitIdsInBounds(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	TArray<uint32>& OutUnitIds) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_QueryUnitIdsInBounds);

	OutUnitIds.Reset();
	if (UnitEntities.IsEmpty())
	{
		return 0;
	}

	UnitSpatialGrid.QueryUnitIds(
		WorldMin,
		WorldMax,
		OutUnitIds);

	return OutUnitIds.Num();
}
