#pragma once

#include "CoreMinimal.h"

struct FAlgonaSoldierSnapshot;
class UAlgonaSimulationSubsystem;
class UCameraComponent;
class UWorld;

/** Runtime A/B switch: false = full soldier export, true = squad-grid export. */
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
	double LastSquadTestMilliseconds = 0.0;
	double AverageSquadTestMilliseconds = 0.0;

	int32 CandidateSquadCount = 0;
	int32 VisibleSquadCount = 0;
	int32 TotalSquadCount = 0;
	int32 ExportedSoldierCount = 0;
	int32 TotalSoldierCount = 0;
	uint64 SampleCount = 0;
};

bool GetAlgonaPresentationSnapshotMetrics(
	const UWorld* World,
	FAlgonaPresentationSnapshotMetrics& OutMetrics);

void ClearAlgonaPresentationSnapshotMetrics(const UWorld* World);
#endif

/**
 * Builds the renderer-neutral soldier snapshot set for the current local view.
 * Presentation owns camera visibility; Simulation receives only selected
 * SquadIds and exports those squads in full when spatial snapshots are enabled.
 */
void CaptureAlgonaPresentationSnapshots(
	UAlgonaSimulationSubsystem& Simulation,
	UWorld* World,
	const UCameraComponent* Camera,
	int32 MaxEntities,
	TArray<FAlgonaSoldierSnapshot>& OutSnapshots);
