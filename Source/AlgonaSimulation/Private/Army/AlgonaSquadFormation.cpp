#include "Core/AlgonaSimulationSubsystem.h"

// Formation generation and soldier-side assignment state.
#include "Army/AlgonaFormation.h"
#include "Army/AlgonaSoldierFragments.h"

// Direct entity views are used only for rare O(squad-size) Reform operations.
#include "Mass/EntityFragments.h"
#include "MassEntityManager.h"
#include "MassEntitySubsystem.h"
#include "MassEntityView.h"

void UAlgonaSimulationSubsystem::ReformSquad(FAlgonaSquad& Squad)
{
	// Keep the old mirror cache as stale data. It is deliberately not rebuilt
	// here; the next >90 degree turn will refresh it lazily if actually needed.
	TArray<int32> StaleMirrorMap = MoveTemp(Squad.Formation.MirrorSlotIndices);
	const uint32 StaleMirrorRevision = Squad.Formation.MirrorRevision;
	const uint32 NextFormationRevision = Squad.Formation.FormationRevision + 1;

	FAlgonaFormationLayout NewFormation =
		FAlgonaFormationGenerator::BuildRectangle(
			Squad.ActiveMemberSoldierIndices.Num(),
			Squad.SoldierSpacingCm,
			Squad.RequestedMaxSlotsPerRow);

	NewFormation.FormationRevision = NextFormationRevision;
	NewFormation.MirrorSlotIndices = MoveTemp(StaleMirrorMap);
	NewFormation.MirrorRevision = StaleMirrorRevision;
	Squad.Formation = MoveTemp(NewFormation);

	// Any physical Reform temporarily suspends cohesion classification. This is
	// set centrally so width changes, Lost compaction and recovery all obey the
	// same rule without depending on which caller requested the Reform.
	Squad.bReformInProgress = !Squad.ActiveMemberSoldierIndices.IsEmpty();
	Squad.ReformUnsettledActiveCount = Squad.ActiveMemberSoldierIndices.Num();

	RefreshActiveMemberSlotFragments(Squad);
}

