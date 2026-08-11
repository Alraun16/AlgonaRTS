#include "AlgonaSimulationSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "Mass/EntityFragments.h"
#include "MassArchetypeTypes.h"
#include "MassCommonFragments.h"
#include "MassEntityManager.h"
#include "MassEntityQuery.h"
#include "MassEntitySubsystem.h"
#include "MassEntityView.h"
#include "MassExecutionContext.h"
#include "MassExecutor.h"
#include "MassMovementFragments.h"
#include "MassNavigationFragments.h"
#include "MassNavigationProcessors.h"
#include "MassNavigationSubsystem.h"
#include "MassProcessingContext.h"
#include "MassProcessingTypes.h"
#include "Avoidance/MassAvoidanceFragments.h"
#include "Avoidance/MassAvoidanceProcessors.h"
#include "Movement/MassMovementProcessors.h"
#include "Steering/MassSteeringFragments.h"
#include "Steering/MassSteeringProcessors.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Math/RotationMatrix.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Stats/Stats.h"
#include "Subsystems/SubsystemCollection.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaSimulation, Log, All);

struct FAlgonaLocalMotionDiagnostics
{
	int32 EntitiesProcessed = 0;
	int32 MovingEntities = 0;
	int32 MoveTargets = 0;
	int32 AvoidanceEntities = 0;
	int32 AvoidanceEdges = 0;
	int32 CorridorClampedEntities = 0;
	int32 CorridorConstraintCount = 0;
	double AverageSpeed = 0.0;
	double MaxSpeed = 0.0;
	double AverageForwardSpeed = 0.0;
	double MinForwardSpeed = 0.0;
	double AverageDesiredSpeed = 0.0;
	double AverageAgentRadius = 0.0;
	double AverageSlotError = 0.0;
	double MaxSlotError = 0.0;
	double AverageConstraintCorrection = 0.0;
	double MaxConstraintCorrection = 0.0;
	double MaxPenetrationDepth = 0.0;
	double AverageNormalMotionClipped = 0.0;
	double MaxNormalMotionClipped = 0.0;
	double MovementMilliseconds = 0.0;
	double SteeringMilliseconds = 0.0;
	double AvoidanceMilliseconds = 0.0;
	double BoundaryAdapterMilliseconds = 0.0;
};

struct FAlgonaLocalMotionForceDiagnostics
{
	FSimGroupId GroupId;
	int32 SampleCount = 0;
	double AverageSteeringForce = 0.0;
	double MaxSteeringForce = 0.0;
	double AverageMovingAvoidanceForce = 0.0;
	double MaxMovingAvoidanceForce = 0.0;
	double AverageEnvironmentAvoidanceForce = 0.0;
	double MaxEnvironmentAvoidanceForce = 0.0;
	bool bValid = false;
};

struct FAlgonaSimulationMassRuntime
{
	TWeakObjectPtr<UMassEntitySubsystem> MassSubsystem;
	FMassEntityManager* EntityManager = nullptr;
	FMassArchetypeHandle EntityArchetype;
	FMassArchetypeHandle LegacyEntityArchetype;
	FMassArchetypeHandle MassStockEntityArchetype;
	FMassArchetypeSharedFragmentValues MassStockSharedFragments;
	TUniquePtr<FMassEntityQuery> MovementQuery;
	TUniquePtr<FMassEntityQuery> MassStockQuery;
	FMassRuntimePipeline ObstacleGridPipeline;
	FMassRuntimePipeline SteeringPipeline;
	FMassRuntimePipeline AvoidancePipeline;
	FMassRuntimePipeline ApplyForcePipeline;
	TArray<TStrongObjectPtr<UMassProcessor>> StockProcessorReferences;
	FAlgonaLocalMotionDiagnostics LocalMotionDiagnostics;
	FAlgonaLocalMotionForceDiagnostics LocalMotionForceDiagnostics;
	bool bStockPipelinesInitialized = false;
	TArray<FMassEntityHandle> Entities;
};

