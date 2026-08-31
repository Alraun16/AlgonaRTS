#include "Core/AlgonaSimulationSubsystem.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY_STATIC(
	LogAlgonaSpatialQueryBenchmark,
	Log,
	All);

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

#if !UE_BUILD_SHIPPING

void UAlgonaSimulationSubsystem::BenchmarkSpatialGridQueries(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	int32 Iterations) const
{
	if (!IsSquadSpatialGridEnabled()
		|| !IsSoldierSpatialGridEnabled())
	{
		UE_LOG(
			LogAlgonaSpatialQueryBenchmark,
			Warning,
			TEXT(
				"Spatial query benchmark requires "
				"algona.P2.SpatialGridMode 3"));

		return;
	}

	const int32 SafeIterations =
		FMath::Clamp(Iterations, 1, 100000);

	TArray<int32> SquadIds;
	TArray<uint32> SoldierIds;

	// Warm the containers before timing.
	SquadSpatialGrid.QuerySquadIds(
		WorldMin,
		WorldMax,
		SquadIds);

	SoldierSpatialGrid.QuerySoldierIds(
		WorldMin,
		WorldMax,
		SoldierIds);

	const double SquadStartSeconds =
		FPlatformTime::Seconds();

	for (int32 Iteration = 0;
		Iteration < SafeIterations;
		++Iteration)
	{
		SquadSpatialGrid.QuerySquadIds(
			WorldMin,
			WorldMax,
			SquadIds);
	}

	const double SquadMilliseconds =
		(FPlatformTime::Seconds() - SquadStartSeconds)
		* 1000.0;

	const double SoldierStartSeconds =
		FPlatformTime::Seconds();

	for (int32 Iteration = 0;
		Iteration < SafeIterations;
		++Iteration)
	{
		SoldierSpatialGrid.QuerySoldierIds(
			WorldMin,
			WorldMax,
			SoldierIds);
	}

	const double SoldierMilliseconds =
		(FPlatformTime::Seconds() - SoldierStartSeconds)
		* 1000.0;

	UE_LOG(
		LogAlgonaSpatialQueryBenchmark,
		Display,
		TEXT(
			"Spatial query | Iterations=%d | "
			"Squads=%d, %.3f us/query | "
			"Soldiers=%d, %.3f us/query"),
		SafeIterations,
		SquadIds.Num(),
		(SquadMilliseconds * 1000.0)
			/ static_cast<double>(SafeIterations),
		SoldierIds.Num(),
		(SoldierMilliseconds * 1000.0)
			/ static_cast<double>(SafeIterations));
}
#endif