#include "P0SimulationCommandBridge.h"

#include "AlgonaSimulationSubsystem.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaP0Network, Log, All);

namespace
{
AP0SimulationCommandBridge* FindP0SimulationCommandBridge(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<AP0SimulationCommandBridge> It(World); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

void NetDumpBridgeP0Command(UWorld* World)
{
	AP0SimulationCommandBridge* CommandBridge =
		FindP0SimulationCommandBridge(World);
	if (CommandBridge == nullptr)
	{
		UE_LOG(LogAlgonaP0Network, Warning, TEXT("algona.P0.NetDumpBridge failed: no command bridge actor."));
		return;
	}

	CommandBridge->DumpDebugState();
}

FAutoConsoleCommandWithWorld GAlgonaP0NetDumpBridgeCommand(
	TEXT("algona.P0.NetDumpBridge"),
	TEXT("Dumps the replicated P0 command bridge aggregate state."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&NetDumpBridgeP0Command)
);
}

AP0SimulationCommandBridge::AP0SimulationCommandBridge()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(4.0f);
	SetReplicatingMovement(false);
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
}

void AP0SimulationCommandBridge::BeginPlay()
{
	Super::BeginPlay();
	RefreshReplicatedSnapshot();
}

void AP0SimulationCommandBridge::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (HasAuthority())
	{
		RefreshReplicatedSnapshot();
	}
}

void AP0SimulationCommandBridge::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AP0SimulationCommandBridge, ServerEntityCount);
	DOREPLIFETIME(AP0SimulationCommandBridge, ServerGroupCount);
	DOREPLIFETIME(AP0SimulationCommandBridge, ServerPendingCommandCount);
	DOREPLIFETIME(AP0SimulationCommandBridge, ServerSimulationTick);
	DOREPLIFETIME(AP0SimulationCommandBridge, ServerLastSubmittedCommandSequence);
	DOREPLIFETIME(AP0SimulationCommandBridge, ServerLastAppliedCommandSequence);
	DOREPLIFETIME(AP0SimulationCommandBridge, ServerLastAppliedCommandGroupId);
}

void AP0SimulationCommandBridge::DumpDebugState() const
{
	UE_LOG(
		LogAlgonaP0Network,
		Log,
		TEXT("P0 command bridge. LocalRole=%d, RemoteRole=%d, ServerEntities=%d, ServerGroups=%d, ServerPendingCommands=%d, ServerTick=%llu, LastSubmitted=%llu, LastApplied=%llu, LastAppliedGroup=%d"),
		static_cast<int32>(GetLocalRole()),
		static_cast<int32>(GetRemoteRole()),
		ServerEntityCount,
		ServerGroupCount,
		ServerPendingCommandCount,
		ServerSimulationTick,
		ServerLastSubmittedCommandSequence,
		ServerLastAppliedCommandSequence,
		ServerLastAppliedCommandGroupId
	);
}

void AP0SimulationCommandBridge::RefreshReplicatedSnapshot()
{
	if (!HasAuthority())
	{
		return;
	}

	UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		World != nullptr
			? World->GetSubsystem<UAlgonaSimulationSubsystem>()
			: nullptr;
	if (SimulationSubsystem == nullptr)
	{
		return;
	}

	const FAlgonaSimBenchmarkSnapshot Snapshot =
		SimulationSubsystem->GetBenchmarkSnapshot();
	ServerEntityCount = Snapshot.EntityCount;
	ServerGroupCount = Snapshot.GroupCount;
	ServerPendingCommandCount = Snapshot.PendingCommandCount;
	ServerSimulationTick = Snapshot.SimulationTick;
	ServerLastSubmittedCommandSequence = Snapshot.LastSubmittedCommandSequence;
	ServerLastAppliedCommandSequence = Snapshot.LastAppliedCommandSequence;
	ServerLastAppliedCommandGroupId = Snapshot.LastAppliedCommandGroupId;
}