namespace
{
const TCHAR* ToLocalMotionModeString(EAlgonaLocalMotionMode Mode)
{
	return Mode == EAlgonaLocalMotionMode::MassStock
		? TEXT("MassStock")
		: TEXT("Legacy");
}

TAutoConsoleVariable<float> CVarAlgonaSimulationRateHz(
	TEXT("algona.Simulation.RateHz"),
	20.0f,
	TEXT("Fixed simulation rate in Hz for the Algona P0 simulation prototype."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaSimulationMaxStepsPerFrame(
	TEXT("algona.Simulation.MaxStepsPerFrame"),
	4,
	TEXT("Maximum fixed simulation steps executed by AlgonaSimulation in one frame."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP0AutoPopulate(
	TEXT("algona.P0.AutoPopulate"),
	0,
	TEXT("When non-zero, populate P0 Mass entities when the runtime world begins play."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP0EntityCount(
	TEXT("algona.P0.EntityCount"),
	0,
	TEXT("P0 Mass entity count used by AutoPopulate and console spawn commands."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP0GroupSize(
	TEXT("algona.P0.GroupSize"),
	100,
	TEXT("Number of P0 Mass entities assigned to one simulation group."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP0InitialSpacing(
	TEXT("algona.P0.InitialSpacing"),
	100.0f,
	TEXT("Grid spacing for P0 Mass entity population."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP0MovementSpeed(
	TEXT("algona.P0.MovementSpeed"),
	300.0f,
	TEXT("Straight-line movement speed for the P0 Mass movement prototype."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP0MovementEnabled(
	TEXT("algona.P0.MovementEnabled"),
	1,
	TEXT("Enables P0 fixed-step Mass movement processing."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP0LODEnabled(
	TEXT("algona.P0.LODEnabled"),
	1,
	TEXT("Enables P0 simulation LOD update stride processing."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1NavigationEnabled(
	TEXT("algona.P1.NavigationEnabled"),
	1,
	TEXT("Enables P1 group-level async NavMesh routes as the movement target source."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1PathSubmitBudget(
	TEXT("algona.P1.PathSubmitBudget"),
	4,
	TEXT("Maximum group NavMesh path requests submitted per fixed simulation step."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1RouteAcceptanceRadius(
	TEXT("algona.P1.RouteAcceptanceRadius"),
	150.0f,
	TEXT("Group-center distance used to advance to the next compact route point."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1NavProjectionExtent(
	TEXT("algona.P1.NavProjectionExtent"),
	500.0f,
	TEXT("XY extent used when projecting group path endpoints onto the NavMesh."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1NavProjectionExtentZ(
	TEXT("algona.P1.NavProjectionExtentZ"),
	10000.0f,
	TEXT("Z extent used when projecting group path endpoints onto the NavMesh."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1FormationEnabled(
	TEXT("algona.P1.FormationEnabled"),
	1,
	TEXT("Enables P1 route-driven formation slot targets."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationSlotSpacing(
	TEXT("algona.P1.FormationSlotSpacing"),
	120.0f,
	TEXT("Spacing between P1 prototype formation slots."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1FormationMaxColumns(
	TEXT("algona.P1.FormationMaxColumns"),
	20,
	TEXT("Maximum columns used by the P1 prototype rectangular formation layout."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationCorridorProbeHalfWidth(
	TEXT("algona.P1.FormationCorridorProbeHalfWidth"),
	2000.0f,
	TEXT("Half-width used by group-level P1 formation corridor raycasts."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationCorridorSafetyMargin(
	TEXT("algona.P1.FormationCorridorSafetyMargin"),
	80.0f,
	TEXT("Safety margin subtracted from group-level P1 formation corridor width."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1FormationCorridorSampleCount(
	TEXT("algona.P1.FormationCorridorSampleCount"),
	7,
	TEXT("Minimum group-level lateral sample count for the adaptive P1 corridor profile."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationAnchorLookAheadDistance(
	TEXT("algona.P1.FormationAnchorLookAheadDistance"),
	600.0f,
	TEXT("Distance ahead of the group center used for the moving P1 formation anchor."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationFunnelLookAheadDistance(
	TEXT("algona.P1.FormationFunnelLookAheadDistance"),
	600.0f,
	TEXT("Member-local look-ahead distance used to begin P1 lateral funnel convergence before a narrowing."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationFunnelLateralSpeedFraction(
	TEXT("algona.P1.FormationFunnelLateralSpeedFraction"),
	0.8f,
	TEXT("Fraction of movement speed treated as available lateral convergence speed for the P1 funnel."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationMinimumForwardSpeedScale(
	TEXT("algona.P1.FormationMinimumForwardSpeedScale"),
	0.15f,
	TEXT("Minimum Legacy forward speed scale while P1 members converge laterally; MassStock keeps this recommendation diagnostic-only."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationForwardSpeedInterpSpeed(
	TEXT("algona.P1.FormationForwardSpeedInterpSpeed"),
	6.0f,
	TEXT("Interpolation speed for Legacy group-level P1 forward speed modulation and MassStock diagnostic recovery to one."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationOrientationInterpSpeed(
	TEXT("algona.P1.FormationOrientationInterpSpeed"),
	8.0f,
	TEXT("Interpolation speed for P1 formation forward/right frame smoothing."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationMinSameGroupSpacing(
	TEXT("algona.P1.FormationMinSameGroupSpacing"),
	60.0f,
	TEXT("Default minimum center spacing allowed between same-group members during local choke deformation."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationLocalDeformationInterpSpeed(
	TEXT("algona.P1.FormationLocalDeformationInterpSpeed"),
	6.0f,
	TEXT("Interpolation speed for P1 per-member local funnel and re-expansion offsets."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1FormationSettlingRadius(
	TEXT("algona.P1.FormationSettlingRadius"),
	75.0f,
	TEXT("Per-entity distance threshold for P1 final formation settling."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1FormationSettlingRequiredTicks(
	TEXT("algona.P1.FormationSettlingRequiredTicks"),
	8,
	TEXT("Consecutive fixed ticks all P1 group members must be within final slots before idling."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1SpatialGridEnabled(
	TEXT("algona.P1.SpatialGridEnabled"),
	1,
	TEXT("Enables P1 uniform spatial grid rebuild for local steering queries."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1LocalSeparationEnabled(
	TEXT("algona.P1.LocalSeparationEnabled"),
	1,
	TEXT("Enables P1 local separation steering using the spatial grid."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1SpatialGridCellSize(
	TEXT("algona.P1.SpatialGridCellSize"),
	240.0f,
	TEXT("Cell size used by the P1 uniform spatial grid."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1NeighborQueryRadius(
	TEXT("algona.P1.NeighborQueryRadius"),
	180.0f,
	TEXT("Neighbor query radius used by P1 local separation steering."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1NeighborMaxCandidates(
	TEXT("algona.P1.NeighborMaxCandidates"),
	32,
	TEXT("Maximum neighbor candidates accumulated per entity for P1 local separation."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1SeparationStrength(
	TEXT("algona.P1.SeparationStrength"),
	220.0f,
	TEXT("Velocity strength of P1 local separation steering."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1SeparationMinimumPadding(
	TEXT("algona.P1.SeparationMinimumPadding"),
	0.0f,
	TEXT("Extra distance added to unit radii before P1 separation starts."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1DesiredVelocityBlend(
	TEXT("algona.P1.DesiredVelocityBlend"),
	0.35f,
	TEXT("Blend factor used when combining slot-target velocity with P1 separation velocity."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1HardBlockersEnabled(
	TEXT("algona.P1.HardBlockersEnabled"),
	1,
	TEXT("Enables P1 debug hard blockers for Algona-side route invalidation and movement clamping."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1StuckDetectionEnabled(
	TEXT("algona.P1.StuckDetectionEnabled"),
	1,
	TEXT("Enables P1 group-level stuck detection and budgeted repath requests."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1StuckDistanceEpsilon(
	TEXT("algona.P1.StuckDistanceEpsilon"),
	150.0f,
	TEXT("Minimum destination-distance improvement that resets P1 stuck ticks."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1StuckObservationTicks(
	TEXT("algona.P1.StuckObservationTicks"),
	60,
	TEXT("Fixed-tick observation window used before P1 stuck progress is evaluated."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1StuckRequiredTicks(
	TEXT("algona.P1.StuckRequiredTicks"),
	180,
	TEXT("Fixed ticks without progress before P1 stuck recovery queues a repath."),
	ECVF_Default
);

TAutoConsoleVariable<int32> CVarAlgonaP1RepathCooldownTicks(
	TEXT("algona.P1.RepathCooldownTicks"),
	120,
	TEXT("Minimum fixed ticks between automatic P1 repath requests for one group."),
	ECVF_Default
);

UAlgonaSimulationSubsystem* GetAlgonaSimulationSubsystem(UWorld* World)
{
	return World != nullptr
		? World->GetSubsystem<UAlgonaSimulationSubsystem>()
		: nullptr;
}

void SpawnP0Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P0.Spawn failed: no simulation subsystem."));
		return;
	}

	const int32 EntityCount = Args.Num() > 0
		? FCString::Atoi(*Args[0])
		: CVarAlgonaP0EntityCount.GetValueOnGameThread();
	const int32 GroupSize = Args.Num() > 1
		? FCString::Atoi(*Args[1])
		: CVarAlgonaP0GroupSize.GetValueOnGameThread();

	if (SimulationSubsystem->RepopulateSimulationEntities(EntityCount, GroupSize))
	{
		UAlgonaSimulationSubsystem::OnDebugSimulationSpawned().Broadcast(World);
	}
}

void SpawnRectP0Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr || Args.Num() < 3)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P0.SpawnRect usage: algona.P0.SpawnRect <GroupCount> <Columns> <Rows>"));
		return;
	}

	if (SimulationSubsystem->RepopulateSimulationRectGroups(
		FCString::Atoi(*Args[0]),
		FCString::Atoi(*Args[1]),
		FCString::Atoi(*Args[2])))
	{
		UAlgonaSimulationSubsystem::OnDebugSimulationSpawned().Broadcast(World);
	}
}

void DestroyP0Command(const TArray<FString>& Args, UWorld* World)
{
	if (UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World))
	{
		SimulationSubsystem->DestroySimulationEntities();
	}
}

void MoveGroupP0Command(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 4)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("Usage: algona.P0.MoveGroup <GroupId> <X> <Y> <Z>"));
		return;
	}

	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		return;
	}

	const FSimGroupId GroupId(FCString::Atoi(*Args[0]));
	const FVector Target(
		FCString::Atod(*Args[1]),
		FCString::Atod(*Args[2]),
		FCString::Atod(*Args[3])
	);

	SimulationSubsystem->SubmitMoveGroupCommand(GroupId, Target);
}

void MoveAllP0Command(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 3)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("Usage: algona.P0.MoveAll <X> <Y> <Z>"));
		return;
	}

	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		return;
	}

	const FVector Target(
		FCString::Atod(*Args[0]),
		FCString::Atod(*Args[1]),
		FCString::Atod(*Args[2])
	);

	const int32 SubmittedCount =
		SimulationSubsystem->SubmitMoveAllGroupsCommand(Target);
	UE_LOG(LogAlgonaSimulation, Log, TEXT("Submitted MoveAll commands. Count=%d"), SubmittedCount);
}

void DumpStateP0Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P0.DumpState failed: no simulation subsystem."));
		return;
	}

	const FSimGroupId GroupId(
		Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1
	);
	SimulationSubsystem->DumpDebugState(GroupId);
}

void DumpNavigationP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.DumpNavigation failed: no simulation subsystem."));
		return;
	}

	const FSimGroupId GroupId(
		Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1
	);
	SimulationSubsystem->DumpNavigationState(GroupId);
}

void DumpFormationP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.DumpFormation failed: no simulation subsystem."));
		return;
	}

	const FSimGroupId GroupId(
		Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1
	);
	const int32 MaxSamples = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 8;
	SimulationSubsystem->DumpFormationState(GroupId, MaxSamples);
}

void LocalMotionModeP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr || Args.IsEmpty())
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.LocalMotionMode usage: algona.P1.LocalMotionMode <Legacy|MassStock>"));
		return;
	}

	SimulationSubsystem->SetDebugLocalMotionMode(Args[0]);
}

void DumpLocalMotionP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.DumpLocalMotion failed: no simulation subsystem."));
		return;
	}

	SimulationSubsystem->DumpLocalMotionState(FSimGroupId(
		Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1));
}

bool TryParseSimLOD(const FString& Value, EAlgonaSimLOD& OutLOD)
{
	if (Value.Equals(TEXT("High"), ESearchCase::IgnoreCase))
	{
		OutLOD = EAlgonaSimLOD::High;
		return true;
	}
	if (Value.Equals(TEXT("Medium"), ESearchCase::IgnoreCase))
	{
		OutLOD = EAlgonaSimLOD::Medium;
		return true;
	}
	if (Value.Equals(TEXT("Low"), ESearchCase::IgnoreCase))
	{
		OutLOD = EAlgonaSimLOD::Low;
		return true;
	}
	return false;
}

void SetSimLODP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.SetSimLOD failed: no simulation subsystem."));
		return;
	}
	if (Args.IsEmpty())
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.SetSimLOD usage: algona.P1.SetSimLOD <High|Medium|Low> [GroupId]"));
		return;
	}

	EAlgonaSimLOD LOD = EAlgonaSimLOD::High;
	if (!TryParseSimLOD(Args[0], LOD))
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.SetSimLOD failed: unknown tier '%s'."), *Args[0]);
		return;
	}

	const FSimGroupId GroupId(
		Args.Num() > 1 ? FCString::Atoi(*Args[1]) : INDEX_NONE
	);
	SimulationSubsystem->SetDebugSimLOD(LOD, GroupId);
}

void SetGroupMinSpacingP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr || Args.Num() < 2)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.SetGroupMinSpacing usage: algona.P1.SetGroupMinSpacing <GroupId> <Spacing>"));
		return;
	}

	SimulationSubsystem->SetDebugGroupMinSpacing(
		FSimGroupId(FCString::Atoi(*Args[0])),
		FCString::Atod(*Args[1])
	);
}

void SetGroupDebugCubeSizeP0Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr || Args.Num() < 2)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P0.SetGroupDebugCubeSize usage: algona.P0.SetGroupDebugCubeSize <GroupId> <Size>"));
		return;
	}

	SimulationSubsystem->SetDebugGroupCubeSize(
		FSimGroupId(FCString::Atoi(*Args[0])),
		FCString::Atod(*Args[1])
	);
}

void AddHardBlockerP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.AddHardBlocker failed: no simulation subsystem."));
		return;
	}
	if (Args.Num() < 4)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.AddHardBlocker usage: algona.P1.AddHardBlocker <X> <Y> <Z> <Radius>"));
		return;
	}

	const FVector Center(
		FCString::Atod(*Args[0]),
		FCString::Atod(*Args[1]),
		FCString::Atod(*Args[2])
	);
	const double Radius = FCString::Atod(*Args[3]);
	SimulationSubsystem->AddDebugHardBlocker(Center, Radius);
}

void AddHardBlockerAtGroupP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.AddHardBlockerAtGroup failed: no simulation subsystem."));
		return;
	}

	const FSimGroupId GroupId(
		Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1
	);
	const double Radius = Args.Num() > 1 ? FCString::Atod(*Args[1]) : 400.0;
	SimulationSubsystem->AddDebugHardBlockerAtGroup(GroupId, Radius);
}

void ClearHardBlockersP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.ClearHardBlockers failed: no simulation subsystem."));
		return;
	}

	SimulationSubsystem->ClearDebugHardBlockers();
}

void DumpHardBlockersP1Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.DumpHardBlockers failed: no simulation subsystem."));
		return;
	}

	SimulationSubsystem->DumpDebugHardBlockers();
}

void RunPopulationSuiteP0Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P0.RunPopulationSuite failed: no simulation subsystem."));
		return;
	}

	const int32 GroupSize = Args.Num() > 0
		? FCString::Atoi(*Args[0])
		: CVarAlgonaP0GroupSize.GetValueOnGameThread();
	const int32 StepCount = Args.Num() > 1
		? FCString::Atoi(*Args[1])
		: 8;
	SimulationSubsystem->RunPopulationSmokeSuite(GroupSize, StepCount);
}

void RunCeilingSmokeP0Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P0.RunCeilingSmoke failed: no simulation subsystem."));
		return;
	}

	const int32 EntityCount = Args.Num() > 0
		? FCString::Atoi(*Args[0])
		: 100000;
	const int32 GroupSize = Args.Num() > 1
		? FCString::Atoi(*Args[1])
		: CVarAlgonaP0GroupSize.GetValueOnGameThread();
	const int32 StepCount = Args.Num() > 2
		? FCString::Atoi(*Args[2])
		: 8;
	SimulationSubsystem->RunCeilingSmoke(EntityCount, GroupSize, StepCount);
}

void RunBenchmarkSuiteP0Command(const TArray<FString>& Args, UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P0.RunBenchmarkSuite failed: no simulation subsystem."));
		return;
	}

	const int32 GroupSize = Args.Num() > 0
		? FCString::Atoi(*Args[0])
		: CVarAlgonaP0GroupSize.GetValueOnGameThread();
	const int32 StepCount = Args.Num() > 1
		? FCString::Atoi(*Args[1])
		: 64;
	SimulationSubsystem->RunBenchmarkSuite(GroupSize, StepCount);
}

void RunLocalMotionSpikeSuiteP1Command(
	const TArray<FString>& Args,
	UWorld* World)
{
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		GetAlgonaSimulationSubsystem(World);
	if (SimulationSubsystem == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("algona.P1.RunLocalMotionSpikeSuite failed: no simulation subsystem."));
		return;
	}

	SimulationSubsystem->RunLocalMotionSpikeSuite(
		Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 100,
		Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 64);
}

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0SpawnCommand(
	TEXT("algona.P0.Spawn"),
	TEXT("Spawns P0 Mass simulation entities. Usage: algona.P0.Spawn <EntityCount> [GroupSize]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0SpawnRectCommand(
	TEXT("algona.P0.SpawnRect"),
	TEXT("Spawns rectangular debug formations. Usage: algona.P0.SpawnRect <GroupCount> <Columns> <Rows>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnRectP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0DestroyCommand(
	TEXT("algona.P0.Destroy"),
	TEXT("Destroys P0 Mass simulation entities."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DestroyP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0MoveGroupCommand(
	TEXT("algona.P0.MoveGroup"),
	TEXT("Submits a high-level Move Group command. Usage: algona.P0.MoveGroup <GroupId> <X> <Y> <Z>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&MoveGroupP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0MoveAllCommand(
	TEXT("algona.P0.MoveAll"),
	TEXT("Submits a high-level Move command for every P0 group. Usage: algona.P0.MoveAll <X> <Y> <Z>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&MoveAllP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0DumpStateCommand(
	TEXT("algona.P0.DumpState"),
	TEXT("Dumps aggregate P0 simulation state and one group. Usage: algona.P0.DumpState [GroupId]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpStateP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1DumpNavigationCommand(
	TEXT("algona.P1.DumpNavigation"),
	TEXT("Dumps compact P1 group navigation and path scheduler state. Usage: algona.P1.DumpNavigation [GroupId]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpNavigationP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1DumpFormationCommand(
	TEXT("algona.P1.DumpFormation"),
	TEXT("Dumps compact P1 group formation state and a few slot target samples. Usage: algona.P1.DumpFormation [GroupId] [MaxSamples=8]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpFormationP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1LocalMotionModeCommand(
	TEXT("algona.P1.LocalMotionMode"),
	TEXT("Selects the debug lower-motion path for the next population. Usage: algona.P1.LocalMotionMode <Legacy|MassStock>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LocalMotionModeP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1DumpLocalMotionCommand(
	TEXT("algona.P1.DumpLocalMotion"),
	TEXT("Dumps compact Legacy/MassStock local-motion diagnostics. Usage: algona.P1.DumpLocalMotion [GroupId]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpLocalMotionP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1SetSimLODCommand(
	TEXT("algona.P1.SetSimLOD"),
	TEXT("Sets existing P0 SimLOD fragments for all entities or one group. Usage: algona.P1.SetSimLOD <High|Medium|Low> [GroupId]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetSimLODP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1SetGroupMinSpacingCommand(
	TEXT("algona.P1.SetGroupMinSpacing"),
	TEXT("Sets the debug minimum same-group formation spacing. Usage: algona.P1.SetGroupMinSpacing <GroupId> <Spacing>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetGroupMinSpacingP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0SetGroupDebugCubeSizeCommand(
	TEXT("algona.P0.SetGroupDebugCubeSize"),
	TEXT("Sets presentation-only debug cube size for one group. Usage: algona.P0.SetGroupDebugCubeSize <GroupId> <Size>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SetGroupDebugCubeSizeP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1AddHardBlockerCommand(
	TEXT("algona.P1.AddHardBlocker"),
	TEXT("Adds a debug circular hard blocker. Usage: algona.P1.AddHardBlocker <X> <Y> <Z> <Radius>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AddHardBlockerP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1AddHardBlockerAtGroupCommand(
	TEXT("algona.P1.AddHardBlockerAtGroup"),
	TEXT("Adds a debug circular hard blocker at the selected group's current route target. Usage: algona.P1.AddHardBlockerAtGroup <GroupId> [Radius=400]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AddHardBlockerAtGroupP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1ClearHardBlockersCommand(
	TEXT("algona.P1.ClearHardBlockers"),
	TEXT("Clears all P1 debug hard blockers and requeues blocked active groups."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ClearHardBlockersP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1DumpHardBlockersCommand(
	TEXT("algona.P1.DumpHardBlockers"),
	TEXT("Dumps P1 debug hard blocker summary."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DumpHardBlockersP1Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0RunPopulationSuiteCommand(
	TEXT("algona.P0.RunPopulationSuite"),
	TEXT("Runs compact P0 smoke sizes 1k/5k/10k/20k. Usage: algona.P0.RunPopulationSuite [GroupSize] [FixedStepsPerSize]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunPopulationSuiteP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0RunCeilingSmokeCommand(
	TEXT("algona.P0.RunCeilingSmoke"),
	TEXT("Runs one simulation-only P0 ceiling smoke. Usage: algona.P0.RunCeilingSmoke [EntityCount=100000] [GroupSize] [FixedSteps]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunCeilingSmokeP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0RunBenchmarkSuiteCommand(
	TEXT("algona.P0.RunBenchmarkSuite"),
	TEXT("Runs compact P0 simulation-only benchmark cases for 1k/5k/10k/20k. Usage: algona.P0.RunBenchmarkSuite [GroupSize] [FixedStepsPerCase]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunBenchmarkSuiteP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP1RunLocalMotionSpikeSuiteCommand(
	TEXT("algona.P1.RunLocalMotionSpikeSuite"),
	TEXT("Compares Legacy and MassStock local motion at 100/1k/10k. Usage: algona.P1.RunLocalMotionSpikeSuite [GroupSize=100] [FixedStepsPerCase=64]"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunLocalMotionSpikeSuiteP1Command)
);

FAlgonaDebugSimulationSpawnedDelegate GAlgonaDebugSimulationSpawnedDelegate;

uint8 GetLODUpdateStride(const EAlgonaSimLOD LOD)
{
	switch (LOD)
	{
	case EAlgonaSimLOD::High:
		return 1;
	case EAlgonaSimLOD::Medium:
		return 2;
	case EAlgonaSimLOD::Low:
		return 4;
	default:
		return 1;
	}
}

double GetSortedPercentile(const TArray<double>& SortedValues, double Percentile)
{
	if (SortedValues.IsEmpty())
	{
		return 0.0;
	}

	const int32 Index = FMath::Clamp(
		FMath::CeilToInt(static_cast<double>(SortedValues.Num()) * Percentile) - 1,
		0,
		SortedValues.Num() - 1
	);
	return SortedValues[Index];
}

struct FAlgonaP0BenchmarkCase
{
	const TCHAR* Name = TEXT("");
	bool bMovementEnabled = true;
	bool bLODEnabled = true;
	bool bSubmitMoveCommand = true;
};

const TCHAR* ToNavigationStatusString(EAlgonaGroupNavigationStatus Status)
{
	switch (Status)
	{
	case EAlgonaGroupNavigationStatus::Idle:
		return TEXT("Idle");
	case EAlgonaGroupNavigationStatus::Queued:
		return TEXT("Queued");
	case EAlgonaGroupNavigationStatus::Requesting:
		return TEXT("Requesting");
	case EAlgonaGroupNavigationStatus::WaitingForAsyncResult:
		return TEXT("WaitingForAsyncResult");
	case EAlgonaGroupNavigationStatus::RouteReady:
		return TEXT("RouteReady");
	case EAlgonaGroupNavigationStatus::Settling:
		return TEXT("Settling");
	case EAlgonaGroupNavigationStatus::Failed:
		return TEXT("Failed");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* ToSimLODString(EAlgonaSimLOD LOD)
{
	switch (LOD)
	{
	case EAlgonaSimLOD::High:
		return TEXT("High");
	case EAlgonaSimLOD::Medium:
		return TEXT("Medium");
	case EAlgonaSimLOD::Low:
		return TEXT("Low");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* ToFormationPhaseString(EAlgonaFormationMemberPhase Phase)
{
	switch (Phase)
	{
	case EAlgonaFormationMemberPhase::Normal:
		return TEXT("Normal");
	case EAlgonaFormationMemberPhase::Funneling:
		return TEXT("Funneling");
	case EAlgonaFormationMemberPhase::InsideChoke:
		return TEXT("InsideChoke");
	case EAlgonaFormationMemberPhase::ReExpanding:
		return TEXT("ReExpanding");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* ToMoveFacingModeString(EAlgonaMoveFacingMode Mode)
{
	switch (Mode)
	{
	case EAlgonaMoveFacingMode::FaceMovement:
		return TEXT("FaceMovement");
	case EAlgonaMoveFacingMode::PreserveFacing:
		return TEXT("PreserveFacing");
	default:
		return TEXT("Unknown");
	}
}

FVector RotateDirectionToward2D(
	const FVector& CurrentDirection,
	const FVector& DesiredDirection,
	double MaxRadians)
{
	FVector Current = CurrentDirection;
	Current.Z = 0.0;
	if (!Current.Normalize())
	{
		Current = FVector::ForwardVector;
	}

	FVector Desired = DesiredDirection;
	Desired.Z = 0.0;
	if (!Desired.Normalize())
	{
		return Current;
	}

	const double SignedAngle = FMath::Atan2(
		FVector::CrossProduct(Current, Desired).Z,
		FVector::DotProduct(Current, Desired)
	);
	const double AppliedAngle = FMath::Clamp(
		SignedAngle,
		-FMath::Max(MaxRadians, 0.0),
		FMath::Max(MaxRadians, 0.0)
	);
	FVector Result = FQuat(FVector::UpVector, AppliedAngle).RotateVector(Current);
	Result.Z = 0.0;
	return Result.GetSafeNormal(
		UE_DOUBLE_SMALL_NUMBER,
		FVector::ForwardVector
	);
}
}

bool UAlgonaSimulationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Cast<UWorld>(Outer);
	if (World == nullptr)
	{
		return false;
	}

	return World->WorldType == EWorldType::Game
		|| World->WorldType == EWorldType::PIE;
}

FAlgonaDebugSimulationSpawnedDelegate&
UAlgonaSimulationSubsystem::OnDebugSimulationSpawned()
{
	return GAlgonaDebugSimulationSpawnedDelegate;
}

void UAlgonaSimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Collection.InitializeDependency<UMassEntitySubsystem>();
	Collection.InitializeDependency<UMassNavigationSubsystem>();
	bWorldTeardownHandled = false;
	FWorldDelegates::OnWorldBeginTearDown.AddUObject(
		this,
		&ThisClass::HandleWorldBeginTearDown
	);

	AccumulatorSeconds = 0.0;
	SimulationTick = 0;
	NextCommandSequence = 1;
	RefreshRuntimeConfiguration();
	EnsureMassRuntime();

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Initialized. World=%s, MassSubsystem=%p, EntityManager=%p"),
		*GetNameSafe(GetWorld()),
		MassRuntime != nullptr ? MassRuntime->MassSubsystem.Get() : nullptr,
		MassRuntime != nullptr ? MassRuntime->EntityManager : nullptr
	);
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	FWorldDelegates::OnWorldBeginTearDown.RemoveAll(this);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Deinitializing. World=%s, SimulationTick=%llu, Entities=%d, Groups=%d"),
		*GetNameSafe(GetWorld()),
		SimulationTick,
		GetSimulationEntityCount(),
		GetSimulationGroupCount()
	);

	ActiveLocalMotionMode = RequestedLocalMotionMode;
	ResetSimulationState();

	delete MassRuntime;
	MassRuntime = nullptr;

	Super::Deinitialize();
}

void UAlgonaSimulationSubsystem::BeginDestroy()
{
	FWorldDelegates::OnWorldBeginTearDown.RemoveAll(this);
	ResetSimulationState();

	delete MassRuntime;
	MassRuntime = nullptr;

	Super::BeginDestroy();
}

void UAlgonaSimulationSubsystem::HandleWorldBeginTearDown(UWorld* World)
{
	if (World != GetWorld() || bWorldTeardownHandled)
	{
		return;
	}

	bWorldTeardownHandled = true;
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("World teardown cleanup before Mass subsystem deinitialization. World=%s, Entities=%d"),
		*GetNameSafe(World),
		GetSimulationEntityCount()
	);
	DestroySimulationEntities();
}

void UAlgonaSimulationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (CVarAlgonaP0AutoPopulate.GetValueOnGameThread() == 0)
	{
		return;
	}

	const int32 EntityCount = CVarAlgonaP0EntityCount.GetValueOnGameThread();
	if (EntityCount > 0)
	{
		RepopulateSimulationEntities(
			EntityCount,
			CVarAlgonaP0GroupSize.GetValueOnGameThread()
		);
	}
}

void UAlgonaSimulationSubsystem::Tick(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_Tick);

	RefreshRuntimeConfiguration();

	if (DeltaTime <= 0.0f)
	{
		return;
	}

	AccumulatorSeconds += static_cast<double>(DeltaTime);

	int32 StepsThisFrame = 0;
	while (AccumulatorSeconds >= FixedDeltaSeconds
		&& StepsThisFrame < MaxSimulationStepsPerFrame)
	{
		RunFixedSimulationStep();
		AccumulatorSeconds -= FixedDeltaSeconds;
		++StepsThisFrame;
	}

	if (AccumulatorSeconds >= FixedDeltaSeconds)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("Dropped simulation catch-up time. World=%s, RemainingSeconds=%.4f, FixedDeltaSeconds=%.4f, MaxSteps=%d"),
			*GetNameSafe(GetWorld()),
			AccumulatorSeconds,
			FixedDeltaSeconds,
			MaxSimulationStepsPerFrame
		);

		AccumulatorSeconds = 0.0;
	}
}

TStatId UAlgonaSimulationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UAlgonaSimulationSubsystem,
		STATGROUP_Tickables
	);
}

int32 UAlgonaSimulationSubsystem::GetSimulationEntityCount() const
{
	return MassRuntime != nullptr ? MassRuntime->Entities.Num() : 0;
}

FAlgonaSimBenchmarkSnapshot UAlgonaSimulationSubsystem::GetBenchmarkSnapshot() const
{
	FAlgonaSimBenchmarkSnapshot Snapshot = BenchmarkSnapshot;
	Snapshot.EntityCount = GetSimulationEntityCount();
	Snapshot.GroupCount = Groups.Num();
	Snapshot.SimulationTick = SimulationTick;
	Snapshot.PendingCommandCount = PendingCommands.Num();
	Snapshot.QueuedPathRequestCount = QueuedPathRequests.Num();
	Snapshot.InFlightPathRequestCount = InFlightPathRequests.Num();
	Snapshot.PendingPathCompletionCount = PendingPathCompletions.Num();
	return Snapshot;
}

bool UAlgonaSimulationSubsystem::RepopulateSimulationEntities(
	int32 EntityCount,
	int32 GroupSize)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_EntitySpawn);

	EntityCount = FMath::Max(EntityCount, 0);
	GroupSize = FMath::Max(GroupSize, 1);

	if (EntityCount == 0)
	{
		DestroySimulationEntities();
		return true;
	}

	if (!IsAuthoritativeSimulationWorld())
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("Rejected P0 Mass population on non-authoritative world. World=%s"),
			*GetNameSafe(GetWorld())
		);
		return false;
	}

	EnsureMassRuntime();
	if (!EnsureEntityArchetype())
	{
		return false;
	}

	DestroySimulationEntities();

	const uint64 StartUsedPhysicalBytes =
		FPlatformMemory::GetStats().UsedPhysical;
	const double StartSeconds = FPlatformTime::Seconds();
	MassRuntime->Entities.Reserve(EntityCount);
	const TSharedRef<FMassEntityManager::FEntityCreationContext> CreationContext =
		ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock
			? MassRuntime->EntityManager->BatchCreateEntities(
				MassRuntime->EntityArchetype,
				MassRuntime->MassStockSharedFragments,
				EntityCount,
				MassRuntime->Entities)
			: MassRuntime->EntityManager->BatchCreateEntities(
				MassRuntime->EntityArchetype,
				EntityCount,
				MassRuntime->Entities);
	(void)CreationContext;

	Groups.Reserve(FMath::DivideAndRoundUp(EntityCount, GroupSize));

	const int32 GridSide =
		FMath::Max(1, FMath::CeilToInt(FMath::Sqrt(static_cast<float>(EntityCount))));
	const double Spacing =
		FMath::Max(CVarAlgonaP0InitialSpacing.GetValueOnGameThread(), 1.0f);

	for (int32 EntityIndex = 0; EntityIndex < MassRuntime->Entities.Num(); ++EntityIndex)
	{
		const int32 GroupIndex = EntityIndex / GroupSize;
		while (Groups.Num() <= GroupIndex)
		{
			FAlgonaSimGroupState& NewGroup = Groups.AddDefaulted_GetRef();
			NewGroup.Id = FSimGroupId(Groups.Num());
			NewGroup.MinSameGroupSpacing =
				static_cast<float>(FormationMinSameGroupSpacing);
		}

		FAlgonaSimGroupState& Group = Groups[GroupIndex];
		const int32 SlotIndex = Group.MemberCount++;

		FMassEntityView EntityView(
			*MassRuntime->EntityManager,
			MassRuntime->Entities[EntityIndex]
		);

		FAlgonaSimPositionFragment& Position =
			EntityView.GetFragmentData<FAlgonaSimPositionFragment>();
		Position.Position = FVector(
			static_cast<double>(EntityIndex % GridSide) * Spacing,
			static_cast<double>(EntityIndex / GridSide) * Spacing,
			0.0
		);

		EntityView.GetFragmentData<FAlgonaSimVelocityFragment>().Velocity =
			FVector::ZeroVector;
		EntityView.GetFragmentData<FAlgonaSimGroupFragment>().GroupId =
			Group.Id.Value;
		EntityView.GetFragmentData<FAlgonaSimStateFragment>().State =
			EAlgonaSimEntityState::Idle;

		FAlgonaSimLODFragment& LODFragment =
			EntityView.GetFragmentData<FAlgonaSimLODFragment>();
		if (EntityIndex % 4 == 0)
		{
			LODFragment.LOD = EAlgonaSimLOD::Low;
			++Group.LowLODMemberCount;
		}
		else if (EntityIndex % 2 == 0)
		{
			LODFragment.LOD = EAlgonaSimLOD::Medium;
			++Group.MediumLODMemberCount;
		}
		else
		{
			LODFragment.LOD = EAlgonaSimLOD::High;
			++Group.HighLODMemberCount;
		}
		LODFragment.UpdateStride = GetLODUpdateStride(LODFragment.LOD);
		LODFragment.TickOffset =
			static_cast<uint8>(EntityIndex % FMath::Max<int32>(LODFragment.UpdateStride, 1));

		FAlgonaSimNavigationFragment& NavigationFragment =
			EntityView.GetFragmentData<FAlgonaSimNavigationFragment>();
		NavigationFragment.SlotIndex = SlotIndex;
		NavigationFragment.DesiredSlotPosition = Position.Position;
		NavigationFragment.DesiredVelocity = FVector::ZeroVector;

		if (ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock)
		{
			const float AgentRadius = FMath::Max(
				Group.MinSameGroupSpacing * 0.5f,
				10.0f);
			EntityView.GetFragmentData<FTransformFragment>().SetTransform(
				FTransform(FQuat::Identity, Position.Position));
			EntityView.GetFragmentData<FAgentRadiusFragment>().Radius =
				AgentRadius;
			EntityView.GetFragmentData<FMassVelocityFragment>().Value =
				FVector::ZeroVector;
			EntityView.GetFragmentData<FMassDesiredMovementFragment>() =
				FMassDesiredMovementFragment();
			EntityView.GetFragmentData<FMassForceFragment>().Value =
				FVector::ZeroVector;
			FMassMoveTargetFragment& MoveTarget =
				EntityView.GetFragmentData<FMassMoveTargetFragment>();
			MoveTarget.Center = Position.Position;
			MoveTarget.Forward = FVector::ForwardVector;
			MoveTarget.DistanceToGoal = 0.0f;
			MoveTarget.EntityDistanceToGoal =
				FMassMoveTargetFragment::UnsetDistance;
			MoveTarget.SlackRadius =
				static_cast<float>(FormationSettlingRadius);
			MoveTarget.DesiredSpeed = FMassInt16Real(0.0f);
			MoveTarget.IntentAtGoal = EMassMovementAction::Stand;
			EntityView.GetFragmentData<FMassAvoidanceColliderFragment>() =
				FMassAvoidanceColliderFragment(
					FMassCircleCollider(AgentRadius));
		}
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		RefreshDerivedGroupSimLOD(Group);
		if (!Group.bHasActiveMoveTarget)
		{
			continue;
		}

		if (!IsNavigationMovementReady(Group))
		{
			continue;
		}

		ConfigureFormationForGroup(Group);
	}
	RefreshSimLODStatistics();
	ResetSimLODMeasurement();
	SynchronizeMassStockTransformsAndObstacleGrid();

	BenchmarkSnapshot.LastSpawnMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	const uint64 EndUsedPhysicalBytes =
		FPlatformMemory::GetStats().UsedPhysical;
	BenchmarkSnapshot.LastSpawnUsedPhysicalDeltaBytes =
		static_cast<int64>(EndUsedPhysicalBytes)
		- static_cast<int64>(StartUsedPhysicalBytes);
	BenchmarkSnapshot.LastUsedPhysicalBytes = EndUsedPhysicalBytes;
	BenchmarkSnapshot.EntityCount = MassRuntime->Entities.Num();
	BenchmarkSnapshot.GroupCount = Groups.Num();
	BenchmarkSnapshot.PendingCommandCount = PendingCommands.Num();

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Spawned P0 Mass entities. EntityCount=%d, GroupSize=%d, GroupCount=%d, LocalMotion=%s, SpawnMs=%.3f, SpawnMemDeltaKB=%lld, UsedPhysicalMB=%llu"),
		MassRuntime->Entities.Num(),
		GroupSize,
		Groups.Num(),
		ToLocalMotionModeString(ActiveLocalMotionMode),
		BenchmarkSnapshot.LastSpawnMilliseconds,
		BenchmarkSnapshot.LastSpawnUsedPhysicalDeltaBytes / 1024,
		BenchmarkSnapshot.LastUsedPhysicalBytes / (1024ULL * 1024ULL)
	);

	return true;
}

bool UAlgonaSimulationSubsystem::RepopulateSimulationRectGroups(
	int32 GroupCount,
	int32 Columns,
	int32 Rows)
{
	GroupCount = FMath::Max(GroupCount, 0);
	Columns = FMath::Max(Columns, 1);
	Rows = FMath::Max(Rows, 1);
	const int64 MembersPerGroup64 =
		static_cast<int64>(Columns) * static_cast<int64>(Rows);
	const int64 EntityCount64 =
		static_cast<int64>(GroupCount) * MembersPerGroup64;
	if (MembersPerGroup64 > MAX_int32 || EntityCount64 > MAX_int32)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("algona.P0.SpawnRect rejected: requested population exceeds int32. Groups=%d, Columns=%d, Rows=%d"),
			GroupCount,
			Columns,
			Rows
		);
		return false;
	}

	const int32 MembersPerGroup = static_cast<int32>(MembersPerGroup64);
	if (!RepopulateSimulationEntities(
		static_cast<int32>(EntityCount64),
		MembersPerGroup))
	{
		return false;
	}
	if (GroupCount == 0)
	{
		return true;
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		Group.DebugFormationColumns = Columns;
		Group.DebugFormationRows = Rows;
		ConfigureFormationForGroup(Group);
		Group.Formation.AnchorPosition = FVector(
			0.0,
			(static_cast<double>(Group.Id.Value - 1)
				- (static_cast<double>(GroupCount) - 1.0) * 0.5)
				* (static_cast<double>(Group.Formation.Width)
					+ Group.Formation.SlotSpacing * 4.0),
			0.0
		);
		Group.Formation.TravelDirection = FVector::ForwardVector;
		Group.Formation.LayoutForward = FVector::ForwardVector;
		Group.Formation.LayoutRight = FVector::RightVector;
		Group.Formation.FacingDirection = FVector::ForwardVector;
		Group.Formation.LastUpdateTick = 0;
	}

	for (const FMassEntityHandle Entity : MassRuntime->Entities)
	{
		FMassEntityView EntityView(*MassRuntime->EntityManager, Entity);
		const int32 EntityGroupId =
			EntityView.GetFragmentData<FAlgonaSimGroupFragment>().GroupId;
		const int32 GroupArrayIndex = EntityGroupId - 1;
		if (!Groups.IsValidIndex(GroupArrayIndex))
		{
			continue;
		}

		FAlgonaSimGroupState& Group = Groups[GroupArrayIndex];
		FAlgonaSimNavigationFragment& Navigation =
			EntityView.GetFragmentData<FAlgonaSimNavigationFragment>();
		Navigation.OriginalSlotRow = Navigation.SlotIndex / Columns;
		Navigation.OriginalSlotColumn = Navigation.SlotIndex % Columns;
		const FVector Position =
			Group.Formation.AnchorPosition
			+ ComputeFormationSlotOffset(
				Group.Formation,
				Navigation.SlotIndex
			);
		EntityView.GetFragmentData<FAlgonaSimPositionFragment>().Position = Position;
		Navigation.DesiredSlotPosition = Position;
		Navigation.DesiredVelocity = FVector::ZeroVector;
		if (ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock)
		{
			EntityView.GetFragmentData<FTransformFragment>().SetTransform(
				FTransform(FQuat::Identity, Position));
		}
	}
	SynchronizeMassStockTransformsAndObstacleGrid();

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Spawned rectangular P0 Mass formations. GroupCount=%d, Columns=%d, Rows=%d, MembersPerGroup=%d, EntityCount=%d"),
		GroupCount,
		Columns,
		Rows,
		MembersPerGroup,
		static_cast<int32>(EntityCount64)
	);
	return true;
}

void UAlgonaSimulationSubsystem::DestroySimulationEntities()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_EntityDestroy);

	AbortAllPathRequests();

	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		Groups.Reset();
		PendingCommands.Reset();
		QueuedPathRequests.Reset();
		InFlightPathRequests.Reset();
		PendingPathCompletions.Reset();
		BenchmarkSnapshot.EntityCount = 0;
		BenchmarkSnapshot.GroupCount = 0;
		BenchmarkSnapshot.PendingCommandCount = 0;
		BenchmarkSnapshot.QueuedPathRequestCount = 0;
		BenchmarkSnapshot.InFlightPathRequestCount = 0;
		BenchmarkSnapshot.PendingPathCompletionCount = 0;
		BenchmarkSnapshot.LastMovementMilliseconds = 0.0;
		BenchmarkSnapshot.LastFixedStepMilliseconds = 0.0;
		BenchmarkSnapshot.bLastFixedStepMissedDeadline = false;
		BenchmarkSnapshot.LastMovementEntityVisits = 0;
		BenchmarkSnapshot.LastMovementUpdatedEntities = 0;
		BenchmarkSnapshot.LastMovementSkippedByLOD = 0;
		BenchmarkSnapshot.LastFormationMilliseconds = 0.0;
		BenchmarkSnapshot.LastFormationUpdatedGroups = 0;
		BenchmarkSnapshot.LastFormationSkippedGroupsByLOD = 0;
		BenchmarkSnapshot.LastFormationTargetUpdatedEntities = 0;
		BenchmarkSnapshot.LastFormationTargetSkippedEntitiesByLOD = 0;
		RefreshSimLODStatistics();
		ResetSimLODMeasurement();
		BenchmarkSnapshot.LastNeighborQueriesSkippedByLOD = 0;
		BenchmarkSnapshot.LastHighLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastMediumLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastLowLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastAvoidanceAppliedCount = 0;
		BenchmarkSnapshot.ActiveHardBlockerCount = HardBlockers.Num();
		BenchmarkSnapshot.LastRouteInvalidationCount = 0;
		BenchmarkSnapshot.LastStuckGroupCount = 0;
		BenchmarkSnapshot.LastHardBlockerMovementBlockedEntities = 0;
		if (MassRuntime != nullptr)
		{
			MassRuntime->LocalMotionDiagnostics = FAlgonaLocalMotionDiagnostics();
			MassRuntime->LocalMotionForceDiagnostics =
				FAlgonaLocalMotionForceDiagnostics();
		}
		bCaptureLocalMotionForcesNextStep = false;
		LocalMotionForceCaptureGroupId = FSimGroupId();
		ActiveLocalMotionMode = RequestedLocalMotionMode;
		return;
	}

	const uint64 StartUsedPhysicalBytes =
		FPlatformMemory::GetStats().UsedPhysical;
	const double StartSeconds = FPlatformTime::Seconds();
	MassRuntime->EntityManager->BatchDestroyEntities(MassRuntime->Entities);
	MassRuntime->Entities.Reset();
	Groups.Reset();
	PendingCommands.Reset();
	QueuedPathRequests.Reset();
	InFlightPathRequests.Reset();
	PendingPathCompletions.Reset();
	SpatialGrid.Cells.Reset();
	SpatialGrid.Entries.Reset();
	MassRuntime->LocalMotionDiagnostics = FAlgonaLocalMotionDiagnostics();
	MassRuntime->LocalMotionForceDiagnostics =
		FAlgonaLocalMotionForceDiagnostics();
	bCaptureLocalMotionForcesNextStep = false;
	LocalMotionForceCaptureGroupId = FSimGroupId();
	ActiveLocalMotionMode = RequestedLocalMotionMode;

	BenchmarkSnapshot.LastDestroyMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	const uint64 EndUsedPhysicalBytes =
		FPlatformMemory::GetStats().UsedPhysical;
	BenchmarkSnapshot.LastDestroyUsedPhysicalDeltaBytes =
		static_cast<int64>(EndUsedPhysicalBytes)
		- static_cast<int64>(StartUsedPhysicalBytes);
	BenchmarkSnapshot.LastUsedPhysicalBytes = EndUsedPhysicalBytes;
	BenchmarkSnapshot.EntityCount = 0;
	BenchmarkSnapshot.GroupCount = 0;
	BenchmarkSnapshot.PendingCommandCount = 0;
	BenchmarkSnapshot.QueuedPathRequestCount = 0;
	BenchmarkSnapshot.InFlightPathRequestCount = 0;
	BenchmarkSnapshot.PendingPathCompletionCount = 0;
	BenchmarkSnapshot.LastFixedStepMilliseconds = 0.0;
	BenchmarkSnapshot.bLastFixedStepMissedDeadline = false;
	BenchmarkSnapshot.LastMovementEntityVisits = 0;
	BenchmarkSnapshot.LastMovementUpdatedEntities = 0;
	BenchmarkSnapshot.LastMovementSkippedByLOD = 0;
	BenchmarkSnapshot.LastFormationMilliseconds = 0.0;
	BenchmarkSnapshot.LastFormationUpdatedGroups = 0;
	BenchmarkSnapshot.LastFormationSkippedGroupsByLOD = 0;
	BenchmarkSnapshot.LastFormationTargetUpdatedEntities = 0;
	BenchmarkSnapshot.LastFormationTargetSkippedEntitiesByLOD = 0;
	RefreshSimLODStatistics();
	ResetSimLODMeasurement();
	BenchmarkSnapshot.LastSpatialGridBuildMilliseconds = 0.0;
	BenchmarkSnapshot.LastSpatialGridEntityCount = 0;
	BenchmarkSnapshot.LastSpatialGridCellCount = 0;
	BenchmarkSnapshot.LastNeighborQueryMilliseconds = 0.0;
	BenchmarkSnapshot.LastNeighborQueryCount = 0;
	BenchmarkSnapshot.LastNeighborQueriesSkippedByLOD = 0;
	BenchmarkSnapshot.LastHighLODNeighborQueryCount = 0;
	BenchmarkSnapshot.LastMediumLODNeighborQueryCount = 0;
	BenchmarkSnapshot.LastLowLODNeighborQueryCount = 0;
	BenchmarkSnapshot.LastNeighborCandidateCount = 0;
	BenchmarkSnapshot.LastNeighborMaxCandidates = 0;
	BenchmarkSnapshot.LastNeighborAverageCandidates = 0.0;
	BenchmarkSnapshot.LastAvoidanceMilliseconds = 0.0;
	BenchmarkSnapshot.LastAvoidanceAppliedCount = 0;
	BenchmarkSnapshot.ActiveHardBlockerCount = HardBlockers.Num();
	BenchmarkSnapshot.LastRouteInvalidationCount = 0;
	BenchmarkSnapshot.LastStuckGroupCount = 0;
	BenchmarkSnapshot.LastHardBlockerMovementBlockedEntities = 0;

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Destroyed P0 Mass entities. DestroyMs=%.3f, DestroyMemDeltaKB=%lld, UsedPhysicalMB=%llu"),
		BenchmarkSnapshot.LastDestroyMilliseconds,
		BenchmarkSnapshot.LastDestroyUsedPhysicalDeltaBytes / 1024,
		BenchmarkSnapshot.LastUsedPhysicalBytes / (1024ULL * 1024ULL)
	);
}

bool UAlgonaSimulationSubsystem::SubmitMoveGroupCommand(
	FSimGroupId GroupId,
	const FVector& TargetPosition,
	EAlgonaMoveFacingMode MoveFacingMode)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("Rejected MoveGroup command on non-authoritative world. GroupId=%d"),
			GroupId.Value
		);
		return false;
	}

	if (FindGroup(GroupId) == nullptr)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("Rejected MoveGroup command for unknown group. GroupId=%d"),
			GroupId.Value
		);
		return false;
	}

	FAlgonaSimCommand& Command = PendingCommands.AddDefaulted_GetRef();
	Command.Sequence = NextCommandSequence++;
	Command.ApplyAtSimulationTick = SimulationTick + 1;
	Command.Type = EAlgonaSimCommandType::MoveGroupToTarget;
	Command.MoveFacingMode = MoveFacingMode;
	Command.GroupId = GroupId;
	Command.TargetPosition = TargetPosition;
	BenchmarkSnapshot.LastSubmittedCommandSequence = Command.Sequence;
	BenchmarkSnapshot.PendingCommandCount = PendingCommands.Num();

	if (bCommandDiagnosticsEnabled)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Log,
			TEXT("Submitted command. Sequence=%llu, ApplyTick=%llu, Type=MoveGroupToTarget, FacingMode=%s, GroupId=%d, Target=%s, Pending=%d"),
			Command.Sequence,
			Command.ApplyAtSimulationTick,
			ToMoveFacingModeString(Command.MoveFacingMode),
			Command.GroupId.Value,
			*Command.TargetPosition.ToCompactString(),
			PendingCommands.Num()
		);
	}

	return true;
}

int32 UAlgonaSimulationSubsystem::SubmitMoveAllGroupsCommand(
	const FVector& TargetPosition,
	EAlgonaMoveFacingMode MoveFacingMode)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("Rejected MoveAll command on non-authoritative world.")
		);
		return 0;
	}

	int32 SubmittedCount = 0;
	for (const FAlgonaSimGroupState& Group : Groups)
	{
		if (SubmitMoveGroupCommand(
			Group.Id,
			TargetPosition,
			MoveFacingMode))
		{
			++SubmittedCount;
		}
	}
	return SubmittedCount;
}

int32 UAlgonaSimulationSubsystem::ExportEntityPositions(
	TArray<FVector>& OutPositions,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_PresentationExport);

	OutPositions.Reset();

	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		return 0;
	}

	MaxEntities = MaxEntities > 0
		? FMath::Min(MaxEntities, MassRuntime->Entities.Num())
		: MassRuntime->Entities.Num();
	OutPositions.Reserve(MaxEntities);

	FMassEntityQuery ExportQuery(MassRuntime->EntityManager->AsShared());
	ExportQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
	ExportQuery.ForEachEntityChunk(
		ExecutionContext,
		[&OutPositions, MaxEntities](FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities()
				&& OutPositions.Num() < MaxEntities; ++Index)
			{
				OutPositions.Add(Positions[Index].Position);
			}
		}
	);

	return OutPositions.Num();
}

int32 UAlgonaSimulationSubsystem::ExportDebugPresentationEntities(
	TArray<FAlgonaDebugPresentationEntity>& OutEntities,
	int32 MaxEntities)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_DebugPresentationExport);

	OutEntities.Reset();
	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		return 0;
	}

	MaxEntities = MaxEntities > 0
		? FMath::Min(MaxEntities, MassRuntime->Entities.Num())
		: MassRuntime->Entities.Num();
	OutEntities.Reserve(MaxEntities);

	FMassEntityQuery ExportQuery(MassRuntime->EntityManager->AsShared());
	ExportQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	ExportQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
	ExportQuery.ForEachEntityChunk(
		ExecutionContext,
		[this, &OutEntities, MaxEntities](FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities()
				&& OutEntities.Num() < MaxEntities; ++Index)
			{
				FAlgonaDebugPresentationEntity& Entity =
					OutEntities.AddDefaulted_GetRef();
				Entity.Position = Positions[Index].Position;
				Entity.GroupId = GroupIds[Index].GroupId;
				const int32 GroupArrayIndex = Entity.GroupId - 1;
				if (Groups.IsValidIndex(GroupArrayIndex)
					&& Groups[GroupArrayIndex].Id.Value == Entity.GroupId)
				{
					const FAlgonaSimGroupState& Group =
						Groups[GroupArrayIndex];
					Entity.CubeSize = Group.DebugPresentationCubeSize;
					Entity.FacingDirection =
						Group.Formation.FacingDirection;
				}
			}
		}
	);

	return OutEntities.Num();
}

bool UAlgonaSimulationSubsystem::FindNearestGroupAtPosition(
	const FVector& WorldPosition,
	double MaxDistance,
	FSimGroupId& OutGroupId,
	FVector* OutNearestPosition) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_DebugPickGroup);

	OutGroupId = FSimGroupId();
	if (OutNearestPosition != nullptr)
	{
		*OutNearestPosition = FVector::ZeroVector;
	}

	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		return false;
	}

	MaxDistance = FMath::Max(MaxDistance, 1.0);
	const double MaxDistanceSquared = FMath::Square(MaxDistance);
	double BestDistanceSquared = MaxDistanceSquared;
	FVector BestPosition = FVector::ZeroVector;
	int32 BestGroupId = INDEX_NONE;

	FMassEntityQuery PickQuery(MassRuntime->EntityManager->AsShared());
	PickQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	PickQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
	PickQuery.ForEachEntityChunk(
		ExecutionContext,
		[&WorldPosition,
			&BestDistanceSquared,
			&BestPosition,
			&BestGroupId](FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				const double DistanceSquared =
					FVector::DistSquared2D(
						WorldPosition,
						Positions[Index].Position
					);
				if (DistanceSquared <= BestDistanceSquared)
				{
					BestDistanceSquared = DistanceSquared;
					BestPosition = Positions[Index].Position;
					BestGroupId = GroupIds[Index].GroupId;
				}
			}
		}
	);

	if (BestGroupId <= 0)
	{
		return false;
	}

	OutGroupId = FSimGroupId(BestGroupId);
	if (OutNearestPosition != nullptr)
	{
		*OutNearestPosition = BestPosition;
	}
	return true;
}

void UAlgonaSimulationSubsystem::DumpDebugState(FSimGroupId GroupId) const
{
	const FAlgonaSimBenchmarkSnapshot Snapshot = GetBenchmarkSnapshot();
	const FAlgonaSimGroupState* Group = FindGroup(GroupId);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P0 state. Tick=%llu, Entities=%d, Groups=%d, PendingCommands=%d, QueuedPaths=%d, InFlightPaths=%d, PendingPathCompletions=%d, PathSubmitted=%llu, PathCompleted=%llu, PathFailed=%llu, PathStale=%llu, PathSchedulerMs=%.3f, RouteProgressMs=%.3f, FormationMs=%.3f, FormationGroups=%d, GridMs=%.3f, GridEntities=%d, GridCells=%d, NeighborMs=%.3f, NeighborQueries=%d, NeighborCandidates=%d, NeighborAvg=%.2f, NeighborMax=%d, AvoidanceMs=%.3f, HardBlockers=%d, LastInvalidated=%d, TotalRepaths=%llu, StuckGroups=%d, BlockedEntities=%d, TotalBlockedEntities=%llu, LastSubmitted=%llu, LastApplied=%llu, LastAppliedGroup=%d, LastSpawnMemDeltaKB=%lld, LastDestroyMemDeltaKB=%lld, UsedPhysicalMB=%llu, LastStepMs=%.3f, MissedDeadline=%s, LastMoveMs=%.3f, Visits=%d, Updated=%d, SkippedByLOD=%d"),
		Snapshot.SimulationTick,
		Snapshot.EntityCount,
		Snapshot.GroupCount,
		Snapshot.PendingCommandCount,
		Snapshot.QueuedPathRequestCount,
		Snapshot.InFlightPathRequestCount,
		Snapshot.PendingPathCompletionCount,
		Snapshot.SubmittedPathRequests,
		Snapshot.CompletedPathRequests,
		Snapshot.FailedPathRequests,
		Snapshot.RejectedStalePathResults,
		Snapshot.LastPathSchedulerMilliseconds,
		Snapshot.LastRouteProgressMilliseconds,
		Snapshot.LastFormationMilliseconds,
		Snapshot.LastFormationUpdatedGroups,
		Snapshot.LastSpatialGridBuildMilliseconds,
		Snapshot.LastSpatialGridEntityCount,
		Snapshot.LastSpatialGridCellCount,
		Snapshot.LastNeighborQueryMilliseconds,
		Snapshot.LastNeighborQueryCount,
		Snapshot.LastNeighborCandidateCount,
		Snapshot.LastNeighborAverageCandidates,
		Snapshot.LastNeighborMaxCandidates,
		Snapshot.LastAvoidanceMilliseconds,
		Snapshot.ActiveHardBlockerCount,
		Snapshot.LastRouteInvalidationCount,
		Snapshot.TotalRepathsRequested,
		Snapshot.LastStuckGroupCount,
		Snapshot.LastHardBlockerMovementBlockedEntities,
		Snapshot.TotalHardBlockerMovementBlockedEntities,
		Snapshot.LastSubmittedCommandSequence,
		Snapshot.LastAppliedCommandSequence,
		Snapshot.LastAppliedCommandGroupId,
		Snapshot.LastSpawnUsedPhysicalDeltaBytes / 1024,
		Snapshot.LastDestroyUsedPhysicalDeltaBytes / 1024,
		Snapshot.LastUsedPhysicalBytes / (1024ULL * 1024ULL),
		Snapshot.LastFixedStepMilliseconds,
		Snapshot.bLastFixedStepMissedDeadline ? TEXT("true") : TEXT("false"),
		Snapshot.LastMovementMilliseconds,
		Snapshot.LastMovementEntityVisits,
		Snapshot.LastMovementUpdatedEntities,
		Snapshot.LastMovementSkippedByLOD
	);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 SimLOD workload. Enabled=%s, Entities[H/M/L]=%d/%d/%d, Groups[H/M/L]=%d/%d/%d, FormationGroups[Updated/Skipped]=%d/%d, FormationTargets[Updated/Skipped]=%d/%d, NeighborQueries[Executed/Skipped]=%d/%d, NeighborQueries[H/M/L]=%d/%d/%d, AvoidanceApplied=%d, FormationMs=%.3f, GridMs=%.3f, NeighborMs=%.3f, AvoidanceMs=%.3f, MovementMs=%.3f"),
		bSimulationLODEnabled ? TEXT("true") : TEXT("false"),
		Snapshot.HighLODEntityCount,
		Snapshot.MediumLODEntityCount,
		Snapshot.LowLODEntityCount,
		Snapshot.HighLODGroupCount,
		Snapshot.MediumLODGroupCount,
		Snapshot.LowLODGroupCount,
		Snapshot.LastFormationUpdatedGroups,
		Snapshot.LastFormationSkippedGroupsByLOD,
		Snapshot.LastFormationTargetUpdatedEntities,
		Snapshot.LastFormationTargetSkippedEntitiesByLOD,
		Snapshot.LastNeighborQueryCount,
		Snapshot.LastNeighborQueriesSkippedByLOD,
		Snapshot.LastHighLODNeighborQueryCount,
		Snapshot.LastMediumLODNeighborQueryCount,
		Snapshot.LastLowLODNeighborQueryCount,
		Snapshot.LastAvoidanceAppliedCount,
		Snapshot.LastFormationMilliseconds,
		Snapshot.LastSpatialGridBuildMilliseconds,
		Snapshot.LastNeighborQueryMilliseconds,
		Snapshot.LastAvoidanceMilliseconds,
		Snapshot.LastMovementMilliseconds
	);
	const double MeasurementSteps = static_cast<double>(
		FMath::Max<uint64>(Snapshot.SimLODMeasurementStepCount, 1)
	);
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 SimLOD measurement. StartTick=%llu, Steps=%llu, AvgFormationMs=%.3f, AvgNeighborMs=%.3f, AvgAvoidanceMs=%.3f, AvgMovementMs=%.3f, FormationGroups[Updated/Skipped]=%llu/%llu, FormationTargets[Updated/Skipped]=%llu/%llu, NeighborQueries[Executed/Skipped]=%llu/%llu"),
		Snapshot.SimLODMeasurementStartTick,
		Snapshot.SimLODMeasurementStepCount,
		Snapshot.SimLODMeasurementFormationMilliseconds / MeasurementSteps,
		Snapshot.SimLODMeasurementNeighborMilliseconds / MeasurementSteps,
		Snapshot.SimLODMeasurementAvoidanceMilliseconds / MeasurementSteps,
		Snapshot.SimLODMeasurementMovementMilliseconds / MeasurementSteps,
		Snapshot.SimLODMeasurementFormationUpdatedGroups,
		Snapshot.SimLODMeasurementFormationSkippedGroups,
		Snapshot.SimLODMeasurementFormationTargetUpdatedEntities,
		Snapshot.SimLODMeasurementFormationTargetSkippedEntities,
		Snapshot.SimLODMeasurementNeighborQueries,
		Snapshot.SimLODMeasurementNeighborQueriesSkipped
	);

	if (Group == nullptr)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("P0 group state unavailable. GroupId=%d"),
			GroupId.Value
		);
		return;
	}

	const FAlgonaGroupRestDiagnostics RestDiagnostics =
		CollectGroupRestDiagnostics(Group->Id);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P0 group state. GroupId=%d, Members=%d, SimLOD=%s, NavigationStride=%u, ActiveMove=%s, CommandSequence=%llu, Target=%s, MaxSlotError=%.1f, AvgSlotError=%.1f, MaxDesiredSpeed=%.1f, AvgDesiredSpeed=%.1f, MaxSeparationSpeed=%.1f, AvgSeparationSpeed=%.1f, MovingBySeparation=%d, SettledMembers=%d"),
		Group->Id.Value,
		Group->MemberCount,
		ToSimLODString(Group->NavigationLOD),
		static_cast<uint32>(Group->NavigationUpdateStride),
		Group->bHasActiveMoveTarget ? TEXT("true") : TEXT("false"),
		Group->CommandSequence,
		*Group->TargetPosition.ToCompactString(),
		RestDiagnostics.MaxSlotError,
		RestDiagnostics.AverageSlotError,
		RestDiagnostics.MaxDesiredSpeed,
		RestDiagnostics.AverageDesiredSpeed,
		RestDiagnostics.MaxSeparationSpeed,
		RestDiagnostics.AverageSeparationSpeed,
		RestDiagnostics.MovingBySeparationCount,
		RestDiagnostics.SettledMembers
	);
}

void UAlgonaSimulationSubsystem::DumpNavigationState(FSimGroupId GroupId) const
{
	const FAlgonaSimBenchmarkSnapshot Snapshot = GetBenchmarkSnapshot();
	const FAlgonaSimGroupState* Group = FindGroup(GroupId);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 scheduler state. Tick=%llu, NavigationEnabled=%s, FormationEnabled=%s, SpatialGridEnabled=%s, LocalSeparationEnabled=%s, HardBlockersEnabled=%s, StuckDetectionEnabled=%s, Queued=%d, InFlight=%d, PendingCompletions=%d, Submitted=%llu, Completed=%llu, Failed=%llu, Stale=%llu, SubmitBudget=%d, SchedulerMs=%.3f, RouteProgressMs=%.3f, FormationMs=%.3f, FormationGroups=%d, GridMs=%.3f, GridEntities=%d, GridCells=%d, NeighborMs=%.3f, NeighborQueries=%d, NeighborAvg=%.2f, NeighborMax=%d, AvoidanceMs=%.3f, HardBlockers=%d, LastInvalidated=%d, TotalRepaths=%llu, StuckGroups=%d, BlockedEntities=%d, TotalBlockedEntities=%llu"),
		Snapshot.SimulationTick,
		bP1NavigationEnabled ? TEXT("true") : TEXT("false"),
		bP1FormationEnabled ? TEXT("true") : TEXT("false"),
		bP1SpatialGridEnabled ? TEXT("true") : TEXT("false"),
		bP1LocalSeparationEnabled ? TEXT("true") : TEXT("false"),
		bP1HardBlockersEnabled ? TEXT("true") : TEXT("false"),
		bP1StuckDetectionEnabled ? TEXT("true") : TEXT("false"),
		Snapshot.QueuedPathRequestCount,
		Snapshot.InFlightPathRequestCount,
		Snapshot.PendingPathCompletionCount,
		Snapshot.SubmittedPathRequests,
		Snapshot.CompletedPathRequests,
		Snapshot.FailedPathRequests,
		Snapshot.RejectedStalePathResults,
		MaxPathRequestsSubmittedPerStep,
		Snapshot.LastPathSchedulerMilliseconds,
		Snapshot.LastRouteProgressMilliseconds,
		Snapshot.LastFormationMilliseconds,
		Snapshot.LastFormationUpdatedGroups,
		Snapshot.LastSpatialGridBuildMilliseconds,
		Snapshot.LastSpatialGridEntityCount,
		Snapshot.LastSpatialGridCellCount,
		Snapshot.LastNeighborQueryMilliseconds,
		Snapshot.LastNeighborQueryCount,
		Snapshot.LastNeighborAverageCandidates,
		Snapshot.LastNeighborMaxCandidates,
		Snapshot.LastAvoidanceMilliseconds,
		Snapshot.ActiveHardBlockerCount,
		Snapshot.LastRouteInvalidationCount,
		Snapshot.TotalRepathsRequested,
		Snapshot.LastStuckGroupCount,
		Snapshot.LastHardBlockerMovementBlockedEntities,
		Snapshot.TotalHardBlockerMovementBlockedEntities
	);

	if (Group == nullptr)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("P1 navigation group unavailable. GroupId=%d"),
			GroupId.Value
		);
		return;
	}

	const FAlgonaGroupNavigationState& Navigation = Group->Navigation;
	const FAlgonaGroupRoute& Route = Navigation.Route;
	const FAlgonaFormationState& Formation = Group->Formation;
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 navigation group. GroupId=%d, SimLOD=%s, NavigationStride=%u, Status=%s, Generation=%llu, PendingRequestId=%u, PendingGeneration=%llu, Destination=%s, RoutePoints=%d, CurrentPoint=%d, CurrentTarget=%s, Partial=%s, Valid=%s, CommitTick=%llu, LastFailure=%s, LastRepath=%s, StuckTicks=%d, LastProgressDist=%.1f, LastProgressSampleTick=%llu, LastRepathTick=%llu, NavData=%s, LastStart=%s, LastDestination=%s, ProjectionExtent=%s, StartProjected=%s, ProjectedStart=%s, DestinationProjected=%s, ProjectedDestination=%s, FacingMode=%s, FormationAnchor=%s, TravelDirection=%s, LayoutForward=%s, LayoutRight=%s, FacingDirection=%s, FrameDot=%.4f, OriginalColumns=%d, OriginalRows=%d, Slots=%d, Width=%.1f, Depth=%.1f, RequiredFootprintHalfWidth=%.1f, SafeAnchorLateralOffset=%.1f, ProfileFront=%.1f, ProfileRear=%.1f, ForwardSpeedScale=%.3f, MaxRequiredLateralSpeed=%.1f, SpeedLimitedMembers=%d, CorridorWidth=%.1f, CorridorHalfWidth=%.1f, CorridorLeftHalf=%.1f, CorridorRightHalf=%.1f, CorridorSamples=%d, LeftEdge=%s, RightEdge=%s, LocallyDeformed=%s, SettledTicks=%d, FormationTick=%llu"),
		Group->Id.Value,
		ToSimLODString(Group->NavigationLOD),
		static_cast<uint32>(Group->NavigationUpdateStride),
		ToNavigationStatusString(Navigation.Status),
		Navigation.RouteGeneration,
		Navigation.PendingRequestId,
		Navigation.PendingRequestGeneration,
		*Navigation.Destination.ToCompactString(),
		Route.Points.Num(),
		Route.CurrentPointIndex,
		*Route.CurrentMoveTarget.ToCompactString(),
		Route.bPartial ? TEXT("true") : TEXT("false"),
		Route.bValid ? TEXT("true") : TEXT("false"),
		Navigation.LastRouteCommitTick,
		*Navigation.LastFailureReason,
		*Navigation.LastRepathReason,
		Navigation.StuckTicks,
		Navigation.LastProgressDistanceToDestination,
		Navigation.LastProgressSampleTick,
		Navigation.LastRepathTick,
		*Navigation.LastNavigationDataName,
		*Navigation.LastPathStart.ToCompactString(),
		*Navigation.LastPathDestination.ToCompactString(),
		*Navigation.LastProjectionExtent.ToCompactString(),
		Navigation.bLastStartProjectionSucceeded ? TEXT("true") : TEXT("false"),
		*Navigation.LastProjectedStart.ToCompactString(),
		Navigation.bLastDestinationProjectionSucceeded ? TEXT("true") : TEXT("false"),
		*Navigation.LastProjectedDestination.ToCompactString(),
		ToMoveFacingModeString(Group->MoveFacingMode),
		*Formation.AnchorPosition.ToCompactString(),
		*Formation.TravelDirection.ToCompactString(),
		*Formation.LayoutForward.ToCompactString(),
		*Formation.LayoutRight.ToCompactString(),
		*Formation.FacingDirection.ToCompactString(),
		FVector::DotProduct(Formation.LayoutForward, Formation.LayoutRight),
		Formation.OriginalColumns,
		Formation.OriginalRows,
		Formation.SlotCount,
		Formation.Width,
		Formation.Depth,
		Formation.RequiredFootprintHalfWidth,
		Formation.SafeAnchorLateralOffset,
		Formation.CorridorProfileFrontOffset,
		Formation.CorridorProfileRearOffset,
		Formation.ForwardSpeedScale,
		Formation.MaxRequiredLateralSpeed,
		Formation.SpeedLimitedMembers,
		Formation.CorridorWidth,
		Formation.CorridorHalfWidth,
		Formation.CorridorLeftHalfWidth,
		Formation.CorridorRightHalfWidth,
		Formation.CorridorValidSamples,
		*Formation.LeftCorridorEdge.ToCompactString(),
		*Formation.RightCorridorEdge.ToCompactString(),
		Formation.bLocallyDeformed ? TEXT("true") : TEXT("false"),
		Formation.SettledTicks,
		Formation.LastUpdateTick
	);

	if (UWorld* World = GetWorld())
	{
		for (int32 PointIndex = 1; PointIndex < Route.Points.Num(); ++PointIndex)
		{
			DrawDebugLine(
				World,
				Route.Points[PointIndex - 1] + FVector(0.0, 0.0, 80.0),
				Route.Points[PointIndex] + FVector(0.0, 0.0, 80.0),
				FColor::Cyan,
				false,
				4.0f,
				0,
				10.0f
			);
		}
	}
}

void UAlgonaSimulationSubsystem::DumpFormationState(
	FSimGroupId GroupId,
	int32 MaxSamples) const
{
	const FAlgonaSimGroupState* Group = FindGroup(GroupId);
	if (Group == nullptr)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("P1 formation group unavailable. GroupId=%d"),
			GroupId.Value
		);
		return;
	}

	MaxSamples = FMath::Clamp(MaxSamples, 1, 32);
	const FAlgonaFormationState& Formation = Group->Formation;
	const int32 StableSlotSampleStride = FMath::Max(
		FMath::DivideAndRoundUp(
			FMath::Max(Formation.SlotCount, 1),
			MaxSamples
		),
		1
	);
	TArray<FString> Samples;
	Samples.Reserve(MaxSamples);
	TArray<FVector> OriginalTargets;
	TArray<FVector> LocalTargets;
	OriginalTargets.Reserve(MaxSamples);
	LocalTargets.Reserve(MaxSamples);

	if (MassRuntime != nullptr && MassRuntime->EntityManager != nullptr
		&& !MassRuntime->Entities.IsEmpty())
	{
		FMassEntityQuery SampleQuery(MassRuntime->EntityManager->AsShared());
		SampleQuery.AddRequirement<FAlgonaSimPositionFragment>(
			EMassFragmentAccess::ReadOnly
		);
		SampleQuery.AddRequirement<FAlgonaSimGroupFragment>(
			EMassFragmentAccess::ReadOnly
		);
		SampleQuery.AddRequirement<FAlgonaSimNavigationFragment>(
			EMassFragmentAccess::ReadOnly
		);
		SampleQuery.AddRequirement<FAlgonaSimVelocityFragment>(
			EMassFragmentAccess::ReadOnly
		);

		FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
		SampleQuery.ForEachEntityChunk(
			ExecutionContext,
			[this, Group, GroupId, MaxSamples, StableSlotSampleStride,
				&Formation, &Samples,
				&OriginalTargets, &LocalTargets](FMassExecutionContext& Context)
			{
				TConstArrayView<FAlgonaSimPositionFragment> Positions =
					Context.GetFragmentView<FAlgonaSimPositionFragment>();
				TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
					Context.GetFragmentView<FAlgonaSimGroupFragment>();
				TConstArrayView<FAlgonaSimNavigationFragment> NavigationFragments =
					Context.GetFragmentView<FAlgonaSimNavigationFragment>();
				TConstArrayView<FAlgonaSimVelocityFragment> Velocities =
					Context.GetFragmentView<FAlgonaSimVelocityFragment>();

				for (int32 Index = 0; Index < Context.GetNumEntities()
					&& Samples.Num() < MaxSamples; ++Index)
				{
					if (GroupIds[Index].GroupId == GroupId.Value)
					{
						const FAlgonaSimNavigationFragment& Navigation =
							NavigationFragments[Index];
						const bool bLastStableSlot =
							Navigation.SlotIndex == Formation.SlotCount - 1;
						if (!bLastStableSlot
							&& Navigation.SlotIndex % StableSlotSampleStride != 0)
						{
							continue;
						}
						double NearestSameGroupDistance = TNumericLimits<double>::Max();
						const FIntPoint CenterCell =
							GetSpatialGridCell(Positions[Index].Position);
						for (int32 CellY = CenterCell.Y - 1;
							CellY <= CenterCell.Y + 1; ++CellY)
						{
							for (int32 CellX = CenterCell.X - 1;
								CellX <= CenterCell.X + 1; ++CellX)
							{
								const TArray<int32>* CellEntries =
									SpatialGrid.Cells.Find(FIntPoint(CellX, CellY));
								if (CellEntries == nullptr)
								{
									continue;
								}
								for (const int32 EntryIndex : *CellEntries)
								{
									if (!SpatialGrid.Entries.IsValidIndex(EntryIndex))
									{
										continue;
									}
									const FAlgonaSpatialGridEntry& Entry =
										SpatialGrid.Entries[EntryIndex];
									if (Entry.Entity == Context.GetEntity(Index)
										|| !(Entry.GroupId == GroupId))
									{
										continue;
									}
									NearestSameGroupDistance = FMath::Min(
										NearestSameGroupDistance,
										FVector::Dist2D(
											Positions[Index].Position,
											Entry.Position
										)
									);
								}
							}
						}
						const double ReportedNeighborDistance =
							NearestSameGroupDistance < TNumericLimits<double>::Max()
								? NearestSameGroupDistance
								: -1.0;
						const FVector OriginalTarget =
							Formation.AnchorPosition
							+ ComputeFormationSlotOffset(
								Formation,
								Navigation.SlotIndex
							);
						const double TargetForwardDelta = FVector::DotProduct(
							Navigation.DesiredSlotPosition
								- Positions[Index].Position,
							Formation.TravelDirection);
						const double ActualForwardSpeed = FVector::DotProduct(
							Velocities[Index].Velocity,
							Formation.TravelDirection);
						Samples.Add(FString::Printf(
							TEXT("StableSlot=%d OriginalRow=%d OriginalColumn=%d OriginalTarget=%s LocalTarget=%s Phase=%s LocalSpacing=%.1f LocalCorridorWidth=%.1f MemberLongitudinal=%.1f ConstraintLongitudinal=%.1f NarrowDistance=%.1f RequiredLateralDistance=%.1f RequiredLateralSpeed=%.1f ForwardSpeedScale=%.3f SoftOverlap=%s TransitLane=-1 TransitOrder=-1 NeighborDistance=%.1f RouteProgress=%d/%d TargetForwardDelta=%.1f ActualForwardSpeed=%.1f DesiredVelocity=%s"),
							Navigation.SlotIndex,
							Navigation.OriginalSlotRow,
							Navigation.OriginalSlotColumn,
							*OriginalTarget.ToCompactString(),
							*Navigation.DesiredSlotPosition.ToCompactString(),
							ToFormationPhaseString(Navigation.FormationPhase),
							Navigation.LastLocalSpacing,
							Navigation.LastLocalCorridorWidth,
							Navigation.MemberLongitudinalOffset,
							Navigation.CorridorConstraintLongitudinalOffset,
							Navigation.DistanceToNarrowBoundary,
							Navigation.RequiredLateralDistance,
							Navigation.RequiredLateralSpeed,
							Navigation.RecommendedForwardSpeedScale,
							Navigation.bSoftOverlap ? TEXT("true") : TEXT("false"),
							ReportedNeighborDistance,
							Group->Navigation.Route.CurrentPointIndex,
							Group->Navigation.Route.Points.Num(),
							TargetForwardDelta,
							ActualForwardSpeed,
							*Navigation.DesiredVelocity.ToCompactString()
						));
						OriginalTargets.Add(OriginalTarget);
						LocalTargets.Add(Navigation.DesiredSlotPosition);
					}
				}
			}
		);
	}

	const FAlgonaGroupRestDiagnostics RestDiagnostics =
		CollectGroupRestDiagnostics(Group->Id);
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 formation group. GroupId=%d, Enabled=%s, SimLOD=%s, NavigationStride=%u, Phase=%s, FacingMode=%s, Anchor=%s, TravelDirection=%s, LayoutForward=%s, LayoutRight=%s, FacingDirection=%s, FrameDot=%.4f, OriginalColumns=%d, OriginalRows=%d, Slots=%d, Width=%.1f, Depth=%.1f, RequiredFootprintHalfWidth=%.1f, SafeAnchorLateralOffset=%.1f, ProfileFront=%.1f, ProfileRear=%.1f, ForwardSpeedScale=%.3f, MaxRequiredLateralSpeed=%.1f, SpeedLimitedMembers=%d, NormalSpacing=%.1f, MinSameGroupSpacing=%.1f, CorridorWidth=%.1f, CorridorHalfWidth=%.1f, CorridorLeftHalf=%.1f, CorridorRightHalf=%.1f, CorridorSamples=%d, LeftEdge=%s, RightEdge=%s, LocallyDeformed=%s, FunnelingMembers=%d, InsideChokeMembers=%d, ReExpandingMembers=%d, TransitMembers=0, TransitLanes=0, SoftOverlapMembers=%d, DebugCubeSize=%.1f, SettledTicks=%d, MaxSlotError=%.1f, AvgSlotError=%.1f, MaxDesiredSpeed=%.1f, AvgDesiredSpeed=%.1f, MaxSeparationSpeed=%.1f, AvgSeparationSpeed=%.1f, MovingBySeparation=%d, SettledMembers=%d, Samples=%s"),
		Group->Id.Value,
		bP1FormationEnabled ? TEXT("true") : TEXT("false"),
		ToSimLODString(Group->NavigationLOD),
		static_cast<uint32>(Group->NavigationUpdateStride),
		ToFormationPhaseString(Formation.Phase),
		ToMoveFacingModeString(Group->MoveFacingMode),
		*Formation.AnchorPosition.ToCompactString(),
		*Formation.TravelDirection.ToCompactString(),
		*Formation.LayoutForward.ToCompactString(),
		*Formation.LayoutRight.ToCompactString(),
		*Formation.FacingDirection.ToCompactString(),
		FVector::DotProduct(Formation.LayoutForward, Formation.LayoutRight),
		Formation.OriginalColumns,
		Formation.OriginalRows,
		Formation.SlotCount,
		Formation.Width,
		Formation.Depth,
		Formation.RequiredFootprintHalfWidth,
		Formation.SafeAnchorLateralOffset,
		Formation.CorridorProfileFrontOffset,
		Formation.CorridorProfileRearOffset,
		Formation.ForwardSpeedScale,
		Formation.MaxRequiredLateralSpeed,
		Formation.SpeedLimitedMembers,
		Formation.SlotSpacing,
		Group->MinSameGroupSpacing,
		Formation.CorridorWidth,
		Formation.CorridorHalfWidth,
		Formation.CorridorLeftHalfWidth,
		Formation.CorridorRightHalfWidth,
		Formation.CorridorValidSamples,
		*Formation.LeftCorridorEdge.ToCompactString(),
		*Formation.RightCorridorEdge.ToCompactString(),
		Formation.bLocallyDeformed ? TEXT("true") : TEXT("false"),
		Formation.MembersFunneling,
		Formation.MembersInsideChoke,
		Formation.MembersReExpanding,
		Formation.SoftOverlapMembers,
		Group->DebugPresentationCubeSize,
		Formation.SettledTicks,
		RestDiagnostics.MaxSlotError,
		RestDiagnostics.AverageSlotError,
		RestDiagnostics.MaxDesiredSpeed,
		RestDiagnostics.AverageDesiredSpeed,
		RestDiagnostics.MaxSeparationSpeed,
		RestDiagnostics.AverageSeparationSpeed,
		RestDiagnostics.MovingBySeparationCount,
		RestDiagnostics.SettledMembers,
		*FString::Join(Samples, TEXT(" | "))
	);

	if (UWorld* World = GetWorld())
	{
		for (const FAlgonaFormationCorridorSample& Sample : Formation.CorridorSamples)
		{
			if (!Sample.bValid)
			{
				continue;
			}
			DrawDebugLine(
				World,
				Sample.Center - Formation.LayoutRight * Sample.LeftHalfWidth
					+ FVector(0.0, 0.0, 50.0),
				Sample.Center + Formation.LayoutRight * Sample.RightHalfWidth
					+ FVector(0.0, 0.0, 50.0),
				FColor::Yellow,
				false,
				4.0f,
				0,
				8.0f
			);
		}
		for (int32 Index = 0; Index < OriginalTargets.Num(); ++Index)
		{
			DrawDebugLine(
				World,
				OriginalTargets[Index] + FVector(0.0, 0.0, 35.0),
				LocalTargets[Index] + FVector(0.0, 0.0, 35.0),
				FColor::Magenta,
				false,
				4.0f,
				0,
				5.0f
			);
		}
	}
}

bool UAlgonaSimulationSubsystem::SetDebugLocalMotionMode(
	const FString& ModeName)
{
	EAlgonaLocalMotionMode ParsedMode = EAlgonaLocalMotionMode::Legacy;
	if (ModeName.Equals(TEXT("Legacy"), ESearchCase::IgnoreCase))
	{
		ParsedMode = EAlgonaLocalMotionMode::Legacy;
	}
	else if (ModeName.Equals(TEXT("MassStock"), ESearchCase::IgnoreCase))
	{
		ParsedMode = EAlgonaLocalMotionMode::MassStock;
	}
	else
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P1 local motion mode rejected: unknown mode '%s'. Expected Legacy or MassStock."), *ModeName);
		return false;
	}

	RequestedLocalMotionMode = ParsedMode;
	const bool bHasPopulation =
		MassRuntime != nullptr && !MassRuntime->Entities.IsEmpty();
	if (!bHasPopulation)
	{
		ActiveLocalMotionMode = RequestedLocalMotionMode;
	}

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 local motion mode selected. Requested=%s, ActivePopulation=%s, AppliesOnNextSpawn=%s"),
		ToLocalMotionModeString(RequestedLocalMotionMode),
		ToLocalMotionModeString(ActiveLocalMotionMode),
		bHasPopulation && RequestedLocalMotionMode != ActiveLocalMotionMode
			? TEXT("true")
			: TEXT("false"));
	return true;
}

void UAlgonaSimulationSubsystem::DumpLocalMotionState(
	FSimGroupId GroupId)
{
	const FAlgonaSimGroupState* Group = FindGroup(GroupId);
	if (Group == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P1 local motion dump failed: GroupId=%d does not exist."), GroupId.Value);
		return;
	}

	FAlgonaLocalMotionDiagnostics Diagnostics;
	TArray<FVector> GroupPositions;
	GroupPositions.Reserve(FMath::Max(Group->MemberCount, 0));
	double TotalSpeed = 0.0;
	double TotalForwardSpeed = 0.0;
	double TotalDesiredSpeed = 0.0;
	double TotalAgentRadius = 0.0;
	double TotalSlotError = 0.0;
	double MinimumForwardSpeed = TNumericLimits<double>::Max();
	double SeparationRadiusScale = 0.0;
	double PredictiveAvoidanceRadiusScale = 0.0;
	double StaticObstacleClearanceScale = 0.0;
	double ObstacleSeparationDistance = 0.0;
	double PredictiveAvoidanceTime = 0.0;
	double PredictiveAvoidanceDistance = 0.0;
	double EnvironmentPredictiveAvoidanceTime = 0.0;
	double EnvironmentPredictiveAvoidanceDistance = 0.0;
	double EnvironmentPredictiveAvoidanceStiffness = 0.0;
	double EnvironmentSeparationDistance = 0.0;
	double EnvironmentSeparationStiffness = 0.0;
	bool bReadAvoidanceParameters = false;
	if (MassRuntime != nullptr && MassRuntime->EntityManager != nullptr)
	{
		for (const FMassEntityHandle Entity : MassRuntime->Entities)
		{
			FMassEntityView EntityView(*MassRuntime->EntityManager, Entity);
			if (EntityView.GetFragmentData<FAlgonaSimGroupFragment>().GroupId
				!= GroupId.Value)
			{
				continue;
			}

			++Diagnostics.EntitiesProcessed;
			const FAlgonaSimPositionFragment& Position =
				EntityView.GetFragmentData<FAlgonaSimPositionFragment>();
			const FAlgonaSimNavigationFragment& Navigation =
				EntityView.GetFragmentData<FAlgonaSimNavigationFragment>();
			GroupPositions.Add(Position.Position);
			const double SlotError = FVector::Dist(
				Position.Position,
				Navigation.DesiredSlotPosition);
			TotalSlotError += SlotError;
			Diagnostics.MaxSlotError = FMath::Max(
				Diagnostics.MaxSlotError,
				SlotError);

			double Speed = 0.0;
			double DesiredSpeed = Navigation.DesiredVelocity.Length();
			double AgentRadius = Group->MinSameGroupSpacing * 0.5;
			if (ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock)
			{
				const FVector StockVelocity = EntityView
					.GetFragmentData<FMassVelocityFragment>().Value;
				Speed = StockVelocity.Length();
				const FMassMoveTargetFragment& MoveTarget = EntityView
					.GetFragmentData<FMassMoveTargetFragment>();
				DesiredSpeed = MoveTarget.DesiredSpeed.Get();
				AgentRadius = EntityView
					.GetFragmentData<FAgentRadiusFragment>().Radius;
				Diagnostics.AvoidanceEdges += EntityView
					.GetFragmentData<FMassNavigationEdgesFragment>()
					.AvoidanceEdges.Num();
				if (!bReadAvoidanceParameters)
				{
					const FMassMovingAvoidanceParameters& Parameters = EntityView
						.GetConstSharedFragmentData<
							FMassMovingAvoidanceParameters>();
					SeparationRadiusScale = Parameters.SeparationRadiusScale;
					PredictiveAvoidanceRadiusScale =
						Parameters.PredictiveAvoidanceRadiusScale;
					StaticObstacleClearanceScale =
						Parameters.StaticObstacleClearanceScale;
					ObstacleSeparationDistance =
						Parameters.ObstacleSeparationDistance;
					PredictiveAvoidanceTime =
						Parameters.PredictiveAvoidanceTime;
					PredictiveAvoidanceDistance =
						Parameters.PredictiveAvoidanceDistance;
					EnvironmentPredictiveAvoidanceTime =
						Parameters.EnvironmentPredictiveAvoidanceTime;
					EnvironmentPredictiveAvoidanceDistance =
						Parameters.EnvironmentPredictiveAvoidanceDistance;
					EnvironmentPredictiveAvoidanceStiffness =
						Parameters.EnvironmentPredictiveAvoidanceStiffness;
					EnvironmentSeparationDistance =
						Parameters.EnvironmentSeparationDistance;
					EnvironmentSeparationStiffness =
						Parameters.EnvironmentSeparationStiffness;
					bReadAvoidanceParameters = true;
				}
			}
			else
			{
				Speed = EntityView
					.GetFragmentData<FAlgonaSimVelocityFragment>()
					.Velocity.Length();
			}
			TotalSpeed += Speed;
			TotalDesiredSpeed += DesiredSpeed;
			TotalAgentRadius += AgentRadius;
			const FVector EntityVelocity = EntityView
				.GetFragmentData<FAlgonaSimVelocityFragment>().Velocity;
			const double ForwardSpeed = FVector::DotProduct(
				EntityVelocity,
				Group->Formation.TravelDirection);
			TotalForwardSpeed += ForwardSpeed;
			MinimumForwardSpeed = FMath::Min(
				MinimumForwardSpeed,
				ForwardSpeed);
			Diagnostics.MaxSpeed = FMath::Max(Diagnostics.MaxSpeed, Speed);
			if (Speed > 1.0)
			{
				++Diagnostics.MovingEntities;
			}
		}

		Diagnostics.AverageSpeed = Diagnostics.EntitiesProcessed > 0
			? TotalSpeed / static_cast<double>(Diagnostics.EntitiesProcessed)
			: 0.0;
		Diagnostics.AverageSlotError = Diagnostics.EntitiesProcessed > 0
			? TotalSlotError / static_cast<double>(Diagnostics.EntitiesProcessed)
			: 0.0;
		Diagnostics.MoveTargets = Group->bHasActiveMoveTarget
			? Diagnostics.EntitiesProcessed
			: 0;
		Diagnostics.AvoidanceEntities = Diagnostics.MovingEntities;
		if (ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock)
		{
			Diagnostics.AverageForwardSpeed = Diagnostics.EntitiesProcessed > 0
				? TotalForwardSpeed
					/ static_cast<double>(Diagnostics.EntitiesProcessed)
				: 0.0;
			Diagnostics.MinForwardSpeed = MinimumForwardSpeed
				< TNumericLimits<double>::Max()
				? MinimumForwardSpeed
				: 0.0;
			Diagnostics.AverageDesiredSpeed = Diagnostics.EntitiesProcessed > 0
				? TotalDesiredSpeed
					/ static_cast<double>(Diagnostics.EntitiesProcessed)
				: 0.0;
			Diagnostics.AverageAgentRadius = Diagnostics.EntitiesProcessed > 0
				? TotalAgentRadius
					/ static_cast<double>(Diagnostics.EntitiesProcessed)
				: 0.0;
			Diagnostics.MovementMilliseconds =
				MassRuntime->LocalMotionDiagnostics.MovementMilliseconds;
			Diagnostics.SteeringMilliseconds =
				MassRuntime->LocalMotionDiagnostics.SteeringMilliseconds;
			Diagnostics.AvoidanceMilliseconds =
				MassRuntime->LocalMotionDiagnostics.AvoidanceMilliseconds;
			Diagnostics.BoundaryAdapterMilliseconds =
				MassRuntime->LocalMotionDiagnostics.BoundaryAdapterMilliseconds;
			Diagnostics.CorridorClampedEntities =
				MassRuntime->LocalMotionDiagnostics.CorridorClampedEntities;
			Diagnostics.CorridorConstraintCount =
				MassRuntime->LocalMotionDiagnostics.CorridorConstraintCount;
			Diagnostics.AverageConstraintCorrection = MassRuntime
				->LocalMotionDiagnostics.AverageConstraintCorrection;
			Diagnostics.MaxConstraintCorrection =
				MassRuntime->LocalMotionDiagnostics.MaxConstraintCorrection;
			Diagnostics.MaxPenetrationDepth =
				MassRuntime->LocalMotionDiagnostics.MaxPenetrationDepth;
			Diagnostics.AverageNormalMotionClipped = MassRuntime
				->LocalMotionDiagnostics.AverageNormalMotionClipped;
			Diagnostics.MaxNormalMotionClipped =
				MassRuntime->LocalMotionDiagnostics.MaxNormalMotionClipped;
		}
		else
		{
			Diagnostics.MovementMilliseconds =
				BenchmarkSnapshot.LastMovementMilliseconds;
			Diagnostics.AvoidanceMilliseconds =
				BenchmarkSnapshot.LastAvoidanceMilliseconds;
		}
	}

	int32 TotalSameGroupNeighbors = 0;
	int32 MaxSameGroupNeighbors = 0;
	const double SameGroupAvoidanceRange =
		Diagnostics.AverageAgentRadius * SeparationRadiusScale
		+ Diagnostics.AverageAgentRadius
		+ ObstacleSeparationDistance;
	const double SameGroupAvoidanceRangeSquared =
		FMath::Square(SameGroupAvoidanceRange);
	static constexpr int32 MaxNeighborDiagnosticSamples = 64;
	const int32 NeighborSampleCount = FMath::Min(
		GroupPositions.Num(),
		MaxNeighborDiagnosticSamples);
	if (SameGroupAvoidanceRange > 0.0)
	{
		for (int32 SampleIndex = 0;
			SampleIndex < NeighborSampleCount;
			++SampleIndex)
		{
			const int32 MemberIndex = NeighborSampleCount > 1
				? FMath::RoundToInt(
					static_cast<double>(SampleIndex)
						* static_cast<double>(GroupPositions.Num() - 1)
						/ static_cast<double>(NeighborSampleCount - 1))
				: 0;
			int32 NeighborCount = 0;
			for (int32 OtherIndex = 0;
				OtherIndex < GroupPositions.Num();
				++OtherIndex)
			{
				if (MemberIndex != OtherIndex
					&& FVector::DistSquared2D(
						GroupPositions[MemberIndex],
						GroupPositions[OtherIndex])
						<= SameGroupAvoidanceRangeSquared)
				{
					++NeighborCount;
				}
			}
			TotalSameGroupNeighbors += NeighborCount;
			MaxSameGroupNeighbors = FMath::Max(
				MaxSameGroupNeighbors,
				NeighborCount);
		}
	}
	const double AverageSameGroupNeighbors = NeighborSampleCount == 0
		? 0.0
		: static_cast<double>(TotalSameGroupNeighbors)
			/ static_cast<double>(NeighborSampleCount);
	const double AvailableCorridorWidth = FMath::Max(
		static_cast<double>(Group->Formation.CorridorWidth)
			- 2.0 * (FormationCorridorSafetyMargin
				+ Diagnostics.AverageAgentRadius),
		0.0);
	const double CurrentSimulationTime =
		static_cast<double>(SimulationTick) * FixedDeltaSeconds;
	const double MoveEndTime = Group->Navigation.RouteArrivalTimeSeconds >= 0.0
		? Group->Navigation.RouteArrivalTimeSeconds
		: CurrentSimulationTime;
	const double ActualMoveTime = Group->Navigation.CommandStartTimeSeconds >= 0.0
		? FMath::Max(
			MoveEndTime - Group->Navigation.CommandStartTimeSeconds,
			0.0)
		: -1.0;
	const double RequestedMoveTime = Group->Navigation.RouteLength > 0.0
		? Group->Navigation.RouteLength / FMath::Max(MovementSpeed, 1.0)
		: -1.0;
	const double BridgeTraversalTime =
		Group->Navigation.BridgeEntryTimeSeconds >= 0.0
			? FMath::Max(
				(Group->Navigation.BridgeExitTimeSeconds >= 0.0
					? Group->Navigation.BridgeExitTimeSeconds
					: CurrentSimulationTime)
					- Group->Navigation.BridgeEntryTimeSeconds,
				0.0)
			: -1.0;
	const double SettlingTime =
		Group->Navigation.RouteArrivalTimeSeconds >= 0.0
			? FMath::Max(
				(Group->Navigation.IdleTimeSeconds >= 0.0
					? Group->Navigation.IdleTimeSeconds
					: CurrentSimulationTime)
					- Group->Navigation.RouteArrivalTimeSeconds,
				0.0)
			: -1.0;
	const bool bWaitingForFormation = Group->Navigation.Status
		== EAlgonaGroupNavigationStatus::Settling;
	const TCHAR* SpeedLimitReason =
		ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock
			? TEXT("NoneMassStockFullSpeed")
			: Group->Formation.ForwardSpeedScale < 0.999f
				? TEXT("FormationLateralDemand")
				: TEXT("None");
	FAlgonaLocalMotionForceDiagnostics ForceDiagnostics;
	if (MassRuntime != nullptr
		&& MassRuntime->LocalMotionForceDiagnostics.bValid
		&& MassRuntime->LocalMotionForceDiagnostics.GroupId == GroupId)
	{
		ForceDiagnostics = MassRuntime->LocalMotionForceDiagnostics;
	}

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 local motion. RequestedMode=%s, ActiveMode=%s, GroupId=%d, Status=%s, FullCadence=%s, EntitiesProcessed=%d, MovingEntities=%d, MoveTargets=%d, AvoidanceEntities=%d, AvoidanceEdges=%d, DesiredSpeed=%.2f, AverageActualSpeed=%.2f, MaxActualSpeed=%.2f, AverageForwardSpeed=%.2f, MinForwardSpeed=%.2f, AverageSlotError=%.2f, MaxSlotError=%.2f, AgentRadius=%.2f, MinSameGroupSpacing=%.2f, CorridorWidth=%.2f, AvailableCorridorWidth=%.2f, SeparationRadiusScale=%.3f, PredictiveAvoidanceRadiusScale=%.3f, StaticObstacleClearanceScale=%.3f, SameGroupAvoidanceRange=%.2f, SameGroupNeighborAvg=%.2f, SameGroupNeighborMax=%d, CorridorConstraintEntities=%d, CorridorConstraintCount=%d, AverageConstraintCorrection=%.3f, MaxConstraintCorrection=%.3f, MaxPenetrationDepth=%.3f, AverageNormalMotionClipped=%.3f, MaxNormalMotionClipped=%.3f, SteeringForceAvg=%.2f, SteeringForceMax=%.2f, MovingAvoidanceForceAvg=%.2f, MovingAvoidanceForceMax=%.2f, EnvironmentAvoidanceResidualAvg=%.2f, EnvironmentAvoidanceResidualMax=%.2f, ForceSamples=%d, RouteLength=%.2f, RequestedMoveTime=%.3f, ActualMoveTime=%.3f, BridgeTraversalTime=%.3f, SettlingTime=%.3f, SpeedLimitReason=%s, WaitingForFormation=%s, MovementMs=%.3f, SteeringMs=%.3f, AvoidanceMs=%.3f, BoundaryAdapterMs=%.3f"),
		ToLocalMotionModeString(RequestedLocalMotionMode),
		ToLocalMotionModeString(ActiveLocalMotionMode),
		GroupId.Value,
		ToNavigationStatusString(Group->Navigation.Status),
		ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock
			? TEXT("true")
			: TEXT("false"),
		Diagnostics.EntitiesProcessed,
		Diagnostics.MovingEntities,
		Diagnostics.MoveTargets,
		Diagnostics.AvoidanceEntities,
		Diagnostics.AvoidanceEdges,
		Diagnostics.AverageDesiredSpeed,
		Diagnostics.AverageSpeed,
		Diagnostics.MaxSpeed,
		Diagnostics.AverageForwardSpeed,
		Diagnostics.MinForwardSpeed,
		Diagnostics.AverageSlotError,
		Diagnostics.MaxSlotError,
		Diagnostics.AverageAgentRadius,
		Group->MinSameGroupSpacing,
		Group->Formation.CorridorWidth,
		AvailableCorridorWidth,
		SeparationRadiusScale,
		PredictiveAvoidanceRadiusScale,
		StaticObstacleClearanceScale,
		SameGroupAvoidanceRange,
		AverageSameGroupNeighbors,
		MaxSameGroupNeighbors,
		Diagnostics.CorridorClampedEntities,
		Diagnostics.CorridorConstraintCount,
		Diagnostics.AverageConstraintCorrection,
		Diagnostics.MaxConstraintCorrection,
		Diagnostics.MaxPenetrationDepth,
		Diagnostics.AverageNormalMotionClipped,
		Diagnostics.MaxNormalMotionClipped,
		ForceDiagnostics.AverageSteeringForce,
		ForceDiagnostics.MaxSteeringForce,
		ForceDiagnostics.AverageMovingAvoidanceForce,
		ForceDiagnostics.MaxMovingAvoidanceForce,
		ForceDiagnostics.AverageEnvironmentAvoidanceForce,
		ForceDiagnostics.MaxEnvironmentAvoidanceForce,
		ForceDiagnostics.SampleCount,
		Group->Navigation.RouteLength,
		RequestedMoveTime,
		ActualMoveTime,
		BridgeTraversalTime,
		SettlingTime,
		SpeedLimitReason,
		bWaitingForFormation ? TEXT("true") : TEXT("false"),
		Diagnostics.MovementMilliseconds,
		Diagnostics.SteeringMilliseconds,
		Diagnostics.AvoidanceMilliseconds,
		Diagnostics.BoundaryAdapterMilliseconds);
	if (bReadAvoidanceParameters)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Log,
			TEXT("P1 local motion avoidance parameters. GroupId=%d, SeparationRadiusScale=%.3f, ObstacleSeparationDistance=%.1f, PredictiveAvoidanceRadiusScale=%.3f, PredictiveAvoidanceTime=%.2f, PredictiveAvoidanceDistance=%.1f, StaticObstacleClearanceScale=%.3f, EnvironmentPredictiveAvoidanceTime=%.2f, EnvironmentPredictiveAvoidanceDistance=%.1f, EnvironmentPredictiveAvoidanceStiffness=%.1f, EnvironmentSeparationDistance=%.1f, EnvironmentSeparationStiffness=%.1f"),
			GroupId.Value,
			SeparationRadiusScale,
			ObstacleSeparationDistance,
			PredictiveAvoidanceRadiusScale,
			PredictiveAvoidanceTime,
			PredictiveAvoidanceDistance,
			StaticObstacleClearanceScale,
			EnvironmentPredictiveAvoidanceTime,
			EnvironmentPredictiveAvoidanceDistance,
			EnvironmentPredictiveAvoidanceStiffness,
			EnvironmentSeparationDistance,
			EnvironmentSeparationStiffness);
	}

	if (ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock
		&& Group->bHasActiveMoveTarget
		&& IsNavigationMovementReady(*Group))
	{
		LocalMotionForceCaptureGroupId = GroupId;
		bCaptureLocalMotionForcesNextStep = true;
		UE_LOG(
			LogAlgonaSimulation,
			Log,
			TEXT("P1 local motion force capture armed. GroupId=%d, SamplesNextFixedStep=8"),
			GroupId.Value);
	}
}

int32 UAlgonaSimulationSubsystem::SetDebugSimLOD(
	EAlgonaSimLOD LOD,
	FSimGroupId GroupId)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P1 SimLOD override rejected on non-authoritative world."));
		return 0;
	}
	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P1 SimLOD override failed: no simulation entities."));
		return 0;
	}

	if (GroupId.IsValid() && FindGroup(GroupId) == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P1 SimLOD override failed: GroupId=%d does not exist."), GroupId.Value);
		return 0;
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		if (!GroupId.IsValid() || Group.Id == GroupId)
		{
			Group.HighLODMemberCount = 0;
			Group.MediumLODMemberCount = 0;
			Group.LowLODMemberCount = 0;
		}
	}

	const uint8 UpdateStride = GetLODUpdateStride(LOD);
	int32 UpdatedEntities = 0;
	for (const FMassEntityHandle Entity : MassRuntime->Entities)
	{
		FMassEntityView EntityView(*MassRuntime->EntityManager, Entity);
		const int32 EntityGroupId =
			EntityView.GetFragmentData<FAlgonaSimGroupFragment>().GroupId;
		if (GroupId.IsValid() && EntityGroupId != GroupId.Value)
		{
			continue;
		}

		FAlgonaSimLODFragment& LODFragment =
			EntityView.GetFragmentData<FAlgonaSimLODFragment>();
		const FAlgonaSimNavigationFragment& NavigationFragment =
			EntityView.GetFragmentData<FAlgonaSimNavigationFragment>();
		LODFragment.LOD = LOD;
		LODFragment.UpdateStride = UpdateStride;
		LODFragment.TickOffset = static_cast<uint8>(
			FMath::Max(NavigationFragment.SlotIndex, 0)
			% FMath::Max<int32>(UpdateStride, 1)
		);

		const int32 GroupArrayIndex = EntityGroupId - 1;
		if (Groups.IsValidIndex(GroupArrayIndex)
			&& Groups[GroupArrayIndex].Id.Value == EntityGroupId)
		{
			FAlgonaSimGroupState& Group = Groups[GroupArrayIndex];
			switch (LOD)
			{
			case EAlgonaSimLOD::High:
				++Group.HighLODMemberCount;
				break;
			case EAlgonaSimLOD::Medium:
				++Group.MediumLODMemberCount;
				break;
			case EAlgonaSimLOD::Low:
				++Group.LowLODMemberCount;
				break;
			default:
				break;
			}
		}
		++UpdatedEntities;
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		if (!GroupId.IsValid() || Group.Id == GroupId)
		{
			RefreshDerivedGroupSimLOD(Group);
		}
	}
	RefreshSimLODStatistics();
	ResetSimLODMeasurement();
	const FString Scope = GroupId.IsValid()
		? FString::Printf(TEXT("GroupId=%d"), GroupId.Value)
		: TEXT("All");

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 SimLOD override applied. Tier=%s, Scope=%s, UpdatedEntities=%d, HighEntities=%d, MediumEntities=%d, LowEntities=%d, HighGroups=%d, MediumGroups=%d, LowGroups=%d"),
		ToSimLODString(LOD),
		*Scope,
		UpdatedEntities,
		BenchmarkSnapshot.HighLODEntityCount,
		BenchmarkSnapshot.MediumLODEntityCount,
		BenchmarkSnapshot.LowLODEntityCount,
		BenchmarkSnapshot.HighLODGroupCount,
		BenchmarkSnapshot.MediumLODGroupCount,
		BenchmarkSnapshot.LowLODGroupCount
	);
	return UpdatedEntities;
}

