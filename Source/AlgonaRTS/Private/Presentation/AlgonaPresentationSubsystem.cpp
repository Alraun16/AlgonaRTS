#include "Presentation/AlgonaPresentationSubsystem.h"

#include "Camera/AlgonaRTSCameraActor.h"
#include "Presentation/AlgonaArmyPresentationActor.h"
#include "Presentation/AlgonaLegacyIsmPresentationActor.h"
#include "Presentation/AlgonaPresentationSettings.h"

#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

bool UAlgonaPresentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
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

	return bPlayableWorld && World->GetNetMode() != NM_DedicatedServer;
}

void UAlgonaPresentationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	
	if (GEngine)
	{
		GEngine->SetMaxFPS(165.0f);
	}
	
	// Keep the two basic benchmark overlays enabled in normal Editor and
	// Development runs. Shipping may suppress engine stats independently.
	if (UGameViewportClient* GameViewport = InWorld.GetGameViewport())
	{
		TArray<FString> EnabledStats;
		if (const TArray<FString>* CurrentStats = GameViewport->GetEnabledStats())
		{
			EnabledStats = *CurrentStats;
		}

		EnabledStats.AddUnique(TEXT("FPS"));
		EnabledStats.AddUnique(TEXT("Unit"));
		GameViewport->SetEnabledStats(EnabledStats);
	}

	if (ArmyPresentationActor || LegacyPresentationActor || CameraActor)
	{
		return;
	}

	const EAlgonaP1PresentationMode PresentationMode =
		GetAlgonaP1PresentationMode();

	if (PresentationMode == EAlgonaP1PresentationMode::SimulationOnly)
	{
		return;
	}

	FActorSpawnParameters CameraSpawnParameters;
	CameraSpawnParameters.Name = TEXT("AlgonaRTSCamera");
	CameraSpawnParameters.ObjectFlags |= RF_Transient;
	CameraSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	CameraActor = InWorld.SpawnActor<AAlgonaRTSCameraActor>(
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		CameraSpawnParameters);

	if (!CameraActor)
	{
		return;
	}

	FActorSpawnParameters PresentationSpawnParameters;
	PresentationSpawnParameters.ObjectFlags |= RF_Transient;
	PresentationSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	if (PresentationMode == EAlgonaP1PresentationMode::LegacyStaticIsm)
	{
		PresentationSpawnParameters.Name = TEXT("AlgonaLegacyIsmPresentation");
		LegacyPresentationActor =
			InWorld.SpawnActor<AAlgonaLegacyIsmPresentationActor>(
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				PresentationSpawnParameters);

		if (!LegacyPresentationActor)
		{
			CameraActor->Destroy();
			CameraActor = nullptr;
			return;
		}

		LegacyPresentationActor->SetPresentationCamera(
			CameraActor->GetCameraComponent());
		LegacyPresentationActor->AddTickPrerequisiteActor(CameraActor);
		return;
	}

	PresentationSpawnParameters.Name = TEXT("AlgonaArmyPresentation");
	ArmyPresentationActor =
		InWorld.SpawnActor<AAlgonaArmyPresentationActor>(
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			PresentationSpawnParameters);

	if (!ArmyPresentationActor)
	{
		CameraActor->Destroy();
		CameraActor = nullptr;
		return;
	}

	ArmyPresentationActor->SetPresentationCamera(
		CameraActor->GetCameraComponent());
	ArmyPresentationActor->AddTickPrerequisiteActor(CameraActor);
}

void UAlgonaPresentationSubsystem::Deinitialize()
{
	if (IsValid(ArmyPresentationActor))
	{
		ArmyPresentationActor->Destroy();
	}
	ArmyPresentationActor = nullptr;

	if (IsValid(LegacyPresentationActor))
	{
		LegacyPresentationActor->Destroy();
	}
	LegacyPresentationActor = nullptr;

	if (IsValid(CameraActor))
	{
		CameraActor->Destroy();
	}
	CameraActor = nullptr;

	Super::Deinitialize();
}
