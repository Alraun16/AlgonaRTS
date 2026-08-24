#include "Presentation/AlgonaPresentationSubsystem.h"

#include "Debug/AlgonaP1TestCameraActor.h"
#include "Presentation/AlgonaArmyPresentationActor.h"
#include "Presentation/AlgonaArmySkinnedPresentationActor.h"

#include "Engine/GameViewportClient.h"

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
	
	/*
	 * Базовые performance stats, которые всегда показываем
	 * в игровом viewport.
	 */
	if (UGameViewportClient* GameViewport =
		InWorld.GetGameViewport())
	{
		TArray<FString> EnabledStats;

		if (const TArray<FString>* CurrentStats =
			GameViewport->GetEnabledStats())
		{
			EnabledStats = *CurrentStats;
		}

		EnabledStats.AddUnique(TEXT("FPS"));
		EnabledStats.AddUnique(TEXT("Unit"));

		GameViewport->SetEnabledStats(EnabledStats);
	}
	
	if (PresentationActor || SkinnedPresentationActor)
	{
		return;
	}

	const EAlgonaP0PresentationBackend Backend =
		GetAlgonaP0PresentationBackend();

	const bool bUseStaticIsmBackend =
		Backend == EAlgonaP0PresentationBackend::IsmCandidate;

	const bool bUseSkinnedIsmBackend =
		Backend
			== EAlgonaP0PresentationBackend::InstancedSkinnedMeshCandidate;

	if (!bUseStaticIsmBackend && !bUseSkinnedIsmBackend)
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

	PresentationSpawnParameters.ObjectFlags |=
		RF_Transient;

	PresentationSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	if (bUseStaticIsmBackend)
	{
		PresentationSpawnParameters.Name =
			TEXT("AlgonaArmyPresentation");

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

		PresentationActor->AddTickPrerequisiteActor(
			CameraActor);

		return;
	}

	PresentationSpawnParameters.Name =
		TEXT("AlgonaArmySkinnedPresentation");

	SkinnedPresentationActor =
		InWorld.SpawnActor<AAlgonaArmySkinnedPresentationActor>(
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			PresentationSpawnParameters);

	if (!SkinnedPresentationActor)
	{
		CameraActor->Destroy();
		TestCameraActor = nullptr;
		return;
	}

	SkinnedPresentationActor->SetPresentationCamera(
		CameraActor->GetCameraComponent());

	SkinnedPresentationActor->AddTickPrerequisiteActor(
		CameraActor);
}

void UAlgonaPresentationSubsystem::Deinitialize()
{
	if (IsValid(PresentationActor))
	{
		PresentationActor->Destroy();
	}

	PresentationActor = nullptr;

	if (IsValid(SkinnedPresentationActor))
	{
		SkinnedPresentationActor->Destroy();
	}

	SkinnedPresentationActor = nullptr;

	if (IsValid(TestCameraActor))
	{
		TestCameraActor->Destroy();
	}

	TestCameraActor = nullptr;

	Super::Deinitialize();
}