bool UAlgonaSimulationSubsystem::SetDebugGroupMinSpacing(
	FSimGroupId GroupId,
	double Spacing)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P1 group minimum spacing override rejected on non-authoritative world."));
		return false;
	}

	FAlgonaSimGroupState* Group = FindGroup(GroupId);
	if (Group == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P1 group minimum spacing override failed: GroupId=%d does not exist."), GroupId.Value);
		return false;
	}

	Group->MinSameGroupSpacing =
		static_cast<float>(FMath::Max(Spacing, 1.0));
	Group->Formation.LastUpdateTick = 0;
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 group minimum spacing override applied. GroupId=%d, MinSameGroupSpacing=%.1f"),
		GroupId.Value,
		Group->MinSameGroupSpacing
	);
	return true;
}

bool UAlgonaSimulationSubsystem::SetDebugGroupCubeSize(
	FSimGroupId GroupId,
	double CubeSize)
{
	if (!IsAuthoritativeSimulationWorld())
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P0 group debug cube size override rejected on non-authoritative world."));
		return false;
	}

	FAlgonaSimGroupState* Group = FindGroup(GroupId);
	if (Group == nullptr)
	{
		UE_LOG(LogAlgonaSimulation, Warning, TEXT("P0 group debug cube size override failed: GroupId=%d does not exist."), GroupId.Value);
		return false;
	}

	Group->DebugPresentationCubeSize =
		static_cast<float>(FMath::Max(CubeSize, 1.0));
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P0 group debug cube size override applied. GroupId=%d, CubeSize=%.1f"),
		GroupId.Value,
		Group->DebugPresentationCubeSize
	);
	return true;
}

int32 UAlgonaSimulationSubsystem::AddDebugHardBlocker(
	const FVector& Center,
	double Radius)
{
	if (!bP1HardBlockersEnabled)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("P1 hard blocker add ignored because hard blockers are disabled.")
		);
		return INDEX_NONE;
	}

	Radius = FMath::Max(Radius, 1.0);
	FAlgonaHardBlocker& Blocker = HardBlockers.AddDefaulted_GetRef();
	Blocker.Id = NextHardBlockerId++;
	Blocker.Center = Center;
	Blocker.Radius = Radius;
	Blocker.Revision = ++HardBlockerRevision;
	BenchmarkSnapshot.ActiveHardBlockerCount = HardBlockers.Num();

	const int32 InvalidatedGroups =
		InvalidateRoutesForHardBlockers(TEXT("HardBlockerAdded"));

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 hard blocker added. Id=%d, Center=%s, Radius=%.1f, Active=%d, InvalidatedGroups=%d"),
		Blocker.Id,
		*Blocker.Center.ToCompactString(),
		Blocker.Radius,
		HardBlockers.Num(),
		InvalidatedGroups
	);
	return Blocker.Id;
}

int32 UAlgonaSimulationSubsystem::AddDebugHardBlockerAtGroup(
	FSimGroupId GroupId,
	double Radius)
{
	const FAlgonaSimGroupState* Group = FindGroup(GroupId);
	if (Group == nullptr)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("P1 hard blocker add-at-group failed. GroupId=%d"),
			GroupId.Value
		);
		return INDEX_NONE;
	}

	FVector Center = Group->TargetPosition;
	FVector GroupCenter = ComputeGroupCenter(GroupId);
	FVector Direction = FVector::ZeroVector;
	if (Group->Navigation.Route.bValid
		&& Group->Navigation.Route.Points.IsValidIndex(
			Group->Navigation.Route.CurrentPointIndex))
	{
		Direction = Group->Navigation.Route.CurrentMoveTarget - GroupCenter;
	}
	if (Direction.IsNearlyZero() && Group->Formation.LastUpdateTick != 0)
	{
		Direction = Group->Formation.AnchorPosition - GroupCenter;
	}
	if (Direction.IsNearlyZero())
	{
		Direction = Group->TargetPosition - GroupCenter;
	}
	Direction.Z = 0.0;
	if (!Direction.Normalize())
	{
		Direction = Group->Formation.TravelDirection;
		Direction.Z = 0.0;
		if (!Direction.Normalize())
		{
			Direction = FVector::ForwardVector;
		}
	}

	const double AheadDistance = FMath::Max(
		Radius * 0.75,
		FormationSlotSpacing * 2.0
	);
	Center = GroupCenter + Direction * AheadDistance;
	Center.Z = Group->Navigation.Route.bValid
		? Group->Navigation.Route.CurrentMoveTarget.Z
		: GroupCenter.Z;

	const int32 BlockerId = AddDebugHardBlocker(Center, Radius);
	FAlgonaSimGroupState* MutableGroup = FindGroup(GroupId);
	if (MutableGroup != nullptr
		&& MutableGroup->bHasActiveMoveTarget
		&& MutableGroup->Navigation.Route.bValid
		&& MutableGroup->Navigation.LastRepathTick != SimulationTick)
	{
		QueueRepathForGroup(*MutableGroup, TEXT("HardBlockerAdded"), 10);
	}

	return BlockerId;
}

