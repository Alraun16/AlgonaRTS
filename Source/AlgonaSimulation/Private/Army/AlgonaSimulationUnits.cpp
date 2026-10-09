#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaUnitFragments.h"

#include "HAL/IConsoleManager.h"
#include "Army/AlgonaUnitTrait.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTemplate.h"
#include "MassEntityView.h"
#include "MassSpawnerSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

FAlgonaSpawnArmyResult UAlgonaSimulationSubsystem::SpawnArmy(
	int32 UnitCount,
	int32 RequestedSquadSize,
	const FVector& Origin,
	const FVector& Forward)
{
	FAlgonaSpawnArmyResult Result;

	if (!IsAuthoritativeSimulationWorld())
	{
		return Result;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	if (!CreateUnits(UnitCount, RequestedSquadSize, Origin, Forward, Result))
	{
		UE_LOG(
			LogAlgonaSimulation,
			Error,
			TEXT("[P2 Spawn] failed to create %d units (state %d)"),
			UnitCount,
			static_cast<int32>(Metrics.StartupState));
		return Result;
	}

#if !UE_BUILD_SHIPPING
	// Полная проверка связи Squad <-> Unit после каждого спавна.
	if (!ValidateSquadMembership())
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::SquadInitializationFailed;
		return Result;
	}
#endif

	Metrics.StartupState = EAlgonaSimulationStartupState::Ready;
	Metrics.EntityCount = UnitEntities.Num();
	Metrics.SquadCount = Squads.Num();

	// Новое состояние доступно Presentation.
	++StateRevision;

	UE_LOG(
		LogAlgonaSimulation,
		Display,
		TEXT("[P2 Spawn] units=%d squads=%d skippedCells=%d%s | origin=%s forward=%s | total units=%d squads=%d | %.0f ms"),
		Result.UnitCount,
		Result.SquadCount,
		Result.SkippedCells,
		Result.bRanOutOfSpace ? TEXT(" (ran out of free space)") : TEXT(""),
		*Origin.ToCompactString(),
		*Forward.GetSafeNormal2D().ToCompactString(),
		UnitEntities.Num(),
		Squads.Num(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);

	return Result;
}

void UAlgonaSimulationSubsystem::ClearArmy()
{
	if (!IsAuthoritativeSimulationWorld())
	{
		return;
	}

	const int32 PreviousUnitCount = UnitEntities.Num();
	const int32 PreviousSquadCount = Squads.Num();

	DestroyUnits();
	++StateRevision;

	Metrics.StartupState = EAlgonaSimulationStartupState::Ready;

	UE_LOG(
		LogAlgonaSimulation,
		Display,
		TEXT("[P2 Spawn] cleared units=%d squads=%d"),
		PreviousUnitCount,
		PreviousSquadCount);
}

bool UAlgonaSimulationSubsystem::CreateUnits(
	int32 UnitCount,
	int32 RequestedSquadSize,
	const FVector& Origin,
	const FVector& Forward,
	FAlgonaSpawnArmyResult& OutResult)
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

	// Шаблон сущности строится один раз на мир и переиспользуется
	// следующими спавнами.
	if (!UnitEntityConfig)
	{
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
	}

	// Simulation creates only authoritative entity state. Presentation chooses
	// its renderer independently and never adds renderer fragments here.
	const FMassEntityTemplate& UnitTemplate =
		UnitEntityConfig->GetOrCreateEntityTemplate(*World);

	// Unit досыпаются к уже существующим: индекс нового Unit (UnitId - 1)
	// продолжает нумерацию, поэтому индексы прежних Unit остаются верными.
	const int32 FirstUnitIndex = UnitEntities.Num();
	UnitEntities.Reserve(FirstUnitIndex + UnitCount);

	// Keep the creation context alive until initial fragment data is filled.
	const TSharedPtr<FMassEntityManager::FEntityCreationContext>
		CreationContext = MassSpawnerSubsystem->SpawnEntities(
			UnitTemplate,
			static_cast<uint32>(UnitCount),
			UnitEntities);

	if (!CreationContext.IsValid()
		|| UnitEntities.Num() != FirstUnitIndex + UnitCount)
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::UnitSpawnFailed;
		return false;
	}

	if (!CreateSquads(RequestedSquadSize, FirstUnitIndex, Origin, Forward, OutResult))
	{
		Metrics.StartupState =
			EAlgonaSimulationStartupState::SquadInitializationFailed;
		return false;
	}

	return true;
}

