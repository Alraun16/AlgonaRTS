#include "Core/AlgonaSimulationSubsystem.h"

// Soldier template, authoritative fragments and formation geometry.
#include "Army/AlgonaFormation.h"
#include "Army/AlgonaSoldierFragments.h"
#include "Army/AlgonaSoldierTrait.h"

// Mass spawning, direct rare entity initialization and P1 snapshot export.
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

namespace
{
	uint32 HashVariation(uint32 Value)
	{
		Value ^= Value >> 16;
		Value *= 0x7FEB352Du;
		Value ^= Value >> 15;
		Value *= 0x846CA68Bu;
		Value ^= Value >> 16;
		return Value;
	}

	float HashToUnitFloat(uint32 Seed, uint32 Salt)
	{
		const uint32 Hash = HashVariation(Seed ^ Salt);
		return static_cast<float>(Hash & 0xFFFFu) / 65535.0f;
	}
}

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

	// Keep the proven 60-squads-per-row layout; P2 intentionally opens soldier spacing to 150 cm.
	constexpr int32 SquadsPerRow = 60;
	constexpr float SpaceBetweenSquadsCm = 600.0f;
	constexpr float SoldierSpacingCm = 150.0f;

	const FAlgonaFormationLayout ReferenceFormation =
		FAlgonaFormationGenerator::BuildRectangle(
			RequestedSquadSize,
			SoldierSpacingCm);
	const float FormationWorldDepthCm =
		ReferenceFormation.HalfExtentsCm.Y * 2.0f;
	const float FormationWorldWidthCm =
		ReferenceFormation.HalfExtentsCm.X * 2.0f;
	const float SquadSpacingX =
		FormationWorldDepthCm + SpaceBetweenSquadsCm;
	const float SquadSpacingY =
		FormationWorldWidthCm + SpaceBetweenSquadsCm;

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
		Squad.SoldierSpacingCm = SoldierSpacingCm;
		Squad.TotalMemberCount = FMath::Min(
			RequestedSquadSize,
			SoldierEntities.Num() - SoldierIndex);
		Squad.ActiveMemberSoldierIndices.Reserve(Squad.TotalMemberCount);
		Squad.Formation = FAlgonaFormationGenerator::BuildRectangle(
			Squad.TotalMemberCount,
			Squad.SoldierSpacingCm,
			Squad.RequestedMaxSlotsPerRow);

		const int32 FirstSoldierIndex = SoldierIndex;
		const int32 SquadX = SquadIndex % SquadsPerRow;
		const int32 SquadY = SquadIndex / SquadsPerRow;

		// Geometric-center anchor preserves the same initial occupied footprint as
		// P1, but removes the old front-center special case from all later logic.
		Squad.AnchorLocation = FVector(
			static_cast<double>(SquadX) * SquadSpacingX
				+ FormationWorldDepthCm * 0.5,
			static_cast<double>(SquadY) * SquadSpacingY
				+ FormationWorldWidthCm * 0.5,
			0.0);
		Squad.FinalTargetLocation = Squad.AnchorLocation;

		for (int32 SlotIndex = 0;
			SlotIndex < Squad.TotalMemberCount;
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
			Member.FormationState = EAlgonaSoldierFormationState::Active;

			FAlgonaSoldierMovementFragment& Movement =
				EntityView.GetFragmentData<FAlgonaSoldierMovementFragment>();
			Movement.Velocity = FVector::ZeroVector;
			Movement.LastProcessedSimulationTick = 0;
			Movement.State = EAlgonaSoldierMovementState::Idle;
			Movement.DistanceState = EAlgonaSoldierDistanceState::Near;
			Movement.PassageState = EAlgonaSoldierPassageState::Formation;

			// Stable speed/turn differences keep 20k soldiers from behaving like one
			// copied transform. Per-command drift data is generated lazily in the
			// existing Mass pass, so no extra army scan is introduced.
			Movement.MotionVariationSeed = HashVariation(Id.Value);
			Movement.PersonalSpeedScale = FMath::Lerp(
				0.90f,
				1.0f,
				HashToUnitFloat(Id.Value, 0xB5297A4Du));
			Movement.BodyTurnSpeedScale = FMath::Lerp(
				0.85f,
				1.15f,
				HashToUnitFloat(Id.Value, 0x1B56C4E9u));
			Movement.CommandVariationRevision = 0;
			Movement.TravelDriftSegmentLengthCm = 2250.0f;
			Movement.CommandStartSideOffsetCm = 0.0f;
			Movement.CurrentSideOffsetCm = 0.0f;

			Squad.ActiveMemberSoldierIndices.Add(SoldierIndex);

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

		// Keep physical spawn ranges unchanged for the optimized P1 exporter.
		FAlgonaSquadEntityRange& EntityRange =
			SquadEntityRanges.AddDefaulted_GetRef();
		EntityRange.FirstSoldierIndex = FirstSoldierIndex;
		EntityRange.Count = Squads.Last().TotalMemberCount;

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
	RecoveredLostSoldierIndices.Reset();
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
	// soldier count before choosing the cheaper P1 export strategy.
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
	// small working set, while one contiguous Mass chunk pass wins at about 6%.
	constexpr int32 FullScanRatioNumerator = 3;
	constexpr int32 FullScanRatioDenominator = 50;
	const bool bUseFilteredFullScan =
		static_cast<int64>(SelectedSoldierCount) * FullScanRatioDenominator
		>= static_cast<int64>(SoldierEntities.Num()) * FullScanRatioNumerator;

	if (!bUseFilteredFullScan)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSelectedSquads_Direct);

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
	// emit snapshots only for squads selected by Presentation.
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
	if (!Squad.Formation.IsValidSlot(SlotIndex))
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
	const FVector2D LocalPosition =
		Squad.Formation.Slots[SlotIndex].LocalPosition;

	return Squad.AnchorLocation
		+ Right * static_cast<double>(LocalPosition.X)
		+ Forward * static_cast<double>(LocalPosition.Y);
}