int32 UAlgonaSimulationSubsystem::ClearDebugHardBlockers()
{
	const int32 RemovedCount = HardBlockers.Num();
	HardBlockers.Reset();
	++HardBlockerRevision;
	BenchmarkSnapshot.ActiveHardBlockerCount = 0;
	BenchmarkSnapshot.LastRouteInvalidationCount = 0;

	int32 RequeuedGroups = 0;
	for (FAlgonaSimGroupState& Group : Groups)
	{
		if (!Group.bHasActiveMoveTarget)
		{
			continue;
		}
		if (Group.Navigation.LastRepathReason == TEXT("HardBlockerAdded")
			|| Group.Navigation.LastFailureReason == TEXT("RouteBlockedByHardBlocker"))
		{
			QueueRepathForGroup(Group, TEXT("HardBlockerCleared"), 1);
			++RequeuedGroups;
		}
	}

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 hard blockers cleared. Removed=%d, RequeuedGroups=%d"),
		RemovedCount,
		RequeuedGroups
	);
	return RemovedCount;
}

void UAlgonaSimulationSubsystem::DumpDebugHardBlockers() const
{
	TArray<FString> Samples;
	const int32 MaxSamples = FMath::Min(HardBlockers.Num(), 8);
	Samples.Reserve(MaxSamples);
	for (int32 Index = 0; Index < MaxSamples; ++Index)
	{
		const FAlgonaHardBlocker& Blocker = HardBlockers[Index];
		Samples.Add(FString::Printf(
			TEXT("Id=%d Center=%s Radius=%.1f Revision=%llu"),
			Blocker.Id,
			*Blocker.Center.ToCompactString(),
			Blocker.Radius,
			Blocker.Revision
		));
	}

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 hard blockers. Enabled=%s, Count=%d, Revision=%llu, LastInvalidated=%d, TotalRepaths=%llu, BlockedEntities=%d, TotalBlockedEntities=%llu, Samples=%s"),
		bP1HardBlockersEnabled ? TEXT("true") : TEXT("false"),
		HardBlockers.Num(),
		HardBlockerRevision,
		BenchmarkSnapshot.LastRouteInvalidationCount,
		BenchmarkSnapshot.TotalRepathsRequested,
		BenchmarkSnapshot.LastHardBlockerMovementBlockedEntities,
		BenchmarkSnapshot.TotalHardBlockerMovementBlockedEntities,
		*FString::Join(Samples, TEXT(" | "))
	);
}

void UAlgonaSimulationSubsystem::RunPopulationSmokeSuite(
	int32 GroupSize,
	int32 SimulationStepsPerSize)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_PopulationSmokeSuite);

	static constexpr int32 Counts[] = { 1000, 5000, 10000, 20000 };

	RefreshRuntimeConfiguration();

	GroupSize = FMath::Max(GroupSize, 1);
	SimulationStepsPerSize = FMath::Max(SimulationStepsPerSize, 1);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Population suite started. GroupSize=%d, FixedStepsPerSize=%d, LODEnabled=%s"),
		GroupSize,
		SimulationStepsPerSize,
		bSimulationLODEnabled ? TEXT("true") : TEXT("false")
	);

	const bool bPreviousCommandDiagnostics = bCommandDiagnosticsEnabled;
	bCommandDiagnosticsEnabled = false;

	for (const int32 EntityCount : Counts)
	{
		const bool bSpawned = RepopulateSimulationEntities(EntityCount, GroupSize);
		const int32 SubmittedCommands = bSpawned
			? SubmitMoveAllGroupsCommand(FVector(20000.0, 20000.0, 0.0))
			: 0;

		TArray<double> MovementStepMilliseconds;
		MovementStepMilliseconds.Reserve(SimulationStepsPerSize);
		double TotalMoveMilliseconds = 0.0;
		for (int32 StepIndex = 0; StepIndex < SimulationStepsPerSize; ++StepIndex)
		{
			RunFixedSimulationStep();
			const FAlgonaSimBenchmarkSnapshot StepSnapshot =
				GetBenchmarkSnapshot();
			MovementStepMilliseconds.Add(StepSnapshot.LastMovementMilliseconds);
			TotalMoveMilliseconds += StepSnapshot.LastMovementMilliseconds;
		}
		MovementStepMilliseconds.Sort();

		const FAlgonaSimBenchmarkSnapshot Snapshot = GetBenchmarkSnapshot();
		const double AverageMoveMilliseconds =
			TotalMoveMilliseconds / static_cast<double>(SimulationStepsPerSize);
		UE_LOG(
			LogAlgonaSimulation,
			Log,
			TEXT("Population suite result. Entities=%d, Groups=%d, SubmittedCommands=%d, Tick=%llu, SpawnMs=%.3f, SpawnMemDeltaKB=%lld, UsedPhysicalMB=%llu, MoveAvgMs=%.3f, MoveP95Ms=%.3f, MoveP99Ms=%.3f, Visits=%d, Updated=%d, SkippedByLOD=%d"),
			Snapshot.EntityCount,
			Snapshot.GroupCount,
			SubmittedCommands,
			Snapshot.SimulationTick,
			Snapshot.LastSpawnMilliseconds,
			Snapshot.LastSpawnUsedPhysicalDeltaBytes / 1024,
			Snapshot.LastUsedPhysicalBytes / (1024ULL * 1024ULL),
			AverageMoveMilliseconds,
			GetSortedPercentile(MovementStepMilliseconds, 0.95),
			GetSortedPercentile(MovementStepMilliseconds, 0.99),
			Snapshot.LastMovementEntityVisits,
			Snapshot.LastMovementUpdatedEntities,
			Snapshot.LastMovementSkippedByLOD
		);

		DestroySimulationEntities();
	}

	bCommandDiagnosticsEnabled = bPreviousCommandDiagnostics;

	UE_LOG(LogAlgonaSimulation, Log, TEXT("Population suite finished."));
}

void UAlgonaSimulationSubsystem::RunCeilingSmoke(
	int32 EntityCount,
	int32 GroupSize,
	int32 SimulationSteps)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_CeilingSmoke);

	RefreshRuntimeConfiguration();

	EntityCount = FMath::Max(EntityCount, 0);
	GroupSize = FMath::Max(GroupSize, 1);
	SimulationSteps = FMath::Max(SimulationSteps, 1);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Ceiling smoke started. Entities=%d, GroupSize=%d, FixedSteps=%d, LODEnabled=%s"),
		EntityCount,
		GroupSize,
		SimulationSteps,
		bSimulationLODEnabled ? TEXT("true") : TEXT("false")
	);

	const bool bPreviousCommandDiagnostics = bCommandDiagnosticsEnabled;
	bCommandDiagnosticsEnabled = false;

	const bool bSpawned = RepopulateSimulationEntities(EntityCount, GroupSize);
	const int32 SubmittedCommands = bSpawned
		? SubmitMoveAllGroupsCommand(FVector(20000.0, 20000.0, 0.0))
		: 0;

	TArray<double> MovementStepMilliseconds;
	MovementStepMilliseconds.Reserve(SimulationSteps);
	double TotalMoveMilliseconds = 0.0;
	for (int32 StepIndex = 0; StepIndex < SimulationSteps; ++StepIndex)
	{
		RunFixedSimulationStep();
		const FAlgonaSimBenchmarkSnapshot StepSnapshot = GetBenchmarkSnapshot();
		MovementStepMilliseconds.Add(StepSnapshot.LastMovementMilliseconds);
		TotalMoveMilliseconds += StepSnapshot.LastMovementMilliseconds;
	}
	MovementStepMilliseconds.Sort();

	const FAlgonaSimBenchmarkSnapshot Snapshot = GetBenchmarkSnapshot();
	const double AverageMoveMilliseconds =
		TotalMoveMilliseconds / static_cast<double>(SimulationSteps);
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Ceiling smoke result. Entities=%d, Groups=%d, SubmittedCommands=%d, Tick=%llu, SpawnMs=%.3f, SpawnMemDeltaKB=%lld, UsedPhysicalMB=%llu, MoveAvgMs=%.3f, MoveP95Ms=%.3f, MoveP99Ms=%.3f, Visits=%d, Updated=%d, SkippedByLOD=%d"),
		Snapshot.EntityCount,
		Snapshot.GroupCount,
		SubmittedCommands,
		Snapshot.SimulationTick,
		Snapshot.LastSpawnMilliseconds,
		Snapshot.LastSpawnUsedPhysicalDeltaBytes / 1024,
		Snapshot.LastUsedPhysicalBytes / (1024ULL * 1024ULL),
		AverageMoveMilliseconds,
		GetSortedPercentile(MovementStepMilliseconds, 0.95),
		GetSortedPercentile(MovementStepMilliseconds, 0.99),
		Snapshot.LastMovementEntityVisits,
		Snapshot.LastMovementUpdatedEntities,
		Snapshot.LastMovementSkippedByLOD
	);

	DestroySimulationEntities();
	bCommandDiagnosticsEnabled = bPreviousCommandDiagnostics;

	UE_LOG(LogAlgonaSimulation, Log, TEXT("Ceiling smoke finished."));
}

void UAlgonaSimulationSubsystem::RunBenchmarkSuite(
	int32 GroupSize,
	int32 SimulationStepsPerCase)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_BenchmarkSuite);

	static constexpr int32 Counts[] = { 1000, 5000, 10000, 20000 };
	static const FAlgonaP0BenchmarkCase Cases[] =
	{
		{ TEXT("Idle_MovementDisabled"), false, true, false },
		{ TEXT("Moving_LODEnabled"), true, true, true },
		{ TEXT("Moving_LODDisabled"), true, false, true }
	};

	RefreshRuntimeConfiguration();

	GroupSize = FMath::Max(GroupSize, 1);
	SimulationStepsPerCase = FMath::Max(SimulationStepsPerCase, 1);

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Benchmark suite started. GroupSize=%d, FixedStepsPerCase=%d, FixedDeltaMs=%.3f"),
		GroupSize,
		SimulationStepsPerCase,
		FixedDeltaSeconds * 1000.0
	);

	const bool bPreviousCommandDiagnostics = bCommandDiagnosticsEnabled;
	const bool bPreviousMovementEnabled = bMovementEnabled;
	const bool bPreviousLODEnabled = bSimulationLODEnabled;
	bCommandDiagnosticsEnabled = false;

	for (const FAlgonaP0BenchmarkCase& BenchmarkCase : Cases)
	{
		for (const int32 EntityCount : Counts)
		{
			const bool bSpawned =
				RepopulateSimulationEntities(EntityCount, GroupSize);
			bMovementEnabled = BenchmarkCase.bMovementEnabled;
			bSimulationLODEnabled = BenchmarkCase.bLODEnabled;

			const int32 SubmittedCommands =
				bSpawned && BenchmarkCase.bSubmitMoveCommand
					? SubmitMoveAllGroupsCommand(FVector(20000.0, 20000.0, 0.0))
					: 0;

			TArray<double> FixedStepMilliseconds;
			TArray<double> MovementMilliseconds;
			FixedStepMilliseconds.Reserve(SimulationStepsPerCase);
			MovementMilliseconds.Reserve(SimulationStepsPerCase);

			double TotalFixedStepMilliseconds = 0.0;
			double TotalMovementMilliseconds = 0.0;
			int32 MissedDeadlines = 0;

			for (int32 StepIndex = 0; StepIndex < SimulationStepsPerCase; ++StepIndex)
			{
				RunFixedSimulationStep();
				const FAlgonaSimBenchmarkSnapshot StepSnapshot =
					GetBenchmarkSnapshot();
				FixedStepMilliseconds.Add(StepSnapshot.LastFixedStepMilliseconds);
				MovementMilliseconds.Add(StepSnapshot.LastMovementMilliseconds);
				TotalFixedStepMilliseconds +=
					StepSnapshot.LastFixedStepMilliseconds;
				TotalMovementMilliseconds +=
					StepSnapshot.LastMovementMilliseconds;
				MissedDeadlines +=
					StepSnapshot.bLastFixedStepMissedDeadline ? 1 : 0;
			}

			FixedStepMilliseconds.Sort();
			MovementMilliseconds.Sort();

			const FAlgonaSimBenchmarkSnapshot Snapshot = GetBenchmarkSnapshot();
			const double AverageFixedStepMilliseconds =
				TotalFixedStepMilliseconds / static_cast<double>(SimulationStepsPerCase);
			const double AverageMovementMilliseconds =
				TotalMovementMilliseconds / static_cast<double>(SimulationStepsPerCase);

			UE_LOG(
				LogAlgonaSimulation,
				Log,
				TEXT("Benchmark suite result. Case=%s, Entities=%d, Groups=%d, Steps=%d, SubmittedCommands=%d, SpawnMs=%.3f, SpawnMemDeltaKB=%lld, UsedPhysicalMB=%llu, StepAvgMs=%.3f, StepP95Ms=%.3f, StepP99Ms=%.3f, MoveAvgMs=%.3f, MoveP95Ms=%.3f, MoveP99Ms=%.3f, MissedDeadlines=%d, Visits=%d, Updated=%d, SkippedByLOD=%d"),
				BenchmarkCase.Name,
				Snapshot.EntityCount,
				Snapshot.GroupCount,
				SimulationStepsPerCase,
				SubmittedCommands,
				Snapshot.LastSpawnMilliseconds,
				Snapshot.LastSpawnUsedPhysicalDeltaBytes / 1024,
				Snapshot.LastUsedPhysicalBytes / (1024ULL * 1024ULL),
				AverageFixedStepMilliseconds,
				GetSortedPercentile(FixedStepMilliseconds, 0.95),
				GetSortedPercentile(FixedStepMilliseconds, 0.99),
				AverageMovementMilliseconds,
				GetSortedPercentile(MovementMilliseconds, 0.95),
				GetSortedPercentile(MovementMilliseconds, 0.99),
				MissedDeadlines,
				Snapshot.LastMovementEntityVisits,
				Snapshot.LastMovementUpdatedEntities,
				Snapshot.LastMovementSkippedByLOD
			);

			DestroySimulationEntities();
		}
	}

	bCommandDiagnosticsEnabled = bPreviousCommandDiagnostics;
	bMovementEnabled = bPreviousMovementEnabled;
	bSimulationLODEnabled = bPreviousLODEnabled;

	UE_LOG(LogAlgonaSimulation, Log, TEXT("Benchmark suite finished."));
}

void UAlgonaSimulationSubsystem::RunLocalMotionSpikeSuite(
	int32 GroupSize,
	int32 SimulationStepsPerCase)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_LocalMotionSpikeSuite);

	static constexpr int32 Counts[] = { 100, 1000, 10000 };
	static constexpr EAlgonaLocalMotionMode Modes[] =
	{
		EAlgonaLocalMotionMode::Legacy,
		EAlgonaLocalMotionMode::MassStock
	};

	RefreshRuntimeConfiguration();
	GroupSize = FMath::Max(GroupSize, 1);
	SimulationStepsPerCase = FMath::Max(SimulationStepsPerCase, 1);

	const bool bPreviousCommandDiagnostics = bCommandDiagnosticsEnabled;
	const bool bPreviousMovementEnabled = bMovementEnabled;
	const bool bPreviousNavigationEnabled = bP1NavigationEnabled;
	const bool bPreviousLODEnabled = bSimulationLODEnabled;
	const EAlgonaLocalMotionMode PreviousRequestedMode =
		RequestedLocalMotionMode;
	bCommandDiagnosticsEnabled = false;
	bMovementEnabled = true;
	bP1NavigationEnabled = false;
	bSimulationLODEnabled = false;

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("P1 local motion spike suite started. GroupSize=%d, FixedStepsPerCase=%d, FixedDeltaMs=%.3f, Navigation=false, SimLOD=false"),
		GroupSize,
		SimulationStepsPerCase,
		FixedDeltaSeconds * 1000.0);

	for (const EAlgonaLocalMotionMode Mode : Modes)
	{
		RequestedLocalMotionMode = Mode;
		ActiveLocalMotionMode = Mode;
		for (const int32 EntityCount : Counts)
		{
			const bool bSpawned =
				RepopulateSimulationEntities(EntityCount, GroupSize);
			const int32 SubmittedCommands = bSpawned
				? SubmitMoveAllGroupsCommand(FVector(20000.0, 20000.0, 0.0))
				: 0;

			for (int32 WarmupStep = 0; WarmupStep < 4; ++WarmupStep)
			{
				RunFixedSimulationStep();
			}

			TArray<double> FixedStepMilliseconds;
			FixedStepMilliseconds.Reserve(SimulationStepsPerCase);
			double TotalFixedMilliseconds = 0.0;
			double TotalMovementMilliseconds = 0.0;
			double TotalSteeringMilliseconds = 0.0;
			double TotalAvoidanceMilliseconds = 0.0;
			double TotalBoundaryMilliseconds = 0.0;
			int32 MissedDeadlines = 0;
			for (int32 StepIndex = 0;
				StepIndex < SimulationStepsPerCase;
				++StepIndex)
			{
				RunFixedSimulationStep();
				FixedStepMilliseconds.Add(
					BenchmarkSnapshot.LastFixedStepMilliseconds);
				TotalFixedMilliseconds +=
					BenchmarkSnapshot.LastFixedStepMilliseconds;
				TotalMovementMilliseconds +=
					BenchmarkSnapshot.LastMovementMilliseconds;
				if (Mode == EAlgonaLocalMotionMode::MassStock
					&& MassRuntime != nullptr)
				{
					TotalSteeringMilliseconds += MassRuntime
						->LocalMotionDiagnostics.SteeringMilliseconds;
					TotalAvoidanceMilliseconds += MassRuntime
						->LocalMotionDiagnostics.AvoidanceMilliseconds;
					TotalBoundaryMilliseconds += MassRuntime
						->LocalMotionDiagnostics.BoundaryAdapterMilliseconds;
				}
				else
				{
					TotalAvoidanceMilliseconds +=
						BenchmarkSnapshot.LastAvoidanceMilliseconds;
				}
				MissedDeadlines +=
					BenchmarkSnapshot.bLastFixedStepMissedDeadline ? 1 : 0;
			}
			FixedStepMilliseconds.Sort();
			const double Divisor =
				static_cast<double>(SimulationStepsPerCase);

			UE_LOG(
				LogAlgonaSimulation,
				Log,
				TEXT("P1 local motion spike result. Mode=%s, Entities=%d, Groups=%d, Steps=%d, SubmittedCommands=%d, StepAvgMs=%.3f, StepP95Ms=%.3f, StepP99Ms=%.3f, MovementAvgMs=%.3f, SteeringAvgMs=%.3f, AvoidanceAvgMs=%.3f, BoundaryAdapterAvgMs=%.3f, MissedDeadlines=%d"),
				ToLocalMotionModeString(Mode),
				BenchmarkSnapshot.EntityCount,
				BenchmarkSnapshot.GroupCount,
				SimulationStepsPerCase,
				SubmittedCommands,
				TotalFixedMilliseconds / Divisor,
				GetSortedPercentile(FixedStepMilliseconds, 0.95),
				GetSortedPercentile(FixedStepMilliseconds, 0.99),
				TotalMovementMilliseconds / Divisor,
				TotalSteeringMilliseconds / Divisor,
				TotalAvoidanceMilliseconds / Divisor,
				TotalBoundaryMilliseconds / Divisor,
				MissedDeadlines);

			DestroySimulationEntities();
		}
	}

	RequestedLocalMotionMode = PreviousRequestedMode;
	ActiveLocalMotionMode = RequestedLocalMotionMode;
	bCommandDiagnosticsEnabled = bPreviousCommandDiagnostics;
	bMovementEnabled = bPreviousMovementEnabled;
	bP1NavigationEnabled = bPreviousNavigationEnabled;
	bSimulationLODEnabled = bPreviousLODEnabled;

	UE_LOG(LogAlgonaSimulation, Log, TEXT("P1 local motion spike suite finished."));
}

void UAlgonaSimulationSubsystem::RunFixedSimulationStep()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_FixedStep);
	const double StartSeconds = FPlatformTime::Seconds();

	++SimulationTick;
	ProcessCommandQueue();
	ProcessPathCompletions();
	ProcessPathScheduler();
	UpdateGroupRouteProgress();
	UpdateGroupFormations();
	if (ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock)
	{
		SpatialGrid.Cells.Reset();
		SpatialGrid.Entries.Reset();
		BenchmarkSnapshot.LastSpatialGridBuildMilliseconds = 0.0;
		BenchmarkSnapshot.LastSpatialGridEntityCount = 0;
		BenchmarkSnapshot.LastSpatialGridCellCount = 0;
		ProcessMassStockMovement();
	}
	else
	{
		BuildSpatialGrid();
		ProcessMovement();
	}
	UpdateStuckRecovery();
	AccumulateSimLODMeasurement();

	BenchmarkSnapshot.LastFixedStepMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	BenchmarkSnapshot.bLastFixedStepMissedDeadline =
		BenchmarkSnapshot.LastFixedStepMilliseconds
		> FixedDeltaSeconds * 1000.0;
	BenchmarkSnapshot.SimulationTick = SimulationTick;
}

void UAlgonaSimulationSubsystem::RefreshRuntimeConfiguration()
{
	const float ConfiguredRateHz =
		CVarAlgonaSimulationRateHz.GetValueOnGameThread();
	const int32 ConfiguredMaxSteps =
		CVarAlgonaSimulationMaxStepsPerFrame.GetValueOnGameThread();

	const double ClampedRateHz =
		static_cast<double>(FMath::Max(ConfiguredRateHz, 1.0f));

	FixedDeltaSeconds = 1.0 / ClampedRateHz;
	MaxSimulationStepsPerFrame = FMath::Max(ConfiguredMaxSteps, 1);
	MovementSpeed = static_cast<double>(
		FMath::Max(CVarAlgonaP0MovementSpeed.GetValueOnGameThread(), 0.0f)
	);
	bMovementEnabled = CVarAlgonaP0MovementEnabled.GetValueOnGameThread() != 0;
	bSimulationLODEnabled = CVarAlgonaP0LODEnabled.GetValueOnGameThread() != 0;
	bP1NavigationEnabled =
		CVarAlgonaP1NavigationEnabled.GetValueOnGameThread() != 0;
	bP1FormationEnabled =
		CVarAlgonaP1FormationEnabled.GetValueOnGameThread() != 0;
	MaxPathRequestsSubmittedPerStep = FMath::Max(
		CVarAlgonaP1PathSubmitBudget.GetValueOnGameThread(),
		0
	);
	RoutePointAcceptanceRadius = static_cast<double>(
		FMath::Max(CVarAlgonaP1RouteAcceptanceRadius.GetValueOnGameThread(), 1.0f)
	);
	NavProjectionExtent = static_cast<double>(
		FMath::Max(CVarAlgonaP1NavProjectionExtent.GetValueOnGameThread(), 1.0f)
	);
	NavProjectionExtentZ = static_cast<double>(
		FMath::Max(CVarAlgonaP1NavProjectionExtentZ.GetValueOnGameThread(), 1.0f)
	);
	FormationSlotSpacing = static_cast<double>(
		FMath::Max(CVarAlgonaP1FormationSlotSpacing.GetValueOnGameThread(), 1.0f)
	);
	FormationMaxColumns =
		FMath::Max(CVarAlgonaP1FormationMaxColumns.GetValueOnGameThread(), 1);
	FormationCorridorProbeHalfWidth = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationCorridorProbeHalfWidth.GetValueOnGameThread(),
			FormationSlotSpacing
		)
	);
	FormationCorridorSafetyMargin = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationCorridorSafetyMargin.GetValueOnGameThread(),
			0.0f
		)
	);
	FormationCorridorSampleCount = FMath::Max(
		CVarAlgonaP1FormationCorridorSampleCount.GetValueOnGameThread(),
		1
	);
	FormationAnchorLookAheadDistance = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationAnchorLookAheadDistance.GetValueOnGameThread(),
			FormationSlotSpacing
		)
	);
	FormationFunnelLookAheadDistance = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationFunnelLookAheadDistance.GetValueOnGameThread(),
			FormationSlotSpacing
		)
	);
	FormationFunnelLateralSpeedFraction = static_cast<double>(
		FMath::Clamp(
			CVarAlgonaP1FormationFunnelLateralSpeedFraction.GetValueOnGameThread(),
			0.05f,
			1.0f
		)
	);
	FormationMinimumForwardSpeedScale = static_cast<double>(
		FMath::Clamp(
			CVarAlgonaP1FormationMinimumForwardSpeedScale.GetValueOnGameThread(),
			0.01f,
			1.0f
		)
	);
	FormationForwardSpeedInterpSpeed = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationForwardSpeedInterpSpeed.GetValueOnGameThread(),
			0.0f
		)
	);
	FormationOrientationInterpSpeed = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationOrientationInterpSpeed.GetValueOnGameThread(),
			0.0f
		)
	);
	FormationMinSameGroupSpacing = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationMinSameGroupSpacing.GetValueOnGameThread(),
			1.0f
		)
	);
	FormationLocalDeformationInterpSpeed = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationLocalDeformationInterpSpeed.GetValueOnGameThread(),
			0.0f
		)
	);
	FormationSettlingRadius = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1FormationSettlingRadius.GetValueOnGameThread(),
			1.0f
		)
	);
	FormationSettlingRequiredTicks = FMath::Max(
		CVarAlgonaP1FormationSettlingRequiredTicks.GetValueOnGameThread(),
		1
	);
	bP1SpatialGridEnabled =
		CVarAlgonaP1SpatialGridEnabled.GetValueOnGameThread() != 0;
	bP1LocalSeparationEnabled =
		CVarAlgonaP1LocalSeparationEnabled.GetValueOnGameThread() != 0;
	SpatialGridCellSize = static_cast<double>(
		FMath::Max(CVarAlgonaP1SpatialGridCellSize.GetValueOnGameThread(), 1.0f)
	);
	NeighborQueryRadius = static_cast<double>(
		FMath::Max(CVarAlgonaP1NeighborQueryRadius.GetValueOnGameThread(), 1.0f)
	);
	NeighborMaxCandidates =
		FMath::Max(CVarAlgonaP1NeighborMaxCandidates.GetValueOnGameThread(), 0);
	SeparationStrength = static_cast<double>(
		FMath::Max(CVarAlgonaP1SeparationStrength.GetValueOnGameThread(), 0.0f)
	);
	SeparationMinimumPadding = static_cast<double>(
		FMath::Max(
			CVarAlgonaP1SeparationMinimumPadding.GetValueOnGameThread(),
			0.0f
		)
	);
	DesiredVelocityBlend = static_cast<double>(
		FMath::Clamp(
			CVarAlgonaP1DesiredVelocityBlend.GetValueOnGameThread(),
			0.0f,
			1.0f
		)
	);
	bP1HardBlockersEnabled =
		CVarAlgonaP1HardBlockersEnabled.GetValueOnGameThread() != 0;
	bP1StuckDetectionEnabled =
		CVarAlgonaP1StuckDetectionEnabled.GetValueOnGameThread() != 0;
	StuckDistanceEpsilon = static_cast<double>(
		FMath::Max(CVarAlgonaP1StuckDistanceEpsilon.GetValueOnGameThread(), 0.0f)
	);
	StuckObservationTicks = FMath::Max(
		CVarAlgonaP1StuckObservationTicks.GetValueOnGameThread(),
		1
	);
	StuckRequiredTicks =
		FMath::Max(CVarAlgonaP1StuckRequiredTicks.GetValueOnGameThread(), 1);
	RepathCooldownTicks =
		FMath::Max(CVarAlgonaP1RepathCooldownTicks.GetValueOnGameThread(), 1);
}

void UAlgonaSimulationSubsystem::EnsureMassRuntime()
{
	if (MassRuntime == nullptr)
	{
		MassRuntime = new FAlgonaSimulationMassRuntime();
	}

	if (MassRuntime->MassSubsystem.IsValid()
		&& MassRuntime->EntityManager != nullptr)
	{
		return;
	}

	UWorld* World = GetWorld();
	UMassEntitySubsystem* MassSubsystem =
		World != nullptr ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	checkf(MassSubsystem != nullptr, TEXT("AlgonaSimulation requires UMassEntitySubsystem."));

	MassRuntime->MassSubsystem = MassSubsystem;
	MassRuntime->EntityManager = &MassSubsystem->GetMutableEntityManager();
}

bool UAlgonaSimulationSubsystem::EnsureEntityArchetype()
{
	EnsureMassRuntime();
	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr)
	{
		return false;
	}

	const UScriptStruct* AlgonaFragmentTypes[] =
	{
		FAlgonaSimPositionFragment::StaticStruct(),
		FAlgonaSimVelocityFragment::StaticStruct(),
		FAlgonaSimGroupFragment::StaticStruct(),
		FAlgonaSimStateFragment::StaticStruct(),
		FAlgonaSimLODFragment::StaticStruct(),
		FAlgonaSimNavigationFragment::StaticStruct()
	};

	if (!MassRuntime->LegacyEntityArchetype.IsValid())
	{
		MassRuntime->LegacyEntityArchetype =
			MassRuntime->EntityManager->CreateArchetype(AlgonaFragmentTypes);
	}

	if (RequestedLocalMotionMode == EAlgonaLocalMotionMode::MassStock
		&& !MassRuntime->MassStockEntityArchetype.IsValid())
	{
		const UScriptStruct* StockFragmentTypes[] =
		{
			FAlgonaSimPositionFragment::StaticStruct(),
			FAlgonaSimVelocityFragment::StaticStruct(),
			FAlgonaSimGroupFragment::StaticStruct(),
			FAlgonaSimStateFragment::StaticStruct(),
			FAlgonaSimLODFragment::StaticStruct(),
			FAlgonaSimNavigationFragment::StaticStruct(),
			FTransformFragment::StaticStruct(),
			FAgentRadiusFragment::StaticStruct(),
			FMassVelocityFragment::StaticStruct(),
			FMassDesiredMovementFragment::StaticStruct(),
			FMassForceFragment::StaticStruct(),
			FMassMoveTargetFragment::StaticStruct(),
			FMassSteeringFragment::StaticStruct(),
			FMassStandingSteeringFragment::StaticStruct(),
			FMassGhostLocationFragment::StaticStruct(),
			FMassAvoidanceColliderFragment::StaticStruct(),
			FMassNavigationEdgesFragment::StaticStruct(),
			FMassNavigationObstacleGridCellLocationFragment::StaticStruct()
		};

		FMassTagBitSet Tags;
		Tags.Add<FMassCodeDrivenMovementTag>();
		Tags.Add<FMassCustomMovementTag>();
		Tags.Add<FMassInNavigationObstacleGridTag>();

		FMassConstSharedFragmentBitSet ConstSharedFragments;
		ConstSharedFragments.Add<FMassMovementParameters>();
		ConstSharedFragments.Add<FMassMovingSteeringParameters>();
		ConstSharedFragments.Add<FMassStandingSteeringParameters>();
		ConstSharedFragments.Add<FMassMovingAvoidanceParameters>();

		const FMassArchetypeCompositionDescriptor Composition(
			StockFragmentTypes,
			Tags,
			FMassChunkFragmentBitSet(),
			FMassSharedFragmentBitSet(),
			ConstSharedFragments
		);
		MassRuntime->MassStockEntityArchetype =
			MassRuntime->EntityManager->CreateArchetype(Composition);

		FMassMovementParameters MovementParameters;
		MovementParameters.MaxSpeed = static_cast<float>(MovementSpeed);
		MovementParameters.MaxAcceleration =
			FMath::Max(static_cast<float>(MovementSpeed) * 3.0f, 600.0f);
		MovementParameters.DefaultDesiredSpeed =
			static_cast<float>(MovementSpeed);
		MovementParameters.DefaultDesiredSpeedVariance = 0.0f;
		MovementParameters.bIsCodeDrivenMovement = true;
		MovementParameters = MovementParameters.GetValidated();

		FMassMovingSteeringParameters MovingSteeringParameters;
		MovingSteeringParameters.bAllowSpeedVariance = false;
		FMassStandingSteeringParameters StandingSteeringParameters;
		FMassMovingAvoidanceParameters MovingAvoidanceParameters;
		MovingAvoidanceParameters.StartOfPathAvoidanceScale = 1.0f;
		MovingAvoidanceParameters = MovingAvoidanceParameters.GetValidated();

		MassRuntime->MassStockSharedFragments.Reset();
		MassRuntime->MassStockSharedFragments.Add(
			MassRuntime->EntityManager->GetOrCreateConstSharedFragment(
				MovementParameters));
		MassRuntime->MassStockSharedFragments.Add(
			MassRuntime->EntityManager->GetOrCreateConstSharedFragment(
				MovingSteeringParameters));
		MassRuntime->MassStockSharedFragments.Add(
			MassRuntime->EntityManager->GetOrCreateConstSharedFragment(
				StandingSteeringParameters));
		MassRuntime->MassStockSharedFragments.Add(
			MassRuntime->EntityManager->GetOrCreateConstSharedFragment(
				MovingAvoidanceParameters));
		MassRuntime->MassStockSharedFragments.Sort();
	}

	MassRuntime->EntityArchetype =
		RequestedLocalMotionMode == EAlgonaLocalMotionMode::MassStock
			? MassRuntime->MassStockEntityArchetype
			: MassRuntime->LegacyEntityArchetype;

	if (RequestedLocalMotionMode == EAlgonaLocalMotionMode::MassStock
		&& !MassRuntime->bStockPipelinesInitialized)
	{
		const TSharedRef<FMassEntityManager> EntityManager =
			MassRuntime->EntityManager->AsShared();
		auto InitializePipeline =
			[this, &EntityManager](
				FMassRuntimePipeline& Pipeline,
				TSubclassOf<UMassProcessor> ProcessorClass)
			{
				const TSubclassOf<UMassProcessor> Classes[] =
				{
					ProcessorClass
				};
				Pipeline.InitializeFromClassArray(
					Classes,
					*this,
					EntityManager
				);
				for (TObjectPtr<UMassProcessor> Processor : Pipeline.GetProcessors())
				{
					MassRuntime->StockProcessorReferences.Emplace(Processor.Get());
				}
			};

		InitializePipeline(
			MassRuntime->ObstacleGridPipeline,
			UMassNavigationObstacleGridProcessor::StaticClass());
		InitializePipeline(
			MassRuntime->SteeringPipeline,
			UMassSteerToMoveTargetProcessor::StaticClass());
		InitializePipeline(
			MassRuntime->AvoidancePipeline,
			UMassMovingAvoidanceProcessor::StaticClass());
		InitializePipeline(
			MassRuntime->ApplyForcePipeline,
			UMassApplyForceProcessor::StaticClass());
		MassRuntime->bStockPipelinesInitialized = true;
	}

	if (!MassRuntime->MovementQuery.IsValid())
	{
		MassRuntime->MovementQuery =
			MakeUnique<FMassEntityQuery>(MassRuntime->EntityManager->AsShared());
		MassRuntime->MovementQuery->AddRequirement<FAlgonaSimPositionFragment>(
			EMassFragmentAccess::ReadWrite
		);
		MassRuntime->MovementQuery->AddRequirement<FAlgonaSimVelocityFragment>(
			EMassFragmentAccess::ReadWrite
		);
		MassRuntime->MovementQuery->AddRequirement<FAlgonaSimGroupFragment>(
			EMassFragmentAccess::ReadOnly
		);
		MassRuntime->MovementQuery->AddRequirement<FAlgonaSimStateFragment>(
			EMassFragmentAccess::ReadWrite
		);
		MassRuntime->MovementQuery->AddRequirement<FAlgonaSimLODFragment>(
			EMassFragmentAccess::ReadWrite
		);
		MassRuntime->MovementQuery->AddRequirement<FAlgonaSimNavigationFragment>(
			EMassFragmentAccess::ReadWrite
		);
	}

	if (RequestedLocalMotionMode == EAlgonaLocalMotionMode::MassStock
		&& !MassRuntime->MassStockQuery.IsValid())
	{
		MassRuntime->MassStockQuery =
			MakeUnique<FMassEntityQuery>(MassRuntime->EntityManager->AsShared());
		MassRuntime->MassStockQuery->AddRequirement<FAlgonaSimPositionFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FAlgonaSimVelocityFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FAlgonaSimGroupFragment>(
			EMassFragmentAccess::ReadOnly);
		MassRuntime->MassStockQuery->AddRequirement<FAlgonaSimStateFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FAlgonaSimNavigationFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FTransformFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FAgentRadiusFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FMassVelocityFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FMassDesiredMovementFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FMassForceFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FMassMoveTargetFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FMassSteeringFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FMassAvoidanceColliderFragment>(
			EMassFragmentAccess::ReadWrite);
		MassRuntime->MassStockQuery->AddRequirement<FMassNavigationEdgesFragment>(
			EMassFragmentAccess::ReadWrite);
	}

	return MassRuntime->EntityArchetype.IsValid();
}

void UAlgonaSimulationSubsystem::ProcessCommandQueue()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_CommandProcessing);

	if (PendingCommands.IsEmpty())
	{
		return;
	}

	TArray<FAlgonaSimCommand> DeferredCommands;
	DeferredCommands.Reserve(PendingCommands.Num());

	for (const FAlgonaSimCommand& Command : PendingCommands)
	{
		if (Command.ApplyAtSimulationTick > SimulationTick)
		{
			DeferredCommands.Add(Command);
			continue;
		}

		FAlgonaSimGroupState* Group = FindGroup(Command.GroupId);
		if (Group == nullptr)
		{
			continue;
		}

		switch (Command.Type)
		{
		case EAlgonaSimCommandType::MoveGroupToTarget:
			Group->TargetPosition = Command.TargetPosition;
			Group->CurrentCommand = Command.Type;
			Group->MoveFacingMode = Command.MoveFacingMode;
			Group->CommandSequence = Command.Sequence;
			Group->bHasActiveMoveTarget = true;
			Group->Navigation.Destination = Command.TargetPosition;
			++Group->Navigation.RouteGeneration;
			Group->Navigation.Route = FAlgonaGroupRoute();
			Group->Navigation.LastFailureReason.Reset();
			Group->Navigation.CommandStartTimeSeconds =
				static_cast<double>(SimulationTick) * FixedDeltaSeconds;
			Group->Navigation.BridgeEntryTimeSeconds = -1.0;
			Group->Navigation.BridgeExitTimeSeconds = -1.0;
			Group->Navigation.RouteArrivalTimeSeconds = -1.0;
			Group->Navigation.IdleTimeSeconds = -1.0;
			Group->Navigation.RouteLength = 0.0;
			Group->Navigation.bWasInsideChoke = false;
			if (MassRuntime != nullptr
				&& MassRuntime->LocalMotionForceDiagnostics.GroupId
					== Group->Id)
			{
				MassRuntime->LocalMotionForceDiagnostics =
					FAlgonaLocalMotionForceDiagnostics();
			}
			Group->Formation.SettledTicks = 0;
			if (bP1NavigationEnabled)
			{
				QueuePathRequestForGroup(*Group);
			}
			else
			{
				Group->Navigation.Status =
					EAlgonaGroupNavigationStatus::Idle;
			}
			BenchmarkSnapshot.LastAppliedCommandSequence = Command.Sequence;
			BenchmarkSnapshot.LastAppliedCommandGroupId = Command.GroupId.Value;
			if (bCommandDiagnosticsEnabled)
			{
				UE_LOG(
					LogAlgonaSimulation,
					Log,
					TEXT("Applied command. Tick=%llu, Sequence=%llu, Type=MoveGroupToTarget, FacingMode=%s, GroupId=%d, Target=%s"),
					SimulationTick,
					Command.Sequence,
					ToMoveFacingModeString(Command.MoveFacingMode),
					Command.GroupId.Value,
					*Command.TargetPosition.ToCompactString()
				);
			}
			break;
		default:
			break;
		}
	}

	PendingCommands = MoveTemp(DeferredCommands);
	BenchmarkSnapshot.PendingCommandCount = PendingCommands.Num();
}