namespace
{
	// Тестовые отряды крупных существ, пока нет типов существ (P3).
	// Каждый N-й отряд создаётся особым: 0 — выключено.
	TAutoConsoleVariable<int32> CVarAlgonaP2LargeSquadEvery(
		TEXT("algona.P2.LargeSquadEvery"),
		50,
		TEXT("Every Nth squad is made of large units (test content). 0 disables it."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP2MixedSquadEvery(
		TEXT("algona.P2.MixedSquadEvery"),
		25,
		TEXT("Every Nth squad mixes large and normal units (test content). 0 disables it."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2LargeUnitRadius(
		TEXT("algona.P2.LargeUnitRadius"),
		70.0f,
		TEXT("Body radius of a large test unit, cm."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2LargeUnitMeshScale(
		TEXT("algona.P2.LargeUnitMeshScale"),
		2.5f,
		TEXT("Mesh scale of a large test unit."),
		ECVF_Default);

	// Состав тестовых отрядов: только крупные и смешанный.
	constexpr int32 LargeSquadMemberCount = 5;
	constexpr int32 MixedSquadMemberCount = 8;
	constexpr int32 MixedSquadLargeCount = 2;

	// Запас кандидатов на один Squad при расстановке: если столько клеток
	// подряд не подошло, свободного места рядом считай что нет.
	constexpr int32 SpawnCandidatesPerSquad = 4;
	constexpr int32 SpawnCandidatesReserve = 64;
}

bool UAlgonaSimulationSubsystem::CreateSquads(
	int32 RequestedSquadSize,
	int32 FirstUnitIndex,
	const FVector& Origin,
	const FVector& Forward,
	FAlgonaSpawnArmyResult& OutResult)
{
	if (!MassEntitySubsystem
		|| UnitEntities.IsEmpty()
		|| RequestedSquadSize <= 0
		|| !UnitEntities.IsValidIndex(FirstUnitIndex))
	{
		return false;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

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

	// Оценка числа отрядов: тестовые отряды крупных существ меньше обычных,
	// поэтому их получится больше. Точное число известно только по ходу.
	const int32 EstimatedSquadCount = FMath::DivideAndRoundUp(
		UnitEntities.Num() - FirstUnitIndex,
		RequestedSquadSize);

	Squads.Reserve(Squads.Num() + EstimatedSquadCount);
	ResizeUnitState(UnitEntities.Num());

	// Клетки раскладки: ряды назад от Origin, внутри ряда слева направо
	// относительно Forward, ряд выровнен по середине. Origin — середина
	// передней линии армии.
	FVector PlacementForward = Forward.GetSafeNormal2D();
	if (PlacementForward.IsNearlyZero())
	{
		PlacementForward = FVector::ForwardVector;
	}

	const FVector PlacementRight =
		FVector::CrossProduct(FVector::UpVector, PlacementForward).GetSafeNormal();

	// Клеток в ряду — корень из их числа: армия встаёт примерно квадратом.
	const int32 CellsPerRow = FMath::Max(
		FMath::CeilToInt32(FMath::Sqrt(static_cast<double>(EstimatedSquadCount))),
		1);

	// Площадка одной клетки: половина шага раскладки по обеим осям.
	const FVector2D PlacementHalfExtent(
		static_cast<double>(SquadSpacingX) * 0.5,
		static_cast<double>(SquadSpacingY) * 0.5);

	// Сколько клеток вообще разрешено перебрать: занятые и непроходимые
	// пропускаются, но бесконечно искать место незачем.
	const int32 MaxCandidates =
		EstimatedSquadCount * SpawnCandidatesPerSquad + SpawnCandidatesReserve;

	// Переиспользуемый буфер запроса к Unit Grid.
	TArray<uint32> PlacementScratch;

	const int32 FirstSquadIndex = Squads.Num();

	int32 CandidateIndex = 0;
	int32 UnitIndex = FirstUnitIndex;
	int32 LargeSquadCount = 0;
	int32 MixedSquadCount = 0;
	int32 LargeUnitCount = 0;

	// Отряды создаются, пока есть нераспределённые Unit: состав отряда
	// зависит от его номера, поэтому число отрядов заранее неизвестно.
	for (int32 SquadIndex = Squads.Num();
		UnitIndex < UnitEntities.Num();
		++SquadIndex)
	{
		// Ссылка живёт только внутри этого витка цикла.
		FAlgonaSquad& Squad = Squads.AddDefaulted_GetRef();
		Squad.SquadId = SquadIndex;

		// Тестовые отряды крупных существ: каждый N-й отряд особого состава.
		// Крупные занимают передние слоты, поэтому в смешанном отряде они
		// оказываются в первой строке.
		const int32 LargeSquadEvery = CVarAlgonaP2LargeSquadEvery.GetValueOnGameThread();
		const int32 MixedSquadEvery = CVarAlgonaP2MixedSquadEvery.GetValueOnGameThread();

		int32 RequestedMemberCount = RequestedSquadSize;
		int32 LargeMemberCount = 0;

		if (LargeSquadEvery > 0 && SquadIndex % LargeSquadEvery == 0)
		{
			RequestedMemberCount = LargeSquadMemberCount;
			LargeMemberCount = LargeSquadMemberCount;
		}
		else if (MixedSquadEvery > 0 && SquadIndex % MixedSquadEvery == 0)
		{
			RequestedMemberCount = MixedSquadMemberCount;
			LargeMemberCount = MixedSquadLargeCount;
		}

		const int32 MemberCount = FMath::Min(
			RequestedMemberCount,
			UnitEntities.Num() - UnitIndex);

		LargeMemberCount = FMath::Min(LargeMemberCount, MemberCount);

		if (LargeMemberCount > 0)
		{
			LargeUnitCount += LargeMemberCount;

			if (LargeMemberCount == MemberCount)
			{
				++LargeSquadCount;
			}
			else
			{
				++MixedSquadCount;
			}
		}

		// Размеры Unit задаются до раскладки: интервал строя зависит от
		// самого крупного Unit состава.
		const float LargeRadius = CVarAlgonaP2LargeUnitRadius.GetValueOnGameThread();
		const float LargeMeshScale = CVarAlgonaP2LargeUnitMeshScale.GetValueOnGameThread();

		for (int32 SlotIndex = 0; SlotIndex < MemberCount; ++SlotIndex)
		{
			const bool bLarge = SlotIndex < LargeMemberCount;
			UnitState.Radii[UnitIndex + SlotIndex] =
				bLarge ? LargeRadius : Squad.UnitRadius;
			UnitState.MeshScales[UnitIndex + SlotIndex] = bLarge ? LargeMeshScale : 1.0f;
		}

		Squad.MaxUnitRadius = LargeMemberCount > 0
			? FMath::Max(LargeRadius, Squad.UnitRadius)
			: Squad.UnitRadius;

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

		// Клетка для Squad — первая свободная из кандидатов: ряды назад от
		// Origin, внутри ряда от середины вправо. Занятые и непроходимые
		// клетки пропускаются.
		FVector CellCenter = FVector::ZeroVector;
		bool bCellFound = false;

		while (CandidateIndex < MaxCandidates)
		{
			const int32 CellRow = CandidateIndex / CellsPerRow;
			const int32 CellColumn = CandidateIndex % CellsPerRow;
			++CandidateIndex;

			const double LateralOffset =
				(static_cast<double>(CellColumn)
					- static_cast<double>(CellsPerRow - 1) * 0.5)
				* static_cast<double>(SquadSpacingY);

			FVector Candidate = Origin
				- PlacementForward
					* (static_cast<double>(CellRow) * static_cast<double>(SquadSpacingX))
				+ PlacementRight * LateralOffset;
			Candidate.Z = Origin.Z;

			if (!IsSquadPlacementFree(
				Candidate,
				PlacementForward,
				PlacementHalfExtent,
				PlacementScratch))
			{
				++OutResult.SkippedCells;
				continue;
			}

			CellCenter = Candidate;
			bCellFound = true;
			break;
		}

		if (!bCellFound)
		{
			// Свободного места не нашлось: этот Squad не создаётся, а его
			// Unit удаляются после цикла.
			Squads.Pop(EAllowShrinking::No);
			OutResult.bRanOutOfSpace = true;
			break;
		}

		// Передние строки отрядов одного ряда стоят на одной линии:
		// центр отсчитывается от передней линии клетки, поэтому мелкий
		// отряд крупных существ не провисает в середину клетки.
		Squad.CenterLocation = CellCenter
			+ PlacementForward
				* (static_cast<double>(FormationWorldDepth) * 0.5
					- static_cast<double>(Squad.FormationLayout.FrontRowLocalX));
		Squad.CenterLocation.Z = Origin.Z;
		Squad.FacingDirection = PlacementForward;
		Squad.FinalFacingDirection = PlacementForward;
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
	}

	// Места не нашлось: Unit, оставшиеся без Squad, удаляются. Они в хвосте
	// массивов, поэтому индексы (UnitId - 1) прежних Unit не меняются.
	if (UnitIndex < UnitEntities.Num() && MassSpawnerSubsystem)
	{
		TArray<FMassEntityHandle> SurplusEntities(
			UnitEntities.GetData() + UnitIndex,
			UnitEntities.Num() - UnitIndex);

		MassSpawnerSubsystem->DestroyEntities(SurplusEntities);
		UnitEntities.SetNum(UnitIndex, EAllowShrinking::No);
		ResizeUnitState(UnitIndex);
	}

	OutResult.UnitCount = UnitIndex - FirstUnitIndex;
	OutResult.SquadCount = Squads.Num() - FirstSquadIndex;

	// Явный отчёт о составе: сразу видно, применились ли настройки
	// тестовых отрядов крупных существ.
	UE_LOG(
		LogAlgonaSimulation,
		Display,
		TEXT("[P2 Squads] composition squads=%d large=%d mixed=%d largeUnits=%d (radius=%.0f meshScale=%.1f, every large=%d mixed=%d)"),
		Squads.Num(),
		LargeSquadCount,
		MixedSquadCount,
		LargeUnitCount,
		CVarAlgonaP2LargeUnitRadius.GetValueOnGameThread(),
		CVarAlgonaP2LargeUnitMeshScale.GetValueOnGameThread(),
		CVarAlgonaP2LargeSquadEvery.GetValueOnGameThread(),
		CVarAlgonaP2MixedSquadEvery.GetValueOnGameThread());

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
	UnitSpatialGrid.Reset(AlgonaSimulationDefaults::SpatialGridCellSizeCm);
	PendingCommands.Reset();
	PathRequestQueue.Reset();
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
	Snapshot.MeshScale = UnitState.MeshScales[UnitIndex];
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
