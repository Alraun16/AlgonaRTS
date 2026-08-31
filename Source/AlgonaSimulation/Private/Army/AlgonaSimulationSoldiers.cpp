#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaSoldierFragments.h"
#include "Army/AlgonaSoldierTrait.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Mass/EntityFragments.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassExecutionContext.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTemplate.h"
#include "MassEntityView.h"
#include "MassSpawnerSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_LOG_CATEGORY_STATIC(
	LogAlgonaSoldierSnapshotBenchmark,
	Log,
	All);

bool UAlgonaSimulationSubsystem::CreateSoldiers(
	int32 SoldierCount,
	int32 RequestedSquadSize)
{
	UWorld* World = GetWorld();
	if (!World
		|| !MassSpawnerSubsystem
		|| SoldierCount <= 0
		|| RequestedSquadSize <= 0)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::SoldierTemplateBuildFailed;
		return false;
	}

	SoldierEntityConfig = NewObject<UMassEntityConfigAsset>(
		this,
		TEXT("AlgonaSoldierRuntimeConfig"));

	UAlgonaSoldierTrait* SoldierTrait =
		SoldierEntityConfig
			? NewObject<UAlgonaSoldierTrait>(SoldierEntityConfig)
			: nullptr;

	if (!SoldierEntityConfig || !SoldierTrait)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::SoldierTemplateBuildFailed;
		return false;
	}

	FMassEntityConfig& SoldierConfig =
		SoldierEntityConfig->GetMutableConfig();
	SoldierConfig.AddTrait(*SoldierTrait);

	// Simulation creates only authoritative entity state. Presentation chooses
	// its renderer independently and never adds renderer fragments here.
	const FMassEntityTemplate& SoldierTemplate =
		SoldierEntityConfig->GetOrCreateEntityTemplate(*World);

	SoldierEntities.Reset();
	SoldierEntities.Reserve(SoldierCount);

	// Keep the creation context alive until initial fragment data is filled.
	const TSharedPtr<FMassEntityManager::FEntityCreationContext>
		CreationContext = MassSpawnerSubsystem->SpawnEntities(
			SoldierTemplate,
			static_cast<uint32>(SoldierCount),
			SoldierEntities);

	if (!CreationContext.IsValid()
		|| SoldierEntities.Num() != SoldierCount)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::SoldierSpawnFailed;
		return false;
	}

	if (!CreateSquads(RequestedSquadSize))
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::SquadInitializationFailed;
		return false;
	}

	return true;
}