void UAlgonaSimulationSubsystem::ProcessPathCompletions()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_P1PathCompletions);

	if (PendingPathCompletions.IsEmpty())
	{
		return;
	}

	for (const FAlgonaPathCompletion& Completion : PendingPathCompletions)
	{
		FAlgonaSimGroupState* Group = FindGroup(Completion.GroupId);
		if (Group == nullptr
			|| Group->Navigation.RouteGeneration != Completion.Generation
			|| Group->Navigation.PendingRequestId != Completion.RequestId)
		{
			++BenchmarkSnapshot.RejectedStalePathResults;
			UE_LOG(
				LogAlgonaSimulation,
				Log,
				TEXT("Discarded stale P1 path result. RequestId=%u, GroupId=%d, Generation=%llu"),
				Completion.RequestId,
				Completion.GroupId.Value,
				Completion.Generation
			);
			continue;
		}

		Group->Navigation.PendingRequestId = 0;
		Group->Navigation.PendingRequestGeneration = 0;

		if (Completion.bSuccess && Completion.Points.Num() >= 2)
		{
			FAlgonaGroupRoute CandidateRoute;
			CandidateRoute.Points = Completion.Points;
			CandidateRoute.bValid = true;
			const FAlgonaHardBlocker* BlockingHardBlocker = nullptr;
			if (IsRouteBlockedByHardBlockers(
				CandidateRoute,
				&BlockingHardBlocker))
			{
				Group->Navigation.Route = FAlgonaGroupRoute();
				Group->Navigation.Status = EAlgonaGroupNavigationStatus::Failed;
				Group->Navigation.LastFailureReason =
					TEXT("RouteBlockedByHardBlocker");
				Group->Navigation.LastRepathReason =
					TEXT("HardBlockerRouteRejected");
				++BenchmarkSnapshot.FailedPathRequests;
				UE_LOG(
					LogAlgonaSimulation,
					Warning,
					TEXT("P1 route rejected by hard blocker. Tick=%llu, RequestId=%u, GroupId=%d, Generation=%llu, BlockerId=%d"),
					SimulationTick,
					Completion.RequestId,
					Completion.GroupId.Value,
					Completion.Generation,
					BlockingHardBlocker != nullptr
						? BlockingHardBlocker->Id
						: INDEX_NONE
				);
				continue;
			}

			Group->Navigation.Route.Points = Completion.Points;
			Group->Navigation.Route.CurrentPointIndex =
				Completion.Points.Num() > 1 ? 1 : 0;
			Group->Navigation.Route.CurrentMoveTarget =
				Group->Navigation.Route.Points[
					Group->Navigation.Route.CurrentPointIndex
				];
			Group->Navigation.Route.bPartial = Completion.bPartial;
			Group->Navigation.Route.bValid = true;
			Group->Navigation.Status =
				EAlgonaGroupNavigationStatus::RouteReady;
			Group->Navigation.LastRouteCommitTick = SimulationTick;
			Group->Navigation.RouteLength = 0.0;
			for (int32 PointIndex = 1;
				PointIndex < Completion.Points.Num();
				++PointIndex)
			{
				Group->Navigation.RouteLength += FVector::Dist2D(
					Completion.Points[PointIndex - 1],
					Completion.Points[PointIndex]);
			}
			Group->Navigation.LastFailureReason.Reset();
			Group->Formation.SettledTicks = 0;
			FVector InitialTravelDirection =
				Group->Navigation.Route.CurrentMoveTarget
				- Group->Navigation.Route.Points[0];
			InitialTravelDirection.Z = 0.0;
			if (InitialTravelDirection.Normalize())
			{
				Group->Formation.TravelDirection = InitialTravelDirection;
			}
			++BenchmarkSnapshot.CompletedPathRequests;

			UE_LOG(
				LogAlgonaSimulation,
				Log,
				TEXT("Committed P1 route. Tick=%llu, RequestId=%u, GroupId=%d, Generation=%llu, Points=%d, Partial=%s, LatencyMs=%.3f"),
				SimulationTick,
				Completion.RequestId,
				Completion.GroupId.Value,
				Completion.Generation,
				Completion.Points.Num(),
				Completion.bPartial ? TEXT("true") : TEXT("false"),
				Completion.LatencyMilliseconds
			);
		}
		else
		{
			Group->Navigation.Route = FAlgonaGroupRoute();
			Group->Navigation.Status = EAlgonaGroupNavigationStatus::Failed;
			Group->Navigation.LastFailureReason = Completion.FailureReason;
			Group->bHasActiveMoveTarget = false;
			++BenchmarkSnapshot.FailedPathRequests;

			UE_LOG(
				LogAlgonaSimulation,
				Warning,
				TEXT("P1 path request failed. Tick=%llu, RequestId=%u, GroupId=%d, Generation=%llu, Reason=%s"),
				SimulationTick,
				Completion.RequestId,
				Completion.GroupId.Value,
				Completion.Generation,
				*Completion.FailureReason
			);
		}
	}

	PendingPathCompletions.Reset();
	BenchmarkSnapshot.PendingPathCompletionCount = 0;
}

void UAlgonaSimulationSubsystem::ProcessPathScheduler()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_P1PathScheduler);

	const double StartSeconds = FPlatformTime::Seconds();

	if (!bP1NavigationEnabled || QueuedPathRequests.IsEmpty()
		|| MaxPathRequestsSubmittedPerStep <= 0)
	{
		BenchmarkSnapshot.LastPathSchedulerMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		return;
	}

	QueuedPathRequests.Sort(
		[](const FAlgonaPendingPathRequest& Left,
			const FAlgonaPendingPathRequest& Right)
		{
			if (Left.Priority != Right.Priority)
			{
				return Left.Priority > Right.Priority;
			}
			return Left.CommandSequence > Right.CommandSequence;
		}
	);

	UWorld* World = GetWorld();
	UNavigationSystemV1* NavigationSystem =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);

	const FVector ProjectionExtent(
		NavProjectionExtent,
		NavProjectionExtent,
		NavProjectionExtentZ
	);
	FNavAgentProperties AgentProperties;

	int32 SubmittedThisStep = 0;
	while (!QueuedPathRequests.IsEmpty()
		&& SubmittedThisStep < MaxPathRequestsSubmittedPerStep)
	{
		FAlgonaPendingPathRequest Request = QueuedPathRequests[0];
		QueuedPathRequests.RemoveAt(0, 1, EAllowShrinking::No);

		FAlgonaSimGroupState* Group = FindGroup(Request.GroupId);
		if (Group == nullptr
			|| Group->Navigation.RouteGeneration != Request.Generation)
		{
			++BenchmarkSnapshot.RejectedStalePathResults;
			continue;
		}

		ANavigationData* NavigationData = ResolveNavigationData(
			NavigationSystem,
			AgentProperties,
			Request.Start,
			ProjectionExtent
		);

		Group->Navigation.LastNavigationDataName =
			NavigationData != nullptr ? GetNameSafe(NavigationData) : TEXT("None");
		Group->Navigation.LastPathStart = Request.Start;
		Group->Navigation.LastPathDestination = Request.End;
		Group->Navigation.LastProjectionExtent = ProjectionExtent;
		Group->Navigation.LastProjectedStart = FVector::ZeroVector;
		Group->Navigation.LastProjectedDestination = FVector::ZeroVector;
		Group->Navigation.bLastStartProjectionSucceeded = false;
		Group->Navigation.bLastDestinationProjectionSucceeded = false;

		if (NavigationSystem == nullptr || NavigationData == nullptr)
		{
			Group->Navigation.Status = EAlgonaGroupNavigationStatus::Failed;
			Group->Navigation.LastFailureReason =
				NavigationSystem == nullptr
					? TEXT("NoNavigationSystem")
					: TEXT("NoNavigationData");
			Group->bHasActiveMoveTarget = false;
			++BenchmarkSnapshot.FailedPathRequests;
			UE_LOG(
				LogAlgonaSimulation,
				Warning,
				TEXT("P1 path pre-submit failed. Reason=%s, HasNavSys=%s, NavData=%s, World=%s, Start=%s, Destination=%s, ProjectionExtent=%s"),
				*Group->Navigation.LastFailureReason,
				NavigationSystem != nullptr ? TEXT("true") : TEXT("false"),
				*Group->Navigation.LastNavigationDataName,
				*GetNameSafe(World),
				*Request.Start.ToCompactString(),
				*Request.End.ToCompactString(),
				*ProjectionExtent.ToCompactString()
			);
			continue;
		}

		FNavLocation ProjectedStart;
		FNavLocation ProjectedEnd;
		const bool bStartProjected =
			NavigationSystem->ProjectPointToNavigation(
				Request.Start,
				ProjectedStart,
				ProjectionExtent,
				NavigationData);
		const bool bEndProjected =
			NavigationSystem->ProjectPointToNavigation(
				Request.End,
				ProjectedEnd,
				ProjectionExtent,
				NavigationData);

		Group->Navigation.bLastStartProjectionSucceeded = bStartProjected;
		Group->Navigation.bLastDestinationProjectionSucceeded = bEndProjected;
		Group->Navigation.LastProjectedStart =
			bStartProjected ? ProjectedStart.Location : FVector::ZeroVector;
		Group->Navigation.LastProjectedDestination =
			bEndProjected ? ProjectedEnd.Location : FVector::ZeroVector;

		if (!bStartProjected || !bEndProjected)
		{
			Group->Navigation.Status = EAlgonaGroupNavigationStatus::Failed;
			Group->Navigation.LastFailureReason = TEXT("ProjectionFailed");
			Group->bHasActiveMoveTarget = false;
			++BenchmarkSnapshot.FailedPathRequests;
			UE_LOG(
				LogAlgonaSimulation,
				Warning,
				TEXT("P1 path pre-submit failed. Reason=ProjectionFailed, HasNavSys=true, NavData=%s, NavDataClass=%s, Start=%s, Destination=%s, ProjectionExtent=%s, StartProjected=%s, ProjectedStart=%s, DestinationProjected=%s, ProjectedDestination=%s"),
				*GetNameSafe(NavigationData),
				*GetNameSafe(NavigationData->GetClass()),
				*Request.Start.ToCompactString(),
				*Request.End.ToCompactString(),
				*ProjectionExtent.ToCompactString(),
				bStartProjected ? TEXT("true") : TEXT("false"),
				*Group->Navigation.LastProjectedStart.ToCompactString(),
				bEndProjected ? TEXT("true") : TEXT("false"),
				*Group->Navigation.LastProjectedDestination.ToCompactString()
			);
			continue;
		}

		FPathFindingQuery Query(
			this,
			*NavigationData,
			ProjectedStart.Location,
			ProjectedEnd.Location,
			nullptr
		);

		Group->Navigation.Status =
			EAlgonaGroupNavigationStatus::Requesting;
		const uint32 RequestId = NavigationSystem->FindPathAsync(
			AgentProperties,
			Query,
			FNavPathQueryDelegate::CreateUObject(
				this,
				&UAlgonaSimulationSubsystem::OnAsyncPathResult
			),
			EPathFindingMode::Regular
		);

		if (RequestId == INVALID_NAVQUERYID)
		{
			Group->Navigation.Status = EAlgonaGroupNavigationStatus::Failed;
			Group->Navigation.LastFailureReason = TEXT("InvalidAsyncRequestId");
			Group->bHasActiveMoveTarget = false;
			++BenchmarkSnapshot.FailedPathRequests;
			continue;
		}

		Group->Navigation.PendingRequestId = RequestId;
		Group->Navigation.PendingRequestGeneration = Request.Generation;
		Group->Navigation.Status =
			EAlgonaGroupNavigationStatus::WaitingForAsyncResult;

		FAlgonaInFlightPathRequest& InFlight =
			InFlightPathRequests.AddDefaulted_GetRef();
		InFlight.GroupId = Request.GroupId;
		InFlight.RequestId = RequestId;
		InFlight.Generation = Request.Generation;
		InFlight.CommandSequence = Request.CommandSequence;
		InFlight.SubmitTimeSeconds = FPlatformTime::Seconds();

		++SubmittedThisStep;
		++BenchmarkSnapshot.SubmittedPathRequests;
	}

	BenchmarkSnapshot.QueuedPathRequestCount = QueuedPathRequests.Num();
	BenchmarkSnapshot.InFlightPathRequestCount = InFlightPathRequests.Num();
	BenchmarkSnapshot.LastPathSchedulerMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
}

void UAlgonaSimulationSubsystem::UpdateGroupRouteProgress()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_P1RouteProgress);

	const double StartSeconds = FPlatformTime::Seconds();

	if (!bP1NavigationEnabled || Groups.IsEmpty())
	{
		BenchmarkSnapshot.LastRouteProgressMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		return;
	}

	TArray<FVector> GroupCenters;
	TArray<int32> GroupCenterCounts;
	GroupCenters.SetNumZeroed(Groups.Num());
	GroupCenterCounts.SetNumZeroed(Groups.Num());

	if (MassRuntime != nullptr && MassRuntime->EntityManager != nullptr
		&& !MassRuntime->Entities.IsEmpty())
	{
		FMassEntityQuery CenterQuery(MassRuntime->EntityManager->AsShared());
		CenterQuery.AddRequirement<FAlgonaSimPositionFragment>(
			EMassFragmentAccess::ReadOnly
		);
		CenterQuery.AddRequirement<FAlgonaSimGroupFragment>(
			EMassFragmentAccess::ReadOnly
		);

		FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
		CenterQuery.ForEachEntityChunk(
			ExecutionContext,
			[this, &GroupCenters, &GroupCenterCounts](
				FMassExecutionContext& Context)
			{
				TConstArrayView<FAlgonaSimPositionFragment> Positions =
					Context.GetFragmentView<FAlgonaSimPositionFragment>();
				TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
					Context.GetFragmentView<FAlgonaSimGroupFragment>();

				for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
				{
					const int32 GroupArrayIndex = GroupIds[Index].GroupId - 1;
					if (Groups.IsValidIndex(GroupArrayIndex)
						&& Groups[GroupArrayIndex].Id.Value
							== GroupIds[Index].GroupId)
					{
						GroupCenters[GroupArrayIndex] +=
							Positions[Index].Position;
						++GroupCenterCounts[GroupArrayIndex];
					}
				}
			}
		);
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		FAlgonaGroupRoute& Route = Group.Navigation.Route;
		if (Group.Navigation.Status
				!= EAlgonaGroupNavigationStatus::RouteReady
			|| !Route.bValid
			|| Route.Points.Num() < 2
			|| !Route.Points.IsValidIndex(Route.CurrentPointIndex))
		{
			continue;
		}

		const int32 GroupArrayIndex = Group.Id.Value - 1;
		const FVector GroupCenter =
			GroupCenters.IsValidIndex(GroupArrayIndex)
			&& GroupCenterCounts[GroupArrayIndex] > 0
				? GroupCenters[GroupArrayIndex]
					/ static_cast<double>(GroupCenterCounts[GroupArrayIndex])
				: Group.TargetPosition;
		while (Route.Points.IsValidIndex(Route.CurrentPointIndex)
			&& FVector::DistSquared(
				GroupCenter,
				Route.Points[Route.CurrentPointIndex]
			) <= FMath::Square(RoutePointAcceptanceRadius))
		{
			++Route.CurrentPointIndex;
		}

		if (!Route.Points.IsValidIndex(Route.CurrentPointIndex))
		{
			Route.CurrentPointIndex = Route.Points.Num() - 1;
			Route.CurrentMoveTarget = Route.Points.Last();
			if (bP1FormationEnabled)
			{
				Group.Navigation.Status =
					EAlgonaGroupNavigationStatus::Settling;
				if (Group.Navigation.RouteArrivalTimeSeconds < 0.0)
				{
					Group.Navigation.RouteArrivalTimeSeconds =
						static_cast<double>(SimulationTick)
						* FixedDeltaSeconds;
				}
				Group.Formation.SettledTicks = 0;
			}
			else
			{
				Route.bValid = false;
				Group.Navigation.Status = EAlgonaGroupNavigationStatus::Idle;
				Group.bHasActiveMoveTarget = false;
				const double CurrentSimulationTime =
					static_cast<double>(SimulationTick) * FixedDeltaSeconds;
				Group.Navigation.RouteArrivalTimeSeconds =
					CurrentSimulationTime;
				Group.Navigation.IdleTimeSeconds = CurrentSimulationTime;
			}
			continue;
		}

		Route.CurrentMoveTarget = Route.Points[Route.CurrentPointIndex];
	}

	BenchmarkSnapshot.LastRouteProgressMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
}

void UAlgonaSimulationSubsystem::UpdateGroupFormations()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_P1Formation);

	const double StartSeconds = FPlatformTime::Seconds();
	BenchmarkSnapshot.LastFormationUpdatedGroups = 0;
	BenchmarkSnapshot.LastFormationSkippedGroupsByLOD = 0;
	BenchmarkSnapshot.LastFormationTargetUpdatedEntities = 0;
	BenchmarkSnapshot.LastFormationTargetSkippedEntitiesByLOD = 0;

	if (!bP1FormationEnabled || Groups.IsEmpty()
		|| MassRuntime == nullptr
		|| MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		BenchmarkSnapshot.LastFormationMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		return;
	}

	TArray<FVector> GroupCenters;
	TArray<int32> GroupCenterCounts;
	GroupCenters.SetNumZeroed(Groups.Num());
	GroupCenterCounts.SetNumZeroed(Groups.Num());

	FMassEntityQuery CenterQuery(MassRuntime->EntityManager->AsShared());
	CenterQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	CenterQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FMassExecutionContext CenterContext(*MassRuntime->EntityManager);
	CenterQuery.ForEachEntityChunk(
		CenterContext,
		[this, &GroupCenters, &GroupCenterCounts](
			FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				const int32 GroupArrayIndex = GroupIds[Index].GroupId - 1;
				if (Groups.IsValidIndex(GroupArrayIndex)
					&& Groups[GroupArrayIndex].Id.Value
						== GroupIds[Index].GroupId)
				{
					GroupCenters[GroupArrayIndex] += Positions[Index].Position;
					++GroupCenterCounts[GroupArrayIndex];
				}
			}
		}
	);

	for (FAlgonaSimGroupState& Group : Groups)
	{
		if (!Group.bHasActiveMoveTarget)
		{
			continue;
		}
		if (!IsNavigationMovementReady(Group))
		{
			continue;
		}

		const uint8 FormationUpdateStride =
			ActiveLocalMotionMode != EAlgonaLocalMotionMode::MassStock
			&& bSimulationLODEnabled
			? FMath::Max<uint8>(Group.NavigationUpdateStride, 1)
			: 1;
		const bool bForceFormationUpdate =
			Group.Formation.LastUpdateTick == 0
			|| Group.Navigation.Status == EAlgonaGroupNavigationStatus::Settling
			|| Group.Navigation.LastRouteCommitTick
				> Group.Formation.LastUpdateTick;
		if (!bForceFormationUpdate
			&& (SimulationTick + Group.NavigationTickOffset)
				% FormationUpdateStride != 0)
		{
			++BenchmarkSnapshot.LastFormationSkippedGroupsByLOD;
			continue;
		}
		const int32 ElapsedFormationTicks = Group.Formation.LastUpdateTick > 0
			? static_cast<int32>(FMath::Min<uint64>(
				SimulationTick - Group.Formation.LastUpdateTick,
				static_cast<uint64>(MAX_int32)
			))
			: 1;

		ConfigureFormationForGroup(Group);

		const int32 GroupArrayIndex = Group.Id.Value - 1;
		const FVector GroupCenter =
			GroupCenters.IsValidIndex(GroupArrayIndex)
			&& GroupCenterCounts[GroupArrayIndex] > 0
				? GroupCenters[GroupArrayIndex]
					/ static_cast<double>(GroupCenterCounts[GroupArrayIndex])
				: Group.Formation.AnchorPosition;

		FVector AnchorTarget = Group.TargetPosition;
		if (bP1NavigationEnabled && Group.Navigation.Route.Points.Num() > 0)
		{
			const FVector RouteAnchor =
				Group.Navigation.Route.CurrentMoveTarget;
			AnchorTarget = RouteAnchor;
			if (Group.Navigation.Status
				== EAlgonaGroupNavigationStatus::RouteReady)
			{
				FVector ToRouteAnchor = RouteAnchor - GroupCenter;
				ToRouteAnchor.Z = 0.0;
				const double DistanceToRouteAnchor = ToRouteAnchor.Length();
				if (DistanceToRouteAnchor > FormationAnchorLookAheadDistance
					&& ToRouteAnchor.Normalize())
				{
					AnchorTarget =
						GroupCenter
						+ ToRouteAnchor * FormationAnchorLookAheadDistance;
					AnchorTarget.Z = RouteAnchor.Z;
				}
			}
		}

		FVector DesiredTravelDirection = AnchorTarget - GroupCenter;
		if (bP1NavigationEnabled && Group.Navigation.Route.Points.Num() > 0
			&& Group.Navigation.Status
				!= EAlgonaGroupNavigationStatus::Settling)
		{
			const FAlgonaGroupRoute& Route = Group.Navigation.Route;
			const int32 CurrentPointIndex = FMath::Clamp(
				Route.CurrentPointIndex,
				0,
				Route.Points.Num() - 1
			);
			const int32 PreviousPointIndex =
				FMath::Max(CurrentPointIndex - 1, 0);
			DesiredTravelDirection =
				Route.Points[CurrentPointIndex]
				- Route.Points[PreviousPointIndex];
			if (DesiredTravelDirection.IsNearlyZero()
				&& Route.Points.IsValidIndex(CurrentPointIndex + 1))
			{
				DesiredTravelDirection =
					Route.Points[CurrentPointIndex + 1]
					- Route.Points[CurrentPointIndex];
			}
		}
		else if (bP1NavigationEnabled
			&& Group.Navigation.Status
				== EAlgonaGroupNavigationStatus::Settling)
		{
			DesiredTravelDirection = Group.Formation.TravelDirection;
		}

		DesiredTravelDirection.Z = 0.0;
		if (!DesiredTravelDirection.Normalize())
		{
			DesiredTravelDirection = Group.Formation.TravelDirection;
			DesiredTravelDirection.Z = 0.0;
			if (!DesiredTravelDirection.Normalize())
			{
				DesiredTravelDirection = FVector::ForwardVector;
			}
		}

		FVector CurrentLayoutForward = Group.Formation.LayoutForward;
		CurrentLayoutForward.Z = 0.0;
		if (!CurrentLayoutForward.Normalize())
		{
			CurrentLayoutForward = FVector::ForwardVector;
		}
		const double LayoutTravelDot = FVector::DotProduct(
			CurrentLayoutForward,
			DesiredTravelDirection
		);
		const FVector DesiredLayoutDirection = LayoutTravelDot >= 0.0
			? DesiredTravelDirection
			: -DesiredTravelDirection;
		const double MaxTurnRadians =
			FormationOrientationInterpSpeed * FixedDeltaSeconds
			* static_cast<double>(FMath::Max(ElapsedFormationTicks, 1));
		const FVector SmoothedLayoutForward = RotateDirectionToward2D(
			CurrentLayoutForward,
			DesiredLayoutDirection,
			MaxTurnRadians
		);
		const FVector DesiredFacingDirection =
			Group.MoveFacingMode == EAlgonaMoveFacingMode::FaceMovement
				? DesiredTravelDirection
				: Group.Formation.FacingDirection;

		Group.Formation.TravelDirection = DesiredTravelDirection;
		Group.Formation.LayoutForward = SmoothedLayoutForward;
		Group.Formation.LayoutRight = FVector::CrossProduct(
			FVector::UpVector,
			SmoothedLayoutForward
		).GetSafeNormal();
		if (Group.Formation.LayoutRight.IsNearlyZero())
		{
			Group.Formation.LayoutRight = FVector::RightVector;
		}
		Group.Formation.FacingDirection = RotateDirectionToward2D(
			Group.Formation.FacingDirection,
			DesiredFacingDirection,
			MaxTurnRadians
		);
		Group.Formation.AnchorPosition = AnchorTarget;

		UpdateFormationCorridorConstraint(
			Group,
			GroupCenter
		);
		if (UpdateFormationSafeAnchor(
			Group,
			AnchorTarget,
			ElapsedFormationTicks))
		{
			UpdateFormationCorridorConstraint(Group, GroupCenter);
		}

		Group.Formation.LastUpdateTick = SimulationTick;
		++BenchmarkSnapshot.LastFormationUpdatedGroups;
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		Group.Formation.Phase = EAlgonaFormationMemberPhase::Normal;
		Group.Formation.MembersFunneling = 0;
		Group.Formation.MembersInsideChoke = 0;
		Group.Formation.MembersReExpanding = 0;
		Group.Formation.SoftOverlapMembers = 0;
		Group.Formation.bLocallyDeformed = false;
	}

	TArray<int32> SettlingMemberCounts;
	TArray<int32> SettlingUnconvergedCounts;
	TArray<float> RecommendedForwardSpeedScales;
	TArray<float> MaxRequiredLateralSpeeds;
	TArray<int32> SpeedLimitedMemberCounts;
	SettlingMemberCounts.SetNumZeroed(Groups.Num());
	SettlingUnconvergedCounts.SetNumZeroed(Groups.Num());
	RecommendedForwardSpeedScales.Init(1.0f, Groups.Num());
	MaxRequiredLateralSpeeds.SetNumZeroed(Groups.Num());
	SpeedLimitedMemberCounts.SetNumZeroed(Groups.Num());

	FMassEntityQuery FormationQuery(MassRuntime->EntityManager->AsShared());
	FormationQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	FormationQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);
	FormationQuery.AddRequirement<FAlgonaSimNavigationFragment>(
		EMassFragmentAccess::ReadWrite
	);
	FormationQuery.AddRequirement<FAlgonaSimLODFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FMassExecutionContext FormationContext(*MassRuntime->EntityManager);
	FormationQuery.ForEachEntityChunk(
		FormationContext,
			[this, &SettlingMemberCounts, &SettlingUnconvergedCounts,
				&RecommendedForwardSpeedScales,
				&MaxRequiredLateralSpeeds,
				&SpeedLimitedMemberCounts](
			FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();
			TArrayView<FAlgonaSimNavigationFragment> NavigationFragments =
				Context.GetMutableFragmentView<FAlgonaSimNavigationFragment>();
			TConstArrayView<FAlgonaSimLODFragment> LODs =
				Context.GetFragmentView<FAlgonaSimLODFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				const int32 GroupArrayIndex = GroupIds[Index].GroupId - 1;
				if (!Groups.IsValidIndex(GroupArrayIndex)
					|| Groups[GroupArrayIndex].Id.Value
						!= GroupIds[Index].GroupId)
				{
					continue;
				}

				FAlgonaSimGroupState& Group = Groups[GroupArrayIndex];
				if (!Group.bHasActiveMoveTarget)
				{
					continue;
				}
				if (!IsNavigationMovementReady(Group))
				{
					continue;
				}

				const uint8 TargetUpdateStride =
					ActiveLocalMotionMode != EAlgonaLocalMotionMode::MassStock
					&& bSimulationLODEnabled
					? FMath::Max<uint8>(LODs[Index].UpdateStride, 1)
					: 1;
				const bool bForceTargetUpdate =
					Group.Navigation.Status
						== EAlgonaGroupNavigationStatus::Settling;
				const bool bShouldUpdateTarget = bForceTargetUpdate
					|| (SimulationTick + LODs[Index].TickOffset)
						% TargetUpdateStride == 0;
				if (bShouldUpdateTarget)
				{
					NavigationFragments[Index].DesiredSlotPosition =
						ComputeLocalFormationTarget(
							Group,
							Positions[Index].Position,
							LODs[Index],
							NavigationFragments[Index]
						);
					++BenchmarkSnapshot.LastFormationTargetUpdatedEntities;
				}
				else
				{
					++BenchmarkSnapshot
						.LastFormationTargetSkippedEntitiesByLOD;
				}

				const FAlgonaSimNavigationFragment& Navigation =
					NavigationFragments[Index];
				RecommendedForwardSpeedScales[GroupArrayIndex] = FMath::Min(
					RecommendedForwardSpeedScales[GroupArrayIndex],
					Navigation.RecommendedForwardSpeedScale
				);
				MaxRequiredLateralSpeeds[GroupArrayIndex] = FMath::Max(
					MaxRequiredLateralSpeeds[GroupArrayIndex],
					Navigation.RequiredLateralSpeed
				);
				if (Navigation.RecommendedForwardSpeedScale < 0.999f)
				{
					++SpeedLimitedMemberCounts[GroupArrayIndex];
				}

				switch (Navigation.FormationPhase)
				{
				case EAlgonaFormationMemberPhase::Funneling:
					++Group.Formation.MembersFunneling;
					break;
				case EAlgonaFormationMemberPhase::InsideChoke:
					++Group.Formation.MembersInsideChoke;
					break;
				case EAlgonaFormationMemberPhase::ReExpanding:
					++Group.Formation.MembersReExpanding;
					break;
				default:
					break;
				}
				if (Navigation.bSoftOverlap)
				{
					++Group.Formation.SoftOverlapMembers;
				}

				if (Group.Navigation.Status
					== EAlgonaGroupNavigationStatus::Settling)
				{
					++SettlingMemberCounts[GroupArrayIndex];
					if (FVector::DistSquared(
						Positions[Index].Position,
						NavigationFragments[Index].DesiredSlotPosition
					) > FMath::Square(FormationSettlingRadius))
					{
						++SettlingUnconvergedCounts[GroupArrayIndex];
					}
				}
			}
		}
	);

	for (FAlgonaSimGroupState& Group : Groups)
	{
		const int32 GroupArrayIndex = Group.Id.Value - 1;
		if (Group.Formation.MembersInsideChoke > 0)
		{
			Group.Formation.Phase = EAlgonaFormationMemberPhase::InsideChoke;
		}
		else if (Group.Formation.MembersFunneling > 0)
		{
			Group.Formation.Phase = EAlgonaFormationMemberPhase::Funneling;
		}
		else if (Group.Formation.MembersReExpanding > 0)
		{
			Group.Formation.Phase = EAlgonaFormationMemberPhase::ReExpanding;
		}
		Group.Formation.bLocallyDeformed =
			Group.Formation.Phase != EAlgonaFormationMemberPhase::Normal;
		const bool bInsideChoke = Group.Formation.MembersInsideChoke > 0;
		const double CurrentSimulationTime =
			static_cast<double>(SimulationTick) * FixedDeltaSeconds;
		if (bInsideChoke
			&& Group.Navigation.BridgeEntryTimeSeconds < 0.0)
		{
			Group.Navigation.BridgeEntryTimeSeconds = CurrentSimulationTime;
		}
		else if (!bInsideChoke
			&& Group.Navigation.bWasInsideChoke
			&& Group.Navigation.BridgeExitTimeSeconds < 0.0)
		{
			Group.Navigation.BridgeExitTimeSeconds = CurrentSimulationTime;
		}
		Group.Navigation.bWasInsideChoke = bInsideChoke;

		const float DesiredForwardSpeedScale =
			ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock
				? 1.0f
				: RecommendedForwardSpeedScales.IsValidIndex(GroupArrayIndex)
				? RecommendedForwardSpeedScales[GroupArrayIndex]
				: 1.0f;
		const double SpeedInterpMultiplier =
			DesiredForwardSpeedScale < Group.Formation.ForwardSpeedScale
				? 2.0
				: 1.0;
		Group.Formation.ForwardSpeedScale = FMath::Clamp(
			FMath::FInterpTo(
				Group.Formation.ForwardSpeedScale,
				DesiredForwardSpeedScale,
				static_cast<float>(FixedDeltaSeconds),
				static_cast<float>(
					FormationForwardSpeedInterpSpeed
						* SpeedInterpMultiplier
				)
			),
			static_cast<float>(FormationMinimumForwardSpeedScale),
			1.0f
		);
		Group.Formation.MaxRequiredLateralSpeed =
			MaxRequiredLateralSpeeds.IsValidIndex(GroupArrayIndex)
				? MaxRequiredLateralSpeeds[GroupArrayIndex]
				: 0.0f;
		Group.Formation.SpeedLimitedMembers =
			SpeedLimitedMemberCounts.IsValidIndex(GroupArrayIndex)
				? SpeedLimitedMemberCounts[GroupArrayIndex]
				: 0;
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		if (!Group.bHasActiveMoveTarget
			|| Group.Navigation.Status
				!= EAlgonaGroupNavigationStatus::Settling)
		{
			continue;
		}

		const int32 GroupArrayIndex = Group.Id.Value - 1;
		const bool bAllMembersSettled =
			SettlingMemberCounts.IsValidIndex(GroupArrayIndex)
			&& SettlingMemberCounts[GroupArrayIndex] > 0
			&& SettlingUnconvergedCounts[GroupArrayIndex] == 0;
		Group.Formation.SettledTicks = bAllMembersSettled
			? Group.Formation.SettledTicks + 1
			: 0;
		if (Group.Formation.SettledTicks >= FormationSettlingRequiredTicks)
		{
			Group.Navigation.Route.bValid = false;
			Group.Navigation.Status = EAlgonaGroupNavigationStatus::Idle;
			Group.bHasActiveMoveTarget = false;
			Group.Navigation.IdleTimeSeconds =
				static_cast<double>(SimulationTick) * FixedDeltaSeconds;
		}
	}

	BenchmarkSnapshot.LastFormationMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
}

void UAlgonaSimulationSubsystem::BuildSpatialGrid()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_P1SpatialGridBuild);

	const double StartSeconds = FPlatformTime::Seconds();
	SpatialGrid.Cells.Reset();
	SpatialGrid.Entries.Reset();
	SpatialGrid.CellSize = SpatialGridCellSize;
	BenchmarkSnapshot.LastSpatialGridEntityCount = 0;
	BenchmarkSnapshot.LastSpatialGridCellCount = 0;

	if (!bP1SpatialGridEnabled
		|| MassRuntime == nullptr
		|| MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		BenchmarkSnapshot.LastSpatialGridBuildMilliseconds =
			(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		return;
	}

	SpatialGrid.Entries.Reserve(MassRuntime->Entities.Num());

	FMassEntityQuery GridQuery(MassRuntime->EntityManager->AsShared());
	GridQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	GridQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);
	GridQuery.AddRequirement<FAlgonaSimNavigationFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
	GridQuery.ForEachEntityChunk(
		ExecutionContext,
		[this](FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();
			TConstArrayView<FAlgonaSimNavigationFragment> NavigationFragments =
				Context.GetFragmentView<FAlgonaSimNavigationFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				FAlgonaSpatialGridEntry& Entry =
					SpatialGrid.Entries.AddDefaulted_GetRef();
				Entry.Position = Positions[Index].Position;
				Entry.GroupId = FSimGroupId(GroupIds[Index].GroupId);
				Entry.Entity = Context.GetEntity(Index);
				Entry.EntityIndex = SpatialGrid.Entries.Num() - 1;
				Entry.Radius = NavigationFragments[Index].Radius;

				const FIntPoint Cell = GetSpatialGridCell(Entry.Position);
				SpatialGrid.Cells.FindOrAdd(Cell).Add(Entry.EntityIndex);
			}
		}
	);

	BenchmarkSnapshot.LastSpatialGridEntityCount = SpatialGrid.Entries.Num();
	BenchmarkSnapshot.LastSpatialGridCellCount = SpatialGrid.Cells.Num();
	BenchmarkSnapshot.LastSpatialGridBuildMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
}

FIntPoint UAlgonaSimulationSubsystem::GetSpatialGridCell(
	const FVector& Position) const
{
	const double SafeCellSize = FMath::Max(SpatialGrid.CellSize, 1.0);
	return FIntPoint(
		FMath::FloorToInt(Position.X / SafeCellSize),
		FMath::FloorToInt(Position.Y / SafeCellSize)
	);
}

FVector UAlgonaSimulationSubsystem::ComputeSeparationVelocity(
	const FMassEntityHandle& Entity,
	FSimGroupId GroupId,
	const FVector& Position,
	float Radius,
	int32& OutNeighborCandidates) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_P1NeighborQuery);

	OutNeighborCandidates = 0;
	if (!bP1SpatialGridEnabled
		|| !bP1LocalSeparationEnabled
		|| SpatialGrid.Entries.IsEmpty()
		|| NeighborMaxCandidates <= 0)
	{
		return FVector::ZeroVector;
	}

	const double QueryRadius = FMath::Max(
		NeighborQueryRadius,
		static_cast<double>(Radius) * 2.0
	);
	const double QueryRadiusSquared = FMath::Square(QueryRadius);
	const FIntPoint CenterCell = GetSpatialGridCell(Position);
	FVector Separation = FVector::ZeroVector;
	const FAlgonaSimGroupState* SourceGroup = FindGroup(GroupId);
	const double SameGroupMinimumDistance = SourceGroup != nullptr
		? FMath::Max(
			static_cast<double>(SourceGroup->MinSameGroupSpacing),
			1.0
		)
		: FMath::Max(static_cast<double>(Radius) * 2.0, 1.0);

	for (int32 CellY = CenterCell.Y - 1; CellY <= CenterCell.Y + 1; ++CellY)
	{
		for (int32 CellX = CenterCell.X - 1; CellX <= CenterCell.X + 1; ++CellX)
		{
			const TArray<int32>* CellEntries =
				SpatialGrid.Cells.Find(FIntPoint(CellX, CellY));
			if (CellEntries == nullptr)
			{
				continue;
			}

			for (const int32 CandidateEntryIndex : *CellEntries)
			{
				if (OutNeighborCandidates >= NeighborMaxCandidates)
				{
					return Separation.GetClampedToMaxSize(1.0)
						* SeparationStrength;
				}
				if (!SpatialGrid.Entries.IsValidIndex(CandidateEntryIndex))
				{
					continue;
				}

				const FAlgonaSpatialGridEntry& Candidate =
					SpatialGrid.Entries[CandidateEntryIndex];
				if (Candidate.Entity == Entity)
				{
					continue;
				}
				FVector Away = Position - Candidate.Position;
				Away.Z = 0.0;
				const double DistanceSquared = Away.SizeSquared();
				if (DistanceSquared <= KINDA_SMALL_NUMBER
					|| DistanceSquared > QueryRadiusSquared)
				{
					continue;
				}

				const bool bSameGroup = Candidate.GroupId == GroupId;
				const double MinimumSeparationDistance = bSameGroup
					? SameGroupMinimumDistance
					: FMath::Max(
						static_cast<double>(Radius + Candidate.Radius)
							+ SeparationMinimumPadding,
						1.0
					);
				if (DistanceSquared
					>= FMath::Square(MinimumSeparationDistance))
				{
					continue;
				}

				++OutNeighborCandidates;
				const double Distance = FMath::Sqrt(DistanceSquared);
				const double PenetrationFactor = FMath::Clamp(
					(MinimumSeparationDistance - Distance)
						/ MinimumSeparationDistance,
					0.0,
					1.0
				);
				Separation += (Away / Distance) * PenetrationFactor;
			}
		}
	}

	return Separation.GetClampedToMaxSize(1.0) * SeparationStrength;
}

