#include "AlgonaPresentationSnapshotSelection.h"

#include "AlgonaPresentationView.h"
#include "Presentation/AlgonaPresentationSettings.h"
#include "Core/AlgonaSimulationSubsystem.h"

#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	constexpr double CullingGuardPixels = 128.0;

	TAutoConsoleVariable<int32> CVarAlgonaP1SpatialSnapshots(
		TEXT("algona.P1.SpatialSnapshots"),
		1,
		TEXT(
			"Presentation snapshot source: "
			"0 = full unit export + per-unit camera culling, "
			"1 = unit spatial grid + selected-unit export."),
		ECVF_Default);

#if !UE_BUILD_SHIPPING
	constexpr double MetricSmoothingAlpha = 0.10;
	TMap<const UWorld*, FAlgonaPresentationSnapshotMetrics> SnapshotMetricsByWorld;

	void PublishSnapshotMetrics(
		const UWorld* World,
		FAlgonaPresentationSnapshotMetrics Sample)
	{
		if (!World)
		{
			return;
		}

		FAlgonaPresentationSnapshotMetrics& Stored =
			SnapshotMetricsByWorld.FindOrAdd(World);
		const bool bHasPreviousSamples =
			Stored.SampleCount > 0
			&& Stored.bSpatialSnapshotsRequested == Sample.bSpatialSnapshotsRequested
			&& Stored.bSpatialPathUsed == Sample.bSpatialPathUsed;

		auto Smooth = [bHasPreviousSamples](double Previous, double Current)
		{
			return bHasPreviousSamples
				? FMath::Lerp(Previous, Current, MetricSmoothingAlpha)
				: Current;
		};

		Sample.AverageTotalMilliseconds = Smooth(
			Stored.AverageTotalMilliseconds,
			Sample.LastTotalMilliseconds);
		Sample.AverageExportMilliseconds = Smooth(
			Stored.AverageExportMilliseconds,
			Sample.LastExportMilliseconds);
		Sample.AverageGridQueryMilliseconds = Smooth(
			Stored.AverageGridQueryMilliseconds,
			Sample.LastGridQueryMilliseconds);
		Sample.SampleCount = bHasPreviousSamples ? Stored.SampleCount + 1 : 1;

		Stored = Sample;
	}
#endif
}

bool IsAlgonaP1SpatialSnapshotsEnabled()
{
	return CVarAlgonaP1SpatialSnapshots.GetValueOnGameThread() != 0;
}

#if !UE_BUILD_SHIPPING
bool GetAlgonaPresentationSnapshotMetrics(
	const UWorld* World,
	FAlgonaPresentationSnapshotMetrics& OutMetrics)
{
	if (!World)
	{
		return false;
	}

	const FAlgonaPresentationSnapshotMetrics* Metrics =
		SnapshotMetricsByWorld.Find(World);
	if (!Metrics)
	{
		return false;
	}

	OutMetrics = *Metrics;
	return true;
}

void ClearAlgonaPresentationSnapshotMetrics(const UWorld* World)
{
	if (World)
	{
		SnapshotMetricsByWorld.Remove(World);
	}
}
#endif

void CaptureAlgonaPresentationSnapshots(
	UAlgonaSimulationSubsystem& Simulation,
	UWorld* World,
	const UCameraComponent* Camera,
	int32 MaxEntities,
	TArray<FAlgonaUnitSnapshot>& OutSnapshots)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaPresentation_SelectUnitSnapshots);

#if !UE_BUILD_SHIPPING
	const double TotalStartSeconds = FPlatformTime::Seconds();
	FAlgonaPresentationSnapshotMetrics Metrics;
	Metrics.bSpatialSnapshotsRequested = IsAlgonaP1SpatialSnapshotsEnabled();
	Metrics.TotalUnitCount = Simulation.GetUnitCount();

	auto FinalizeMetrics = [&]()
	{
		Metrics.ExportedUnitCount = OutSnapshots.Num();
		Metrics.LastTotalMilliseconds =
			(FPlatformTime::Seconds() - TotalStartSeconds) * 1000.0;
		PublishSnapshotMetrics(World, Metrics);
	};
#endif

	// Fail open when spatial selection is disabled or the current view cannot
	// provide a ground footprint. Presentation still receives authoritative data.
	if (!IsAlgonaP1SpatialSnapshotsEnabled()
		|| !IsAlgonaP1PresentationCameraCullingEnabled()
		|| !World
		|| !Camera)
	{
#if !UE_BUILD_SHIPPING
		const double ExportStartSeconds = FPlatformTime::Seconds();
#endif
		Simulation.ExportUnitSnapshots(OutSnapshots, MaxEntities);
#if !UE_BUILD_SHIPPING
		Metrics.LastExportMilliseconds =
			(FPlatformTime::Seconds() - ExportStartSeconds) * 1000.0;
		FinalizeMetrics();
#endif
		return;
	}

	FAlgonaPresentationView View;
	FVector2D QueryMin = FVector2D::ZeroVector;
	FVector2D QueryMax = FVector2D::ZeroVector;

	if (!View.Build(*World, *Camera)
		|| !View.GetGroundBounds(CullingGuardPixels, QueryMin, QueryMax))
	{
#if !UE_BUILD_SHIPPING
		const double ExportStartSeconds = FPlatformTime::Seconds();
#endif
		Simulation.ExportUnitSnapshots(OutSnapshots, MaxEntities);
#if !UE_BUILD_SHIPPING
		Metrics.LastExportMilliseconds =
			(FPlatformTime::Seconds() - ExportStartSeconds) * 1000.0;
		FinalizeMetrics();
#endif
		return;
	}

#if !UE_BUILD_SHIPPING
	Metrics.bSpatialPathUsed = true;
	const double GridQueryStartSeconds = FPlatformTime::Seconds();
#endif

	TArray<uint32> CandidateUnitIds;
	Simulation.QueryUnitIdsInBounds(
		QueryMin,
		QueryMax,
		CandidateUnitIds);

#if !UE_BUILD_SHIPPING
	Metrics.LastGridQueryMilliseconds =
		(FPlatformTime::Seconds() - GridQueryStartSeconds) * 1000.0;
	Metrics.CandidateUnitCount = CandidateUnitIds.Num();
	const double ExportStartSeconds = FPlatformTime::Seconds();
#endif

	Simulation.ExportUnitSnapshotsForUnitIds(
		CandidateUnitIds,
		OutSnapshots,
		MaxEntities);

#if !UE_BUILD_SHIPPING
	Metrics.LastExportMilliseconds =
		(FPlatformTime::Seconds() - ExportStartSeconds) * 1000.0;
	FinalizeMetrics();
#endif
}
