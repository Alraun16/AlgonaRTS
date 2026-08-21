#include "Core/AlgonaSimulationSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#if !UE_BUILD_SHIPPING

namespace
{
	void MoveAllSquadsByOffsetCommand(
		const TArray<FString>& Arguments,
		UWorld* World)
	{
		if (!World || Arguments.Num() < 2)
		{
			return;
		}

		const double X = FCString::Atod(*Arguments[0]);
		const double Y = FCString::Atod(*Arguments[1]);
		const double Z = Arguments.Num() >= 3
			? FCString::Atod(*Arguments[2])
			: 0.0;

		UAlgonaSimulationSubsystem* Simulation =
			World->GetSubsystem<UAlgonaSimulationSubsystem>();

		if (!Simulation)
		{
			return;
		}

		Simulation->SubmitMoveAllSquadsByOffset(
			FVector(X, Y, Z));
	}

	FAutoConsoleCommandWithWorldAndArgs
		GAlgonaP1MoveAllByOffsetCommand(
			TEXT("algona.P1.MoveAllBy"),
			TEXT("Move all authoritative squads by offset: X Y [Z]."),
			FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
				&MoveAllSquadsByOffsetCommand),
			ECVF_Cheat);
}

#endif