#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityFragments.h"

#include "AlgonaSimulationTypes.generated.h"

USTRUCT(BlueprintType)
struct ALGONASIMULATION_API FSimGroupId
{
	GENERATED_BODY()

	FSimGroupId() = default;
	explicit FSimGroupId(const int32 InValue)
		: Value(InValue)
	{
	}

	bool IsValid() const { return Value > 0; }

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Algona Simulation")
	int32 Value = INDEX_NONE;
};

FORCEINLINE bool operator==(const FSimGroupId& Left, const FSimGroupId& Right)
{
	return Left.Value == Right.Value;
}

FORCEINLINE uint32 GetTypeHash(const FSimGroupId& GroupId)
{
	return GetTypeHash(GroupId.Value);
}

UENUM(BlueprintType)
enum class EAlgonaSimEntityState : uint8
{
	Idle,
	Moving
};

UENUM(BlueprintType)
enum class EAlgonaSimLOD : uint8
{
	High,
	Medium,
	Low
};

UENUM(BlueprintType)
enum class EAlgonaSimCommandType : uint8
{
	MoveGroupToTarget
};

UENUM(BlueprintType)
enum class EAlgonaMoveFacingMode : uint8
{
	FaceMovement,
	PreserveFacing
};

UENUM(BlueprintType)
enum class EAlgonaGroupNavigationStatus : uint8
{
	Idle,
	Queued,
	Requesting,
	WaitingForAsyncResult,
	RouteReady,
	Settling,
	Failed
};

UENUM(BlueprintType)
enum class EAlgonaFormationMemberPhase : uint8
{
	Normal,
	Funneling,
	InsideChoke,
	ReExpanding
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimPositionFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Position = FVector::ZeroVector;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimVelocityFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Velocity = FVector::ZeroVector;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimGroupFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	int32 GroupId = INDEX_NONE;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimStateFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	EAlgonaSimEntityState State = EAlgonaSimEntityState::Idle;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimLODFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	EAlgonaSimLOD LOD = EAlgonaSimLOD::High;

	UPROPERTY()
	uint8 UpdateStride = 1;

	UPROPERTY()
	uint8 TickOffset = 0;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimNavigationFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	int32 SlotIndex = INDEX_NONE;

	UPROPERTY()
	int32 OriginalSlotRow = INDEX_NONE;

	UPROPERTY()
	int32 OriginalSlotColumn = INDEX_NONE;

	UPROPERTY()
	FVector DesiredSlotPosition = FVector::ZeroVector;

	UPROPERTY()
	FVector DesiredVelocity = FVector::ZeroVector;

	UPROPERTY()
	float Radius = 40.0f;

	UPROPERTY()
	float LocalLateralOffset = 0.0f;

	UPROPERTY()
	float LastLocalSpacing = 0.0f;

	UPROPERTY()
	float LastLocalCorridorWidth = 0.0f;

	UPROPERTY()
	float MemberLongitudinalOffset = 0.0f;

	UPROPERTY()
	float CorridorConstraintLongitudinalOffset = 0.0f;

	UPROPERTY()
	float DistanceToNarrowBoundary = 0.0f;

	UPROPERTY()
	float RequiredLateralDistance = 0.0f;

	UPROPERTY()
	float RequiredLateralSpeed = 0.0f;

	UPROPERTY()
	float RecommendedForwardSpeedScale = 1.0f;

	UPROPERTY()
	EAlgonaFormationMemberPhase FormationPhase =
		EAlgonaFormationMemberPhase::Normal;

