#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitFragments.h"
#include "Army/AlgonaUnitTrait.h"

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

bool UAlgonaSimulationSubsystem::CreateUnits(
	int32 UnitCount,
	int32 RequestedSquadSize)
{
	UWorld* World = GetWorld();
	if (!World
		|| !MassSpawnerSubsystem
		|| UnitCount <= 0
		|| RequestedSquadSize <= 0)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::UnitTemplateBuildFailed;
		return false;
	}

	UnitEntityConfig = NewObject<UMassEntityConfigAsset>(
		this,
		TEXT("AlgonaUnitRuntimeConfig"));

	UAlgonaUnitTrait* UnitTrait =
		UnitEntityConfig
			? NewObject<UAlgonaUnitTrait>(UnitEntityConfig)
			: nullptr;

	if (!UnitEntityConfig || !UnitTrait)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::UnitTemplateBuildFailed;
		return false;
	}

	FMassEntityConfig& UnitConfig =
		UnitEntityConfig->GetMutableConfig();
	UnitConfig.AddTrait(*UnitTrait);

	// Simulation creates only authoritative entity state. Presentation chooses
	// its renderer independently and never adds renderer fragments here.
	const FMassEntityTemplate& UnitTemplate =
		UnitEntityConfig->GetOrCreateEntityTemplate(*World);

	UnitEntities.Reset();
	UnitEntities.Reserve(UnitCount);

	// Keep the creation context alive until initial fragment data is filled.
	const TSharedPtr<FMassEntityManager::FEntityCreationContext>
		CreationContext = MassSpawnerSubsystem->SpawnEntities(
			UnitTemplate,
			static_cast<uint32>(UnitCount),
			UnitEntities);

	if (!CreationContext.IsValid()
		|| UnitEntities.Num() != UnitCount)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::UnitSpawnFailed;
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
		|| UnitEntities.IsEmpty()
		|| RequestedSquadSize <= 0)
	{
		return false;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	const int32 SquadsPerRow = AlgonaSimulationDefaults::SquadsPerRow;
	constexpr float SpaceBetweenSquads = 600.0f;
	constexpr float UnitSpacing = 150.0f;

	// Размер полного отряда на земле задаёт шаг расстановки отрядов на карте.
	FAlgonaFormationParams ReferenceParams;
	ReferenceParams.RowLength = GetAlgonaDefaultRowLength(RequestedSquadSize);
	ReferenceParams.SlotSpacing = UnitSpacing;
	ReferenceParams.RowSpacing = UnitSpacing;

	FAlgonaFormationLayout ReferenceLayout;
	BuildAlgonaFormationLayout(
		ReferenceParams,
		RequestedSquadSize,
		ReferenceLayout);

	const float FormationWorldDepth =
		static_cast<float>(ReferenceLayout.RowCount - 1)
		* ReferenceParams.RowSpacing;
	const float FormationWorldWidth =
		static_cast<float>(
			FMath::Min(ReferenceParams.RowLength, RequestedSquadSize) - 1)
		* ReferenceParams.SlotSpacing;

	const float SquadSpacingX =
		FormationWorldDepth + SpaceBetweenSquads;
	const float SquadSpacingY =
		FormationWorldWidth + SpaceBetweenSquads;

	const int32 SquadCount = FMath::DivideAndRoundUp(
		UnitEntities.Num(),
		RequestedSquadSize);

	Squads.Reset();
	Squads.Reserve(SquadCount);
	SquadSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	UnitSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);

	int32 UnitIndex = 0;

	for (int32 SquadIndex = 0;
		SquadIndex < SquadCount;
		++SquadIndex)
	{
		// Массив зарезервирован заранее, поэтому ссылка остаётся валидной.
		FAlgonaSquad& Squad = Squads.AddDefaulted_GetRef();
		Squad.SquadId = SquadIndex;

		const int32 MemberCount = FMath::Min(
			RequestedSquadSize,
			UnitEntities.Num() - UnitIndex);

		// Состав заполняется заранее: UnitId детерминирован (номер Unit + 1),
		// поэтому раскладку можно построить до записи фрагментов. Раскладка
		// строится тем же путём, что и при Reform.
		Squad.FormationParams = ReferenceParams;
		Squad.ActiveUnitIds.Reserve(MemberCount);

		for (int32 SlotIndex = 0;
			SlotIndex < MemberCount;
			++SlotIndex)
		{
			Squad.AddActiveUnit(
				static_cast<uint32>(UnitIndex + SlotIndex + 1));
		}

		Squad.RebuildFormationLayout();

		// Передние строки отрядов одного ряда стоят на одной линии,
		// центр отряда отсчитывается от передней строки.
		const int32 SquadX = SquadIndex % SquadsPerRow;
		const int32 SquadY = SquadIndex / SquadsPerRow;

		const double FrontRowX =
			static_cast<double>(SquadX) * SquadSpacingX
			+ FormationWorldDepth;

		Squad.CenterLocation = FVector(
			FrontRowX - Squad.FormationLayout.FrontRowLocalX,
			static_cast<double>(SquadY) * SquadSpacingY
				+ FormationWorldWidth * 0.5,
			0.0);
		Squad.TargetCenterLocation = Squad.CenterLocation;

		// Фрагменты Unit заполняются по слотам: слот SlotIndex занимает Unit
		// ActiveUnitIds[SlotIndex] = UnitIndex + 1.
		for (int32 SlotIndex = 0;
			SlotIndex < MemberCount;
			++SlotIndex)
		{
			if (!UnitEntities.IsValidIndex(UnitIndex))
			{
				return false;
			}

			const FMassEntityHandle UnitEntity =
				UnitEntities[UnitIndex];

			if (!EntityManager.IsEntityValid(UnitEntity))
			{
				return false;
			}

			FMassEntityView EntityView(EntityManager, UnitEntity);

			FAlgonaUnitIdFragment& Id =
				EntityView.GetFragmentData<FAlgonaUnitIdFragment>();
			Id.Value = static_cast<uint32>(UnitIndex + 1);

			FAlgonaSquadMemberFragment& Member =
				EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();
			Member.SquadId = Squad.SquadId;
			Member.SlotIndex = SlotIndex;

			FAlgonaUnitMovementFragment& Movement =
				EntityView.GetFragmentData<FAlgonaUnitMovementFragment>();
			Movement.Velocity = FVector::ZeroVector;
			Movement.LastProcessedSimulationTick = 0;
			Movement.State = EAlgonaUnitMovementState::Idle;

			FTransform InitialTransform = FTransform::Identity;
			InitialTransform.SetLocation(
				ComputeSlotWorldPosition(Squad, SlotIndex));
			InitialTransform.SetRotation(
				Squad.FacingDirection.Rotation().Quaternion());

			FTransformFragment& Transform =
				EntityView.GetFragmentData<FTransformFragment>();
			Transform.SetTransform(InitialTransform);

			UnitSpatialGrid.AddUnit(
				Id.Value,
				InitialTransform.GetLocation());

			++UnitIndex;
		}

		if (IsSquadSpatialGridEnabled())
		{
			SquadSpatialGrid.AddSquad(
				Squad.SquadId,
				Squad.CenterLocation);
		}
	}

	return UnitIndex == UnitEntities.Num();
}

