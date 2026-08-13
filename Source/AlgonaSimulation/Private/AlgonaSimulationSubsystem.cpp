#include "AlgonaSimulationSubsystem.h"

#include "MassEntitySubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaSimulation, Log, All);

void UAlgonaSimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UWorld* World = GetWorld();
	MassEntitySubsystem = World
		? World->GetSubsystem<UMassEntitySubsystem>()
		: nullptr;

	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Initialized. World=%s, MassSubsystem=%s"),
		*GetNameSafe(World),
		*GetNameSafe(MassEntitySubsystem)
	);
}

void UAlgonaSimulationSubsystem::Deinitialize()
{
	UE_LOG(
		LogAlgonaSimulation,
		Log,
		TEXT("Deinitialized.")
	);

	MassEntitySubsystem = nullptr;

	Super::Deinitialize();
}

void UAlgonaSimulationSubsystem::Tick(float DeltaTime)
{
	RunSimulationStep(DeltaTime);
}

TStatId UAlgonaSimulationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(
		UAlgonaSimulationSubsystem,
		STATGROUP_Tickables
	);
}

void UAlgonaSimulationSubsystem::RunSimulationStep(float DeltaTime)
{
	if (!MassEntitySubsystem)
	{
		return;
	}

	// P0.1 intentionally does not simulate soldiers yet.
	// P0.3 will introduce Mass soldier entities and their state.
}