bool UAlgonaSimulationSubsystem::CreateSquads(int32 RequestedSquadSize)
{
	if (!MassEntitySubsystem
		|| SoldierEntities.IsEmpty()
		|| RequestedSquadSize <= 0)
	{
		return false;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	constexpr int32 SquadsPerRow = 60;
	constexpr float SpaceBetweenSquads = 600.0f;
	constexpr float SoldierSpacing = 100.0f;

	const int32 FormationWidth = FMath::Min(
		10,
		RequestedSquadSize);
	const int32 FormationDepth = FMath::DivideAndRoundUp(
		RequestedSquadSize,
		FormationWidth);

	const float FormationWorldDepth =
		static_cast<float>(FormationDepth - 1) * SoldierSpacing;
	const float FormationWorldWidth =
		static_cast<float>(FormationWidth - 1) * SoldierSpacing;

	const float SquadSpacingX =
		FormationWorldDepth + SpaceBetweenSquads;
	const float SquadSpacingY =
		FormationWorldWidth + SpaceBetweenSquads;

	const int32 SquadCount = FMath::DivideAndRoundUp(
		SoldierEntities.Num(),
		RequestedSquadSize);

	Squads.Reset();
	Squads.Reserve(SquadCount);
	SquadEntityRanges.Reset();
	SquadEntityRanges.Reserve(SquadCount);
	SquadSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	SoldierSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);

	int32 SoldierIndex = 0;

	for (int32 SquadIndex = 0;
		SquadIndex < SquadCount;
		++SquadIndex)
	{
		FAlgonaSquad Squad;
		Squad.SquadId = SquadIndex;
		Squad.FormationWidth = FormationWidth;
		Squad.FormationDepth = FormationDepth;
		Squad.SoldierSpacing = SoldierSpacing;
		Squad.MemberCount = FMath::Min(
			RequestedSquadSize,
			SoldierEntities.Num() - SoldierIndex);

		const int32 FirstSoldierIndex = SoldierIndex;
		const int32 SquadX = SquadIndex % SquadsPerRow;
		const int32 SquadY = SquadIndex / SquadsPerRow;

		Squad.AnchorLocation = FVector(
			static_cast<double>(SquadX) * SquadSpacingX
				+ FormationWorldDepth,
			static_cast<double>(SquadY) * SquadSpacingY
				+ FormationWorldWidth * 0.5,
			0.0);
		Squad.TargetAnchorLocation = Squad.AnchorLocation;

		for (int32 SlotIndex = 0;
			SlotIndex < Squad.MemberCount;
			++SlotIndex)
		{
			if (!SoldierEntities.IsValidIndex(SoldierIndex))
			{
				return false;
			}

			const FMassEntityHandle SoldierEntity =
				SoldierEntities[SoldierIndex];

			if (!EntityManager.IsEntityValid(SoldierEntity))
			{
				return false;
			}

			FMassEntityView EntityView(EntityManager, SoldierEntity);

			FAlgonaSoldierIdFragment& Id =
				EntityView.GetFragmentData<FAlgonaSoldierIdFragment>();
			Id.Value = static_cast<uint32>(SoldierIndex + 1);

			FAlgonaSquadMemberFragment& Member =
				EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();
			Member.SquadId = Squad.SquadId;
			Member.SlotIndex = SlotIndex;

			FAlgonaSoldierMovementFragment& Movement =
				EntityView.GetFragmentData<FAlgonaSoldierMovementFragment>();
			Movement.Velocity = FVector::ZeroVector;
			Movement.LastProcessedSimulationTick = 0;
			Movement.State = EAlgonaSoldierMovementState::Idle;

			FTransform InitialTransform = FTransform::Identity;
			InitialTransform.SetLocation(
				ComputeSlotWorldPosition(Squad, SlotIndex));
			InitialTransform.SetRotation(
				Squad.FacingDirection.Rotation().Quaternion());

			FTransformFragment& Transform =
				EntityView.GetFragmentData<FTransformFragment>();
			Transform.SetTransform(InitialTransform);
			
			if (IsSoldierSpatialGridEnabled())
			{
				SoldierSpatialGrid.AddSoldier(
					Id.Value,
					InitialTransform.GetLocation());
			}
			
			++SoldierIndex;
		}

		Squads.Add(MoveTemp(Squad));

		FAlgonaSquadEntityRange& EntityRange =
			SquadEntityRanges.AddDefaulted_GetRef();
		EntityRange.FirstSoldierIndex = FirstSoldierIndex;
		EntityRange.Count = Squads.Last().MemberCount;

		if (IsSquadSpatialGridEnabled())
		{
			SquadSpatialGrid.AddSquad(
				Squads.Last().SquadId,
				Squads.Last().GetSpatialCenter());
		}
	}

	return SoldierIndex == SoldierEntities.Num()
		&& SquadEntityRanges.Num() == Squads.Num();
}

void UAlgonaSimulationSubsystem::DestroySoldiers()
{
	const bool bHadSoldiers = !SoldierEntities.IsEmpty();

	if (MassSpawnerSubsystem && bHadSoldiers)
	{
		MassSpawnerSubsystem->DestroyEntities(SoldierEntities);
	}

	SoldierEntities.Reset();
	Squads.Reset();
	SquadEntityRanges.Reset();
	SquadSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	SoldierSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	PendingMoveCommands.Reset();
	SoldierEntityConfig = nullptr;

	Metrics.EntityCount = 0;
	Metrics.SquadCount = 0;
}

