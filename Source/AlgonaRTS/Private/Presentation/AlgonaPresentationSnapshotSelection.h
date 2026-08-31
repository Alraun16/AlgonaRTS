#pragma once

#include "CoreMinimal.h"

struct FAlgonaUnitSnapshot;
class UAlgonaSimulationSubsystem;
class UCameraComponent;
class UWorld;

/** Runtime A/B switch: false = full unit export, true = unit-grid export. */
bool IsAlgonaP1SpatialSnapshotsEnabled();

#if !UE_BUILD_SHIPPING
/** Last and smoothed timings for the local snapshot-selection pipeline. */
struct FAlgonaPresentationSnapshotMetrics
{
	bool bSpatialSnapshotsRequested = false;
	bool bSpatialPathUsed = false;

	double LastTotalMilliseconds = 0.0;
	double AverageTotalMilliseconds = 0.0;
	double LastExportMilliseconds = 0.0;
	double AverageExportMilliseconds = 0.0;
	double LastGridQueryMilliseconds = 0.0;
	double AverageGridQueryMilliseconds = 0.0;

	int32 CandidateUnitCount = 0;
	int32 ExportedUnitCount = 0;
	int32 TotalUnitCount = 0;
	uint64 SampleCount = 0;
};

bool GetAlgonaPresentationSnapshotMetrics(
	const UWorld* World,
	FAlgonaPresentationSnapshotMetrics& OutMetrics);

void ClearAlgonaPresentationSnapshotMetrics(const UWorld* World);
#endif

/**
 * Builds the renderer-neutral unit snapshot set for the current local view.
 * Presentation owns camera visibility; Simulation receives only world bounds
 * and UnitIds, never camera or renderer state.
 */
void CaptureAlgonaPresentationSnapshots(
	UAlgonaSimulationSubsystem& Simulation,
	UWorld* World,
	const UCameraComponent* Camera,
	int32 MaxEntities,
	TArray<FAlgonaUnitSnapshot>& OutSnapshots);