FAlgonaGroupRestDiagnostics
UAlgonaSimulationSubsystem::CollectGroupRestDiagnostics(
	FSimGroupId GroupId) const
{
	FAlgonaGroupRestDiagnostics Diagnostics;
	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty() || !GroupId.IsValid())
	{
		return Diagnostics;
	}

	FMassEntityQuery DiagnosticQuery(MassRuntime->EntityManager->AsShared());
	DiagnosticQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	DiagnosticQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);
	DiagnosticQuery.AddRequirement<FAlgonaSimNavigationFragment>(
		EMassFragmentAccess::ReadOnly
	);

	double TotalSlotError = 0.0;
	double TotalDesiredSpeed = 0.0;
	double TotalSeparationSpeed = 0.0;
	FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
	DiagnosticQuery.ForEachEntityChunk(
		ExecutionContext,
		[this, GroupId, &Diagnostics, &TotalSlotError, &TotalDesiredSpeed,
			&TotalSeparationSpeed](FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();
			TConstArrayView<FAlgonaSimNavigationFragment> NavigationFragments =
				Context.GetFragmentView<FAlgonaSimNavigationFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				if (GroupIds[Index].GroupId != GroupId.Value)
				{
					continue;
				}

				++Diagnostics.MemberCount;
				const double SlotError = FVector::Dist2D(
					Positions[Index].Position,
					NavigationFragments[Index].DesiredSlotPosition
				);
				const double DesiredSpeed =
					NavigationFragments[Index].DesiredVelocity.Size2D();
				int32 SeparationCandidateCount = 0;
				const double SeparationSpeed =
					ComputeSeparationVelocity(
						Context.GetEntity(Index),
						GroupId,
						Positions[Index].Position,
						NavigationFragments[Index].Radius,
						SeparationCandidateCount
					).Size2D();

				TotalSlotError += SlotError;
				TotalDesiredSpeed += DesiredSpeed;
				TotalSeparationSpeed += SeparationSpeed;
				Diagnostics.MaxSlotError =
					FMath::Max(Diagnostics.MaxSlotError, SlotError);
				Diagnostics.MaxDesiredSpeed =
					FMath::Max(Diagnostics.MaxDesiredSpeed, DesiredSpeed);
				Diagnostics.MaxSeparationSpeed =
					FMath::Max(Diagnostics.MaxSeparationSpeed, SeparationSpeed);

				if (SlotError <= FormationSettlingRadius)
				{
					++Diagnostics.SettledMembers;
					if (DesiredSpeed > 1.0 && SeparationSpeed > 1.0)
					{
						++Diagnostics.MovingBySeparationCount;
					}
				}
			}
		}
	);

	if (Diagnostics.MemberCount > 0)
	{
		const double MemberCount =
			static_cast<double>(Diagnostics.MemberCount);
		Diagnostics.AverageSlotError = TotalSlotError / MemberCount;
		Diagnostics.AverageDesiredSpeed = TotalDesiredSpeed / MemberCount;
		Diagnostics.AverageSeparationSpeed =
			TotalSeparationSpeed / MemberCount;
	}

	return Diagnostics;
}

bool UAlgonaSimulationSubsystem::IsSegmentBlockedByHardBlockers(
	const FVector& Start,
	const FVector& End,
	const FAlgonaHardBlocker** OutBlocker) const
{
	if (!bP1HardBlockersEnabled || HardBlockers.IsEmpty())
	{
		return false;
	}

	FVector Segment = End - Start;
	Segment.Z = 0.0;
	const double SegmentLengthSquared = Segment.SizeSquared2D();
	for (const FAlgonaHardBlocker& Blocker : HardBlockers)
	{
		const double RadiusSquared = FMath::Square(Blocker.Radius);
		if (SegmentLengthSquared <= KINDA_SMALL_NUMBER)
		{
			if (FVector::DistSquared2D(Start, Blocker.Center) <= RadiusSquared)
			{
				if (OutBlocker != nullptr)
				{
					*OutBlocker = &Blocker;
				}
				return true;
			}
			continue;
		}

		FVector ToBlocker = Blocker.Center - Start;
		ToBlocker.Z = 0.0;
		const double Alpha = FMath::Clamp(
			FVector::DotProduct(ToBlocker, Segment) / SegmentLengthSquared,
			0.0,
			1.0
		);
		const FVector Closest = Start + Segment * Alpha;
		if (FVector::DistSquared2D(Closest, Blocker.Center) <= RadiusSquared)
		{
			if (OutBlocker != nullptr)
			{
				*OutBlocker = &Blocker;
			}
			return true;
		}
	}

	return false;
}

bool UAlgonaSimulationSubsystem::IsRouteBlockedByHardBlockers(
	const FAlgonaGroupRoute& Route,
	const FAlgonaHardBlocker** OutBlocker) const
{
	if (!Route.bValid || Route.Points.Num() < 2)
	{
		return false;
	}

	for (int32 PointIndex = 1; PointIndex < Route.Points.Num(); ++PointIndex)
	{
		if (IsSegmentBlockedByHardBlockers(
			Route.Points[PointIndex - 1],
			Route.Points[PointIndex],
			OutBlocker))
		{
			return true;
		}
	}

	return false;
}

FVector UAlgonaSimulationSubsystem::ConstrainMovementAgainstHardBlockers(
	const FVector& CurrentPosition,
	const FVector& ProposedPosition,
	float Radius,
	bool& bOutBlocked) const
{
	bOutBlocked = false;
	if (!bP1HardBlockersEnabled || HardBlockers.IsEmpty())
	{
		return ProposedPosition;
	}

	FVector Segment = ProposedPosition - CurrentPosition;
	Segment.Z = 0.0;
	const double SegmentLengthSquared = Segment.SizeSquared2D();
	for (const FAlgonaHardBlocker& Blocker : HardBlockers)
	{
		const double EffectiveRadius =
			FMath::Max(Blocker.Radius + static_cast<double>(Radius), 1.0);
		if (FVector::DistSquared2D(ProposedPosition, Blocker.Center)
			<= FMath::Square(EffectiveRadius))
		{
			bOutBlocked = true;
			return CurrentPosition;
		}
		if (SegmentLengthSquared <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		FVector ToBlocker = Blocker.Center - CurrentPosition;
		ToBlocker.Z = 0.0;
		const double Alpha = FMath::Clamp(
			FVector::DotProduct(ToBlocker, Segment) / SegmentLengthSquared,
			0.0,
			1.0
		);
		const FVector Closest = CurrentPosition + Segment * Alpha;
		if (FVector::DistSquared2D(Closest, Blocker.Center)
			<= FMath::Square(EffectiveRadius))
		{
			bOutBlocked = true;
			return CurrentPosition;
		}
	}

	return ProposedPosition;
}

FVector UAlgonaSimulationSubsystem::ConstrainMovementToFormationCorridor(
	const FAlgonaSimGroupState& Group,
	const FVector& CurrentPosition,
	const FVector& ProposedPosition,
	float AgentRadius,
	bool& bOutConstrained,
	int32& OutConstraintCount,
	double& OutSafetyCorrectionDistance,
	double& OutPenetrationDepth,
	double& OutNormalMotionClipped) const
{
	bOutConstrained = false;
	OutConstraintCount = 0;
	OutSafetyCorrectionDistance = 0.0;
	OutPenetrationDepth = 0.0;
	OutNormalMotionClipped = 0.0;
	if (!bP1NavigationEnabled || !Group.bHasActiveMoveTarget
		|| !IsNavigationMovementReady(Group)
		|| Group.Formation.CorridorSamples.IsEmpty())
	{
		return ProposedPosition;
	}

	FVector CorridorForward = Group.Formation.TravelDirection;
	CorridorForward.Z = 0.0;
	FVector CorridorRight = Group.Formation.LayoutRight;
	CorridorRight.Z = 0.0;
	if (!CorridorForward.Normalize() || !CorridorRight.Normalize())
	{
		return ProposedPosition;
	}

	struct FCorridorBoundaryEvaluation
	{
		bool bValid = false;
		bool bInside = true;
		double PenetrationDepth = 0.0;
		FVector OutwardNormal = FVector::ZeroVector;
	};

	const double Clearance = FormationCorridorSafetyMargin
		+ FMath::Max(static_cast<double>(AgentRadius), 0.0);
	const double TangentProbeDistance = FMath::Max(
		FMath::Abs(FVector::DotProduct(
			ProposedPosition - CurrentPosition,
			CorridorForward)),
		1.0);
	const auto EvaluateBoundary =
		[&](const FVector& Position)
		{
			FCorridorBoundaryEvaluation Result;
			const double LongitudinalOffset = FVector::DotProduct(
				Position - Group.Formation.AnchorPosition,
				CorridorForward);
			const FAlgonaFormationCorridorSample Sample =
				SampleFormationCorridor(
					Group.Formation,
					LongitudinalOffset);
			if (!Sample.bValid)
			{
				return Result;
			}

			Result.bValid = true;
			const double LeftBound = -FMath::Max(
				static_cast<double>(Sample.LeftHalfWidth) - Clearance,
				0.0);
			const double RightBound = FMath::Max(
				static_cast<double>(Sample.RightHalfWidth) - Clearance,
				0.0);
			const double LateralOffset = FVector::DotProduct(
				Position - Sample.Center,
				CorridorRight);
			if (LateralOffset >= LeftBound - 0.05
				&& LateralOffset <= RightBound + 0.05)
			{
				return Result;
			}

			Result.bInside = false;
			const double BoundarySide = LateralOffset < LeftBound
				? -1.0
				: 1.0;
			Result.PenetrationDepth = BoundarySide < 0.0
				? LeftBound - LateralOffset
				: LateralOffset - RightBound;
			const auto GetBoundaryPoint =
				[&](const FAlgonaFormationCorridorSample& BoundarySample)
				{
					const double HalfWidth = BoundarySide < 0.0
						? BoundarySample.LeftHalfWidth
						: BoundarySample.RightHalfWidth;
					return BoundarySample.Center
						+ CorridorRight * BoundarySide
							* FMath::Max(HalfWidth - Clearance, 0.0);
				};
			const FAlgonaFormationCorridorSample PreviousSample =
				SampleFormationCorridor(
					Group.Formation,
					LongitudinalOffset - TangentProbeDistance);
			const FAlgonaFormationCorridorSample NextSample =
				SampleFormationCorridor(
					Group.Formation,
					LongitudinalOffset + TangentProbeDistance);
			FVector BoundaryTangent = CorridorForward;
			if (PreviousSample.bValid && NextSample.bValid)
			{
				BoundaryTangent = GetBoundaryPoint(NextSample)
					- GetBoundaryPoint(PreviousSample);
				BoundaryTangent.Z = 0.0;
				if (!BoundaryTangent.Normalize())
				{
					BoundaryTangent = CorridorForward;
				}
			}
			const FVector OutwardReference = CorridorRight * BoundarySide;
			Result.OutwardNormal = OutwardReference
				- BoundaryTangent * FVector::DotProduct(
					OutwardReference,
					BoundaryTangent);
			Result.OutwardNormal.Z = 0.0;
			if (!Result.OutwardNormal.Normalize())
			{
				Result.OutwardNormal = OutwardReference;
			}
			return Result;
		};

	FCorridorBoundaryEvaluation CurrentEvaluation =
		EvaluateBoundary(CurrentPosition);
	if (!CurrentEvaluation.bValid)
	{
		return ProposedPosition;
	}

	FVector RemainingDelta = ProposedPosition - CurrentPosition;
	FVector ConstrainedPosition = CurrentPosition;
	const double MaxSafetyCorrection = FMath::Max(
		static_cast<double>(AgentRadius) * 0.1,
		1.0);
	if (!CurrentEvaluation.bInside)
	{
		bOutConstrained = true;
		++OutConstraintCount;
		const double OutwardMotion = FMath::Max(
			FVector::DotProduct(
				RemainingDelta,
				CurrentEvaluation.OutwardNormal),
			0.0);
		RemainingDelta -=
			CurrentEvaluation.OutwardNormal * OutwardMotion;
		OutNormalMotionClipped += OutwardMotion;
		const double RecoveryDistance = FMath::Min(
			CurrentEvaluation.PenetrationDepth,
			MaxSafetyCorrection);
		ConstrainedPosition = CurrentPosition
			+ RemainingDelta
			- CurrentEvaluation.OutwardNormal * RecoveryDistance;
		OutSafetyCorrectionDistance = RecoveryDistance;
		const FCorridorBoundaryEvaluation FinalEvaluation =
			EvaluateBoundary(ConstrainedPosition);
		if (!FinalEvaluation.bValid
			|| FinalEvaluation.PenetrationDepth
				> CurrentEvaluation.PenetrationDepth + 0.05)
		{
			ConstrainedPosition = CurrentPosition
				- CurrentEvaluation.OutwardNormal * RecoveryDistance;
		}
		const FCorridorBoundaryEvaluation RecoveredEvaluation =
			EvaluateBoundary(ConstrainedPosition);
		OutPenetrationDepth = RecoveredEvaluation.bValid
			? RecoveredEvaluation.PenetrationDepth
			: CurrentEvaluation.PenetrationDepth;
		return ConstrainedPosition;
	}

	for (int32 ContactIteration = 0;
		ContactIteration < 2 && !RemainingDelta.IsNearlyZero();
		++ContactIteration)
	{
		const FVector CandidatePosition =
			ConstrainedPosition + RemainingDelta;
		FCorridorBoundaryEvaluation ContactEvaluation;
		double SafeAlpha = 0.0;
		double UnsafeAlpha = 1.0;
		bool bCrossesBoundary = false;
		bool bHasInvalidSample = false;
		static constexpr int32 SweepCoarseSamples = 4;
		for (int32 SampleIndex = 1;
			SampleIndex <= SweepCoarseSamples;
			++SampleIndex)
		{
			const double SampleAlpha =
				static_cast<double>(SampleIndex)
				/ static_cast<double>(SweepCoarseSamples);
			const FCorridorBoundaryEvaluation SampleEvaluation =
				EvaluateBoundary(
					ConstrainedPosition + RemainingDelta * SampleAlpha);
			if (!SampleEvaluation.bValid)
			{
				bHasInvalidSample = true;
				break;
			}
			if (!SampleEvaluation.bInside)
			{
				ContactEvaluation = SampleEvaluation;
				UnsafeAlpha = SampleAlpha;
				bCrossesBoundary = true;
				break;
			}
			SafeAlpha = SampleAlpha;
		}
		if (bHasInvalidSample)
		{
			RemainingDelta = FVector::ZeroVector;
			break;
		}
		if (!bCrossesBoundary)
		{
			ConstrainedPosition = CandidatePosition;
			RemainingDelta = FVector::ZeroVector;
			break;
		}

		bOutConstrained = true;
		++OutConstraintCount;
		for (int32 SweepIteration = 0; SweepIteration < 12; ++SweepIteration)
		{
			const double TestAlpha = (SafeAlpha + UnsafeAlpha) * 0.5;
			const FCorridorBoundaryEvaluation TestEvaluation =
				EvaluateBoundary(
					ConstrainedPosition + RemainingDelta * TestAlpha);
			if (TestEvaluation.bValid && TestEvaluation.bInside)
			{
				SafeAlpha = TestAlpha;
			}
			else
			{
				UnsafeAlpha = TestAlpha;
			}
		}

		ConstrainedPosition += RemainingDelta * SafeAlpha;
		RemainingDelta *= 1.0 - SafeAlpha;
		const double OutwardMotion = FMath::Max(
			FVector::DotProduct(
				RemainingDelta,
				ContactEvaluation.OutwardNormal),
			0.0);
		RemainingDelta -=
			ContactEvaluation.OutwardNormal * OutwardMotion;
		OutNormalMotionClipped += OutwardMotion;
	}

	if (!RemainingDelta.IsNearlyZero())
	{
		const FVector CandidatePosition =
			ConstrainedPosition + RemainingDelta;
		const FCorridorBoundaryEvaluation CandidateEvaluation =
			EvaluateBoundary(CandidatePosition);
		if (CandidateEvaluation.bValid && CandidateEvaluation.bInside)
		{
			ConstrainedPosition = CandidatePosition;
		}
	}

	FCorridorBoundaryEvaluation FinalEvaluation =
		EvaluateBoundary(ConstrainedPosition);
	if (FinalEvaluation.bValid && !FinalEvaluation.bInside)
	{
		const double SafetyCorrection = FMath::Min(
			FinalEvaluation.PenetrationDepth,
			MaxSafetyCorrection);
		ConstrainedPosition -=
			FinalEvaluation.OutwardNormal * SafetyCorrection;
		OutSafetyCorrectionDistance += SafetyCorrection;
		FinalEvaluation = EvaluateBoundary(ConstrainedPosition);
	}
	OutPenetrationDepth = FinalEvaluation.bValid
		? FinalEvaluation.PenetrationDepth
		: 0.0;
	return ConstrainedPosition;
}

void UAlgonaSimulationSubsystem::QueueRepathForGroup(
	FAlgonaSimGroupState& Group,
	const TCHAR* Reason,
	int32 Priority)
{
	if (!bP1NavigationEnabled || !Group.bHasActiveMoveTarget)
	{
		return;
	}

	++Group.Navigation.RouteGeneration;
	Group.Navigation.LastRepathReason = Reason;
	Group.Navigation.LastFailureReason.Reset();
	Group.Navigation.StuckTicks = 0;
	Group.Navigation.LastRepathTick = SimulationTick;
	Group.Navigation.LastProgressSampleTick = 0;
	Group.Navigation.LastProgressRoutePointIndex = INDEX_NONE;
	Group.Formation.SettledTicks = 0;
	QueuePathRequestForGroup(Group);
	if (!QueuedPathRequests.IsEmpty())
	{
		QueuedPathRequests.Last().Priority = Priority;
	}
	++BenchmarkSnapshot.TotalRepathsRequested;
}

int32 UAlgonaSimulationSubsystem::InvalidateRoutesForHardBlockers(
	const TCHAR* Reason)
{
	int32 InvalidatedGroups = 0;
	BenchmarkSnapshot.LastRouteInvalidationCount = 0;
	if (!bP1HardBlockersEnabled || HardBlockers.IsEmpty())
	{
		return 0;
	}

	for (FAlgonaSimGroupState& Group : Groups)
	{
		if (!Group.bHasActiveMoveTarget
			|| !IsRouteBlockedByHardBlockers(Group.Navigation.Route))
		{
			continue;
		}

		QueueRepathForGroup(Group, Reason, 10);
		++InvalidatedGroups;
	}

	BenchmarkSnapshot.LastRouteInvalidationCount = InvalidatedGroups;
	return InvalidatedGroups;
}

void UAlgonaSimulationSubsystem::RefreshDerivedGroupSimLOD(
	FAlgonaSimGroupState& Group)
{
	if (Group.HighLODMemberCount > 0)
	{
		Group.NavigationLOD = EAlgonaSimLOD::High;
	}
	else if (Group.MediumLODMemberCount > 0)
	{
		Group.NavigationLOD = EAlgonaSimLOD::Medium;
	}
	else
	{
		Group.NavigationLOD = EAlgonaSimLOD::Low;
	}

	Group.NavigationUpdateStride = GetLODUpdateStride(Group.NavigationLOD);
	Group.NavigationTickOffset = static_cast<uint8>(
		FMath::Max(Group.Id.Value - 1, 0)
		% FMath::Max<int32>(Group.NavigationUpdateStride, 1)
	);
}

void UAlgonaSimulationSubsystem::RefreshSimLODStatistics()
{
	BenchmarkSnapshot.HighLODEntityCount = 0;
	BenchmarkSnapshot.MediumLODEntityCount = 0;
	BenchmarkSnapshot.LowLODEntityCount = 0;
	BenchmarkSnapshot.HighLODGroupCount = 0;
	BenchmarkSnapshot.MediumLODGroupCount = 0;
	BenchmarkSnapshot.LowLODGroupCount = 0;

	for (const FAlgonaSimGroupState& Group : Groups)
	{
		BenchmarkSnapshot.HighLODEntityCount += Group.HighLODMemberCount;
		BenchmarkSnapshot.MediumLODEntityCount += Group.MediumLODMemberCount;
		BenchmarkSnapshot.LowLODEntityCount += Group.LowLODMemberCount;
		switch (Group.NavigationLOD)
		{
		case EAlgonaSimLOD::High:
			++BenchmarkSnapshot.HighLODGroupCount;
			break;
		case EAlgonaSimLOD::Medium:
			++BenchmarkSnapshot.MediumLODGroupCount;
			break;
		case EAlgonaSimLOD::Low:
			++BenchmarkSnapshot.LowLODGroupCount;
			break;
		default:
			break;
		}
	}
}

void UAlgonaSimulationSubsystem::ResetSimLODMeasurement()
{
	BenchmarkSnapshot.SimLODMeasurementStartTick = SimulationTick;
	BenchmarkSnapshot.SimLODMeasurementStepCount = 0;
	BenchmarkSnapshot.SimLODMeasurementFormationMilliseconds = 0.0;
	BenchmarkSnapshot.SimLODMeasurementNeighborMilliseconds = 0.0;
	BenchmarkSnapshot.SimLODMeasurementAvoidanceMilliseconds = 0.0;
	BenchmarkSnapshot.SimLODMeasurementMovementMilliseconds = 0.0;
	BenchmarkSnapshot.SimLODMeasurementFormationUpdatedGroups = 0;
	BenchmarkSnapshot.SimLODMeasurementFormationSkippedGroups = 0;
	BenchmarkSnapshot.SimLODMeasurementFormationTargetUpdatedEntities = 0;
	BenchmarkSnapshot.SimLODMeasurementFormationTargetSkippedEntities = 0;
	BenchmarkSnapshot.SimLODMeasurementNeighborQueries = 0;
	BenchmarkSnapshot.SimLODMeasurementNeighborQueriesSkipped = 0;
}

void UAlgonaSimulationSubsystem::AccumulateSimLODMeasurement()
{
	++BenchmarkSnapshot.SimLODMeasurementStepCount;
	BenchmarkSnapshot.SimLODMeasurementFormationMilliseconds +=
		BenchmarkSnapshot.LastFormationMilliseconds;
	BenchmarkSnapshot.SimLODMeasurementNeighborMilliseconds +=
		BenchmarkSnapshot.LastNeighborQueryMilliseconds;
	BenchmarkSnapshot.SimLODMeasurementAvoidanceMilliseconds +=
		BenchmarkSnapshot.LastAvoidanceMilliseconds;
	BenchmarkSnapshot.SimLODMeasurementMovementMilliseconds +=
		BenchmarkSnapshot.LastMovementMilliseconds;
	BenchmarkSnapshot.SimLODMeasurementFormationUpdatedGroups +=
		BenchmarkSnapshot.LastFormationUpdatedGroups;
	BenchmarkSnapshot.SimLODMeasurementFormationSkippedGroups +=
		BenchmarkSnapshot.LastFormationSkippedGroupsByLOD;
	BenchmarkSnapshot.SimLODMeasurementFormationTargetUpdatedEntities +=
		BenchmarkSnapshot.LastFormationTargetUpdatedEntities;
	BenchmarkSnapshot.SimLODMeasurementFormationTargetSkippedEntities +=
		BenchmarkSnapshot.LastFormationTargetSkippedEntitiesByLOD;
	BenchmarkSnapshot.SimLODMeasurementNeighborQueries +=
		BenchmarkSnapshot.LastNeighborQueryCount;
	BenchmarkSnapshot.SimLODMeasurementNeighborQueriesSkipped +=
		BenchmarkSnapshot.LastNeighborQueriesSkippedByLOD;
}

void UAlgonaSimulationSubsystem::SynchronizeMassStockTransformsAndObstacleGrid()
{
	if (ActiveLocalMotionMode != EAlgonaLocalMotionMode::MassStock
		|| MassRuntime == nullptr
		|| MassRuntime->EntityManager == nullptr
		|| MassRuntime->MassStockQuery == nullptr
		|| !MassRuntime->bStockPipelinesInitialized
		|| MassRuntime->Entities.IsEmpty())
	{
		return;
	}

	FMassExecutionContext SyncContext(*MassRuntime->EntityManager);
	MassRuntime->MassStockQuery->ForEachEntityChunk(
		SyncContext,
		[](FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();
			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				Transforms[Index].GetMutableTransform().SetLocation(
					Positions[Index].Position);
			}
		}
	);

	const FMassArchetypeEntityCollection Collection(
		MassRuntime->MassStockEntityArchetype,
		MassRuntime->Entities,
		FMassArchetypeEntityCollection::NoDuplicates);
	UE::Mass::FProcessingContext ProcessingContext(
		*MassRuntime->EntityManager,
		0.0f);
	UE::Mass::Executor::RunWithCollection(
		MassRuntime->ObstacleGridPipeline,
		ProcessingContext,
		Collection);
}