int32 UAlgonaSimulationSubsystem::ExportSoldierSnapshotsForSquads(
	TConstArrayView<int32> SquadIds,
	TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSelectedSquadSnapshots);

	OutSnapshots.Reset();
	if (!MassEntitySubsystem
		|| !SoldierSnapshotQuery
		|| SquadIds.IsEmpty()
		|| MaxEntities <= 0)
	{
		return 0;
	}

	// Count complete valid squads first. This gives us the exact selected
	// soldier count before choosing the cheaper export strategy.
	int32 SelectedSoldierCount = 0;
	int32 AcceptedSquadCount = 0;
	for (const int32 SquadId : SquadIds)
	{
		if (!Squads.IsValidIndex(SquadId)
			|| Squads[SquadId].SquadId != SquadId
			|| !SquadEntityRanges.IsValidIndex(SquadId))
		{
			continue;
		}

		const FAlgonaSquadEntityRange& EntityRange =
			SquadEntityRanges[SquadId];
		if (EntityRange.Count <= 0
			|| SelectedSoldierCount + EntityRange.Count > MaxEntities)
		{
			break;
		}

		SelectedSoldierCount += EntityRange.Count;
		++AcceptedSquadCount;
	}

	if (SelectedSoldierCount <= 0 || AcceptedSquadCount <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SelectedSoldierCount);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	// Measured P1 crossover: direct selected-entity reads are cheaper for a
	// small working set, while one contiguous Mass chunk pass wins once a
	// meaningful fraction of the army is selected. 20% is intentionally
	// conservative and should be retuned only from measurements.
	constexpr int32 FullScanRatioNumerator = 3;
	constexpr int32 FullScanRatioDenominator = 50;
	const bool bUseFilteredFullScan =
		static_cast<int64>(SelectedSoldierCount) * FullScanRatioDenominator
		>= static_cast<int64>(SoldierEntities.Num()) * FullScanRatioNumerator;

	if (!bUseFilteredFullScan)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSelectedSquads_Direct);

		// Small working set: restore the original selective path. It avoids
		// touching unrelated soldiers and benchmarked faster than constructing
		// a temporary Mass collection for selected handles.
		int32 ProcessedSquadCount = 0;
		for (const int32 SquadId : SquadIds)
		{
			if (ProcessedSquadCount >= AcceptedSquadCount)
			{
				break;
			}

			if (!Squads.IsValidIndex(SquadId)
				|| Squads[SquadId].SquadId != SquadId
				|| !SquadEntityRanges.IsValidIndex(SquadId))
			{
				continue;
			}

			const FAlgonaSquadEntityRange& EntityRange =
				SquadEntityRanges[SquadId];
			if (EntityRange.Count <= 0)
			{
				continue;
			}

			++ProcessedSquadCount;

			for (int32 MemberOffset = 0;
				MemberOffset < EntityRange.Count;
				++MemberOffset)
			{
				const int32 SoldierIndex =
					EntityRange.FirstSoldierIndex + MemberOffset;
				if (!SoldierEntities.IsValidIndex(SoldierIndex))
				{
					continue;
				}

				const FMassEntityHandle Entity = SoldierEntities[SoldierIndex];
				if (!EntityManager.IsEntityValid(Entity))
				{
					continue;
				}

				FMassEntityView EntityView(EntityManager, Entity);
				const FAlgonaSoldierIdFragment& Id =
					EntityView.GetFragmentData<FAlgonaSoldierIdFragment>();
				const FTransformFragment& TransformFragment =
					EntityView.GetFragmentData<FTransformFragment>();
				const FTransform& Transform = TransformFragment.GetTransform();

				FAlgonaSoldierSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();
				Snapshot.EntityId = Id.Value;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		}

		return OutSnapshots.Num();
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSelectedSquads_FilteredFullScan);

	// Large working set: scan the soldier archetype once in chunk order, but
	// emit snapshots only for squads selected by Presentation. The caller still
	// supplies only renderer-neutral SquadIds; Simulation never sees a camera.
	TArray<uint8> SelectedSquadMask;
	SelectedSquadMask.Init(0, Squads.Num());

	int32 MaskedSquadCount = 0;
	for (const int32 SquadId : SquadIds)
	{
		if (MaskedSquadCount >= AcceptedSquadCount)
		{
			break;
		}

		if (!Squads.IsValidIndex(SquadId)
			|| Squads[SquadId].SquadId != SquadId
			|| !SquadEntityRanges.IsValidIndex(SquadId)
			|| SquadEntityRanges[SquadId].Count <= 0)
		{
			continue;
		}

		SelectedSquadMask[SquadId] = 1;
		++MaskedSquadCount;
	}

	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	SoldierSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[&OutSnapshots, &SelectedSquadMask](FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaSoldierIdFragment> Ids =
				Context.GetFragmentView<FAlgonaSoldierIdFragment>();
			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();
			const TConstArrayView<FAlgonaSquadMemberFragment> Members =
				Context.GetFragmentView<FAlgonaSquadMemberFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				const int32 SquadId = Members[Index].SquadId;
				if (!SelectedSquadMask.IsValidIndex(SquadId)
					|| SelectedSquadMask[SquadId] == 0)
				{
					continue;
				}

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

int32 UAlgonaSimulationSubsystem::ExportSoldierSnapshotsForSoldierIds(
	TConstArrayView<uint32> SoldierIds,
	TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	const int32 SelectedSoldierCount =
		FMath::Min(
			SoldierIds.Num(),
			FMath::Max(MaxEntities, 0));

	if (SelectedSoldierCount <= 0
		|| SoldierEntities.IsEmpty())
	{
		OutSnapshots.Reset();
		return 0;
	}

	// Temporary value. The P2 crossover benchmark will replace it
	// with the measured soldier-specific threshold.
	constexpr int32 FullScanRatioNumerator = 3;
	constexpr int32 FullScanRatioDenominator = 50;

	const bool bUseFilteredFullScan =
		static_cast<int64>(SelectedSoldierCount)
			* FullScanRatioDenominator
		>= static_cast<int64>(SoldierEntities.Num())
			* FullScanRatioNumerator;

	return ExportSoldierSnapshotsForSoldierIdsInternal(
		SoldierIds,
		OutSnapshots,
		MaxEntities,
		bUseFilteredFullScan);
}

int32 UAlgonaSimulationSubsystem::ExportSoldierSnapshotsForSoldierIdsInternal(
	TConstArrayView<uint32> SoldierIds,
	TArray<FAlgonaSoldierSnapshot>& OutSnapshots,
	int32 MaxEntities,
	bool bUseFilteredFullScan)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSimulation_ExportSelectedSoldierSnapshots);

	OutSnapshots.Reset();

	if (!MassEntitySubsystem
		|| !SoldierSnapshotQuery
		|| SoldierIds.IsEmpty()
		|| MaxEntities <= 0)
	{
		return 0;
	}

	const int32 SelectedSoldierCount =
		FMath::Min(
			SoldierIds.Num(),
			MaxEntities);

	if (SelectedSoldierCount <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SelectedSoldierCount);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	if (!bUseFilteredFullScan)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(
			AlgonaSimulation_ExportSelectedSoldiers_Direct);

		for (int32 Index = 0;
			Index < SelectedSoldierCount;
			++Index)
		{
			const uint32 SoldierId =
				SoldierIds[Index];

			if (SoldierId == 0)
			{
				continue;
			}

			const int32 SoldierIndex =
				static_cast<int32>(SoldierId - 1);

			if (!SoldierEntities.IsValidIndex(SoldierIndex))
			{
				continue;
			}

			const FMassEntityHandle Entity =
				SoldierEntities[SoldierIndex];

			if (!EntityManager.IsEntityValid(Entity))
			{
				continue;
			}

			FMassEntityView EntityView(
				EntityManager,
				Entity);

			const FTransformFragment& TransformFragment =
				EntityView.GetFragmentData<FTransformFragment>();

			const FTransform& Transform =
				TransformFragment.GetTransform();

			FAlgonaSoldierSnapshot& Snapshot =
				OutSnapshots.AddDefaulted_GetRef();

			Snapshot.EntityId = SoldierId;
			Snapshot.Position = Transform.GetLocation();
			Snapshot.Facing = Transform.GetRotation();
		}

		return OutSnapshots.Num();
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSimulation_ExportSelectedSoldiers_FilteredFullScan);

	TArray<uint8> SelectedSoldierMask;
	SelectedSoldierMask.Init(
		0,
		SoldierEntities.Num());

	for (int32 Index = 0;
		Index < SelectedSoldierCount;
		++Index)
	{
		const uint32 SoldierId =
			SoldierIds[Index];

		if (SoldierId == 0)
		{
			continue;
		}

		const int32 SoldierIndex =
			static_cast<int32>(SoldierId - 1);

		if (SelectedSoldierMask.IsValidIndex(SoldierIndex))
		{
			SelectedSoldierMask[SoldierIndex] = 1;
		}
	}

	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	SoldierSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[
			&OutSnapshots,
			&SelectedSoldierMask
		](FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaSoldierIdFragment> Ids =
				Context.GetFragmentView<FAlgonaSoldierIdFragment>();

			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				const uint32 SoldierId =
					Ids[Index].Value;

				if (SoldierId == 0)
				{
					continue;
				}

				const int32 SoldierIndex =
					static_cast<int32>(SoldierId - 1);

				if (!SelectedSoldierMask.IsValidIndex(SoldierIndex)
					|| SelectedSoldierMask[SoldierIndex] == 0)
				{
					continue;
				}

				const FTransform& Transform =
					Transforms[Index].GetTransform();

				FAlgonaSoldierSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();

				Snapshot.EntityId = SoldierId;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		});

	return OutSnapshots.Num();
}

