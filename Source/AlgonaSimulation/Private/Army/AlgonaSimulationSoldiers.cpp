#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaSoldierFragments.h"
#include "Army/AlgonaSoldierTrait.h"

#include "Engine/World.h"
#include "Mass/EntityFragments.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassExecutionContext.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTemplate.h"
#include "MassEntityView.h"
#include "MassSpawnerSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

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

			++SoldierIndex;
		}

		Squads.Add(MoveTemp(Squad));

		FAlgonaSquadEntityRange& EntityRange =
			SquadEntityRanges.AddDefaulted_GetRef();
		EntityRange.FirstSoldierIndex = FirstSoldierIndex;
		EntityRange.Count = Squads.Last().MemberCount;

		SquadSpatialGrid.AddSquad(
			Squads.Last().SquadId,
			Squads.Last().GetSpatialCenter());
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