void UAlgonaSimulationSubsystem::RefreshActiveMemberSlotFragments(
	FAlgonaSquad& Squad)
{
	if (!MassEntitySubsystem)
	{
		return;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();

	// Position in ActiveMemberSoldierIndices is the authoritative current slot.
	// Reform touches only this squad, never all 20k soldiers.
	for (int32 SlotIndex = 0;
		SlotIndex < Squad.ActiveMemberSoldierIndices.Num();
		++SlotIndex)
	{
		const int32 SoldierIndex =
			Squad.ActiveMemberSoldierIndices[SlotIndex];
		if (!SoldierEntities.IsValidIndex(SoldierIndex))
		{
			continue;
		}

		const FMassEntityHandle SoldierEntity = SoldierEntities[SoldierIndex];
		if (!EntityManager.IsEntityValid(SoldierEntity))
		{
			continue;
		}

		FMassEntityView EntityView(EntityManager, SoldierEntity);
		FAlgonaSquadMemberFragment& Member =
			EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();
		Member.SquadId = Squad.SquadId;
		Member.SlotIndex = SlotIndex;
		Member.FormationState = EAlgonaSoldierFormationState::Active;
	}
}

void UAlgonaSimulationSubsystem::EnsureMirrorSlotMap(FAlgonaSquad& Squad)
{
	const bool bMirrorMapStale =
		Squad.Formation.MirrorRevision != Squad.Formation.FormationRevision
		|| Squad.Formation.MirrorSlotIndices.Num() != Squad.Formation.Num();

	if (bMirrorMapStale)
	{
		FAlgonaFormationGenerator::BuildMirrorSlotMap(Squad.Formation);
	}
}

void UAlgonaSimulationSubsystem::MirrorActiveMemberAssignments(
	FAlgonaSquad& Squad)
{
	EnsureMirrorSlotMap(Squad);

	const int32 ActiveCount = Squad.ActiveMemberSoldierIndices.Num();
	if (ActiveCount <= 1
		|| Squad.Formation.MirrorSlotIndices.Num() != ActiveCount)
	{
		return;
	}

	TArray<int32> MirroredMembers;
	MirroredMembers.SetNumUninitialized(ActiveCount);

	// The formation itself will flip 180 degrees. Moving each soldier's slot to
	// local -Position keeps its world-space target in the same place, avoiding
	// the classic cross-formation sprint during an about-face.
	for (int32 OldSlotIndex = 0;
		OldSlotIndex < ActiveCount;
		++OldSlotIndex)
	{
		const int32 MirroredSlotIndex =
			Squad.Formation.MirrorSlotIndices[OldSlotIndex];
		if (!MirroredMembers.IsValidIndex(MirroredSlotIndex))
		{
			return;
		}

		MirroredMembers[MirroredSlotIndex] =
			Squad.ActiveMemberSoldierIndices[OldSlotIndex];
	}

	Squad.ActiveMemberSoldierIndices = MoveTemp(MirroredMembers);
	RefreshActiveMemberSlotFragments(Squad);
}

int32 UAlgonaSimulationSubsystem::MarkCurrentFarMembersLost(
	FAlgonaSquad& Squad)
{
	if (!MassEntitySubsystem || Squad.ActiveMemberSoldierIndices.IsEmpty())
	{
		return 0;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	int32 LostCount = 0;

	// Compact in place. A removed active member's hole is filled by the last
	// active member exactly as specified; there is no nearest-unit search and no
	// full-army redistribution.
	int32 ActiveIndex = 0;
	while (ActiveIndex < Squad.ActiveMemberSoldierIndices.Num())
	{
		const int32 SoldierIndex =
			Squad.ActiveMemberSoldierIndices[ActiveIndex];
		if (!SoldierEntities.IsValidIndex(SoldierIndex))
		{
			Squad.ActiveMemberSoldierIndices[ActiveIndex] =
				Squad.ActiveMemberSoldierIndices.Last();
			Squad.ActiveMemberSoldierIndices.Pop(EAllowShrinking::No);
			continue;
		}

		const FMassEntityHandle SoldierEntity = SoldierEntities[SoldierIndex];
		if (!EntityManager.IsEntityValid(SoldierEntity))
		{
			Squad.ActiveMemberSoldierIndices[ActiveIndex] =
				Squad.ActiveMemberSoldierIndices.Last();
			Squad.ActiveMemberSoldierIndices.Pop(EAllowShrinking::No);
			continue;
		}

		FMassEntityView EntityView(EntityManager, SoldierEntity);
		FAlgonaSoldierMovementFragment& Movement =
			EntityView.GetFragmentData<FAlgonaSoldierMovementFragment>();

		if (Movement.DistanceState != EAlgonaSoldierDistanceState::Far)
		{
			++ActiveIndex;
			continue;
		}

		FAlgonaSquadMemberFragment& Member =
			EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();
		Member.SlotIndex = INDEX_NONE;
		Member.FormationState = EAlgonaSoldierFormationState::Lost;
		Movement.DistanceState = EAlgonaSoldierDistanceState::Lost;
		Movement.PassageState = EAlgonaSoldierPassageState::Formation;

		Squad.ActiveMemberSoldierIndices[ActiveIndex] =
			Squad.ActiveMemberSoldierIndices.Last();
		Squad.ActiveMemberSoldierIndices.Pop(EAllowShrinking::No);
		++LostCount;
	}

	if (LostCount <= 0)
	{
		return 0;
	}

	// If the active formation disappeared completely, restore one Lost member as
	// the new center before rebuilding slots so the squad remains recoverable.
	if (Squad.ActiveMemberSoldierIndices.IsEmpty())
	{
		RestoreEmptySquadFromNearestLostMember(Squad);
	}
	else
	{
		ReformSquad(Squad);
	}

	return LostCount;
}

void UAlgonaSimulationSubsystem::ProcessRecoveredLostMembers(
	TConstArrayView<int32> SoldierIndices)
{
	if (!MassEntitySubsystem || SoldierIndices.IsEmpty() || Squads.IsEmpty())
	{
		return;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	TArray<uint8> SquadsNeedingReform;
	SquadsNeedingReform.Init(0, Squads.Num());

	// Recovered Lost members append to the end. Multiple recoveries in the same
	// fixed step still cause at most one Reform per affected squad.
	for (const int32 SoldierIndex : SoldierIndices)
	{
		if (!SoldierEntities.IsValidIndex(SoldierIndex))
		{
			continue;
		}

		const FMassEntityHandle SoldierEntity = SoldierEntities[SoldierIndex];
		if (!EntityManager.IsEntityValid(SoldierEntity))
		{
			continue;
		}

		FMassEntityView EntityView(EntityManager, SoldierEntity);
		FAlgonaSquadMemberFragment& Member =
			EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();

		if (Member.FormationState != EAlgonaSoldierFormationState::Lost
			|| !Squads.IsValidIndex(Member.SquadId))
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[Member.SquadId];
		if (Squad.SquadId != Member.SquadId)
		{
			continue;
		}

		Squad.ActiveMemberSoldierIndices.Add(SoldierIndex);
		Member.FormationState = EAlgonaSoldierFormationState::Active;

		FAlgonaSoldierMovementFragment& Movement =
			EntityView.GetFragmentData<FAlgonaSoldierMovementFragment>();
		Movement.DistanceState = EAlgonaSoldierDistanceState::Near;
		Movement.PassageState = EAlgonaSoldierPassageState::Formation;

		SquadsNeedingReform[Member.SquadId] = 1;
	}

	for (int32 SquadIndex = 0;
		SquadIndex < SquadsNeedingReform.Num();
		++SquadIndex)
	{
		if (SquadsNeedingReform[SquadIndex] != 0)
		{
			ReformSquad(Squads[SquadIndex]);
		}
	}
}

void UAlgonaSimulationSubsystem::RestoreEmptySquadFromNearestLostMember(
	FAlgonaSquad& Squad)
{
	if (!MassEntitySubsystem
		|| !SquadEntityRanges.IsValidIndex(Squad.SquadId))
	{
		ReformSquad(Squad);
		return;
	}

	FMassEntityManager& EntityManager =
		MassEntitySubsystem->GetMutableEntityManager();
	const FAlgonaSquadEntityRange& EntityRange =
		SquadEntityRanges[Squad.SquadId];

	int32 BestSoldierIndex = INDEX_NONE;
	double BestDistanceSquared = TNumericLimits<double>::Max();
	FVector BestLocation = Squad.AnchorLocation;

	// This scans only the original members of the empty squad, never all Mass
	// entities. The member nearest the former center becomes the replacement.
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

		const FMassEntityHandle SoldierEntity = SoldierEntities[SoldierIndex];
		if (!EntityManager.IsEntityValid(SoldierEntity))
		{
			continue;
		}

		FMassEntityView EntityView(EntityManager, SoldierEntity);
		const FAlgonaSquadMemberFragment& Member =
			EntityView.GetFragmentData<FAlgonaSquadMemberFragment>();
		if (Member.SquadId != Squad.SquadId
			|| Member.FormationState != EAlgonaSoldierFormationState::Lost)
		{
			continue;
		}

		const FVector Location =
			EntityView.GetFragmentData<FTransformFragment>()
				.GetTransform()
				.GetLocation();
		const double DistanceSquared =
			FVector::DistSquared(Location, Squad.AnchorLocation);

		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			BestSoldierIndex = SoldierIndex;
			BestLocation = Location;
		}
	}

	if (BestSoldierIndex == INDEX_NONE)
	{
		ReformSquad(Squad);
		return;
	}

	const FVector OldSpatialCenter = Squad.GetSpatialCenter();
	Squad.AnchorLocation = BestLocation;
	Squad.ActiveMemberSoldierIndices.Add(BestSoldierIndex);

	const FMassEntityHandle BestEntity = SoldierEntities[BestSoldierIndex];
	FMassEntityView BestEntityView(EntityManager, BestEntity);
	FAlgonaSquadMemberFragment& BestMember =
		BestEntityView.GetFragmentData<FAlgonaSquadMemberFragment>();
	BestMember.FormationState = EAlgonaSoldierFormationState::Active;
	BestMember.SlotIndex = 0;

	FAlgonaSoldierMovementFragment& BestMovement =
		BestEntityView.GetFragmentData<FAlgonaSoldierMovementFragment>();
	BestMovement.DistanceState = EAlgonaSoldierDistanceState::Near;

	ReformSquad(Squad);
	SquadSpatialGrid.UpdateSquad(
		Squad.SquadId,
		OldSpatialCenter,
		Squad.GetSpatialCenter());
}
