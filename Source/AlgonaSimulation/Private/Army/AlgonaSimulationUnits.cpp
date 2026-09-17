#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitFragments.h"
#include "Army/AlgonaUnitTrait.h"

#include "Engine/World.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
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
	InitializeUnitState(UnitEntities.Num());

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
		// поэтому раскладку можно построить до записи состояния Unit.
		// Раскладка строится тем же путём, что и при Reform.
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

		// Unit стартуют в своих слотах и смотрят по направлению Squad.
		const FVector SquadForward = Squad.GetForwardDirection2D();
		const float SquadFacingYaw = static_cast<float>(
			FMath::Atan2(SquadForward.Y, SquadForward.X));

		// Слот SlotIndex занимает Unit ActiveUnitIds[SlotIndex] = UnitIndex + 1.
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

			// Mass-сущность хранит только идентичность Unit.
			FMassEntityView EntityView(EntityManager, UnitEntity);

			FAlgonaUnitIdFragment& Id =
				EntityView.GetFragmentData<FAlgonaUnitIdFragment>();
			Id.Value = static_cast<uint32>(UnitIndex + 1);

			// Состояние Unit — в плоских массивах по индексу UnitId - 1.
			UnitState.Positions[UnitIndex] =
				ComputeSlotWorldPosition(Squad, SlotIndex);
			UnitState.FacingYaws[UnitIndex] = SquadFacingYaw;
			UnitState.SquadIds[UnitIndex] = Squad.SquadId;
			UnitState.SlotIndices[UnitIndex] = SlotIndex;

			UnitSpatialGrid.AddUnit(
				Id.Value,
				UnitState.Positions[UnitIndex]);

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
	UnitState = FAlgonaUnitStateArrays();
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

void UAlgonaSimulationSubsystem::AppendUnitSnapshot(
	uint32 UnitId,
	TArray<FAlgonaUnitSnapshot>& OutSnapshots) const
{
	const int32 UnitIndex = static_cast<int32>(UnitId) - 1;

	if (UnitId == 0 || !UnitState.Positions.IsValidIndex(UnitIndex))
	{
		return;
	}

	FAlgonaUnitSnapshot& Snapshot = OutSnapshots.AddDefaulted_GetRef();
	Snapshot.EntityId = UnitId;
	Snapshot.Position = UnitState.Positions[UnitIndex];
	Snapshot.Facing = FQuat(
		FVector::UpVector,
		UnitState.FacingYaws[UnitIndex]);
}

int32 UAlgonaSimulationSubsystem::ExportUnitSnapshotsForSquads(
	TConstArrayView<int32> SquadIds,
	TArray<FAlgonaUnitSnapshot>& OutSnapshots,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_ExportSelectedSquadSnapshots);

	OutSnapshots.Reset();

	if (SquadIds.IsEmpty() || MaxEntities <= 0)
	{
		return 0;
	}

	for (const int32 SquadId : SquadIds)
	{
		if (!Squads.IsValidIndex(SquadId)
			|| Squads[SquadId].SquadId != SquadId)
		{
			continue;
		}

		const TArray<uint32>& ActiveUnitIds = Squads[SquadId].ActiveUnitIds;

		// Squad экспортируется только целиком.
		if (OutSnapshots.Num() + ActiveUnitIds.Num() > MaxEntities)
		{
			break;
		}

		for (const uint32 UnitId : ActiveUnitIds)
		{
			AppendUnitSnapshot(UnitId, OutSnapshots);
		}
	}

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

	if (UnitIds.IsEmpty() || MaxEntities <= 0)
	{
		return 0;
	}

	const int32 SelectedUnitCount = FMath::Min(
		UnitIds.Num(),
		MaxEntities);

	OutSnapshots.Reserve(SelectedUnitCount);

	// Состояние читается напрямую из плоских массивов по UnitId, поэтому
	// отдельный путь с полным проходом по всем Unit не нужен.
	for (int32 Index = 0; Index < SelectedUnitCount; ++Index)
	{
		AppendUnitSnapshot(UnitIds[Index], OutSnapshots);

	}

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

	// Массивы состояния покрывают ровно все Unit.
	if (UnitState.Positions.Num() != UnitEntities.Num())
	{
		UE_LOG(
			LogAlgonaSimulation,
			Error,
			TEXT("[P2 Squads] Unit state holds %d units, but %d units exist"),
			UnitState.Positions.Num(),
			UnitEntities.Num());
		return false;
	}

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
			const int32 StateSquadId = UnitState.SquadIds[UnitIndex];
			const int32 StateSlotIndex = UnitState.SlotIndices[UnitIndex];

			if (StateSquadId != Squad.SquadId
				|| StateSlotIndex != SlotIndex)
			{
				UE_LOG(
					LogAlgonaSimulation,
					Error,
					TEXT("[P2 Squads] Unit %u is in squad %d slot %d, but its state says squad %d slot %d"),
					UnitId,
					Squad.SquadId,
					SlotIndex,
					StateSquadId,
					StateSlotIndex);
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
