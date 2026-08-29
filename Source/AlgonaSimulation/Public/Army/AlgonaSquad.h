#pragma once

// Formation geometry is authoritative Simulation data, not Presentation data.
#include "Army/AlgonaFormation.h"
#include "CoreMinimal.h"

/** Current commanded movement pace of the squad center. */
enum class EAlgonaSquadMovePace : uint8
{
	Walk,
	Run
};

/** Per-step formation cohesion counters accumulated in the soldier Mass pass. */
struct FAlgonaSquadDistanceCounters
{
	void Reset()
	{
		Near = 0;
		Medium = 0;
		Far = 0;
	}

	int32 Near = 0;
	int32 Medium = 0;
	int32 Far = 0;
};

/** Small renderer-neutral view used only while composing an RTS move command. */
struct ALGONASIMULATION_API FAlgonaSquadCommandPreview
{
	FVector Center = FVector::ZeroVector;
	int32 ActiveUnitCount = 0;
	int32 CurrentMaxSlotsPerRow = 0;
	float SlotSpacingCm = 150.0f;
};

/**
 * Authoritative state of one RTS squad.
 * The squad owns formation geometry and rare squad-level state; soldiers keep
 * only their current membership/slot state in Mass fragments.
 */
struct ALGONASIMULATION_API FAlgonaSquad
{
	int32 GetFormationCapacity() const
	{
		return Formation.Num();
	}

	/** P1 spatial consumers now use the actual geometric formation center. */
	FVector GetSpatialCenter() const
	{
		return AnchorLocation;
	}

	float GetCommandedBaseSpeed() const
	{
		return MovePace == EAlgonaSquadMovePace::Walk
			? WalkSpeedCmPerSecond
			: RunSpeedCmPerSecond;
	}

	float GetCenterMoveSpeed() const
	{
		return bWaitingForStragglers || bCenterAtDestination
			? 0.0f
			: GetCommandedBaseSpeed() * CohesionMoveScale;
	}

	float GetActiveSoldierSpeedLimit() const
	{
		// A stopped center must not make soldiers unable to catch their slots.
		// Run 5 m/s therefore gives an exact 6 m/s active-soldier cap.
		return GetCommandedBaseSpeed() * 1.2f;
	}

	float GetLostSoldierReturnSpeedLimit() const
	{
		// Lost soldiers ignore walk/stop orders and return at running pace, but
		// still respect the same absolute 6 m/s cap at the default run speed.
		return RunSpeedCmPerSecond * 1.2f;
	}

	int32 SquadId = INDEX_NONE;
	int32 TotalMemberCount = 0;

	// The position in this compact array is the soldier's current slot index.
	// Removing an active member is cheap; Reform rewrites only this squad.
	TArray<int32> ActiveMemberSoldierIndices;

	FAlgonaFormationLayout Formation;
	int32 RequestedMaxSlotsPerRow = 0;
	float SoldierSpacingCm = 150.0f;

	// Held-RMB width changes are delayed until the final 15 m of the accepted
	// route. A newer accepted command cancels an old reform that has not begun.
	int32 PendingReformMaxSlotsPerRow = 0;
	bool bReformPending = false;
	bool bReformInProgress = false;
	int32 ReformUnsettledActiveCount = 0;

	// Anchor is always the geometric center of the current formation.
	FVector AnchorLocation = FVector::ZeroVector;
	FVector FacingDirection = FVector::ForwardVector;

	// One remembered squad path. Individual soldiers do not request paths.
	TArray<FVector> RoutePoints;
	int32 NextRoutePointIndex = INDEX_NONE;
	FVector FinalTargetLocation = FVector::ZeroVector;

	// A drag-RMB command can request a final facing independently from the path.
	// The formation may logically mirror instantly on a >90 degree turn, while
	// individual soldier bodies still rotate smoothly in their Mass transforms.
	bool bHasExplicitFinalFacing = false;
	bool bFinalFacingPending = false;
	FVector FinalFacingDirection = FVector::ForwardVector;

	// Every accepted move increments this lightweight revision. Soldiers derive
	// deterministic distance-based travel drift from real distance travelled, not
	// route percentage or NavMesh waypoint count. Their current offset persists
	// after the command and becomes the exact starting offset of the next one.
	uint32 MoveCommandRevision = 0;
	double CommandRouteLengthCm = 0.0;
	double CommandTravelDistanceCm = 0.0;

	float WalkSpeedCmPerSecond = 300.0f;
	float RunSpeedCmPerSecond = 500.0f;
	EAlgonaSquadMovePace MovePace = EAlgonaSquadMovePace::Run;

	bool bHasMoveTarget = false;
	bool bCenterAtDestination = false;

	// Cohesion slowdown/waiting state from the movement algorithm.
	float CohesionMoveScale = 1.0f;
	bool bWaitingForStragglers = false;
	float StragglerWaitRemainingSeconds = 0.0f;
	FAlgonaSquadDistanceCounters DistanceCounters;

	// Once the center reaches the exact target, soldiers get up to five seconds
	// to settle. The center never slides to rescue an unreachable final slot.
	float FinalAssemblyRemainingSeconds = 0.0f;
};