#if !UE_BUILD_SHIPPING

void UAlgonaSimulationSubsystem::BenchmarkSoldierSnapshotCrossover(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	int32 Iterations)
{
	if (!IsSoldierSpatialGridEnabled()
		|| SoldierEntities.IsEmpty())
	{
		UE_LOG(
			LogAlgonaSoldierSnapshotBenchmark,
			Warning,
			TEXT(
				"Soldier snapshot crossover benchmark requires "
				"the soldier spatial grid."));

		return;
	}

	TArray<uint32> CandidateSoldierIds;

	SoldierSpatialGrid.QuerySoldierIds(
		WorldMin,
		WorldMax,
		CandidateSoldierIds);

	if (CandidateSoldierIds.IsEmpty())
	{
		UE_LOG(
			LogAlgonaSoldierSnapshotBenchmark,
			Warning,
			TEXT("Soldier snapshot crossover benchmark found no soldiers."));

		return;
	}

	const int32 SafeIterations =
		FMath::Clamp(
			Iterations,
			10,
			500);

	TArray<FAlgonaSoldierSnapshot> DirectSnapshots;
	TArray<FAlgonaSoldierSnapshot> FullScanSnapshots;

	auto MeasureAtCount =
		[
			this,
			&CandidateSoldierIds,
			&DirectSnapshots,
			&FullScanSnapshots,
			SafeIterations
		](
			int32 TestCount,
			double& OutDirectMicroseconds,
			double& OutFullScanMicroseconds)
		{
			TestCount =
				FMath::Clamp(
					TestCount,
					1,
					CandidateSoldierIds.Num());

			const TConstArrayView<uint32> TestSoldierIds(
				CandidateSoldierIds.GetData(),
				TestCount);

			// Warm both paths so retained output-array capacity is not
			// counted as an advantage for either strategy.
			ExportSoldierSnapshotsForSoldierIdsInternal(
				TestSoldierIds,
				DirectSnapshots,
				TestCount,
				false);

			ExportSoldierSnapshotsForSoldierIdsInternal(
				TestSoldierIds,
				FullScanSnapshots,
				TestCount,
				true);

			double DirectSeconds = 0.0;
			double FullScanSeconds = 0.0;

			auto RunDirect =
				[
					this,
					&TestSoldierIds,
					&DirectSnapshots,
					TestCount,
					&DirectSeconds
				]()
				{
					const double StartSeconds =
						FPlatformTime::Seconds();

					ExportSoldierSnapshotsForSoldierIdsInternal(
						TestSoldierIds,
						DirectSnapshots,
						TestCount,
						false);

					DirectSeconds +=
						FPlatformTime::Seconds() - StartSeconds;
				};

			auto RunFullScan =
				[
					this,
					&TestSoldierIds,
					&FullScanSnapshots,
					TestCount,
					&FullScanSeconds
				]()
				{
					const double StartSeconds =
						FPlatformTime::Seconds();

					ExportSoldierSnapshotsForSoldierIdsInternal(
						TestSoldierIds,
						FullScanSnapshots,
						TestCount,
						true);

					FullScanSeconds +=
						FPlatformTime::Seconds() - StartSeconds;
				};

			for (int32 Iteration = 0;
				Iteration < SafeIterations;
				++Iteration)
			{
				if ((Iteration & 1) == 0)
				{
					RunDirect();
					RunFullScan();
				}
				else
				{
					RunFullScan();
					RunDirect();
				}
			}

			const double MicrosecondsPerIteration =
				1000000.0
				/ static_cast<double>(SafeIterations);

			OutDirectMicroseconds =
				DirectSeconds * MicrosecondsPerIteration;

			OutFullScanMicroseconds =
				FullScanSeconds * MicrosecondsPerIteration;
		};

	auto LogProbe =
		[
			this
		](
			int32 Count,
			double DirectMicroseconds,
			double FullScanMicroseconds)
		{
			const double Percent =
				SoldierEntities.IsEmpty()
					? 0.0
					: static_cast<double>(Count)
						* 100.0
						/ static_cast<double>(SoldierEntities.Num());

			UE_LOG(
				LogAlgonaSoldierSnapshotBenchmark,
				Display,
				TEXT(
					"Crossover probe | Count=%d | %.3f%% | "
					"Direct=%.3f us | FullScan=%.3f us | Winner=%s"),
				Count,
				Percent,
				DirectMicroseconds,
				FullScanMicroseconds,
				FullScanMicroseconds <= DirectMicroseconds
					? TEXT("FullScan")
					: TEXT("Direct"));
		};

	int32 LowCount = 1;
	int32 HighCount = CandidateSoldierIds.Num();

	double LowDirectMicroseconds = 0.0;
	double LowFullScanMicroseconds = 0.0;
	double HighDirectMicroseconds = 0.0;
	double HighFullScanMicroseconds = 0.0;

	MeasureAtCount(
		LowCount,
		LowDirectMicroseconds,
		LowFullScanMicroseconds);

	LogProbe(
		LowCount,
		LowDirectMicroseconds,
		LowFullScanMicroseconds);

	if (HighCount != LowCount)
	{
		MeasureAtCount(
			HighCount,
			HighDirectMicroseconds,
			HighFullScanMicroseconds);

		LogProbe(
			HighCount,
			HighDirectMicroseconds,
			HighFullScanMicroseconds);
	}
	else
	{
		HighDirectMicroseconds = LowDirectMicroseconds;
		HighFullScanMicroseconds = LowFullScanMicroseconds;
	}

	if (LowFullScanMicroseconds <= LowDirectMicroseconds)
	{
		UE_LOG(
			LogAlgonaSoldierSnapshotBenchmark,
			Display,
			TEXT(
				"Crossover result | Soldiers=%d | "
				"FullScan already wins at Count=1."),
			SoldierEntities.Num());

		return;
	}

	if (HighDirectMicroseconds < HighFullScanMicroseconds)
	{
		UE_LOG(
			LogAlgonaSoldierSnapshotBenchmark,
			Display,
			TEXT(
				"Crossover result | Soldiers=%d | Candidates=%d | "
				"No crossover found: Direct still wins at the "
				"largest tested selection."),
			SoldierEntities.Num(),
			CandidateSoldierIds.Num());

		return;
	}

	const int32 StopWidth =
		FMath::Max(
			1,
			SoldierEntities.Num() / 1000);

	while (HighCount - LowCount > StopWidth)
	{
		const int32 MidCount =
			LowCount + (HighCount - LowCount) / 2;

		double MidDirectMicroseconds = 0.0;
		double MidFullScanMicroseconds = 0.0;

		MeasureAtCount(
			MidCount,
			MidDirectMicroseconds,
			MidFullScanMicroseconds);

		LogProbe(
			MidCount,
			MidDirectMicroseconds,
			MidFullScanMicroseconds);

		if (MidFullScanMicroseconds <= MidDirectMicroseconds)
		{
			HighCount = MidCount;
			HighDirectMicroseconds = MidDirectMicroseconds;
			HighFullScanMicroseconds = MidFullScanMicroseconds;
		}
		else
		{
			LowCount = MidCount;
			LowDirectMicroseconds = MidDirectMicroseconds;
			LowFullScanMicroseconds = MidFullScanMicroseconds;
		}
	}

	const double LowPercent =
		static_cast<double>(LowCount)
		* 100.0
		/ static_cast<double>(SoldierEntities.Num());

	const double HighPercent =
		static_cast<double>(HighCount)
		* 100.0
		/ static_cast<double>(SoldierEntities.Num());

	UE_LOG(
		LogAlgonaSoldierSnapshotBenchmark,
		Display,
		TEXT(
			"Crossover result | Soldiers=%d | Candidates=%d | "
			"Direct wins through %d (%.3f%%): "
			"Direct=%.3f us FullScan=%.3f us | "
			"FullScan wins from %d (%.3f%%): "
			"Direct=%.3f us FullScan=%.3f us | "
			"SuggestedThreshold=%d (%.3f%%)"),
		SoldierEntities.Num(),
		CandidateSoldierIds.Num(),
		LowCount,
		LowPercent,
		LowDirectMicroseconds,
		LowFullScanMicroseconds,
		HighCount,
		HighPercent,
		HighDirectMicroseconds,
		HighFullScanMicroseconds,
		HighCount,
		HighPercent);
}

#endif

FVector UAlgonaSimulationSubsystem::ComputeSlotWorldPosition(
	const FAlgonaSquad& Squad,
	int32 SlotIndex) const
{
	if (SlotIndex < 0 || Squad.FormationWidth <= 0)
	{
		return Squad.AnchorLocation;
	}

	FVector Forward = Squad.FacingDirection.GetSafeNormal2D();
	if (Forward.IsNearlyZero())
	{
		Forward = FVector::ForwardVector;
	}

	const FVector Right = FVector::CrossProduct(
		FVector::UpVector,
		Forward).GetSafeNormal();

	const int32 Row = SlotIndex / Squad.FormationWidth;
	const int32 Column = SlotIndex % Squad.FormationWidth;

	const double ForwardOffset =
		-static_cast<double>(Row) * Squad.SoldierSpacing;
	const double RightOffset =
		(static_cast<double>(Column)
			- static_cast<double>(Squad.FormationWidth - 1) * 0.5)
		* Squad.SoldierSpacing;

	return Squad.AnchorLocation
		+ Forward * ForwardOffset
		+ Right * RightOffset;
}