void UAlgonaSimulationSubsystem::ProcessMassStockMovement()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_MassStockMovement);

	if (!bMovementEnabled || MassRuntime == nullptr
		|| MassRuntime->EntityManager == nullptr
		|| MassRuntime->MassStockQuery == nullptr
		|| !MassRuntime->bStockPipelinesInitialized
		|| MassRuntime->Entities.IsEmpty())
	{
		if (MassRuntime != nullptr)
		{
			MassRuntime->LocalMotionDiagnostics = FAlgonaLocalMotionDiagnostics();
		}
		BenchmarkSnapshot.LastMovementMilliseconds = 0.0;
		BenchmarkSnapshot.LastMovementEntityVisits = 0;
		BenchmarkSnapshot.LastMovementUpdatedEntities = 0;
		BenchmarkSnapshot.LastMovementSkippedByLOD = 0;
		BenchmarkSnapshot.LastNeighborQueryMilliseconds = 0.0;
		BenchmarkSnapshot.LastNeighborQueryCount = 0;
		BenchmarkSnapshot.LastNeighborQueriesSkippedByLOD = 0;
		BenchmarkSnapshot.LastHighLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastMediumLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastLowLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastNeighborCandidateCount = 0;
		BenchmarkSnapshot.LastNeighborMaxCandidates = 0;
		BenchmarkSnapshot.LastNeighborAverageCandidates = 0.0;
		BenchmarkSnapshot.LastAvoidanceMilliseconds = 0.0;
		BenchmarkSnapshot.LastAvoidanceAppliedCount = 0;
		BenchmarkSnapshot.LastHardBlockerMovementBlockedEntities = 0;
		return;
	}

	FAlgonaLocalMotionDiagnostics Diagnostics;
	double TotalSpeed = 0.0;
	double TotalForwardSpeed = 0.0;
	double TotalDesiredSpeed = 0.0;
	double TotalAgentRadius = 0.0;
	double TotalSlotError = 0.0;
	double MinimumForwardSpeed = TNumericLimits<double>::Max();
	UWorld* World = GetWorld();
	const double BoundaryStartSeconds = FPlatformTime::Seconds();
	FMassExecutionContext PrepareContext(
		*MassRuntime->EntityManager,
		static_cast<float>(FixedDeltaSeconds));
	MassRuntime->MassStockQuery->ForEachEntityChunk(
		PrepareContext,
		[this, World, &Diagnostics, &TotalSpeed, &TotalForwardSpeed,
			&TotalDesiredSpeed, &TotalAgentRadius, &TotalSlotError,
			&MinimumForwardSpeed](
			FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimVelocityFragment> AlgonaVelocities =
				Context.GetFragmentView<FAlgonaSimVelocityFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();
			TConstArrayView<FAlgonaSimNavigationFragment> Navigation =
				Context.GetFragmentView<FAlgonaSimNavigationFragment>();
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();
			TArrayView<FAgentRadiusFragment> AgentRadii =
				Context.GetMutableFragmentView<FAgentRadiusFragment>();
			TArrayView<FMassVelocityFragment> StockVelocities =
				Context.GetMutableFragmentView<FMassVelocityFragment>();
			TArrayView<FMassDesiredMovementFragment> DesiredMovement =
				Context.GetMutableFragmentView<FMassDesiredMovementFragment>();
			TArrayView<FMassForceFragment> Forces =
				Context.GetMutableFragmentView<FMassForceFragment>();
			TArrayView<FMassMoveTargetFragment> MoveTargets =
				Context.GetMutableFragmentView<FMassMoveTargetFragment>();
			TArrayView<FMassSteeringFragment> Steering =
				Context.GetMutableFragmentView<FMassSteeringFragment>();
			TArrayView<FMassAvoidanceColliderFragment> Colliders =
				Context.GetMutableFragmentView<FMassAvoidanceColliderFragment>();
			TArrayView<FMassNavigationEdgesFragment> NavigationEdges =
				Context.GetMutableFragmentView<FMassNavigationEdgesFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				++Diagnostics.EntitiesProcessed;
				const int32 GroupArrayIndex = GroupIds[Index].GroupId - 1;
				const FAlgonaSimGroupState* Group =
					Groups.IsValidIndex(GroupArrayIndex)
					&& Groups[GroupArrayIndex].Id.Value == GroupIds[Index].GroupId
						? &Groups[GroupArrayIndex]
						: nullptr;
				const FVector CurrentPosition = Positions[Index].Position;
				FVector CurrentVelocity = AlgonaVelocities[Index].Velocity;
				CurrentVelocity.Z = 0.0;
				const double CurrentSpeed = CurrentVelocity.Length();
				TotalSpeed += CurrentSpeed;
				Diagnostics.MaxSpeed = FMath::Max(
					Diagnostics.MaxSpeed,
					CurrentSpeed);

				FTransform& Transform = Transforms[Index].GetMutableTransform();
				Transform.SetLocation(CurrentPosition);
				StockVelocities[Index].Value = CurrentVelocity;
				DesiredMovement[Index].DesiredVelocity = CurrentVelocity;
				DesiredMovement[Index].DesiredMaxSpeedOverride = FLT_MAX;
				Forces[Index].Value = FVector::ZeroVector;
				Steering[Index].Reset();
				NavigationEdges[Index].AvoidanceEdges.Reset();
				NavigationEdges[Index].bExtrudedEdges = false;

				const float AgentRadius = Group != nullptr
					? FMath::Max(Group->MinSameGroupSpacing * 0.5f, 10.0f)
					: 30.0f;
				TotalAgentRadius += AgentRadius;
				AgentRadii[Index].Radius = AgentRadius;
				Colliders[Index] = FMassAvoidanceColliderFragment(
					FMassCircleCollider(AgentRadius));

				FMassMoveTargetFragment& MoveTarget = MoveTargets[Index];
				const bool bCanMove = Group != nullptr
					&& Group->bHasActiveMoveTarget
					&& IsNavigationMovementReady(*Group);
				const FVector DesiredTarget = bCanMove
					? GetGroupMovementTarget(*Group, Navigation[Index])
					: CurrentPosition;
				FVector ToTarget = DesiredTarget - CurrentPosition;
				ToTarget.Z = 0.0;
				const double DistanceToTarget = ToTarget.Length();
				FVector MoveForward = Group != nullptr
					? Group->Formation.TravelDirection
					: FVector::ForwardVector;
				MoveForward.Z = 0.0;
				if (DistanceToTarget > KINDA_SMALL_NUMBER)
				{
					MoveForward = ToTarget / DistanceToTarget;
				}
				else if (!MoveForward.Normalize())
				{
					MoveForward = FVector::ForwardVector;
				}

				MoveTarget.Center = DesiredTarget;
				MoveTarget.Forward = MoveForward;
				MoveTarget.DistanceToGoal = static_cast<float>(DistanceToTarget);
				MoveTarget.EntityDistanceToGoal =
					static_cast<float>(DistanceToTarget);
				MoveTarget.SlackRadius =
					static_cast<float>(FormationSettlingRadius);
				const float DesiredSpeed = bCanMove
					&& DistanceToTarget > KINDA_SMALL_NUMBER
						? static_cast<float>(MovementSpeed)
						: 0.0f;
				TotalDesiredSpeed += DesiredSpeed;
				FVector GroupForward = Group != nullptr
					? Group->Formation.TravelDirection
					: MoveForward;
				GroupForward.Z = 0.0;
				if (!GroupForward.Normalize())
				{
					GroupForward = MoveForward;
				}
				const double ForwardSpeed = FVector::DotProduct(
					CurrentVelocity,
					GroupForward);
				TotalForwardSpeed += ForwardSpeed;
				MinimumForwardSpeed = FMath::Min(
					MinimumForwardSpeed,
					ForwardSpeed);
				const EMassMovementAction DesiredAction = bCanMove
					? EMassMovementAction::Move
					: EMassMovementAction::Stand;
				if (World != nullptr
					&& MoveTarget.GetCurrentAction() != DesiredAction)
				{
					MoveTarget.CreateNewAction(DesiredAction, *World);
				}
				MoveTarget.DesiredSpeed = FMassInt16Real(DesiredSpeed);
				MoveTarget.IntentAtGoal = EMassMovementAction::Stand;
				MoveTarget.bOffBoundaries = false;
				MoveTarget.bSteeringFallingBehind = false;

				if (bCanMove)
				{
					++Diagnostics.MovingEntities;
					++Diagnostics.MoveTargets;
					++Diagnostics.AvoidanceEntities;
				}
				TotalSlotError += DistanceToTarget;
				Diagnostics.MaxSlotError = FMath::Max(
					Diagnostics.MaxSlotError,
					DistanceToTarget);

				if (Group == nullptr
					|| Group->Formation.CorridorSamples.Num() < 2)
				{
					continue;
				}

				struct FEdgePairCandidate
				{
					double DistanceSquared = 0.0;
					int32 SampleIndex = INDEX_NONE;
				};
				static constexpr int32 MaxEdgePairCandidates = 8;
				TArray<
					FEdgePairCandidate,
					TFixedAllocator<MaxEdgePairCandidates>> Candidates;
				const auto SortCandidatesByDistance =
					[](const FEdgePairCandidate& A, const FEdgePairCandidate& B)
					{
						return A.DistanceSquared < B.DistanceSquared;
					};
				for (int32 SampleIndex = 0;
					SampleIndex + 1 < Group->Formation.CorridorSamples.Num();
					++SampleIndex)
				{
					const FAlgonaFormationCorridorSample& Front =
						Group->Formation.CorridorSamples[SampleIndex];
					const FAlgonaFormationCorridorSample& Rear =
						Group->Formation.CorridorSamples[SampleIndex + 1];
					if (!Front.bValid || !Rear.bValid)
					{
						continue;
					}

					FEdgePairCandidate Candidate;
					Candidate.DistanceSquared = FVector::DistSquared(
						CurrentPosition,
						(Front.Center + Rear.Center) * 0.5);
					Candidate.SampleIndex = SampleIndex;
					if (Candidates.Num() < MaxEdgePairCandidates)
					{
						Candidates.Add(Candidate);
						Candidates.Sort(SortCandidatesByDistance);
					}
					else if (Candidate.DistanceSquared
						< Candidates.Last().DistanceSquared)
					{
						Candidates.Last() = Candidate;
						Candidates.Sort(SortCandidatesByDistance);
					}
				}

				for (const FEdgePairCandidate& Candidate : Candidates)
				{
					const FAlgonaFormationCorridorSample& Front =
						Group->Formation.CorridorSamples[Candidate.SampleIndex];
					const FAlgonaFormationCorridorSample& Rear =
						Group->Formation.CorridorSamples[Candidate.SampleIndex + 1];
					const FVector& Right = Group->Formation.LayoutRight;
					const double FrontLeftWidth = FMath::Max(
						static_cast<double>(Front.LeftHalfWidth)
							- FormationCorridorSafetyMargin,
						0.0);
					const double RearLeftWidth = FMath::Max(
						static_cast<double>(Rear.LeftHalfWidth)
							- FormationCorridorSafetyMargin,
						0.0);
					const double FrontRightWidth = FMath::Max(
						static_cast<double>(Front.RightHalfWidth)
							- FormationCorridorSafetyMargin,
						0.0);
					const double RearRightWidth = FMath::Max(
						static_cast<double>(Rear.RightHalfWidth)
							- FormationCorridorSafetyMargin,
						0.0);
					const FVector FrontLeft =
						Front.Center - Right * FrontLeftWidth;
					const FVector RearLeft =
						Rear.Center - Right * RearLeftWidth;
					const FVector FrontRight =
						Front.Center + Right * FrontRightWidth;
					const FVector RearRight =
						Rear.Center + Right * RearRightWidth;
					const double RelevantEdgeDistanceSquared = FMath::Square(
						FMath::Max(600.0, static_cast<double>(AgentRadius) * 4.0));
					if (!FrontLeft.Equals(RearLeft, KINDA_SMALL_NUMBER)
						&& FVector::DistSquared(
							CurrentPosition,
							FMath::ClosestPointOnSegment(
								CurrentPosition,
								FrontLeft,
								RearLeft)) <= RelevantEdgeDistanceSquared)
					{
						NavigationEdges[Index].AvoidanceEdges.Emplace(
							FrontLeft,
							RearLeft);
					}
					if (!RearRight.Equals(FrontRight, KINDA_SMALL_NUMBER)
						&& FVector::DistSquared(
							CurrentPosition,
							FMath::ClosestPointOnSegment(
								CurrentPosition,
								RearRight,
								FrontRight)) <= RelevantEdgeDistanceSquared
						&& NavigationEdges[Index].AvoidanceEdges.Num()
							< FMassNavigationEdgesFragment::MaxEdgesCount)
					{
						NavigationEdges[Index].AvoidanceEdges.Emplace(
							RearRight,
							FrontRight);
					}
				}
				Diagnostics.AvoidanceEdges +=
					NavigationEdges[Index].AvoidanceEdges.Num();
			}
		}
	);
	Diagnostics.BoundaryAdapterMilliseconds =
		(FPlatformTime::Seconds() - BoundaryStartSeconds) * 1000.0;

	const FMassArchetypeEntityCollection Collection(
		MassRuntime->MassStockEntityArchetype,
		MassRuntime->Entities,
		FMassArchetypeEntityCollection::NoDuplicates);
	const auto RunPipelineForCollection =
		[this](FMassRuntimePipeline& Pipeline,
			const FMassArchetypeEntityCollection& PipelineCollection)
		{
			UE::Mass::FProcessingContext ProcessingContext(
				*MassRuntime->EntityManager,
				static_cast<float>(FixedDeltaSeconds));
			UE::Mass::Executor::RunWithCollection(
				Pipeline,
				ProcessingContext,
				PipelineCollection);
		};
	const auto RunPipeline =
		[&RunPipelineForCollection, &Collection](FMassRuntimePipeline& Pipeline)
		{
			RunPipelineForCollection(Pipeline, Collection);
		};

	const double AvoidanceStartSeconds = FPlatformTime::Seconds();
	RunPipeline(MassRuntime->ObstacleGridPipeline);
	const double SteeringStartSeconds = FPlatformTime::Seconds();
	RunPipeline(MassRuntime->SteeringPipeline);
	Diagnostics.SteeringMilliseconds =
		(FPlatformTime::Seconds() - SteeringStartSeconds) * 1000.0;

	struct FForceCaptureSample
	{
		FMassEntityHandle Entity;
		FVector SteeringForce = FVector::ZeroVector;
		FVector MovingAvoidanceForce = FVector::ZeroVector;
		FMassDesiredMovementFragment DesiredMovement;
		FMassNavigationEdgesFragment NavigationEdges;
	};
	static constexpr int32 MaxForceCaptureSamples = 8;
	TArray<FForceCaptureSample, TInlineAllocator<MaxForceCaptureSamples>>
		ForceCaptureSamples;
	double ForceCaptureMilliseconds = 0.0;
	if (bCaptureLocalMotionForcesNextStep
		&& LocalMotionForceCaptureGroupId.IsValid())
	{
		TArray<FMassEntityHandle> GroupCaptureEntities;
		for (const FMassEntityHandle Entity : MassRuntime->Entities)
		{
			FMassEntityView EntityView(*MassRuntime->EntityManager, Entity);
			if (EntityView.GetFragmentData<FAlgonaSimGroupFragment>().GroupId
				!= LocalMotionForceCaptureGroupId.Value)
			{
				continue;
			}
			GroupCaptureEntities.Add(Entity);
		}
		const int32 CaptureSampleCount = FMath::Min(
			GroupCaptureEntities.Num(),
			MaxForceCaptureSamples);
		for (int32 SampleIndex = 0;
			SampleIndex < CaptureSampleCount;
			++SampleIndex)
		{
			const int32 EntityIndex = CaptureSampleCount > 1
				? FMath::RoundToInt(
					static_cast<double>(SampleIndex)
						* static_cast<double>(GroupCaptureEntities.Num() - 1)
						/ static_cast<double>(CaptureSampleCount - 1))
				: 0;
			const FMassEntityHandle Entity =
				GroupCaptureEntities[EntityIndex];
			FMassEntityView EntityView(*MassRuntime->EntityManager, Entity);
			FForceCaptureSample& Sample =
				ForceCaptureSamples.AddDefaulted_GetRef();
			Sample.Entity = Entity;
			Sample.SteeringForce =
				EntityView.GetFragmentData<FMassForceFragment>().Value;
			Sample.DesiredMovement = EntityView
				.GetFragmentData<FMassDesiredMovementFragment>();
			Sample.NavigationEdges = EntityView
				.GetFragmentData<FMassNavigationEdgesFragment>();
			EntityView.GetFragmentData<FMassNavigationEdgesFragment>()
				.AvoidanceEdges.Reset();
		}

		if (!ForceCaptureSamples.IsEmpty())
		{
			TArray<FMassEntityHandle, TInlineAllocator<MaxForceCaptureSamples>>
				CaptureEntities;
			CaptureEntities.Reserve(ForceCaptureSamples.Num());
			for (const FForceCaptureSample& Sample : ForceCaptureSamples)
			{
				CaptureEntities.Add(Sample.Entity);
			}
			const FMassArchetypeEntityCollection CaptureCollection(
				MassRuntime->MassStockEntityArchetype,
				CaptureEntities,
				FMassArchetypeEntityCollection::NoDuplicates);
			const double ForceCaptureStartSeconds = FPlatformTime::Seconds();
			RunPipelineForCollection(
				MassRuntime->AvoidancePipeline,
				CaptureCollection);
			ForceCaptureMilliseconds =
				(FPlatformTime::Seconds() - ForceCaptureStartSeconds) * 1000.0;

			for (FForceCaptureSample& Sample : ForceCaptureSamples)
			{
				FMassEntityView EntityView(
					*MassRuntime->EntityManager,
					Sample.Entity);
				FMassForceFragment& Force =
					EntityView.GetFragmentData<FMassForceFragment>();
				Sample.MovingAvoidanceForce =
					Force.Value - Sample.SteeringForce;
				Force.Value = Sample.SteeringForce;
				EntityView.GetFragmentData<FMassDesiredMovementFragment>() =
					Sample.DesiredMovement;
				EntityView.GetFragmentData<FMassNavigationEdgesFragment>() =
					Sample.NavigationEdges;
			}
		}
	}
	RunPipeline(MassRuntime->AvoidancePipeline);
	Diagnostics.AvoidanceMilliseconds =
		(FPlatformTime::Seconds() - AvoidanceStartSeconds) * 1000.0
		- Diagnostics.SteeringMilliseconds
		- ForceCaptureMilliseconds;
	if (bCaptureLocalMotionForcesNextStep)
	{
		FAlgonaLocalMotionForceDiagnostics ForceDiagnostics;
		ForceDiagnostics.GroupId = LocalMotionForceCaptureGroupId;
		double TotalSteeringForce = 0.0;
		double TotalMovingAvoidanceForce = 0.0;
		double TotalEnvironmentAvoidanceForce = 0.0;
		for (const FForceCaptureSample& Sample : ForceCaptureSamples)
		{
			FMassEntityView EntityView(
				*MassRuntime->EntityManager,
				Sample.Entity);
			const FVector TotalAvoidanceForce =
				EntityView.GetFragmentData<FMassForceFragment>().Value
				- Sample.SteeringForce;
			const FVector EnvironmentAvoidanceForce =
				TotalAvoidanceForce - Sample.MovingAvoidanceForce;
			const double SteeringMagnitude = Sample.SteeringForce.Length();
			const double MovingMagnitude =
				Sample.MovingAvoidanceForce.Length();
			const double EnvironmentMagnitude =
				EnvironmentAvoidanceForce.Length();
			TotalSteeringForce += SteeringMagnitude;
			TotalMovingAvoidanceForce += MovingMagnitude;
			TotalEnvironmentAvoidanceForce += EnvironmentMagnitude;
			ForceDiagnostics.MaxSteeringForce = FMath::Max(
				ForceDiagnostics.MaxSteeringForce,
				SteeringMagnitude);
			ForceDiagnostics.MaxMovingAvoidanceForce = FMath::Max(
				ForceDiagnostics.MaxMovingAvoidanceForce,
				MovingMagnitude);
			ForceDiagnostics.MaxEnvironmentAvoidanceForce = FMath::Max(
				ForceDiagnostics.MaxEnvironmentAvoidanceForce,
				EnvironmentMagnitude);
		}
		ForceDiagnostics.SampleCount = ForceCaptureSamples.Num();
		if (ForceDiagnostics.SampleCount > 0)
		{
			const double SampleDivisor =
				static_cast<double>(ForceDiagnostics.SampleCount);
			ForceDiagnostics.AverageSteeringForce =
				TotalSteeringForce / SampleDivisor;
			ForceDiagnostics.AverageMovingAvoidanceForce =
				TotalMovingAvoidanceForce / SampleDivisor;
			ForceDiagnostics.AverageEnvironmentAvoidanceForce =
				TotalEnvironmentAvoidanceForce / SampleDivisor;
			ForceDiagnostics.bValid = true;
		}
		MassRuntime->LocalMotionForceDiagnostics = ForceDiagnostics;
		UE_LOG(
			LogAlgonaSimulation,
			Log,
			TEXT("P1 local motion force capture. GroupId=%d, Samples=%d, SteeringForceAvg=%.2f, SteeringForceMax=%.2f, MovingAvoidanceForceAvg=%.2f, MovingAvoidanceForceMax=%.2f, EnvironmentAvoidanceResidualAvg=%.2f, EnvironmentAvoidanceResidualMax=%.2f, DiagnosticPassMs=%.3f, DiagnosticPassOnly=true"),
			ForceDiagnostics.GroupId.Value,
			ForceDiagnostics.SampleCount,
			ForceDiagnostics.AverageSteeringForce,
			ForceDiagnostics.MaxSteeringForce,
			ForceDiagnostics.AverageMovingAvoidanceForce,
			ForceDiagnostics.MaxMovingAvoidanceForce,
			ForceDiagnostics.AverageEnvironmentAvoidanceForce,
			ForceDiagnostics.MaxEnvironmentAvoidanceForce,
			ForceCaptureMilliseconds);
		bCaptureLocalMotionForcesNextStep = false;
		LocalMotionForceCaptureGroupId = FSimGroupId();
	}

	const double MovementStartSeconds = FPlatformTime::Seconds();
	RunPipeline(MassRuntime->ApplyForcePipeline);
	int32 BlockedEntities = 0;
	int32 CorridorClampedEntities = 0;
	int32 CorridorConstraintCount = 0;
	double TotalConstraintCorrection = 0.0;
	double MaxConstraintCorrection = 0.0;
	double MaxPenetrationDepth = 0.0;
	double TotalNormalMotionClipped = 0.0;
	double MaxNormalMotionClipped = 0.0;
	FMassExecutionContext IntegrateContext(
		*MassRuntime->EntityManager,
		static_cast<float>(FixedDeltaSeconds));
	MassRuntime->MassStockQuery->ForEachEntityChunk(
		IntegrateContext,
		[this, &BlockedEntities, &CorridorClampedEntities,
			&CorridorConstraintCount, &TotalConstraintCorrection,
			&MaxConstraintCorrection, &MaxPenetrationDepth,
			&TotalNormalMotionClipped, &MaxNormalMotionClipped](
			FMassExecutionContext& Context)
		{
			TArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetMutableFragmentView<FAlgonaSimPositionFragment>();
			TArrayView<FAlgonaSimVelocityFragment> AlgonaVelocities =
				Context.GetMutableFragmentView<FAlgonaSimVelocityFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();
			TArrayView<FAlgonaSimStateFragment> States =
				Context.GetMutableFragmentView<FAlgonaSimStateFragment>();
			TArrayView<FAlgonaSimNavigationFragment> Navigation =
				Context.GetMutableFragmentView<FAlgonaSimNavigationFragment>();
			TArrayView<FTransformFragment> Transforms =
				Context.GetMutableFragmentView<FTransformFragment>();
			TConstArrayView<FAgentRadiusFragment> AgentRadii =
				Context.GetFragmentView<FAgentRadiusFragment>();
			TArrayView<FMassVelocityFragment> StockVelocities =
				Context.GetMutableFragmentView<FMassVelocityFragment>();
			TArrayView<FMassDesiredMovementFragment> DesiredMovement =
				Context.GetMutableFragmentView<FMassDesiredMovementFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				FVector Velocity = DesiredMovement[Index].DesiredVelocity;
				Velocity.Z = 0.0;
				FVector ProposedPosition =
					Positions[Index].Position + Velocity * FixedDeltaSeconds;
				const int32 GroupArrayIndex = GroupIds[Index].GroupId - 1;
				bool bCorridorClamped = false;
				int32 EntityConstraintCount = 0;
				double SafetyCorrectionDistance = 0.0;
				double PenetrationDepth = 0.0;
				double NormalMotionClipped = 0.0;
				if (Groups.IsValidIndex(GroupArrayIndex)
					&& Groups[GroupArrayIndex].Id.Value
						== GroupIds[Index].GroupId)
				{
					ProposedPosition = ConstrainMovementToFormationCorridor(
						Groups[GroupArrayIndex],
						Positions[Index].Position,
						ProposedPosition,
						AgentRadii[Index].Radius,
						bCorridorClamped,
						EntityConstraintCount,
						SafetyCorrectionDistance,
						PenetrationDepth,
						NormalMotionClipped
					);
				}
				if (bCorridorClamped)
				{
					++CorridorClampedEntities;
				}
				CorridorConstraintCount += EntityConstraintCount;
				TotalConstraintCorrection += SafetyCorrectionDistance;
				MaxConstraintCorrection = FMath::Max(
					MaxConstraintCorrection,
					SafetyCorrectionDistance);
				MaxPenetrationDepth = FMath::Max(
					MaxPenetrationDepth,
					PenetrationDepth);
				TotalNormalMotionClipped += NormalMotionClipped;
				MaxNormalMotionClipped = FMath::Max(
					MaxNormalMotionClipped,
					NormalMotionClipped);
				bool bBlocked = false;
				const FVector ConstrainedPosition =
					ConstrainMovementAgainstHardBlockers(
						Positions[Index].Position,
						ProposedPosition,
						AgentRadii[Index].Radius,
						bBlocked);
				if (bBlocked)
				{
					++BlockedEntities;
				}
				Velocity = FixedDeltaSeconds > SMALL_NUMBER
					? (ConstrainedPosition - Positions[Index].Position)
						/ FixedDeltaSeconds
					: FVector::ZeroVector;
				Positions[Index].Position = ConstrainedPosition;
				AlgonaVelocities[Index].Velocity = Velocity;
				Navigation[Index].DesiredVelocity = Velocity;
				StockVelocities[Index].Value = Velocity;
				DesiredMovement[Index].DesiredVelocity = Velocity;
				States[Index].State = Velocity.IsNearlyZero(1.0)
					? EAlgonaSimEntityState::Idle
					: EAlgonaSimEntityState::Moving;

				FTransform& Transform = Transforms[Index].GetMutableTransform();
				Transform.SetLocation(ConstrainedPosition);
				FVector Facing = Velocity;
				Facing.Z = 0.0;
				if (!Facing.Normalize())
				{
					Facing = Groups.IsValidIndex(GroupArrayIndex)
						? Groups[GroupArrayIndex].Formation.FacingDirection
						: FVector::ForwardVector;
				}
				Transform.SetRotation(FRotationMatrix::MakeFromX(Facing).ToQuat());
			}
		}
	);
	Diagnostics.MovementMilliseconds =
		(FPlatformTime::Seconds() - MovementStartSeconds) * 1000.0;
	Diagnostics.AverageSpeed = Diagnostics.EntitiesProcessed > 0
		? TotalSpeed / static_cast<double>(Diagnostics.EntitiesProcessed)
		: 0.0;
	Diagnostics.AverageSlotError = Diagnostics.EntitiesProcessed > 0
		? TotalSlotError / static_cast<double>(Diagnostics.EntitiesProcessed)
		: 0.0;
	Diagnostics.CorridorClampedEntities = CorridorClampedEntities;
	Diagnostics.CorridorConstraintCount = CorridorConstraintCount;
	Diagnostics.AverageForwardSpeed = Diagnostics.EntitiesProcessed > 0
		? TotalForwardSpeed / static_cast<double>(Diagnostics.EntitiesProcessed)
		: 0.0;
	Diagnostics.MinForwardSpeed = MinimumForwardSpeed
		< TNumericLimits<double>::Max()
		? MinimumForwardSpeed
		: 0.0;
	Diagnostics.AverageDesiredSpeed = Diagnostics.EntitiesProcessed > 0
		? TotalDesiredSpeed / static_cast<double>(Diagnostics.EntitiesProcessed)
		: 0.0;
	Diagnostics.AverageAgentRadius = Diagnostics.EntitiesProcessed > 0
		? TotalAgentRadius / static_cast<double>(Diagnostics.EntitiesProcessed)
		: 0.0;
	Diagnostics.AverageConstraintCorrection = CorridorConstraintCount > 0
		? TotalConstraintCorrection
			/ static_cast<double>(CorridorConstraintCount)
		: 0.0;
	Diagnostics.MaxConstraintCorrection = MaxConstraintCorrection;
	Diagnostics.MaxPenetrationDepth = MaxPenetrationDepth;
	Diagnostics.AverageNormalMotionClipped = CorridorConstraintCount > 0
		? TotalNormalMotionClipped
			/ static_cast<double>(CorridorConstraintCount)
		: 0.0;
	Diagnostics.MaxNormalMotionClipped = MaxNormalMotionClipped;
	MassRuntime->LocalMotionDiagnostics = Diagnostics;

	BenchmarkSnapshot.LastMovementMilliseconds =
		Diagnostics.MovementMilliseconds;
	BenchmarkSnapshot.LastMovementEntityVisits = Diagnostics.EntitiesProcessed;
	BenchmarkSnapshot.LastMovementUpdatedEntities = Diagnostics.EntitiesProcessed;
	BenchmarkSnapshot.LastMovementSkippedByLOD = 0;
	BenchmarkSnapshot.LastNeighborQueryMilliseconds = 0.0;
	BenchmarkSnapshot.LastNeighborQueryCount = Diagnostics.AvoidanceEntities;
	BenchmarkSnapshot.LastNeighborQueriesSkippedByLOD = 0;
	BenchmarkSnapshot.LastHighLODNeighborQueryCount =
		Diagnostics.AvoidanceEntities;
	BenchmarkSnapshot.LastMediumLODNeighborQueryCount = 0;
	BenchmarkSnapshot.LastLowLODNeighborQueryCount = 0;
	BenchmarkSnapshot.LastNeighborCandidateCount = 0;
	BenchmarkSnapshot.LastNeighborMaxCandidates = 0;
	BenchmarkSnapshot.LastNeighborAverageCandidates = 0.0;
	BenchmarkSnapshot.LastAvoidanceMilliseconds =
		Diagnostics.AvoidanceMilliseconds;
	BenchmarkSnapshot.LastAvoidanceAppliedCount =
		Diagnostics.AvoidanceEntities;
	BenchmarkSnapshot.LastHardBlockerMovementBlockedEntities = BlockedEntities;
	BenchmarkSnapshot.TotalHardBlockerMovementBlockedEntities += BlockedEntities;
}

void UAlgonaSimulationSubsystem::ProcessMovement()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_MassMovement);

	if (!bMovementEnabled || MassRuntime == nullptr
		|| MassRuntime->EntityManager == nullptr
		|| MassRuntime->MovementQuery == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		BenchmarkSnapshot.LastMovementMilliseconds = 0.0;
		BenchmarkSnapshot.LastMovementEntityVisits = 0;
		BenchmarkSnapshot.LastMovementUpdatedEntities = 0;
		BenchmarkSnapshot.LastMovementSkippedByLOD = 0;
		BenchmarkSnapshot.LastNeighborQueryMilliseconds = 0.0;
		BenchmarkSnapshot.LastNeighborQueryCount = 0;
		BenchmarkSnapshot.LastNeighborQueriesSkippedByLOD = 0;
		BenchmarkSnapshot.LastHighLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastMediumLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastLowLODNeighborQueryCount = 0;
		BenchmarkSnapshot.LastNeighborCandidateCount = 0;
		BenchmarkSnapshot.LastNeighborMaxCandidates = 0;
		BenchmarkSnapshot.LastNeighborAverageCandidates = 0.0;
		BenchmarkSnapshot.LastAvoidanceMilliseconds = 0.0;
		BenchmarkSnapshot.LastAvoidanceAppliedCount = 0;
		BenchmarkSnapshot.LastHardBlockerMovementBlockedEntities = 0;
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	int32 VisitedEntities = 0;
	int32 UpdatedEntities = 0;
	int32 SkippedByLOD = 0;
	int32 NeighborQueryCount = 0;
	int32 NeighborQueriesSkippedByLOD = 0;
	int32 HighLODNeighborQueryCount = 0;
	int32 MediumLODNeighborQueryCount = 0;
	int32 LowLODNeighborQueryCount = 0;
	int32 NeighborCandidateCount = 0;
	int32 NeighborMaxCandidateCount = 0;
	int32 AvoidanceAppliedCount = 0;
	int32 HardBlockerMovementBlockedEntities = 0;
	double NeighborQueryMilliseconds = 0.0;
	double AvoidanceMilliseconds = 0.0;
	FMassExecutionContext ExecutionContext(
		*MassRuntime->EntityManager,
		static_cast<float>(FixedDeltaSeconds)
	);

	MassRuntime->MovementQuery->ForEachEntityChunk(
		ExecutionContext,
		[this, &VisitedEntities, &UpdatedEntities, &SkippedByLOD,
			&NeighborQueryCount, &NeighborQueriesSkippedByLOD,
			&HighLODNeighborQueryCount, &MediumLODNeighborQueryCount,
			&LowLODNeighborQueryCount, &NeighborCandidateCount,
			&NeighborMaxCandidateCount, &HardBlockerMovementBlockedEntities,
			&AvoidanceAppliedCount, &NeighborQueryMilliseconds,
			&AvoidanceMilliseconds](
			FMassExecutionContext& Context)
		{
			TArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetMutableFragmentView<FAlgonaSimPositionFragment>();
			TArrayView<FAlgonaSimVelocityFragment> Velocities =
				Context.GetMutableFragmentView<FAlgonaSimVelocityFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();
			TArrayView<FAlgonaSimStateFragment> States =
				Context.GetMutableFragmentView<FAlgonaSimStateFragment>();
			TArrayView<FAlgonaSimLODFragment> LODs =
				Context.GetMutableFragmentView<FAlgonaSimLODFragment>();
			TArrayView<FAlgonaSimNavigationFragment> NavigationFragments =
				Context.GetMutableFragmentView<FAlgonaSimNavigationFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				++VisitedEntities;

				const uint8 UpdateStride = bSimulationLODEnabled
					? FMath::Max<uint8>(LODs[Index].UpdateStride, 1)
					: 1;
				if ((SimulationTick + LODs[Index].TickOffset) % UpdateStride != 0)
				{
					++SkippedByLOD;
					const int32 GroupArrayIndex =
						GroupIds[Index].GroupId - 1;
					if (bP1LocalSeparationEnabled
						&& bP1SpatialGridEnabled
						&& NeighborMaxCandidates > 0
						&& !SpatialGrid.Entries.IsEmpty()
						&& Groups.IsValidIndex(GroupArrayIndex)
						&& Groups[GroupArrayIndex].Id.Value
							== GroupIds[Index].GroupId
						&& Groups[GroupArrayIndex].bHasActiveMoveTarget
						&& IsNavigationMovementReady(Groups[GroupArrayIndex]))
					{
						++NeighborQueriesSkippedByLOD;
					}
					continue;
				}

				++UpdatedEntities;

				const int32 GroupArrayIndex = GroupIds[Index].GroupId - 1;
				if (!Groups.IsValidIndex(GroupArrayIndex)
					|| Groups[GroupArrayIndex].Id.Value != GroupIds[Index].GroupId)
				{
					Velocities[Index].Velocity = FVector::ZeroVector;
					NavigationFragments[Index].DesiredVelocity =
						FVector::ZeroVector;
					States[Index].State = EAlgonaSimEntityState::Idle;
					continue;
				}

				const FAlgonaSimGroupState& Group = Groups[GroupArrayIndex];
				if (!Group.bHasActiveMoveTarget)
				{
					Velocities[Index].Velocity = FVector::ZeroVector;
					NavigationFragments[Index].DesiredVelocity =
						FVector::ZeroVector;
					States[Index].State = EAlgonaSimEntityState::Idle;
					continue;
				}
				if (!IsNavigationMovementReady(Group))
				{
					Velocities[Index].Velocity = FVector::ZeroVector;
					NavigationFragments[Index].DesiredVelocity =
						FVector::ZeroVector;
					States[Index].State = EAlgonaSimEntityState::Idle;
					continue;
				}

				const FVector MovementTarget =
					GetGroupMovementTarget(
						Group,
						NavigationFragments[Index]
					);
				const FVector ToTarget =
					MovementTarget - Positions[Index].Position;
				const double Distance = ToTarget.Length();
				if (Distance <= KINDA_SMALL_NUMBER)
				{
					Velocities[Index].Velocity = FVector::ZeroVector;
					NavigationFragments[Index].DesiredVelocity =
						FVector::ZeroVector;
					States[Index].State = EAlgonaSimEntityState::Idle;
					continue;
				}

				const double StepSeconds =
					FixedDeltaSeconds * static_cast<double>(UpdateStride);
				FVector TravelDirection = Group.Formation.TravelDirection;
				TravelDirection.Z = 0.0;
				if (!TravelDirection.Normalize())
				{
					TravelDirection = ToTarget;
					TravelDirection.Z = 0.0;
					if (!TravelDirection.Normalize())
					{
						TravelDirection = FVector::ForwardVector;
					}
				}

				const double ForwardDistance = FVector::DotProduct(
					ToTarget,
					TravelDirection
				);
				const double MaxForwardSpeed =
					MovementSpeed
					* FMath::Clamp(
						static_cast<double>(
							Group.Formation.ForwardSpeedScale
						),
						0.0,
						1.0
					);
				const double ForwardSpeed = FMath::Clamp(
					ForwardDistance / StepSeconds,
					-MovementSpeed,
					MaxForwardSpeed
				);
				const FVector LateralDelta =
					ToTarget - TravelDirection * ForwardDistance;
				const double LateralDistance = LateralDelta.Length();
				FVector FinalVelocity =
					TravelDirection * ForwardSpeed;
				if (LateralDistance > KINDA_SMALL_NUMBER)
				{
					const double LateralSpeed = FMath::Min(
						LateralDistance / StepSeconds,
						MovementSpeed
					);
					FinalVelocity +=
						LateralDelta / LateralDistance * LateralSpeed;
				}
				const double BaseSpeed = FinalVelocity.Length();
				if (BaseSpeed > MovementSpeed && BaseSpeed > KINDA_SMALL_NUMBER)
				{
					FinalVelocity *= MovementSpeed / BaseSpeed;
				}
				if (bP1LocalSeparationEnabled
					&& bP1SpatialGridEnabled
					&& NeighborMaxCandidates > 0
					&& !SpatialGrid.Entries.IsEmpty())
				{
					int32 CandidateCount = 0;
					const double QueryStartSeconds = FPlatformTime::Seconds();
					const FVector SeparationVelocity =
						ComputeSeparationVelocity(
							Context.GetEntity(Index),
							Group.Id,
							Positions[Index].Position,
							NavigationFragments[Index].Radius,
							CandidateCount
						);
					NeighborQueryMilliseconds +=
						(FPlatformTime::Seconds() - QueryStartSeconds) * 1000.0;
					++NeighborQueryCount;
					switch (LODs[Index].LOD)
					{
					case EAlgonaSimLOD::High:
						++HighLODNeighborQueryCount;
						break;
					case EAlgonaSimLOD::Medium:
						++MediumLODNeighborQueryCount;
						break;
					case EAlgonaSimLOD::Low:
						++LowLODNeighborQueryCount;
						break;
					default:
						break;
					}
					NeighborCandidateCount += CandidateCount;
					NeighborMaxCandidateCount =
						FMath::Max(NeighborMaxCandidateCount, CandidateCount);

					if (!SeparationVelocity.IsNearlyZero())
					{
						++AvoidanceAppliedCount;
						const double AvoidanceStartSeconds =
							FPlatformTime::Seconds();
						FinalVelocity +=
							SeparationVelocity * DesiredVelocityBlend;
						FinalVelocity.Z = 0.0;
						const double BlendedSpeed = FinalVelocity.Length();
						if (BlendedSpeed > MovementSpeed
							&& BlendedSpeed > KINDA_SMALL_NUMBER)
						{
							FinalVelocity *= MovementSpeed / BlendedSpeed;
						}
						AvoidanceMilliseconds +=
							(FPlatformTime::Seconds() - AvoidanceStartSeconds)
							* 1000.0;
					}
				}
				const double FinalSpeed = FinalVelocity.Length();
				if (FinalSpeed <= KINDA_SMALL_NUMBER)
				{
					Velocities[Index].Velocity = FVector::ZeroVector;
					NavigationFragments[Index].DesiredVelocity =
						FVector::ZeroVector;
					States[Index].State = EAlgonaSimEntityState::Idle;
					continue;
				}
				const FVector FinalDirection = FinalVelocity / FinalSpeed;
				const double AppliedDistance = FMath::Min(
					FinalSpeed * StepSeconds,
					Distance
				);
				const FVector ProposedPosition =
					Positions[Index].Position
					+ FinalDirection * AppliedDistance;
				bool bBlockedByHardBlocker = false;
				const FVector ConstrainedPosition =
					ConstrainMovementAgainstHardBlockers(
						Positions[Index].Position,
						ProposedPosition,
						NavigationFragments[Index].Radius,
						bBlockedByHardBlocker
					);
				if (bBlockedByHardBlocker)
				{
					++HardBlockerMovementBlockedEntities;
					++BenchmarkSnapshot.TotalHardBlockerMovementBlockedEntities;
					Velocities[Index].Velocity = FVector::ZeroVector;
					NavigationFragments[Index].DesiredVelocity =
						FVector::ZeroVector;
					States[Index].State = EAlgonaSimEntityState::Idle;
					continue;
				}

				Velocities[Index].Velocity =
					FinalDirection * (AppliedDistance / StepSeconds);
				NavigationFragments[Index].DesiredVelocity =
					Velocities[Index].Velocity;
				Positions[Index].Position = ConstrainedPosition;
				States[Index].State = EAlgonaSimEntityState::Moving;
			}
		}
	);

	BenchmarkSnapshot.LastMovementMilliseconds =
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	BenchmarkSnapshot.LastMovementEntityVisits = VisitedEntities;
	BenchmarkSnapshot.LastMovementUpdatedEntities = UpdatedEntities;
	BenchmarkSnapshot.LastMovementSkippedByLOD = SkippedByLOD;
	BenchmarkSnapshot.LastNeighborQueryMilliseconds =
		NeighborQueryMilliseconds;
	BenchmarkSnapshot.LastNeighborQueryCount = NeighborQueryCount;
	BenchmarkSnapshot.LastNeighborQueriesSkippedByLOD =
		NeighborQueriesSkippedByLOD;
	BenchmarkSnapshot.LastHighLODNeighborQueryCount =
		HighLODNeighborQueryCount;
	BenchmarkSnapshot.LastMediumLODNeighborQueryCount =
		MediumLODNeighborQueryCount;
	BenchmarkSnapshot.LastLowLODNeighborQueryCount =
		LowLODNeighborQueryCount;
	BenchmarkSnapshot.LastNeighborCandidateCount = NeighborCandidateCount;
	BenchmarkSnapshot.LastNeighborMaxCandidates = NeighborMaxCandidateCount;
	BenchmarkSnapshot.LastNeighborAverageCandidates =
		NeighborQueryCount > 0
			? static_cast<double>(NeighborCandidateCount)
				/ static_cast<double>(NeighborQueryCount)
			: 0.0;
	BenchmarkSnapshot.LastAvoidanceMilliseconds = AvoidanceMilliseconds;
	BenchmarkSnapshot.LastAvoidanceAppliedCount = AvoidanceAppliedCount;
	BenchmarkSnapshot.LastHardBlockerMovementBlockedEntities =
		HardBlockerMovementBlockedEntities;
}

void UAlgonaSimulationSubsystem::UpdateStuckRecovery()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_P1StuckRecovery);

	BenchmarkSnapshot.LastStuckGroupCount = 0;
	if (!bP1NavigationEnabled
		|| !bP1StuckDetectionEnabled
		|| Groups.IsEmpty()
		|| MassRuntime == nullptr
		|| MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		return;
	}

	TArray<FVector> GroupCenters;
	TArray<int32> GroupCenterCounts;
	GroupCenters.SetNumZeroed(Groups.Num());
	GroupCenterCounts.SetNumZeroed(Groups.Num());

	FMassEntityQuery CenterQuery(MassRuntime->EntityManager->AsShared());
	CenterQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	CenterQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
	CenterQuery.ForEachEntityChunk(
		ExecutionContext,
		[this, &GroupCenters, &GroupCenterCounts](
			FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				const int32 GroupArrayIndex = GroupIds[Index].GroupId - 1;
				if (Groups.IsValidIndex(GroupArrayIndex)
					&& Groups[GroupArrayIndex].Id.Value
						== GroupIds[Index].GroupId)
				{
					GroupCenters[GroupArrayIndex] += Positions[Index].Position;
					++GroupCenterCounts[GroupArrayIndex];
				}
			}
		}
	);

	for (FAlgonaSimGroupState& Group : Groups)
	{
		if (!Group.bHasActiveMoveTarget
			|| Group.Navigation.Status
				!= EAlgonaGroupNavigationStatus::RouteReady)
		{
			Group.Navigation.StuckTicks = 0;
			Group.Navigation.LastProgressDistanceToDestination = 0.0;
			Group.Navigation.LastProgressSampleTick = 0;
			Group.Navigation.LastProgressRoutePointIndex = INDEX_NONE;
			continue;
		}

		const int32 GroupArrayIndex = Group.Id.Value - 1;
		if (!GroupCenters.IsValidIndex(GroupArrayIndex)
			|| GroupCenterCounts[GroupArrayIndex] <= 0)
		{
			continue;
		}

		const FVector GroupCenter =
			GroupCenters[GroupArrayIndex]
			/ static_cast<double>(GroupCenterCounts[GroupArrayIndex]);
		const double DistanceToDestination =
			FVector::Dist2D(GroupCenter, Group.Navigation.Destination);
		const int32 CurrentRoutePointIndex =
			Group.Navigation.Route.CurrentPointIndex;
		const bool bHasBaseline =
			Group.Navigation.LastProgressSampleTick != 0;
		if (!bHasBaseline)
		{
			Group.Navigation.LastProgressCenter = GroupCenter;
			Group.Navigation.LastProgressAnchor = Group.Formation.AnchorPosition;
			Group.Navigation.LastProgressDistanceToDestination =
				DistanceToDestination;
			Group.Navigation.LastProgressRoutePointIndex =
				CurrentRoutePointIndex;
			Group.Navigation.LastProgressSampleTick = SimulationTick;
			continue;
		}

		const uint64 ElapsedTicks =
			SimulationTick - Group.Navigation.LastProgressSampleTick;
		if (ElapsedTicks < static_cast<uint64>(StuckObservationTicks))
		{
			continue;
		}

		const double CenterDisplacement = FVector::Dist2D(
			GroupCenter,
			Group.Navigation.LastProgressCenter
		);
		const double AnchorDisplacement = FVector::Dist2D(
			Group.Formation.AnchorPosition,
			Group.Navigation.LastProgressAnchor
		);
		const double DestinationImprovement =
			Group.Navigation.LastProgressDistanceToDestination
			- DistanceToDestination;
		const bool bRouteAdvanced =
			CurrentRoutePointIndex
			!= Group.Navigation.LastProgressRoutePointIndex;
		const bool bMadeProgress =
			bRouteAdvanced
			|| CenterDisplacement >= StuckDistanceEpsilon
			|| AnchorDisplacement >= StuckDistanceEpsilon
			|| DestinationImprovement >= StuckDistanceEpsilon;

		Group.Navigation.LastProgressCenter = GroupCenter;
		Group.Navigation.LastProgressAnchor = Group.Formation.AnchorPosition;
		Group.Navigation.LastProgressDistanceToDestination =
			DistanceToDestination;
		Group.Navigation.LastProgressRoutePointIndex = CurrentRoutePointIndex;
		Group.Navigation.LastProgressSampleTick = SimulationTick;
		Group.Navigation.StuckTicks = bMadeProgress
			? 0
			: Group.Navigation.StuckTicks + static_cast<int32>(ElapsedTicks);

		if (Group.Navigation.StuckTicks < StuckRequiredTicks)
		{
			continue;
		}

		++BenchmarkSnapshot.LastStuckGroupCount;
		const bool bCooldownElapsed =
			Group.Navigation.LastRepathTick == 0
			|| SimulationTick - Group.Navigation.LastRepathTick
				>= static_cast<uint64>(RepathCooldownTicks);
		if (bCooldownElapsed)
		{
			QueueRepathForGroup(Group, TEXT("StuckRecovery"), 5);
			UE_LOG(
				LogAlgonaSimulation,
				Log,
				TEXT("P1 stuck recovery queued repath. Tick=%llu, GroupId=%d, DistanceToDestination=%.1f, CenterDelta=%.1f, AnchorDelta=%.1f, DestinationImprovement=%.1f, ObservationTicks=%llu"),
				SimulationTick,
				Group.Id.Value,
				DistanceToDestination,
				CenterDisplacement,
				AnchorDisplacement,
				DestinationImprovement,
				ElapsedTicks
			);
		}
	}
}

FVector UAlgonaSimulationSubsystem::ComputeGroupCenter(FSimGroupId GroupId) const
{
	if (MassRuntime == nullptr || MassRuntime->EntityManager == nullptr
		|| MassRuntime->Entities.IsEmpty())
	{
		const FAlgonaSimGroupState* Group = FindGroup(GroupId);
		return Group != nullptr
			? Group->TargetPosition
			: FVector::ZeroVector;
	}

	FMassEntityQuery CenterQuery(MassRuntime->EntityManager->AsShared());
	CenterQuery.AddRequirement<FAlgonaSimPositionFragment>(
		EMassFragmentAccess::ReadOnly
	);
	CenterQuery.AddRequirement<FAlgonaSimGroupFragment>(
		EMassFragmentAccess::ReadOnly
	);

	FVector AccumulatedPosition = FVector::ZeroVector;
	int32 MatchingEntities = 0;
	FMassExecutionContext ExecutionContext(*MassRuntime->EntityManager);
	CenterQuery.ForEachEntityChunk(
		ExecutionContext,
		[GroupId, &AccumulatedPosition, &MatchingEntities](
			FMassExecutionContext& Context)
		{
			TConstArrayView<FAlgonaSimPositionFragment> Positions =
				Context.GetFragmentView<FAlgonaSimPositionFragment>();
			TConstArrayView<FAlgonaSimGroupFragment> GroupIds =
				Context.GetFragmentView<FAlgonaSimGroupFragment>();

			for (int32 Index = 0; Index < Context.GetNumEntities(); ++Index)
			{
				if (GroupIds[Index].GroupId == GroupId.Value)
				{
					AccumulatedPosition += Positions[Index].Position;
					++MatchingEntities;
				}
			}
		}
	);

	if (MatchingEntities == 0)
	{
		const FAlgonaSimGroupState* Group = FindGroup(GroupId);
		return Group != nullptr
			? Group->TargetPosition
			: FVector::ZeroVector;
	}

	return AccumulatedPosition / static_cast<double>(MatchingEntities);
}

FVector UAlgonaSimulationSubsystem::GetGroupMovementTarget(
	const FAlgonaSimGroupState& Group,
	const FAlgonaSimNavigationFragment& NavigationFragment) const
{
	if (bP1NavigationEnabled)
	{
		if (IsNavigationMovementReady(Group))
		{
			if (bP1FormationEnabled
				&& NavigationFragment.SlotIndex != INDEX_NONE)
			{
				return NavigationFragment.DesiredSlotPosition;
			}

			return Group.Navigation.Route.CurrentMoveTarget;
		}

		return NavigationFragment.DesiredSlotPosition;
	}

	if (bP1FormationEnabled
		&& NavigationFragment.SlotIndex != INDEX_NONE)
	{
		return NavigationFragment.DesiredSlotPosition;
	}

	return Group.TargetPosition;
}

FVector UAlgonaSimulationSubsystem::ComputeFormationSlotOffset(
	const FAlgonaFormationState& Formation,
	int32 SlotIndex) const
{
	if (SlotIndex < 0)
	{
		return FVector::ZeroVector;
	}

	const int32 Columns = FMath::Max(Formation.OriginalColumns, 1);
	const int32 Rows = FMath::Max(Formation.OriginalRows, 1);
	const int32 Row = SlotIndex / Columns;
	const int32 Column = SlotIndex % Columns;
	const double SlotSpacing = static_cast<double>(Formation.SlotSpacing);
	const double XOffset =
		(static_cast<double>(Column) - (static_cast<double>(Columns) - 1.0) * 0.5)
		* SlotSpacing;
	const double YOffset =
		((static_cast<double>(Rows) - 1.0) * 0.5 - static_cast<double>(Row))
		* SlotSpacing;

	return Formation.LayoutRight * XOffset
		+ Formation.LayoutForward * YOffset;
}