void UAlgonaSimulationSubsystem::DestroyUnits()
{
	const bool bHadUnits = !UnitEntities.IsEmpty();

	if (MassSpawnerSubsystem && bHadUnits)
	{
		MassSpawnerSubsystem->DestroyEntities(UnitEntities);
	}

	UnitEntities.Reset();
	Squads.Reset();
	SquadSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	UnitSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	PendingCommands.Reset();
	bStressMoveEnabled = false;
	StressMoveDirections.Reset();
	UnitEntityConfig = nullptr;

	Metrics.EntityCount = 0;
	Metrics.SquadCount = 0;
}

int32 UAlgonaSimulationSubsystem::ExportUnitSnapshotsForSquads(
	TConstArrayView<int32> SquadIds,
	TArray<FAlgonaUnitSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSelectedSquadSnapshots);

	OutSnapshots.Reset();
	if (!MassEntitySubsystem
		|| !UnitSnapshotQuery
		|| SquadIds.IsEmpty()
		|| MaxEntities <= 0)
	{
		return 0;
	}

	// Count complete valid squads first. This gives us the exact selected
	// unit count before choosing the cheaper export strategy.
	int32 SelectedUnitCount = 0;
	int32 AcceptedSquadCount = 0;
	for (const int32 SquadId : SquadIds)
	{
		if (!Squads.IsValidIndex(SquadId)
			|| Squads[SquadId].SquadId != SquadId)
		{
			continue;
		}

		const int32 SquadUnitCount = Squads[SquadId].ActiveUnitIds.Num();
		if (SquadUnitCount <= 0
			|| SelectedUnitCount + SquadUnitCount > MaxEntities)
		{
			break;
		}

		SelectedUnitCount += SquadUnitCount;
		++AcceptedSquadCount;
	}

	if (SelectedUnitCount <= 0 || AcceptedSquadCount <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SelectedUnitCount);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	// Existing P1 squad-export crossover: direct entity reads win for a small
	// selected set; one sequential Mass pass wins from roughly 6% upward.
	constexpr int32 FullScanRatioNumerator = 3;
	constexpr int32 FullScanRatioDenominator = 50;
	const bool bUseFilteredFullScan =
		static_cast<int64>(SelectedUnitCount) * FullScanRatioDenominator
		>= static_cast<int64>(UnitEntities.Num()) * FullScanRatioNumerator;

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
				|| Squads[SquadId].SquadId != SquadId)
			{
				continue;
			}

			const TArray<uint32>& ActiveUnitIds =
				Squads[SquadId].ActiveUnitIds;
			if (ActiveUnitIds.IsEmpty())
			{
				continue;
			}

			++ProcessedSquadCount;

			for (const uint32 UnitId : ActiveUnitIds)
			{
				const int32 UnitIndex = static_cast<int32>(UnitId) - 1;
				if (UnitId == 0 || !UnitEntities.IsValidIndex(UnitIndex))
				{
					continue;
				}

				const FMassEntityHandle Entity = UnitEntities[UnitIndex];
				if (!EntityManager.IsEntityValid(Entity))
				{
					continue;
				}

				FMassEntityView EntityView(EntityManager, Entity);
				const FTransformFragment& TransformFragment =
					EntityView.GetFragmentData<FTransformFragment>();
				const FTransform& Transform = TransformFragment.GetTransform();

				FAlgonaUnitSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();
				Snapshot.EntityId = UnitId;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		}

		return OutSnapshots.Num();
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSelectedSquads_FilteredFullScan);

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
			|| Squads[SquadId].ActiveUnitIds.IsEmpty())
		{
			continue;
		}

		SelectedSquadMask[SquadId] = 1;
		++MaskedSquadCount;
	}

	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	UnitSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[&OutSnapshots, &SelectedSquadMask](FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaUnitIdFragment> Ids =
				Context.GetFragmentView<FAlgonaUnitIdFragment>();
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

				FAlgonaUnitSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();
				Snapshot.EntityId = Ids[Index].Value;
				Snapshot.Position = Transform.GetLocation();
				Snapshot.Facing = Transform.GetRotation();
			}
		});

	return OutSnapshots.Num();
}

