#include "P0TestGameMode.h"

#include "AlgonaSimulationSubsystem.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "HAL/IConsoleManager.h"
#include "P0SimulationCommandBridge.h"
#include "P0SimulationPlayerController.h"
#include "P0SimulationPresentationActor.h"

namespace
{
TAutoConsoleVariable<int32> CVarAlgonaP0SpawnPresentation(
	TEXT("algona.P0.SpawnPresentation"),
	0,
	TEXT("When non-zero, AP0TestGameMode spawns one ISM presentation actor for P0 visualization."),
	ECVF_Default
);

void SpawnPresentationP0Command(UWorld* World)
{
	AP0SimulationPresentationActor* PresentationActor =
		AP0TestGameMode::EnsurePresentationActor(World);
	if (PresentationActor == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("algona.P0.SpawnPresentationActor failed."));
		return;
	}

	UE_LOG(
		LogTemp,
		Log,
		TEXT("algona.P0.SpawnPresentationActor ready. Actor=%s"),
		*GetNameSafe(PresentationActor)
	);
}

FAutoConsoleCommandWithWorld GAlgonaP0SpawnPresentationActorCommand(
	TEXT("algona.P0.SpawnPresentationActor"),
	TEXT("Spawns or reuses one P0 ISM presentation actor in the current world."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&SpawnPresentationP0Command)
);
}

AP0TestGameMode::AP0TestGameMode()
{
	DefaultPawnClass = nullptr;
	HUDClass = nullptr;
	PlayerControllerClass = AP0SimulationPlayerController::StaticClass();
	bStartPlayersAsSpectators = true;
}

AP0SimulationPresentationActor* AP0TestGameMode::FindPresentationActor(
	UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	for (TActorIterator<AP0SimulationPresentationActor> It(World); It; ++It)
	{
		return *It;
	}

	return nullptr;
}

AP0SimulationPresentationActor* AP0TestGameMode::EnsurePresentationActor(
	UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}

	if (AP0SimulationPresentationActor* ExistingActor =
		FindPresentationActor(World))
	{
		ExistingActor->RefreshInstances();
		return ExistingActor;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = TEXT("P0SimulationPresentation");
	SpawnParameters.ObjectFlags |= RF_Transient;
	AP0SimulationPresentationActor* NewActor =
		World->SpawnActor<AP0SimulationPresentationActor>(SpawnParameters);
	if (NewActor != nullptr)
	{
		NewActor->RefreshInstances();
	}
	return NewActor;
}

void AP0TestGameMode::StartPlay()
{
	Super::StartPlay();

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	World->GetSubsystem<UAlgonaSimulationSubsystem>();

	if (HasAuthority() && CommandBridge == nullptr)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Name = TEXT("P0SimulationCommandBridge");
		SpawnParameters.ObjectFlags |= RF_Transient;
		CommandBridge =
			World->SpawnActor<AP0SimulationCommandBridge>(SpawnParameters);
	}

	if (CVarAlgonaP0SpawnPresentation.GetValueOnGameThread() != 0
		&& PresentationActor == nullptr)
	{
		PresentationActor = EnsurePresentationActor(World);
	}
}