	UPROPERTY()
	bool bSoftOverlap = false;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaFormationCorridorSample
{
	GENERATED_BODY()

	UPROPERTY()
	float LongitudinalOffset = 0.0f;

	UPROPERTY()
	FVector Center = FVector::ZeroVector;

	UPROPERTY()
	float LeftHalfWidth = 0.0f;

	UPROPERTY()
	float RightHalfWidth = 0.0f;

	UPROPERTY()
	float Width = 0.0f;

	UPROPERTY()
	bool bValid = false;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaGroupRoute
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FVector> Points;

	UPROPERTY()
	int32 CurrentPointIndex = 0;

	UPROPERTY()
	FVector CurrentMoveTarget = FVector::ZeroVector;

	UPROPERTY()
	bool bPartial = false;

	UPROPERTY()
	bool bValid = false;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaGroupNavigationState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Destination = FVector::ZeroVector;

	UPROPERTY()
	EAlgonaGroupNavigationStatus Status =
		EAlgonaGroupNavigationStatus::Idle;

	UPROPERTY()
	uint64 RouteGeneration = 0;

	UPROPERTY()
	FAlgonaGroupRoute Route;

	UPROPERTY()
	uint32 PendingRequestId = 0;

	UPROPERTY()
	uint64 PendingRequestGeneration = 0;

	UPROPERTY()
	FString LastFailureReason;

	UPROPERTY()
	FString LastNavigationDataName;

	UPROPERTY()
	FVector LastPathStart = FVector::ZeroVector;

	UPROPERTY()
	FVector LastPathDestination = FVector::ZeroVector;

	UPROPERTY()
	FVector LastProjectionExtent = FVector::ZeroVector;

	UPROPERTY()
	FVector LastProjectedStart = FVector::ZeroVector;

	UPROPERTY()
	FVector LastProjectedDestination = FVector::ZeroVector;

	UPROPERTY()
	bool bLastStartProjectionSucceeded = false;

	UPROPERTY()
	bool bLastDestinationProjectionSucceeded = false;

	UPROPERTY()
	uint64 LastRouteCommitTick = 0;

	UPROPERTY()
	FString LastRepathReason;

	UPROPERTY()
	FVector LastProgressCenter = FVector::ZeroVector;

	UPROPERTY()
	FVector LastProgressAnchor = FVector::ZeroVector;

	UPROPERTY()
	double LastProgressDistanceToDestination = 0.0;

	UPROPERTY()
	int32 LastProgressRoutePointIndex = INDEX_NONE;

	UPROPERTY()
	uint64 LastProgressSampleTick = 0;

	UPROPERTY()
	int32 StuckTicks = 0;

	UPROPERTY()
	uint64 LastRepathTick = 0;

	UPROPERTY()
	double CommandStartTimeSeconds = -1.0;

	UPROPERTY()
	double BridgeEntryTimeSeconds = -1.0;

	UPROPERTY()
	double BridgeExitTimeSeconds = -1.0;

	UPROPERTY()
	double RouteArrivalTimeSeconds = -1.0;

	UPROPERTY()
	double IdleTimeSeconds = -1.0;

	UPROPERTY()
	double RouteLength = 0.0;

	UPROPERTY()
	bool bWasInsideChoke = false;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaFormationState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector AnchorPosition = FVector::ZeroVector;

	UPROPERTY()
	FVector TravelDirection = FVector::ForwardVector;

	UPROPERTY()
	FVector LayoutForward = FVector::ForwardVector;

	UPROPERTY()
	FVector LayoutRight = FVector::RightVector;

	UPROPERTY()
	FVector FacingDirection = FVector::ForwardVector;

	UPROPERTY()
	float SlotSpacing = 100.0f;

	UPROPERTY()
	int32 OriginalColumns = 1;

	UPROPERTY()
	int32 OriginalRows = 1;

	UPROPERTY()
	int32 SlotCount = 0;

	UPROPERTY()
	float Width = 0.0f;

	UPROPERTY()
	float Depth = 0.0f;

	UPROPERTY()
	float CorridorHalfWidth = 0.0f;

	UPROPERTY()
	float CorridorLeftHalfWidth = 0.0f;

	UPROPERTY()
	float CorridorRightHalfWidth = 0.0f;

	UPROPERTY()
	float CorridorWidth = 0.0f;

	UPROPERTY()
	float RequiredFootprintHalfWidth = 0.0f;

	UPROPERTY()
	float SafeAnchorLateralOffset = 0.0f;

	UPROPERTY()
	float CorridorProfileFrontOffset = 0.0f;

	UPROPERTY()
	float CorridorProfileRearOffset = 0.0f;

	UPROPERTY()
	float ForwardSpeedScale = 1.0f;

	UPROPERTY()
	float MaxRequiredLateralSpeed = 0.0f;

	UPROPERTY()
	int32 SpeedLimitedMembers = 0;

	UPROPERTY()
	int32 CorridorValidSamples = 0;

	UPROPERTY()
	TArray<FAlgonaFormationCorridorSample> CorridorSamples;

	UPROPERTY()
	FVector LeftCorridorEdge = FVector::ZeroVector;

	UPROPERTY()
	FVector RightCorridorEdge = FVector::ZeroVector;

	UPROPERTY()
	bool bLocallyDeformed = false;

	UPROPERTY()
	EAlgonaFormationMemberPhase Phase =
		EAlgonaFormationMemberPhase::Normal;

	UPROPERTY()
	int32 MembersFunneling = 0;

	UPROPERTY()
	int32 MembersInsideChoke = 0;

	UPROPERTY()
	int32 MembersReExpanding = 0;

	UPROPERTY()
	int32 SoftOverlapMembers = 0;

	UPROPERTY()
	int32 SettledTicks = 0;

	UPROPERTY()
	uint64 LastUpdateTick = 0;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimGroupState
{
	GENERATED_BODY()

	UPROPERTY()
	FSimGroupId Id;

	UPROPERTY()
	FVector TargetPosition = FVector::ZeroVector;

	UPROPERTY()
	EAlgonaSimCommandType CurrentCommand = EAlgonaSimCommandType::MoveGroupToTarget;

	UPROPERTY()
	EAlgonaMoveFacingMode MoveFacingMode = EAlgonaMoveFacingMode::FaceMovement;

	UPROPERTY()
	int32 MemberCount = 0;

	UPROPERTY()
	uint64 CommandSequence = 0;

	UPROPERTY()
	bool bHasActiveMoveTarget = false;

	UPROPERTY()
	EAlgonaSimLOD NavigationLOD = EAlgonaSimLOD::High;

	UPROPERTY()
	uint8 NavigationUpdateStride = 1;

	UPROPERTY()
	uint8 NavigationTickOffset = 0;

	UPROPERTY()
	int32 HighLODMemberCount = 0;

	UPROPERTY()
	int32 MediumLODMemberCount = 0;

	UPROPERTY()
	int32 LowLODMemberCount = 0;

	UPROPERTY()
	float MinSameGroupSpacing = 60.0f;

	UPROPERTY()
	float DebugPresentationCubeSize = 25.0f;

	UPROPERTY()
	int32 DebugFormationColumns = 0;

	UPROPERTY()
	int32 DebugFormationRows = 0;

	UPROPERTY()
	FAlgonaGroupNavigationState Navigation;

	UPROPERTY()
	FAlgonaFormationState Formation;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimCommand
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 Sequence = 0;

	UPROPERTY()
	uint64 ApplyAtSimulationTick = 0;

	UPROPERTY()
	EAlgonaSimCommandType Type = EAlgonaSimCommandType::MoveGroupToTarget;

	UPROPERTY()
	EAlgonaMoveFacingMode MoveFacingMode = EAlgonaMoveFacingMode::FaceMovement;

	UPROPERTY()
	FSimGroupId GroupId;

	UPROPERTY()
	FVector TargetPosition = FVector::ZeroVector;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaDebugPresentationEntity
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Position = FVector::ZeroVector;

	UPROPERTY()
	int32 GroupId = INDEX_NONE;

	UPROPERTY()
	float CubeSize = 25.0f;

	UPROPERTY()
	FVector FacingDirection = FVector::ForwardVector;
};

USTRUCT()
struct ALGONASIMULATION_API FAlgonaSimBenchmarkSnapshot
{
	GENERATED_BODY()

	UPROPERTY()
	int32 EntityCount = 0;

	UPROPERTY()
	int32 GroupCount = 0;

	UPROPERTY()
	uint64 SimulationTick = 0;

	UPROPERTY()
	double LastSpawnMilliseconds = 0.0;

	UPROPERTY()
	double LastDestroyMilliseconds = 0.0;

	UPROPERTY()
	int64 LastSpawnUsedPhysicalDeltaBytes = 0;

	UPROPERTY()
	int64 LastDestroyUsedPhysicalDeltaBytes = 0;

	UPROPERTY()
	uint64 LastUsedPhysicalBytes = 0;

	UPROPERTY()
	double LastMovementMilliseconds = 0.0;

	UPROPERTY()
	double LastFixedStepMilliseconds = 0.0;

	UPROPERTY()
	bool bLastFixedStepMissedDeadline = false;

	UPROPERTY()
	int32 LastMovementEntityVisits = 0;

	UPROPERTY()
	int32 LastMovementUpdatedEntities = 0;

	UPROPERTY()
	int32 LastMovementSkippedByLOD = 0;

	UPROPERTY()
	uint64 LastSubmittedCommandSequence = 0;

	UPROPERTY()
	uint64 LastAppliedCommandSequence = 0;

	UPROPERTY()
	int32 LastAppliedCommandGroupId = INDEX_NONE;

	UPROPERTY()
	int32 PendingCommandCount = 0;

	UPROPERTY()
	int32 QueuedPathRequestCount = 0;

	UPROPERTY()
	int32 InFlightPathRequestCount = 0;

	UPROPERTY()
	int32 PendingPathCompletionCount = 0;

	UPROPERTY()
	uint64 SubmittedPathRequests = 0;

	UPROPERTY()
	uint64 CompletedPathRequests = 0;

	UPROPERTY()
	uint64 FailedPathRequests = 0;

	UPROPERTY()
	uint64 RejectedStalePathResults = 0;

	UPROPERTY()
	double LastPathSchedulerMilliseconds = 0.0;

	UPROPERTY()
	double LastRouteProgressMilliseconds = 0.0;

	UPROPERTY()
	double LastFormationMilliseconds = 0.0;

	UPROPERTY()
	int32 LastFormationUpdatedGroups = 0;

	UPROPERTY()
	int32 LastFormationSkippedGroupsByLOD = 0;

	UPROPERTY()
	int32 LastFormationTargetUpdatedEntities = 0;

	UPROPERTY()
	int32 LastFormationTargetSkippedEntitiesByLOD = 0;

	UPROPERTY()
	int32 HighLODEntityCount = 0;

	UPROPERTY()
	int32 MediumLODEntityCount = 0;

	UPROPERTY()
	int32 LowLODEntityCount = 0;

	UPROPERTY()
	int32 HighLODGroupCount = 0;

	UPROPERTY()
	int32 MediumLODGroupCount = 0;

	UPROPERTY()
	int32 LowLODGroupCount = 0;

	UPROPERTY()
	double LastSpatialGridBuildMilliseconds = 0.0;

	UPROPERTY()
	int32 LastSpatialGridEntityCount = 0;

	UPROPERTY()
	int32 LastSpatialGridCellCount = 0;

	UPROPERTY()
	double LastNeighborQueryMilliseconds = 0.0;

	UPROPERTY()
	int32 LastNeighborQueryCount = 0;

	UPROPERTY()
	int32 LastNeighborQueriesSkippedByLOD = 0;

	UPROPERTY()
	int32 LastHighLODNeighborQueryCount = 0;

	UPROPERTY()
	int32 LastMediumLODNeighborQueryCount = 0;

	UPROPERTY()
	int32 LastLowLODNeighborQueryCount = 0;

	UPROPERTY()
	int32 LastNeighborCandidateCount = 0;

	UPROPERTY()
	int32 LastNeighborMaxCandidates = 0;

	UPROPERTY()
	double LastNeighborAverageCandidates = 0.0;

	UPROPERTY()
	double LastAvoidanceMilliseconds = 0.0;

	UPROPERTY()
	int32 LastAvoidanceAppliedCount = 0;

	UPROPERTY()
	uint64 SimLODMeasurementStartTick = 0;

	UPROPERTY()
	uint64 SimLODMeasurementStepCount = 0;

	UPROPERTY()
	double SimLODMeasurementFormationMilliseconds = 0.0;

	UPROPERTY()
	double SimLODMeasurementNeighborMilliseconds = 0.0;

	UPROPERTY()
	double SimLODMeasurementAvoidanceMilliseconds = 0.0;

	UPROPERTY()
	double SimLODMeasurementMovementMilliseconds = 0.0;

	UPROPERTY()
	uint64 SimLODMeasurementFormationUpdatedGroups = 0;

	UPROPERTY()
	uint64 SimLODMeasurementFormationSkippedGroups = 0;

	UPROPERTY()
	uint64 SimLODMeasurementFormationTargetUpdatedEntities = 0;

	UPROPERTY()
	uint64 SimLODMeasurementFormationTargetSkippedEntities = 0;

	UPROPERTY()
	uint64 SimLODMeasurementNeighborQueries = 0;

	UPROPERTY()
	uint64 SimLODMeasurementNeighborQueriesSkipped = 0;

	UPROPERTY()
	int32 ActiveHardBlockerCount = 0;

	UPROPERTY()
	int32 LastRouteInvalidationCount = 0;

	UPROPERTY()
	uint64 TotalRepathsRequested = 0;

	UPROPERTY()
	int32 LastStuckGroupCount = 0;

	UPROPERTY()
	int32 LastHardBlockerMovementBlockedEntities = 0;

	UPROPERTY()
	uint64 TotalHardBlockerMovementBlockedEntities = 0;
};