int32 UAlgonaSimulationSubsystem::ExportUnitSnapshotsForUnitIds(
	TConstArrayView<uint32> UnitIds,
	TArray<FAlgonaUnitSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSimulation_ExportSelectedUnitSnapshots);

	OutSnapshots.Reset();

	if (!MassEntitySubsystem
		|| !UnitSnapshotQuery
		|| UnitIds.IsEmpty()
		|| MaxEntities <= 0)
	{
		return 0;
	}

	const int32 SelectedUnitCount =
		FMath::Min(
			UnitIds.Num(),
			MaxEntities);

	if (SelectedUnitCount <= 0)
	{
		return 0;
	}

	OutSnapshots.Reserve(SelectedUnitCount);

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	// Measured in P2 with 20k and 100k units: direct reads win below about 7%
	// of the army; one filtered sequential Mass pass wins from about 7% upward.
	constexpr int32 FullScanRatioNumerator = 6;
	constexpr int32 FullScanRatioDenominator = 100;

	const bool bUseFilteredFullScan =
		static_cast<int64>(SelectedUnitCount)
			* FullScanRatioDenominator
		>= static_cast<int64>(UnitEntities.Num())
			* FullScanRatioNumerator;

	if (!bUseFilteredFullScan)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(
			AlgonaSimulation_ExportSelectedUnits_Direct);

		for (int32 Index = 0;
			Index < SelectedUnitCount;
			++Index)
		{
			const uint32 UnitId =
				UnitIds[Index];

			if (UnitId == 0)
			{
				continue;
			}

			const int32 UnitIndex =
				static_cast<int32>(UnitId - 1);

			if (!UnitEntities.IsValidIndex(UnitIndex))
			{
				continue;
			}

			const FMassEntityHandle Entity =
				UnitEntities[UnitIndex];

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

			FAlgonaUnitSnapshot& Snapshot =
				OutSnapshots.AddDefaulted_GetRef();

			Snapshot.EntityId = UnitId;
			Snapshot.Position = Transform.GetLocation();
			Snapshot.Facing = Transform.GetRotation();
		}

		return OutSnapshots.Num();
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSimulation_ExportSelectedUnits_FilteredFullScan);

	TArray<uint8> SelectedUnitMask;
	SelectedUnitMask.Init(
		0,
		UnitEntities.Num());

	for (int32 Index = 0;
		Index < SelectedUnitCount;
		++Index)
	{
		const uint32 UnitId =
			UnitIds[Index];

		if (UnitId == 0)
		{
			continue;
		}

		const int32 UnitIndex =
			static_cast<int32>(UnitId - 1);

		if (SelectedUnitMask.IsValidIndex(UnitIndex))
		{
			SelectedUnitMask[UnitIndex] = 1;
		}
	}

	FMassExecutionContext ExecutionContext =
		EntityManager.CreateExecutionContext(0.0f);

	UnitSnapshotQuery->ForEachEntityChunk(
		ExecutionContext,
		[
			&OutSnapshots,
			&SelectedUnitMask
		](FMassExecutionContext& Context)
		{
			const TConstArrayView<FAlgonaUnitIdFragment> Ids =
				Context.GetFragmentView<FAlgonaUnitIdFragment>();

			const TConstArrayView<FTransformFragment> Transforms =
				Context.GetFragmentView<FTransformFragment>();

			for (int32 Index = 0;
				Index < Context.GetNumEntities();
				++Index)
			{
				const uint32 UnitId =
					Ids[Index].Value;

				if (UnitId == 0)
				{
					continue;
				}

				const int32 UnitIndex =
					static_cast<int32>(UnitId - 1);

				if (!SelectedUnitMask.IsValidIndex(UnitIndex)
					|| SelectedUnitMask[UnitIndex] == 0)
				{
					continue;
				}

				const FTransform& Transform =
					Transforms[Index].GetTransform();

				FAlgonaUnitSnapshot& Snapshot =
					OutSnapshots.AddDefaulted_GetRef();

				Snapshot.EntityId = UnitId;
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
	const TArray<FAlgonaFormationSlot>& Slots = Squad.FormationLayout.Slots;
	if (!Slots.IsValidIndex(SlotIndex))
	{
		return Squad.CenterLocation;
	}

	const FVector Forward = Squad.GetForwardDirection2D();
	const FVector Right = FVector::CrossProduct(
		FVector::UpVector,
		Forward).GetSafeNormal();

	// Смещение слота из раскладки поворачивается по направлению Squad:
	// X раскладки — вперёд, Y — вправо.
	const FVector2f& LocalOffset = Slots[SlotIndex].LocalOffset;

	return Squad.CenterLocation
		+ Forward * static_cast<double>(LocalOffset.X)
		+ Right * static_cast<double>(LocalOffset.Y);
}

bool UAlgonaSimulationSubsystem::ValidateSquadMembership()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ValidateSquadMembership);

	if (!MassEntitySubsystem)
	{
		return false;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	int32 CheckedUnitCount = 0;

	for (const FAlgonaSquad& Squad : Squads)
	{
		// Каждому слоту раскладки соответствует ровно один активный Unit.
		if (Squad.ActiveUnitIds.Num() != Squad.FormationLayout.Slots.Num())
		{
			UE_LOG(
				LogAlgonaSimulation,
				Error,
				TEXT("[P2 Squads] Squad %d: %d active units for %d slots"),
				Squad.SquadId,
				Squad.ActiveUnitIds.Num(),
				Squad.FormationLayout.Slots.Num());
			return false;
		}

		for (int32 SlotIndex = 0;
			SlotIndex < Squad.ActiveUnitIds.Num();
			++SlotIndex)
		{
			const uint32 UnitId = Squad.ActiveUnitIds[SlotIndex];
			const int32 UnitIndex = static_cast<int32>(UnitId) - 1;

			if (UnitId == 0
				|| !UnitEntities.IsValidIndex(UnitIndex)
				|| !EntityManager.IsEntityValid(UnitEntities[UnitIndex]))
			{
				UE_LOG(
					LogAlgonaSimulation,
					Error,
					TEXT("[P2 Squads] Squad %d slot %d: invalid UnitId %u"),
					Squad.SquadId,
					SlotIndex,
					UnitId);
				return false;
			}

			// Обратная связь: Unit помнит тот же Squad и тот же слот.
			// Если UnitId повторяется в двух слотах, одна из проверок не сойдётся.
			FMassEntityView EntityView(
				EntityManager,
				UnitEntities[UnitIndex]);

			const FAlgonaSquadMemberFragment& Member =
				EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();

			if (Member.SquadId != Squad.SquadId
				|| Member.SlotIndex != SlotIndex)
			{
				UE_LOG(
					LogAlgonaSimulation,
					Error,
					TEXT("[P2 Squads] Unit %u is in squad %d slot %d, but its fragment says squad %d slot %d"),
					UnitId,
					Squad.SquadId,
					SlotIndex,
					Member.SquadId,
					Member.SlotIndex);
				return false;
			}

			++CheckedUnitCount;
		}
	}

	// Все Unit распределены по слотам, ни один не остался вне Squad.
	if (CheckedUnitCount != UnitEntities.Num())
	{
		UE_LOG(
			LogAlgonaSimulation,
			Error,
			TEXT("[P2 Squads] %d units in slots, but %d units exist"),
			CheckedUnitCount,
			UnitEntities.Num());
		return false;
	}

	UE_LOG(
		LogAlgonaSimulation,
		Display,
		TEXT("[P2 Squads] membership OK squads=%d units=%d"),
		Squads.Num(),
		CheckedUnitCount);

	return true;
}
