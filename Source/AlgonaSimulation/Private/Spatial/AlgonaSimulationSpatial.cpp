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

void UAlgonaSimulationSubsystem::BenchmarkSpatialSnapshotPaths(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	int32 Iterations)
{
	if (!IsSquadSpatialGridEnabled()
		|| !IsSoldierSpatialGridEnabled())
	{
		UE_LOG(
			LogAlgonaSpatialQueryBenchmark,
			Warning,
			TEXT(
				"Spatial snapshot benchmark requires "
				"algona.P2.SpatialGridMode 3"));

		return;
	}

	const int32 SafeIterations =
		FMath::Clamp(
			Iterations,
			1,
			1000);

	TArray<FAlgonaSquadSpatialSnapshot> SquadCandidates;
	TArray<int32> SquadIds;
	TArray<uint32> SoldierIds;

	TArray<FAlgonaSoldierSnapshot> SquadSnapshots;
	TArray<FAlgonaSoldierSnapshot> SoldierSnapshots;

	// Один проход без измерения, чтобы заранее выделилась память массивов.
	QuerySquadsInBounds(
		WorldMin,
		WorldMax,
		SquadCandidates);

	SquadIds.Reserve(SquadCandidates.Num());

	for (const FAlgonaSquadSpatialSnapshot& Squad
		: SquadCandidates)
	{
		SquadIds.Add(Squad.SquadId);
	}

	ExportSoldierSnapshotsForSquads(
		SquadIds,
		SquadSnapshots,
		SoldierEntities.Num());

	SoldierSpatialGrid.QuerySoldierIds(
		WorldMin,
		WorldMax,
		SoldierIds);

	SoldierIds.Sort();

	ExportSoldierSnapshotsForSoldierIds(
		SoldierIds,
		SoldierSnapshots,
		SoldierEntities.Num());

	double SquadQuerySeconds = 0.0;
	double SquadPrepareSeconds = 0.0;
	double SquadExportSeconds = 0.0;

	double SoldierQuerySeconds = 0.0;
	double SoldierSortSeconds = 0.0;
	double SoldierExportSeconds = 0.0;

	auto RunSquadPath =
		[
			this,
			&WorldMin,
			&WorldMax,
			&SquadCandidates,
			&SquadIds,
			&SquadSnapshots,
			&SquadQuerySeconds,
			&SquadPrepareSeconds,
			&SquadExportSeconds
		]()
		{
			double StartSeconds =
				FPlatformTime::Seconds();

			QuerySquadsInBounds(
				WorldMin,
				WorldMax,
				SquadCandidates);

			SquadQuerySeconds +=
				FPlatformTime::Seconds() - StartSeconds;

			StartSeconds =
				FPlatformTime::Seconds();

			SquadIds.Reset();
			SquadIds.Reserve(
				SquadCandidates.Num());

			for (const FAlgonaSquadSpatialSnapshot& Squad
				: SquadCandidates)
			{
				SquadIds.Add(Squad.SquadId);
			}

			SquadPrepareSeconds +=
				FPlatformTime::Seconds() - StartSeconds;

			StartSeconds =
				FPlatformTime::Seconds();

			ExportSoldierSnapshotsForSquads(
				SquadIds,
				SquadSnapshots,
				SoldierEntities.Num());

			SquadExportSeconds +=
				FPlatformTime::Seconds() - StartSeconds;
		};

	auto RunSoldierPath =
		[
			this,
			&WorldMin,
			&WorldMax,
			&SoldierIds,
			&SoldierSnapshots,
			&SoldierQuerySeconds,
			&SoldierSortSeconds,
			&SoldierExportSeconds
		]()
		{
			double StartSeconds =
				FPlatformTime::Seconds();

			SoldierSpatialGrid.QuerySoldierIds(
				WorldMin,
				WorldMax,
				SoldierIds);

			SoldierQuerySeconds +=
				FPlatformTime::Seconds() - StartSeconds;

			StartSeconds =
				FPlatformTime::Seconds();

			SoldierIds.Sort();

			SoldierSortSeconds +=
				FPlatformTime::Seconds() - StartSeconds;

			StartSeconds =
				FPlatformTime::Seconds();

			ExportSoldierSnapshotsForSoldierIds(
				SoldierIds,
				SoldierSnapshots,
				SoldierEntities.Num());

			SoldierExportSeconds +=
				FPlatformTime::Seconds() - StartSeconds;
		};

	// Чередуем порядок, чтобы один путь не получал постоянного преимущества.
	for (int32 Iteration = 0;
		Iteration < SafeIterations;
		++Iteration)
	{
		if ((Iteration & 1) == 0)
		{
			RunSquadPath();
			RunSoldierPath();
		}
		else
		{
			RunSoldierPath();
			RunSquadPath();
		}
	}

	const double MicrosecondsPerIteration =
		1000000.0 / static_cast<double>(SafeIterations);

	const double SquadQueryUs =
		SquadQuerySeconds * MicrosecondsPerIteration;

	const double SquadPrepareUs =
		SquadPrepareSeconds * MicrosecondsPerIteration;

	const double SquadExportUs =
		SquadExportSeconds * MicrosecondsPerIteration;

	const double SoldierQueryUs =
		SoldierQuerySeconds * MicrosecondsPerIteration;

	const double SoldierSortUs =
		SoldierSortSeconds * MicrosecondsPerIteration;

	const double SoldierExportUs =
		SoldierExportSeconds * MicrosecondsPerIteration;

	const double SquadTotalUs =
		SquadQueryUs
		+ SquadPrepareUs
		+ SquadExportUs;

	const double SoldierTotalUs =
		SoldierQueryUs
		+ SoldierSortUs
		+ SoldierExportUs;

	UE_LOG(
		LogAlgonaSpatialQueryBenchmark,
		Display,
		TEXT(
			"Spatial snapshot breakdown | Iterations=%d | "
			"Squads: Candidates=%d Snapshots=%d "
			"Query=%.3f us Prepare=%.3f us Export=%.3f us Total=%.3f us | "
			"Soldiers: Candidates=%d Snapshots=%d "
			"Query=%.3f us Sort=%.3f us Export=%.3f us Total=%.3f us"),
		SafeIterations,
		SquadCandidates.Num(),
		SquadSnapshots.Num(),
		SquadQueryUs,
		SquadPrepareUs,
		SquadExportUs,
		SquadTotalUs,
		SoldierIds.Num(),
		SoldierSnapshots.Num(),
		SoldierQueryUs,
		SoldierSortUs,
		SoldierExportUs,
		SoldierTotalUs);
}

#endif