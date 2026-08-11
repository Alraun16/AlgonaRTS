#pragma once

#include "AlgonaSimulationTypes.h"
#include "AI/Navigation/NavigationTypes.h"
#include "CoreMinimal.h"
#include "Mass/EntityHandle.h"
#include "Subsystems/WorldSubsystem.h"

#include "AlgonaSimulationSubsystem.generated.h"

struct FAlgonaSimulationMassRuntime;
class ANavigationData;
class UNavigationSystemV1;

enum class EAlgonaLocalMotionMode : uint8
{
	Legacy,
	MassStock
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FAlgonaDebugSimulationSpawnedDelegate,
	UWorld*
);

struct FAlgonaPendingPathRequest
{
	FSimGroupId GroupId;
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	uint64 Generation = 0;
	uint64 CommandSequence = 0;
	int32 Priority = 0;
};

struct FAlgonaInFlightPathRequest
{
	FSimGroupId GroupId;
	uint32 RequestId = 0;
	uint64 Generation = 0;
	uint64 CommandSequence = 0;
	double SubmitTimeSeconds = 0.0;
};

struct FAlgonaPathCompletion
{
	FSimGroupId GroupId;
	uint32 RequestId = 0;
	uint64 Generation = 0;
	uint64 CommandSequence = 0;
	bool bSuccess = false;
	bool bPartial = false;
	FString FailureReason;
	double LatencyMilliseconds = 0.0;
	TArray<FVector> Points;
};

struct FAlgonaSpatialGridEntry
{
	FVector Position = FVector::ZeroVector;
	FSimGroupId GroupId;
	FMassEntityHandle Entity;
	int32 EntityIndex = INDEX_NONE;
	float Radius = 40.0f;
};

struct FAlgonaSpatialGrid
{
	TMap<FIntPoint, TArray<int32>> Cells;
	TArray<FAlgonaSpatialGridEntry> Entries;
	double CellSize = 240.0;
};

struct FAlgonaGroupRestDiagnostics
{
	int32 MemberCount = 0;
	int32 SettledMembers = 0;
	int32 MovingBySeparationCount = 0;
	double MaxSlotError = 0.0;
	double AverageSlotError = 0.0;
	double MaxDesiredSpeed = 0.0;
	double AverageDesiredSpeed = 0.0;
	double MaxSeparationSpeed = 0.0;
	double AverageSeparationSpeed = 0.0;
};

struct FAlgonaHardBlocker
{
	int32 Id = INDEX_NONE;
	FVector Center = FVector::ZeroVector;
	double Radius = 0.0;
	uint64 Revision = 0;
};

UCLASS()
class ALGONASIMULATION_API UAlgonaSimulationSubsystem final
	: public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	static FAlgonaDebugSimulationSpawnedDelegate& OnDebugSimulationSpawned();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void BeginDestroy() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	uint64 GetSimulationTick() const { return SimulationTick; }
	double GetFixedDeltaSeconds() const { return FixedDeltaSeconds; }
	int32 GetSimulationEntityCount() const;
	int32 GetSimulationGroupCount() const { return Groups.Num(); }
	FAlgonaSimBenchmarkSnapshot GetBenchmarkSnapshot() const;

	bool RepopulateSimulationEntities(int32 EntityCount, int32 GroupSize);
	bool RepopulateSimulationRectGroups(
		int32 GroupCount,
		int32 Columns,
		int32 Rows);
	void DestroySimulationEntities();
	bool SubmitMoveGroupCommand(
		FSimGroupId GroupId,
		const FVector& TargetPosition,
		EAlgonaMoveFacingMode MoveFacingMode =
			EAlgonaMoveFacingMode::FaceMovement);
	int32 SubmitMoveAllGroupsCommand(
		const FVector& TargetPosition,
		EAlgonaMoveFacingMode MoveFacingMode =
			EAlgonaMoveFacingMode::FaceMovement);
	int32 ExportEntityPositions(TArray<FVector>& OutPositions, int32 MaxEntities);
	int32 ExportDebugPresentationEntities(
		TArray<FAlgonaDebugPresentationEntity>& OutEntities,
		int32 MaxEntities);
	bool FindNearestGroupAtPosition(
		const FVector& WorldPosition,
		double MaxDistance,
		FSimGroupId& OutGroupId,
		FVector* OutNearestPosition = nullptr) const;
	void DumpDebugState(FSimGroupId GroupId) const;
	void DumpNavigationState(FSimGroupId GroupId) const;
	void DumpFormationState(FSimGroupId GroupId, int32 MaxSamples) const;
	bool SetDebugLocalMotionMode(const FString& ModeName);
	void DumpLocalMotionState(FSimGroupId GroupId);
	int32 SetDebugSimLOD(EAlgonaSimLOD LOD, FSimGroupId GroupId);
	bool SetDebugGroupMinSpacing(FSimGroupId GroupId, double Spacing);
	bool SetDebugGroupCubeSize(FSimGroupId GroupId, double CubeSize);
	int32 AddDebugHardBlocker(const FVector& Center, double Radius);
	int32 AddDebugHardBlockerAtGroup(FSimGroupId GroupId, double Radius);
	int32 ClearDebugHardBlockers();
	void DumpDebugHardBlockers() const;
	void RunPopulationSmokeSuite(int32 GroupSize, int32 SimulationStepsPerSize);
	void RunCeilingSmoke(int32 EntityCount, int32 GroupSize, int32 SimulationSteps);
	void RunBenchmarkSuite(int32 GroupSize, int32 SimulationStepsPerCase);
	void RunLocalMotionSpikeSuite(int32 GroupSize, int32 SimulationStepsPerCase);