FAlgonaFormationCorridorSample
UAlgonaSimulationSubsystem::SampleFormationCorridor(
	const FAlgonaFormationState& Formation,
	double LongitudinalOffset) const
{
	FAlgonaFormationCorridorSample Result;
	Result.LongitudinalOffset = static_cast<float>(LongitudinalOffset);
	FVector CorridorForward = Formation.TravelDirection;
	CorridorForward.Z = 0.0;
	if (!CorridorForward.Normalize())
	{
		CorridorForward = Formation.LayoutForward;
		CorridorForward.Z = 0.0;
		if (!CorridorForward.Normalize())
		{
			CorridorForward = FVector::ForwardVector;
		}
	}
	Result.Center =
		Formation.AnchorPosition
			+ CorridorForward * LongitudinalOffset;

	if (Formation.CorridorSamples.IsEmpty())
	{
		return Result;
	}

	const double FrontOffset = static_cast<double>(
		Formation.CorridorSamples[0].LongitudinalOffset
	);
	const double RearOffset = static_cast<double>(
		Formation.CorridorSamples.Last().LongitudinalOffset
	);
	const double ProfileLength = FMath::Max(FrontOffset - RearOffset, 1.0);
	const double Alpha = FMath::Clamp(
		(FrontOffset - LongitudinalOffset) / ProfileLength,
		0.0,
		1.0
	);
	const double SamplePosition =
		Alpha * static_cast<double>(Formation.CorridorSamples.Num() - 1);
	int32 LowerIndex = FMath::FloorToInt(SamplePosition);
	int32 UpperIndex = FMath::CeilToInt(SamplePosition);
	while (LowerIndex >= 0
		&& !Formation.CorridorSamples[LowerIndex].bValid)
	{
		--LowerIndex;
	}
	while (UpperIndex < Formation.CorridorSamples.Num()
		&& !Formation.CorridorSamples[UpperIndex].bValid)
	{
		++UpperIndex;
	}

	if (LowerIndex < 0 && UpperIndex >= Formation.CorridorSamples.Num())
	{
		return Result;
	}
	if (LowerIndex < 0)
	{
		return Formation.CorridorSamples[UpperIndex];
	}
	if (UpperIndex >= Formation.CorridorSamples.Num())
	{
		return Formation.CorridorSamples[LowerIndex];
	}
	if (LowerIndex == UpperIndex)
	{
		return Formation.CorridorSamples[LowerIndex];
	}

	const FAlgonaFormationCorridorSample& Lower =
		Formation.CorridorSamples[LowerIndex];
	const FAlgonaFormationCorridorSample& Upper =
		Formation.CorridorSamples[UpperIndex];
	const double BlendAlpha = FMath::Clamp(
		(SamplePosition - static_cast<double>(LowerIndex))
			/ static_cast<double>(UpperIndex - LowerIndex),
		0.0,
		1.0
	);
	Result.Center = FMath::Lerp(Lower.Center, Upper.Center, BlendAlpha);
	Result.LongitudinalOffset = FMath::Lerp(
		Lower.LongitudinalOffset,
		Upper.LongitudinalOffset,
		static_cast<float>(BlendAlpha)
	);
	Result.LeftHalfWidth = FMath::Lerp(
		Lower.LeftHalfWidth,
		Upper.LeftHalfWidth,
		static_cast<float>(BlendAlpha)
	);
	Result.RightHalfWidth = FMath::Lerp(
		Lower.RightHalfWidth,
		Upper.RightHalfWidth,
		static_cast<float>(BlendAlpha)
	);
	Result.Width = Result.LeftHalfWidth + Result.RightHalfWidth;
	Result.bValid = true;
	return Result;
}

FVector UAlgonaSimulationSubsystem::ComputeLocalFormationTarget(
	const FAlgonaSimGroupState& Group,
	const FVector& Position,
	const FAlgonaSimLODFragment& LOD,
	FAlgonaSimNavigationFragment& NavigationFragment) const
{
	const FAlgonaFormationState& Formation = Group.Formation;
	if (NavigationFragment.SlotIndex < 0)
	{
		return Formation.AnchorPosition;
	}

	const int32 Columns = FMath::Max(Formation.OriginalColumns, 1);
	const int32 Rows = FMath::Max(Formation.OriginalRows, 1);
	const int32 Row = FMath::Clamp(
		NavigationFragment.SlotIndex / Columns,
		0,
		Rows - 1
	);
	const int32 Column = NavigationFragment.SlotIndex % Columns;
	NavigationFragment.OriginalSlotRow = Row;
	NavigationFragment.OriginalSlotColumn = Column;

	const double NormalSpacing = FMath::Max(
		static_cast<double>(Formation.SlotSpacing),
		1.0
	);
	const double MinSpacing = FMath::Clamp(
		static_cast<double>(Group.MinSameGroupSpacing),
		1.0,
		NormalSpacing
	);
	const double BoundaryClearance = FormationCorridorSafetyMargin
		+ (ActiveLocalMotionMode == EAlgonaLocalMotionMode::MassStock
			? FMath::Max(
				static_cast<double>(Group.MinSameGroupSpacing) * 0.5,
				10.0
			)
			: 0.0);
	const double LongitudinalOffset =
		((static_cast<double>(Rows) - 1.0) * 0.5
			- static_cast<double>(Row))
		* NormalSpacing;
	const FVector OriginalRowCenter =
		Formation.AnchorPosition
		+ Formation.LayoutForward * LongitudinalOffset;
	const double ColumnCenter =
		(static_cast<double>(Columns) - 1.0) * 0.5;
	const double OriginalDesiredLateralOffset =
		(static_cast<double>(Column) - ColumnCenter) * NormalSpacing;

	FVector CorridorForward = Formation.TravelDirection;
	CorridorForward.Z = 0.0;
	if (!CorridorForward.Normalize())
	{
		CorridorForward = Formation.LayoutForward;
		CorridorForward.Z = 0.0;
		if (!CorridorForward.Normalize())
		{
			CorridorForward = FVector::ForwardVector;
		}
	}
	const double MemberLongitudinalOffset = FVector::DotProduct(
		Position - Formation.AnchorPosition,
		CorridorForward
	);
	const double CurrentMemberLateralOffset = FVector::DotProduct(
		Position - OriginalRowCenter,
		Formation.LayoutRight
	);

	struct FLocalCorridorConstraint
	{
		FAlgonaFormationCorridorSample Sample;
		double LocalSpacing = 0.0;
		double DesiredLateralOffset = 0.0;
		double LeftLateralBound = -TNumericLimits<double>::Max();
		double RightLateralBound = TNumericLimits<double>::Max();
		bool bValid = false;
	};

	const auto BuildConstraint =
		[&](const FAlgonaFormationCorridorSample& Sample)
		{
			FLocalCorridorConstraint Constraint;
			Constraint.Sample = Sample;
			Constraint.LocalSpacing = NormalSpacing;
			Constraint.DesiredLateralOffset =
				OriginalDesiredLateralOffset;
			Constraint.bValid = Sample.bValid;
			if (!Sample.bValid)
			{
				return Constraint;
			}

			const double UsableLeftHalfWidth = FMath::Max(
				static_cast<double>(Sample.LeftHalfWidth)
					- BoundaryClearance,
				0.0
			);
			const double UsableRightHalfWidth = FMath::Max(
				static_cast<double>(Sample.RightHalfWidth)
					- BoundaryClearance,
				0.0
			);
			const double SampleCenterLateralOffset = FVector::DotProduct(
				Sample.Center - OriginalRowCenter,
				Formation.LayoutRight
			);
			Constraint.LeftLateralBound =
				SampleCenterLateralOffset - UsableLeftHalfWidth;
			Constraint.RightLateralBound =
				SampleCenterLateralOffset + UsableRightHalfWidth;
			const double AvailableWidth = FMath::Max(
				Constraint.RightLateralBound
					- Constraint.LeftLateralBound,
				0.0
			);
			if (Columns > 1)
			{
				Constraint.LocalSpacing = FMath::Min(
					NormalSpacing,
					AvailableWidth / static_cast<double>(Columns - 1)
				);
			}

			const double RequiredHalfWidth =
				ColumnCenter * Constraint.LocalSpacing;
			const double MinimumCenterOffset =
				Constraint.LeftLateralBound + RequiredHalfWidth;
			const double MaximumCenterOffset =
				Constraint.RightLateralBound - RequiredHalfWidth;
			const double LocalCenterOffset =
				MinimumCenterOffset <= MaximumCenterOffset
					? FMath::Clamp(
						0.0,
						MinimumCenterOffset,
						MaximumCenterOffset
					)
					: (Constraint.LeftLateralBound
						+ Constraint.RightLateralBound) * 0.5;
			Constraint.DesiredLateralOffset = FMath::Clamp(
				LocalCenterOffset
					+ (static_cast<double>(Column) - ColumnCenter)
						* Constraint.LocalSpacing,
				Constraint.LeftLateralBound,
				Constraint.RightLateralBound
			);
			return Constraint;
		};

	const FLocalCorridorConstraint CurrentConstraint = BuildConstraint(
		SampleFormationCorridor(Formation, MemberLongitudinalOffset)
	);
	FLocalCorridorConstraint ControllingConstraint = CurrentConstraint;
	double DistanceToNarrowBoundary = 0.0;
	double MostUrgentRequiredLateralSpeed = 0.0;
	bool bHasLookAheadConstraint = false;
	for (const FAlgonaFormationCorridorSample& CandidateSample
		: Formation.CorridorSamples)
	{
		if (!CandidateSample.bValid)
		{
			continue;
		}
		const double DistanceAhead =
			static_cast<double>(CandidateSample.LongitudinalOffset)
			- MemberLongitudinalOffset;
		if (DistanceAhead <= 1.0
			|| DistanceAhead > FormationFunnelLookAheadDistance)
		{
			continue;
		}

		const FLocalCorridorConstraint CandidateConstraint =
			BuildConstraint(CandidateSample);
		const double CurrentRequiredDistance = FMath::Abs(
			CurrentConstraint.DesiredLateralOffset
				- CurrentMemberLateralOffset
		);
		const double CandidateRequiredDistance = FMath::Abs(
			CandidateConstraint.DesiredLateralOffset
				- CurrentMemberLateralOffset
		);
		const bool bNarrower =
			CandidateConstraint.LocalSpacing
				< CurrentConstraint.LocalSpacing - 1.0;
		const bool bSameWidthButMoreRestrictive =
			CandidateConstraint.LocalSpacing
				<= CurrentConstraint.LocalSpacing + 1.0
			&& CandidateRequiredDistance
				> CurrentRequiredDistance + 1.0;
		if (!bNarrower && !bSameWidthButMoreRestrictive)
		{
			continue;
		}

		const double TimeAtFullForwardSpeed = DistanceAhead
			/ FMath::Max(MovementSpeed, 1.0);
		const double CandidateRequiredLateralSpeed =
			CandidateRequiredDistance
			/ FMath::Max(TimeAtFullForwardSpeed, FixedDeltaSeconds);
		if (!bHasLookAheadConstraint
			|| CandidateRequiredLateralSpeed
				> MostUrgentRequiredLateralSpeed + KINDA_SMALL_NUMBER)
		{
			ControllingConstraint = CandidateConstraint;
			DistanceToNarrowBoundary = DistanceAhead;
			MostUrgentRequiredLateralSpeed =
				CandidateRequiredLateralSpeed;
			bHasLookAheadConstraint = true;
		}
	}

	const double LocalSpacing = ControllingConstraint.LocalSpacing;
	const double DesiredLateralOffset =
		ControllingConstraint.DesiredLateralOffset;
	const double LeftLateralBound =
		ControllingConstraint.LeftLateralBound;
	const double RightLateralBound =
		ControllingConstraint.RightLateralBound;

	const EAlgonaFormationMemberPhase PreviousPhase =
		NavigationFragment.FormationPhase;
	const double PreviousSpacing = NavigationFragment.LastLocalSpacing;
	const bool bFirstTarget = PreviousSpacing <= 0.0;
	const uint8 DeformationUpdateStride =
		bSimulationLODEnabled
		&& Group.Navigation.Status != EAlgonaGroupNavigationStatus::Settling
			? FMath::Max<uint8>(LOD.UpdateStride, 1)
			: 1;
	const double InterpolationAlpha = bFirstTarget
		? 1.0
		: FMath::Clamp(
			FormationLocalDeformationInterpSpeed * FixedDeltaSeconds
				* static_cast<double>(DeformationUpdateStride),
			0.0,
			1.0
		);
	double SmoothedLateralOffset = FMath::Lerp(
		static_cast<double>(NavigationFragment.LocalLateralOffset),
		DesiredLateralOffset,
		InterpolationAlpha
	);
	if (ControllingConstraint.bValid)
	{
		SmoothedLateralOffset = FMath::Clamp(
			SmoothedLateralOffset,
			LeftLateralBound,
			RightLateralBound
		);
	}
	if (CurrentConstraint.bValid)
	{
		SmoothedLateralOffset = FMath::Clamp(
			SmoothedLateralOffset,
			CurrentConstraint.LeftLateralBound,
			CurrentConstraint.RightLateralBound
		);
	}

	const bool bLocallyConstrained =
		LocalSpacing < NormalSpacing - 1.0;
	const bool bCurrentCrossSectionConstrained =
		CurrentConstraint.LocalSpacing < NormalSpacing - 1.0;
	const bool bWithinCurrentCrossSection =
		!CurrentConstraint.bValid
		|| (CurrentMemberLateralOffset
				>= CurrentConstraint.LeftLateralBound
			&& CurrentMemberLateralOffset
				<= CurrentConstraint.RightLateralBound);
	const double CurrentBoundaryCorrectionDistance =
		CurrentConstraint.bValid
			? FMath::Max(
				FMath::Max(
					CurrentConstraint.LeftLateralBound
						- CurrentMemberLateralOffset,
					CurrentMemberLateralOffset
						- CurrentConstraint.RightLateralBound
				),
				0.0
			)
			: 0.0;
	const bool bWasConstrained =
		PreviousSpacing > 0.0 && PreviousSpacing < NormalSpacing - 1.0;
	const bool bSoftOverlap =
		LocalSpacing < MinSpacing - 1.0;
	EAlgonaFormationMemberPhase Phase =
		EAlgonaFormationMemberPhase::Normal;
	if (CurrentConstraint.bValid && !bWithinCurrentCrossSection)
	{
		Phase = EAlgonaFormationMemberPhase::Funneling;
	}
	else if (bCurrentCrossSectionConstrained)
	{
		Phase = EAlgonaFormationMemberPhase::InsideChoke;
	}
	else if (bLocallyConstrained)
	{
		Phase = EAlgonaFormationMemberPhase::Funneling;
	}
	else if ((bWasConstrained
			|| PreviousPhase == EAlgonaFormationMemberPhase::ReExpanding)
		&& FMath::Abs(SmoothedLateralOffset - DesiredLateralOffset) > 1.0)
	{
		Phase = EAlgonaFormationMemberPhase::ReExpanding;
	}

	const double RequiredLateralDistance = FMath::Max(
		FMath::Abs(DesiredLateralOffset - CurrentMemberLateralOffset),
		CurrentBoundaryCorrectionDistance
	);
	const double MaxLateralSpeed = FMath::Max(
		MovementSpeed * FormationFunnelLateralSpeedFraction,
		1.0
	);
	double RequiredLateralSpeed = 0.0;
	double RecommendedForwardSpeedScale = 1.0;
	if (CurrentConstraint.bValid
		&& !bWithinCurrentCrossSection
		&& CurrentBoundaryCorrectionDistance > 1.0)
	{
		RequiredLateralSpeed =
			CurrentBoundaryCorrectionDistance / FixedDeltaSeconds;
		RecommendedForwardSpeedScale = FormationMinimumForwardSpeedScale;
	}
	else if (bHasLookAheadConstraint && RequiredLateralDistance > 1.0)
	{
		if (DistanceToNarrowBoundary > 1.0 && MovementSpeed > 1.0)
		{
			const double TimeAtFullForwardSpeed =
				DistanceToNarrowBoundary / MovementSpeed;
			RequiredLateralSpeed =
				RequiredLateralDistance
				/ FMath::Max(TimeAtFullForwardSpeed, FixedDeltaSeconds);
			if (RequiredLateralSpeed > MaxLateralSpeed)
			{
				RecommendedForwardSpeedScale =
					MaxLateralSpeed / RequiredLateralSpeed;
			}
		}
	}
	RecommendedForwardSpeedScale = FMath::Clamp(
		RecommendedForwardSpeedScale,
		FormationMinimumForwardSpeedScale,
		1.0
	);

	NavigationFragment.LocalLateralOffset =
		static_cast<float>(SmoothedLateralOffset);
	NavigationFragment.LastLocalSpacing = static_cast<float>(LocalSpacing);
	NavigationFragment.LastLocalCorridorWidth =
		ControllingConstraint.bValid
		? ControllingConstraint.Sample.Width
		: 0.0f;
	NavigationFragment.MemberLongitudinalOffset =
		static_cast<float>(MemberLongitudinalOffset);
	NavigationFragment.CorridorConstraintLongitudinalOffset =
		ControllingConstraint.Sample.LongitudinalOffset;
	NavigationFragment.DistanceToNarrowBoundary =
		static_cast<float>(FMath::Max(DistanceToNarrowBoundary, 0.0));
	NavigationFragment.RequiredLateralDistance =
		static_cast<float>(RequiredLateralDistance);
	NavigationFragment.RequiredLateralSpeed =
		static_cast<float>(RequiredLateralSpeed);
	NavigationFragment.RecommendedForwardSpeedScale =
		static_cast<float>(RecommendedForwardSpeedScale);
	NavigationFragment.FormationPhase = Phase;
	NavigationFragment.bSoftOverlap = bSoftOverlap;

	return OriginalRowCenter
		+ Formation.LayoutRight * SmoothedLateralOffset;
}

void UAlgonaSimulationSubsystem::ConfigureFormationForGroup(
	FAlgonaSimGroupState& Group)
{
	const int32 SlotCount = FMath::Max(Group.MemberCount, 0);
	const int32 PreferredColumns = SlotCount > 0
		? FMath::CeilToInt(FMath::Sqrt(static_cast<float>(SlotCount)))
		: 1;
	Group.Formation.SlotCount = SlotCount;
	Group.Formation.SlotSpacing = static_cast<float>(FMath::Max(
		FormationSlotSpacing,
		static_cast<double>(Group.MinSameGroupSpacing)
	));
	const bool bHasExplicitShape =
		Group.DebugFormationColumns > 0
		&& Group.DebugFormationRows > 0
		&& static_cast<int64>(Group.DebugFormationColumns)
			* static_cast<int64>(Group.DebugFormationRows) >= SlotCount;
	Group.Formation.OriginalColumns = bHasExplicitShape
		? Group.DebugFormationColumns
		: FMath::Clamp(
			PreferredColumns,
			1,
			FMath::Max(FormationMaxColumns, 1)
		);
	Group.Formation.OriginalRows = bHasExplicitShape
		? Group.DebugFormationRows
		: FMath::Max(
			1,
			FMath::DivideAndRoundUp(
				SlotCount,
				Group.Formation.OriginalColumns
			)
		);
	Group.Formation.Width =
		static_cast<float>(
			(Group.Formation.OriginalColumns - 1)
				* Group.Formation.SlotSpacing
		);
	Group.Formation.Depth =
		static_cast<float>(
			(Group.Formation.OriginalRows - 1)
				* Group.Formation.SlotSpacing
		);
}

void UAlgonaSimulationSubsystem::UpdateFormationCorridorConstraint(
	FAlgonaSimGroupState& Group,
	const FVector& GroupCenter)
{
	(void)GroupCenter;
	Group.Formation.CorridorSamples.Reset();
	Group.Formation.CorridorHalfWidth =
		static_cast<float>(FormationCorridorProbeHalfWidth);
	Group.Formation.CorridorLeftHalfWidth =
		static_cast<float>(FormationCorridorProbeHalfWidth);
	Group.Formation.CorridorRightHalfWidth =
		static_cast<float>(FormationCorridorProbeHalfWidth);
	Group.Formation.CorridorWidth =
		static_cast<float>(FormationCorridorProbeHalfWidth * 2.0);
	const double FrontProfileExtension = FMath::Max(
		FormationFunnelLookAheadDistance,
		static_cast<double>(Group.Formation.SlotSpacing)
	);
	const double RearProfileExtension = FMath::Max(
		FormationAnchorLookAheadDistance
			+ FormationFunnelLookAheadDistance,
		static_cast<double>(Group.Formation.SlotSpacing)
	);
	Group.Formation.CorridorProfileFrontOffset = static_cast<float>(
		static_cast<double>(Group.Formation.Depth) * 0.5
		+ FrontProfileExtension
	);
	Group.Formation.CorridorProfileRearOffset = static_cast<float>(
		-static_cast<double>(Group.Formation.Depth) * 0.5
		- RearProfileExtension
	);
	Group.Formation.CorridorValidSamples = 0;
	Group.Formation.LeftCorridorEdge =
		Group.Formation.AnchorPosition
		- Group.Formation.LayoutRight * FormationCorridorProbeHalfWidth;
	Group.Formation.RightCorridorEdge =
		Group.Formation.AnchorPosition
		+ Group.Formation.LayoutRight * FormationCorridorProbeHalfWidth;

	UWorld* World = GetWorld();
	UNavigationSystemV1* NavigationSystem =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
	FNavAgentProperties AgentProperties;
	const FVector ProjectionExtent(
		NavProjectionExtent,
		NavProjectionExtent,
		NavProjectionExtentZ
	);
	ANavigationData* NavigationData = ResolveNavigationData(
		NavigationSystem,
		AgentProperties,
		Group.Formation.AnchorPosition,
		ProjectionExtent
	);
	if (NavigationData == nullptr)
	{
		return;
	}

	const auto ProbeHalfWidth =
		[this, NavigationData](
			const FVector& ProbeCenter,
			const FVector& Direction)
		{
			const FVector ProbeEnd =
				ProbeCenter + Direction * FormationCorridorProbeHalfWidth;
			FVector HitLocation = ProbeEnd;
			const bool bBlocked = NavigationData->Raycast(
				ProbeCenter,
				ProbeEnd,
				HitLocation,
				nullptr,
				this
			);
			if (!bBlocked)
			{
				return FormationCorridorProbeHalfWidth;
			}

			FVector Delta = HitLocation - ProbeCenter;
			Delta.Z = 0.0;
			return FMath::Clamp(
				static_cast<double>(FVector::DotProduct(Delta, Direction)),
				0.0,
				FormationCorridorProbeHalfWidth
			);
		};

	double BestLeftHalfWidth = FormationCorridorProbeHalfWidth;
	double BestRightHalfWidth = FormationCorridorProbeHalfWidth;
	double BestTotalWidth = FormationCorridorProbeHalfWidth * 2.0;
	FVector CorridorForward = Group.Formation.TravelDirection;
	CorridorForward.Z = 0.0;
	if (!CorridorForward.Normalize())
	{
		CorridorForward = Group.Formation.LayoutForward;
		CorridorForward.Z = 0.0;
		if (!CorridorForward.Normalize())
		{
			CorridorForward = FVector::ForwardVector;
		}
	}
	const double ProfileLength =
		static_cast<double>(Group.Formation.CorridorProfileFrontOffset)
		- static_cast<double>(Group.Formation.CorridorProfileRearOffset);
	const double DesiredSampleSpacing = FMath::Max(
		static_cast<double>(Group.Formation.SlotSpacing) * 2.0,
		1.0
	);
	const int32 SampleCount = FMath::Clamp(
		FMath::Max(
			FormationCorridorSampleCount,
			FMath::CeilToInt(ProfileLength / DesiredSampleSpacing) + 1
		),
		3,
		33
	);
	Group.Formation.CorridorSamples.Reserve(SampleCount);
	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const double Alpha = SampleCount > 1
			? static_cast<double>(SampleIndex)
				/ static_cast<double>(SampleCount - 1)
			: 0.0;
		const double LongitudinalOffset = FMath::Lerp(
			static_cast<double>(
				Group.Formation.CorridorProfileFrontOffset
			),
			static_cast<double>(
				Group.Formation.CorridorProfileRearOffset
			),
			Alpha
		);
		const FVector RawCenter =
			Group.Formation.AnchorPosition
				+ CorridorForward * LongitudinalOffset;

		FAlgonaFormationCorridorSample& Sample =
			Group.Formation.CorridorSamples.AddDefaulted_GetRef();
		Sample.LongitudinalOffset = static_cast<float>(LongitudinalOffset);
		Sample.Center = RawCenter;
		FNavLocation ProjectedCenter;
		if (!NavigationSystem->ProjectPointToNavigation(
			RawCenter,
			ProjectedCenter,
			ProjectionExtent,
			NavigationData))
		{
			continue;
		}

		Sample.Center = ProjectedCenter.Location;
		Sample.LeftHalfWidth = static_cast<float>(ProbeHalfWidth(
			Sample.Center,
			-Group.Formation.LayoutRight
		));
		Sample.RightHalfWidth = static_cast<float>(ProbeHalfWidth(
			Sample.Center,
			Group.Formation.LayoutRight
		));
		Sample.Width = Sample.LeftHalfWidth + Sample.RightHalfWidth;
		Sample.bValid =
			Sample.Width > static_cast<float>(FormationSlotSpacing * 0.1);
		if (!Sample.bValid)
		{
			continue;
		}

		++Group.Formation.CorridorValidSamples;
		if (Sample.Width < BestTotalWidth)
		{
			BestLeftHalfWidth = Sample.LeftHalfWidth;
			BestRightHalfWidth = Sample.RightHalfWidth;
			BestTotalWidth = Sample.Width;
		}
	}

	Group.Formation.CorridorLeftHalfWidth =
		static_cast<float>(BestLeftHalfWidth);
	Group.Formation.CorridorRightHalfWidth =
		static_cast<float>(BestRightHalfWidth);
	Group.Formation.CorridorWidth = static_cast<float>(BestTotalWidth);
	Group.Formation.CorridorHalfWidth =
		static_cast<float>(FMath::Min(BestLeftHalfWidth, BestRightHalfWidth));
	Group.Formation.LeftCorridorEdge =
		Group.Formation.AnchorPosition
		- Group.Formation.LayoutRight * BestLeftHalfWidth;
	Group.Formation.RightCorridorEdge =
		Group.Formation.AnchorPosition
		+ Group.Formation.LayoutRight * BestRightHalfWidth;
}

bool UAlgonaSimulationSubsystem::UpdateFormationSafeAnchor(
	FAlgonaSimGroupState& Group,
	const FVector& ProposedAnchor,
	int32 ElapsedFormationTicks)
{
	FAlgonaFormationState& Formation = Group.Formation;
	const double NormalHalfWidth =
		FMath::Max(static_cast<double>(Formation.Width) * 0.5, 0.0);
	const double MinimumSpacingHalfWidth =
		static_cast<double>(FMath::Max(Formation.OriginalColumns - 1, 0))
		* FMath::Max(static_cast<double>(Group.MinSameGroupSpacing), 1.0)
		* 0.5;

	double CommonLeftBound = -TNumericLimits<double>::Max();
	double CommonRightBound = TNumericLimits<double>::Max();
	int32 ValidSamples = 0;
	FVector CorridorForward = Formation.TravelDirection;
	CorridorForward.Z = 0.0;
	if (!CorridorForward.Normalize())
	{
		CorridorForward = Formation.LayoutForward;
		CorridorForward.Z = 0.0;
		if (!CorridorForward.Normalize())
		{
			CorridorForward = FVector::ForwardVector;
		}
	}
	for (int32 SampleIndex = 0;
		SampleIndex < Formation.CorridorSamples.Num();
		++SampleIndex)
	{
		const FAlgonaFormationCorridorSample& Sample =
			Formation.CorridorSamples[SampleIndex];
		if (!Sample.bValid)
		{
			continue;
		}

		const FVector RawCenter =
			ProposedAnchor
			+ CorridorForward
				* static_cast<double>(Sample.LongitudinalOffset);
		const double ProjectedCenterOffset = FVector::DotProduct(
			Sample.Center - RawCenter,
			Formation.LayoutRight
		);
		const double UsableLeftHalfWidth = FMath::Max(
			static_cast<double>(Sample.LeftHalfWidth)
				- FormationCorridorSafetyMargin,
			0.0
		);
		const double UsableRightHalfWidth = FMath::Max(
			static_cast<double>(Sample.RightHalfWidth)
				- FormationCorridorSafetyMargin,
			0.0
		);
		CommonLeftBound = FMath::Max(
			CommonLeftBound,
			ProjectedCenterOffset - UsableLeftHalfWidth
		);
		CommonRightBound = FMath::Min(
			CommonRightBound,
			ProjectedCenterOffset + UsableRightHalfWidth
		);
		++ValidSamples;
	}

	if (ValidSamples == 0)
	{
		Formation.RequiredFootprintHalfWidth =
			static_cast<float>(NormalHalfWidth);
		Formation.SafeAnchorLateralOffset = 0.0f;
		Formation.AnchorPosition = ProposedAnchor;
		return false;
	}

	const bool bHasCommonInterval = CommonLeftBound <= CommonRightBound;
	const double CommonHalfWidth = bHasCommonInterval
		? (CommonRightBound - CommonLeftBound) * 0.5
		: 0.0;
	double RequiredHalfWidth = CommonHalfWidth >= MinimumSpacingHalfWidth
		? FMath::Min(NormalHalfWidth, CommonHalfWidth)
		: CommonHalfWidth;
	RequiredHalfWidth = FMath::Max(RequiredHalfWidth, 0.0);

	double DesiredOffset =
		(CommonLeftBound + CommonRightBound) * 0.5;
	double MinimumAnchorOffset = DesiredOffset;
	double MaximumAnchorOffset = DesiredOffset;
	if (bHasCommonInterval)
	{
		MinimumAnchorOffset = CommonLeftBound + RequiredHalfWidth;
		MaximumAnchorOffset = CommonRightBound - RequiredHalfWidth;
		if (MinimumAnchorOffset <= MaximumAnchorOffset)
		{
			DesiredOffset = FMath::Clamp(
				0.0,
				MinimumAnchorOffset,
				MaximumAnchorOffset
			);
		}
	}

	const double InterpolationAlpha = FMath::Clamp(
		FormationLocalDeformationInterpSpeed * FixedDeltaSeconds
			* static_cast<double>(FMath::Max(ElapsedFormationTicks, 1)),
		0.0,
		1.0
	);
	double SafeOffset = FMath::Lerp(
		static_cast<double>(Formation.SafeAnchorLateralOffset),
		DesiredOffset,
		InterpolationAlpha
	);
	if (MinimumAnchorOffset <= MaximumAnchorOffset)
	{
		SafeOffset = FMath::Clamp(
			SafeOffset,
			MinimumAnchorOffset,
			MaximumAnchorOffset
		);
	}
	else
	{
		SafeOffset = DesiredOffset;
	}

	Formation.RequiredFootprintHalfWidth =
		static_cast<float>(RequiredHalfWidth);
	Formation.SafeAnchorLateralOffset = static_cast<float>(SafeOffset);
	Formation.AnchorPosition =
		ProposedAnchor + Formation.LayoutRight * SafeOffset;
	return !FMath::IsNearlyZero(SafeOffset, 1.0);
}

void UAlgonaSimulationSubsystem::QueuePathRequestForGroup(
	FAlgonaSimGroupState& Group)
{
	CancelPathRequestForGroup(Group);

	QueuedPathRequests.RemoveAll(
		[GroupId = Group.Id](const FAlgonaPendingPathRequest& Request)
		{
			return Request.GroupId == GroupId;
		}
	);

	FAlgonaPendingPathRequest& Request =
		QueuedPathRequests.AddDefaulted_GetRef();
	Request.GroupId = Group.Id;
	Request.Start = ComputeGroupCenter(Group.Id);
	Request.End = Group.Navigation.Destination;
	Request.Generation = Group.Navigation.RouteGeneration;
	Request.CommandSequence = Group.CommandSequence;
	Request.Priority = 0;

	Group.Navigation.Status = EAlgonaGroupNavigationStatus::Queued;
	Group.Navigation.PendingRequestId = 0;
	Group.Navigation.PendingRequestGeneration = Request.Generation;

	BenchmarkSnapshot.QueuedPathRequestCount = QueuedPathRequests.Num();
}

void UAlgonaSimulationSubsystem::CancelPathRequestForGroup(
	FAlgonaSimGroupState& Group)
{
	QueuedPathRequests.RemoveAll(
		[GroupId = Group.Id](const FAlgonaPendingPathRequest& Request)
		{
			return Request.GroupId == GroupId;
		}
	);

	const uint32 PendingRequestId = Group.Navigation.PendingRequestId;
	if (PendingRequestId == 0)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (UNavigationSystemV1* NavigationSystem =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
	{
		NavigationSystem->AbortAsyncFindPathRequest(
			PendingRequestId
		);
	}

	InFlightPathRequests.RemoveAll(
		[PendingRequestId](const FAlgonaInFlightPathRequest& Request)
		{
			return Request.RequestId == PendingRequestId;
		}
	);

	Group.Navigation.PendingRequestId = 0;
	Group.Navigation.PendingRequestGeneration = 0;
	BenchmarkSnapshot.InFlightPathRequestCount = InFlightPathRequests.Num();
}

void UAlgonaSimulationSubsystem::AbortAllPathRequests()
{
	if (InFlightPathRequests.IsEmpty())
	{
		QueuedPathRequests.Reset();
		PendingPathCompletions.Reset();
		return;
	}

	UWorld* World = GetWorld();
	if (UNavigationSystemV1* NavigationSystem =
		FNavigationSystem::GetCurrent<UNavigationSystemV1>(World))
	{
		for (const FAlgonaInFlightPathRequest& Request
			: InFlightPathRequests)
		{
			if (Request.RequestId != 0)
			{
				NavigationSystem->AbortAsyncFindPathRequest(
					Request.RequestId
				);
			}
		}
	}

	QueuedPathRequests.Reset();
	InFlightPathRequests.Reset();
	PendingPathCompletions.Reset();
}

ANavigationData* UAlgonaSimulationSubsystem::ResolveNavigationData(
	UNavigationSystemV1* NavigationSystem,
	const FNavAgentProperties& AgentProperties,
	const FVector& AgentLocation,
	const FVector& ProjectionExtent) const
{
	if (NavigationSystem == nullptr)
	{
		return nullptr;
	}

	ANavigationData* NavigationData =
		NavigationSystem->GetNavDataForProps(
			AgentProperties,
			AgentLocation,
			ProjectionExtent
		);
	if (NavigationData != nullptr)
	{
		return NavigationData;
	}

	return NavigationSystem->GetDefaultNavDataInstance(
		FNavigationSystem::DontCreate
	);
}

void UAlgonaSimulationSubsystem::OnAsyncPathResult(
	uint32 RequestId,
	ENavigationQueryResult::Type Result,
	FNavPathSharedPtr Path)
{
	const int32 InFlightIndex = InFlightPathRequests.IndexOfByPredicate(
		[RequestId](const FAlgonaInFlightPathRequest& Request)
		{
			return Request.RequestId == RequestId;
		}
	);

	if (InFlightIndex == INDEX_NONE)
	{
		++BenchmarkSnapshot.RejectedStalePathResults;
		return;
	}

	const FAlgonaInFlightPathRequest InFlight =
		InFlightPathRequests[InFlightIndex];
	InFlightPathRequests.RemoveAtSwap(
		InFlightIndex,
		1,
		EAllowShrinking::No
	);

	FAlgonaPathCompletion& Completion =
		PendingPathCompletions.AddDefaulted_GetRef();
	Completion.GroupId = InFlight.GroupId;
	Completion.RequestId = RequestId;
	Completion.Generation = InFlight.Generation;
	Completion.CommandSequence = InFlight.CommandSequence;
	Completion.bSuccess = Result == ENavigationQueryResult::Success
		&& Path.IsValid()
		&& Path->GetPathPoints().Num() >= 2;
	Completion.bPartial = Path.IsValid() && Path->IsPartial();
	Completion.LatencyMilliseconds =
		(FPlatformTime::Seconds() - InFlight.SubmitTimeSeconds) * 1000.0;

	if (Path.IsValid())
	{
		const TArray<FNavPathPoint>& PathPoints = Path->GetPathPoints();
		Completion.Points.Reserve(PathPoints.Num());
		for (const FNavPathPoint& PathPoint : PathPoints)
		{
			Completion.Points.Add(PathPoint.Location);
		}
	}

	if (!Completion.bSuccess)
	{
		Completion.FailureReason = FString::Printf(
			TEXT("NavigationResult=%d, Points=%d"),
			static_cast<int32>(Result),
			Completion.Points.Num()
		);
	}

	BenchmarkSnapshot.InFlightPathRequestCount = InFlightPathRequests.Num();
	BenchmarkSnapshot.PendingPathCompletionCount =
		PendingPathCompletions.Num();
}

void UAlgonaSimulationSubsystem::ResetSimulationState()
{
	AbortAllPathRequests();
	AccumulatorSeconds = 0.0;
	SimulationTick = 0;
	NextCommandSequence = 1;
	Groups.Reset();
	PendingCommands.Reset();
	QueuedPathRequests.Reset();
	InFlightPathRequests.Reset();
	PendingPathCompletions.Reset();
	HardBlockers.Reset();
	NextHardBlockerId = 1;
	HardBlockerRevision = 0;
	SpatialGrid.Cells.Reset();
	SpatialGrid.Entries.Reset();
	BenchmarkSnapshot = FAlgonaSimBenchmarkSnapshot();
}

bool UAlgonaSimulationSubsystem::IsAuthoritativeSimulationWorld() const
{
	const UWorld* World = GetWorld();
	return World == nullptr || World->GetNetMode() != NM_Client;
}

FAlgonaSimGroupState* UAlgonaSimulationSubsystem::FindGroup(FSimGroupId GroupId)
{
	if (!GroupId.IsValid())
	{
		return nullptr;
	}

	const int32 GroupArrayIndex = GroupId.Value - 1;
	if (Groups.IsValidIndex(GroupArrayIndex)
		&& Groups[GroupArrayIndex].Id == GroupId)
	{
		return &Groups[GroupArrayIndex];
	}

	return Groups.FindByPredicate(
		[GroupId](const FAlgonaSimGroupState& Group)
		{
			return Group.Id == GroupId;
		}
	);
}

const FAlgonaSimGroupState* UAlgonaSimulationSubsystem::FindGroup(
	FSimGroupId GroupId) const
{
	return const_cast<UAlgonaSimulationSubsystem*>(this)->FindGroup(GroupId);
}

bool UAlgonaSimulationSubsystem::IsNavigationMovementReady(
	const FAlgonaSimGroupState& Group) const
{
	if (!bP1NavigationEnabled)
	{
		return true;
	}

	if (Group.Navigation.Status == EAlgonaGroupNavigationStatus::RouteReady)
	{
		return Group.Navigation.Route.bValid;
	}

	if ((Group.Navigation.Status == EAlgonaGroupNavigationStatus::Queued
			|| Group.Navigation.Status == EAlgonaGroupNavigationStatus::Requesting
			|| Group.Navigation.Status
				== EAlgonaGroupNavigationStatus::WaitingForAsyncResult)
		&& Group.Navigation.Route.bValid)
	{
		return true;
	}

	return Group.Navigation.Status == EAlgonaGroupNavigationStatus::Settling
		&& Group.Navigation.Route.Points.Num() > 0;
}
