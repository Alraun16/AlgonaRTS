#include "Core/AlgonaSimulationSubsystem.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

int32 UAlgonaSimulationSubsystem::QuerySquadsInBounds(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	TArray<FAlgonaSquadSpatialSnapshot>& OutSquads) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_QuerySquadsInBounds);

	OutSquads.Reset();
	if (Squads.IsEmpty())
	{
		return 0;
	}

	TArray<int32> CandidateSquadIds;
	SquadSpatialGrid.QuerySquadIds(WorldMin, WorldMax, CandidateSquadIds);

	// Grid cell arrays can change insertion order as moving squads cross cells.
	// Stable SquadId ordering prevents needless Presentation instance rebuilds.
	CandidateSquadIds.Sort();
	OutSquads.Reserve(CandidateSquadIds.Num());

	for (const int32 SquadId : CandidateSquadIds)
	{
		if (!Squads.IsValidIndex(SquadId)
			|| Squads[SquadId].SquadId != SquadId)
		{
			continue;
		}

		FAlgonaSquadSpatialSnapshot& Snapshot = OutSquads.AddDefaulted_GetRef();
		Snapshot.SquadId = SquadId;
		Snapshot.Center = Squads[SquadId].GetSpatialCenter();
	}

	return OutSquads.Num();
}
