// Copyright Epic Games, Inc. All Rights Reserved.

#include "AlgonaRTS.h"

#include "AlgonaSimulationSubsystem.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "P0TestGameMode.h"

class FAlgonaRTSModule final : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();

		DebugSimulationSpawnedHandle =
			UAlgonaSimulationSubsystem::OnDebugSimulationSpawned().AddStatic(
				&FAlgonaRTSModule::HandleDebugSimulationSpawned
			);
	}

	virtual void ShutdownModule() override
	{
		UAlgonaSimulationSubsystem::OnDebugSimulationSpawned().Remove(
			DebugSimulationSpawnedHandle
		);
		DebugSimulationSpawnedHandle.Reset();

		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
	static void HandleDebugSimulationSpawned(UWorld* World)
	{
		AP0TestGameMode::EnsurePresentationActor(World);
	}

	FDelegateHandle DebugSimulationSpawnedHandle;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FAlgonaRTSModule, AlgonaRTS, "AlgonaRTS");
