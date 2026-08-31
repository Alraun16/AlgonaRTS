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

int32 UAlgonaSimulationSubsystem::QuerySquadsInBounds(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	TArray<FAlgonaSquadSpatialSnapshot>& OutSquads) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_QuerySquadsInBounds);

	OutSquads.Reset();
	if (!IsSquadSpatialGridEnabled() || Squads.IsEmpty())
	{
		return 0;
	}

	TArray<int32> CandidateSquadIds;
	SquadSpatialGrid.QuerySquadIds(WorldMin, WorldMax, CandidateSquadIds);

	// Cell arrays can change insertion order as squads cross cells. The dormant
	// squad path keeps stable SquadId ordering so it can be reconnected safely.
	CandidateSquadIds.Sort();
	OutSquads.Reserve(CandidateSquadIds.Num());

	for (const int32 SquadId : CandidateSquadIds)
	{
		if (!Squads.IsValidIndex(SquadId)
			|| Squads[SquadId].SquadId != SquadId)
		{
			continue;
		}

		FAlgonaSquadSpatialSnapshot& Snapshot =
			OutSquads.AddDefaulted_GetRef();
		Snapshot.SquadId = SquadId;
		Snapshot.Center = Squads[SquadId].GetSpatialCenter();
	}

	return OutSquads.Num();
}
