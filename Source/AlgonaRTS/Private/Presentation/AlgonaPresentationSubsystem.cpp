#include "Presentation/AlgonaPresentationSubsystem.h"

#include "Debug/AlgonaP1TestCameraActor.h"
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

	FActorSpawnParameters CameraSpawnParameters;

	CameraSpawnParameters.Name =
		TEXT("AlgonaP1TestCamera");

	CameraSpawnParameters.ObjectFlags |=
		RF_Transient;

	CameraSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AAlgonaP1TestCameraActor* CameraActor =
		InWorld.SpawnActor<AAlgonaP1TestCameraActor>(
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			CameraSpawnParameters);

	if (!CameraActor)
	{
		return;
	}

	TestCameraActor = CameraActor;

	FActorSpawnParameters PresentationSpawnParameters;

	PresentationSpawnParameters.Name =
		TEXT("AlgonaArmyPresentation");

	PresentationSpawnParameters.ObjectFlags |=
		RF_Transient;

	PresentationSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	PresentationActor =
		InWorld.SpawnActor<AAlgonaArmyPresentationActor>(
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			PresentationSpawnParameters);

	if (!PresentationActor)
	{
		CameraActor->Destroy();
		TestCameraActor = nullptr;
		return;
	}

	PresentationActor->SetPresentationCamera(
		CameraActor->GetCameraComponent());

	/*
	 * Камера сначала меняет своё положение,
	 * затем Presentation читает уже новый view.
	 */
	PresentationActor->AddTickPrerequisiteActor(
		CameraActor);
}

void UAlgonaPresentationSubsystem::Deinitialize()
{
	if (IsValid(PresentationActor))
	{
		PresentationActor->Destroy();
	}

	PresentationActor = nullptr;

	if (IsValid(TestCameraActor))
	{
		TestCameraActor->Destroy();
	}

	TestCameraActor = nullptr;

	Super::Deinitialize();
}