private:
	void HandleWorldBeginTearDown(UWorld* World);
	void RunFixedSimulationStep();
	void RefreshRuntimeConfiguration();
	void EnsureMassRuntime();
	bool EnsureEntityArchetype();
	void ProcessCommandQueue();
	void ProcessPathScheduler();
	void ProcessPathCompletions();
	void UpdateGroupRouteProgress();
	void UpdateGroupFormations();
	void BuildSpatialGrid();
	void ProcessMovement();
	void ProcessMassStockMovement();
	void SynchronizeMassStockTransformsAndObstacleGrid();
	void UpdateStuckRecovery();
	void RefreshDerivedGroupSimLOD(FAlgonaSimGroupState& Group);
	void RefreshSimLODStatistics();
	void ResetSimLODMeasurement();
	void AccumulateSimLODMeasurement();
	void ResetSimulationState();
	bool IsAuthoritativeSimulationWorld() const;
	FAlgonaSimGroupState* FindGroup(FSimGroupId GroupId);
	const FAlgonaSimGroupState* FindGroup(FSimGroupId GroupId) const;
	bool IsNavigationMovementReady(const FAlgonaSimGroupState& Group) const;
	FVector ComputeGroupCenter(FSimGroupId GroupId) const;
	FVector GetGroupMovementTarget(
		const FAlgonaSimGroupState& Group,
		const FAlgonaSimNavigationFragment& NavigationFragment) const;
	FVector ComputeFormationSlotOffset(
		const FAlgonaFormationState& Formation,
		int32 SlotIndex) const;
	FVector ComputeLocalFormationTarget(
		const FAlgonaSimGroupState& Group,
		const FVector& Position,
		const FAlgonaSimLODFragment& LOD,
		FAlgonaSimNavigationFragment& NavigationFragment) const;
	FAlgonaFormationCorridorSample SampleFormationCorridor(
		const FAlgonaFormationState& Formation,
		double LongitudinalOffset) const;
	bool UpdateFormationSafeAnchor(
		FAlgonaSimGroupState& Group,
		const FVector& ProposedAnchor,
		int32 ElapsedFormationTicks);
	FIntPoint GetSpatialGridCell(const FVector& Position) const;
	FVector ComputeSeparationVelocity(
		const FMassEntityHandle& Entity,
		FSimGroupId GroupId,
		const FVector& Position,
		float Radius,
		int32& OutNeighborCandidates) const;
	FAlgonaGroupRestDiagnostics CollectGroupRestDiagnostics(
		FSimGroupId GroupId) const;
	bool IsSegmentBlockedByHardBlockers(
		const FVector& Start,
		const FVector& End,
		const FAlgonaHardBlocker** OutBlocker = nullptr) const;
	bool IsRouteBlockedByHardBlockers(
		const FAlgonaGroupRoute& Route,
		const FAlgonaHardBlocker** OutBlocker = nullptr) const;
	FVector ConstrainMovementAgainstHardBlockers(
		const FVector& CurrentPosition,
		const FVector& ProposedPosition,
		float Radius,
		bool& bOutBlocked) const;
	FVector ConstrainMovementToFormationCorridor(
		const FAlgonaSimGroupState& Group,
		const FVector& CurrentPosition,
		const FVector& ProposedPosition,
		float AgentRadius,
		bool& bOutConstrained,
		int32& OutConstraintCount,
		double& OutSafetyCorrectionDistance,
		double& OutPenetrationDepth,
		double& OutNormalMotionClipped) const;
	void QueueRepathForGroup(
		FAlgonaSimGroupState& Group,
		const TCHAR* Reason,
		int32 Priority);
	int32 InvalidateRoutesForHardBlockers(const TCHAR* Reason);
	void ConfigureFormationForGroup(FAlgonaSimGroupState& Group);
	void UpdateFormationCorridorConstraint(
		FAlgonaSimGroupState& Group,
		const FVector& GroupCenter);
	void QueuePathRequestForGroup(FAlgonaSimGroupState& Group);
	void CancelPathRequestForGroup(FAlgonaSimGroupState& Group);
	void AbortAllPathRequests();
	ANavigationData* ResolveNavigationData(
		UNavigationSystemV1* NavigationSystem,
		const FNavAgentProperties& AgentProperties,
		const FVector& AgentLocation,
		const FVector& ProjectionExtent) const;
	void OnAsyncPathResult(
		uint32 RequestId,
		ENavigationQueryResult::Type Result,
		FNavPathSharedPtr Path);

	double AccumulatorSeconds = 0.0;
	double FixedDeltaSeconds = 0.05;
	uint64 SimulationTick = 0;
	uint64 NextCommandSequence = 1;
	int32 MaxSimulationStepsPerFrame = 4;
	double MovementSpeed = 300.0;
	bool bMovementEnabled = true;
	bool bSimulationLODEnabled = true;
	bool bCommandDiagnosticsEnabled = true;
	bool bP1NavigationEnabled = true;
	bool bP1FormationEnabled = true;
	int32 MaxPathRequestsSubmittedPerStep = 4;
	double RoutePointAcceptanceRadius = 150.0;
	double NavProjectionExtent = 500.0;
	double NavProjectionExtentZ = 10000.0;
	double FormationSlotSpacing = 120.0;
	int32 FormationMaxColumns = 20;
	double FormationCorridorProbeHalfWidth = 2000.0;
	double FormationCorridorSafetyMargin = 80.0;
	int32 FormationCorridorSampleCount = 7;
	double FormationAnchorLookAheadDistance = 600.0;
	double FormationFunnelLookAheadDistance = 600.0;
	double FormationFunnelLateralSpeedFraction = 0.8;
	double FormationMinimumForwardSpeedScale = 0.15;
	double FormationForwardSpeedInterpSpeed = 6.0;
	double FormationOrientationInterpSpeed = 8.0;
	double FormationMinSameGroupSpacing = 60.0;
	double FormationLocalDeformationInterpSpeed = 6.0;
	double FormationSettlingRadius = 75.0;
	int32 FormationSettlingRequiredTicks = 8;
	bool bP1SpatialGridEnabled = true;
	bool bP1LocalSeparationEnabled = true;
	double SpatialGridCellSize = 240.0;
	double NeighborQueryRadius = 180.0;
	int32 NeighborMaxCandidates = 32;
	double SeparationStrength = 220.0;
	double SeparationMinimumPadding = 0.0;
	double DesiredVelocityBlend = 0.35;
	bool bP1HardBlockersEnabled = true;
	bool bP1StuckDetectionEnabled = true;
	EAlgonaLocalMotionMode RequestedLocalMotionMode =
		EAlgonaLocalMotionMode::Legacy;
	EAlgonaLocalMotionMode ActiveLocalMotionMode =
		EAlgonaLocalMotionMode::Legacy;
	double StuckDistanceEpsilon = 150.0;
	int32 StuckObservationTicks = 60;
	int32 StuckRequiredTicks = 180;
	int32 RepathCooldownTicks = 120;

	TArray<FAlgonaSimGroupState> Groups;
	TArray<FAlgonaSimCommand> PendingCommands;
	TArray<FAlgonaPendingPathRequest> QueuedPathRequests;
	TArray<FAlgonaInFlightPathRequest> InFlightPathRequests;
	TArray<FAlgonaPathCompletion> PendingPathCompletions;
	TArray<FAlgonaHardBlocker> HardBlockers;
	FAlgonaSpatialGrid SpatialGrid;
	FAlgonaSimBenchmarkSnapshot BenchmarkSnapshot;
	FAlgonaSimulationMassRuntime* MassRuntime = nullptr;
	FSimGroupId LocalMotionForceCaptureGroupId;
	bool bCaptureLocalMotionForcesNextStep = false;
	bool bWorldTeardownHandled = false;
	int32 NextHardBlockerId = 1;
	uint64 HardBlockerRevision = 0;
};
