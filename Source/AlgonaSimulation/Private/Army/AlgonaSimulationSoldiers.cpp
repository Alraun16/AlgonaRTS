#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaSoldierFragments.h"
#include "Army/AlgonaSoldierTrait.h"

#include "Engine/World.h"
#include "Mass/EntityFragments.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTemplate.h"
#include "MassEntityView.h"
#include "MassSpawnerSubsystem.h"

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

	constexpr int32 SquadsPerRow = 20;
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
	}

	return SoldierIndex == SoldierEntities.Num();
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
	PendingMoveCommands.Reset();
	SoldierEntityConfig = nullptr;

	Metrics.EntityCount = 0;
	Metrics.SquadCount = 0;
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
