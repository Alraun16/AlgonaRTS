#include "Presentation/AlgonaPresentationSubsystem.h"

#include "Presentation/AlgonaArmyPresentationActor.h"
#include "AlgonaP0PresentationExperiment.h"
#include "Engine/World.h"

bool UAlgonaPresentationSubsystem::ShouldCreateSubsystem(
	UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Cast<UWorld>(Outer);
	if (!World)
	{
		return false;
	}

	const bool bPlayableWorld =
		World->WorldType == EWorldType::Game
		|| World->WorldType == EWorldType::PIE;

	// Dedicated server не рисует армию.
	return bPlayableWorld
		&& World->GetNetMode() != NM_DedicatedServer;
}

void UAlgonaPresentationSubsystem::OnWorldBeginPlay(
	UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (PresentationActor
		|| GetAlgonaP0PresentationBackend()
			!= EAlgonaP0PresentationBackend::IsmCandidate)
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = TEXT("AlgonaArmyPresentation");
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	PresentationActor =
		InWorld.SpawnActor<AAlgonaArmyPresentationActor>(
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			SpawnParameters);
}

void UAlgonaPresentationSubsystem::Deinitialize()
{
	if (IsValid(PresentationActor))
	{
		PresentationActor->Destroy();
	}

	PresentationActor = nullptr;
	Super::Deinitialize();
}