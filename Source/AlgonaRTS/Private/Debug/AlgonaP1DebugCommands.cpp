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

		if (Simulation)
		{
			Simulation->SubmitMoveAllSquadsByOffset(FVector(X, Y, Z));
		}
	}
	
	void BenchmarkSpatialQueryCommand(
	const TArray<FString>& Arguments,
	UWorld* World)
	{
		if (!World || Arguments.Num() < 3)
		{
			return;
		}

		const double CenterX =
			FCString::Atod(*Arguments[0]);

		const double CenterY =
			FCString::Atod(*Arguments[1]);

		const double HalfExtent =
			FMath::Max(
				FCString::Atod(*Arguments[2]),
				1.0);

		const int32 Iterations =
			Arguments.Num() >= 4
				? FMath::Max(
					FCString::Atoi(*Arguments[3]),
					1)
				: 5000;

		UAlgonaSimulationSubsystem* Simulation =
			World->GetSubsystem<UAlgonaSimulationSubsystem>();

		if (!Simulation)
		{
			return;
		}

		Simulation->BenchmarkSpatialGridQueries(
			FVector2D(
				CenterX - HalfExtent,
				CenterY - HalfExtent),
			FVector2D(
				CenterX + HalfExtent,
				CenterY + HalfExtent),
			Iterations);
	}
	
	void BenchmarkSpatialSnapshotsCommand(
	const TArray<FString>& Arguments,
	UWorld* World)
	{
		if (!World || Arguments.Num() < 3)
		{
			return;
		}

		const double CenterX =
			FCString::Atod(*Arguments[0]);

		const double CenterY =
			FCString::Atod(*Arguments[1]);

		const double HalfExtent =
			FMath::Max(
				FCString::Atod(*Arguments[2]),
				1.0);

		const int32 Iterations =
			Arguments.Num() >= 4
				? FMath::Max(
					FCString::Atoi(*Arguments[3]),
					1)
				: 100;

		UAlgonaSimulationSubsystem* Simulation =
			World->GetSubsystem<UAlgonaSimulationSubsystem>();

		if (!Simulation)
		{
			return;
		}

		Simulation->BenchmarkSpatialSnapshotPaths(
			FVector2D(
				CenterX - HalfExtent,
				CenterY - HalfExtent),
			FVector2D(
				CenterX + HalfExtent,
				CenterY + HalfExtent),
			Iterations);
	}
	
	FAutoConsoleCommandWithWorldAndArgs GAlgonaP1MoveAllByOffsetCommand(
		TEXT("algona.P1.MoveAllBy"),
		TEXT("Move all authoritative squads by offset: X Y [Z]."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			&MoveAllSquadsByOffsetCommand),
		ECVF_Cheat);
	
	FAutoConsoleCommandWithWorldAndArgs GAlgonaP2BenchmarkSpatialQueryCommand(
	TEXT("algona.P2.BenchmarkSpatialQuery"),
	TEXT(
		"Compare squad and soldier grid queries: "
		"CenterX CenterY HalfExtent [Iterations]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&BenchmarkSpatialQueryCommand),
	ECVF_Cheat);
	
	FAutoConsoleCommandWithWorldAndArgs GAlgonaP2BenchmarkSpatialSnapshotsCommand(
	TEXT("algona.P2.BenchmarkSpatialSnapshots"),
	TEXT(
		"Compare full squad and soldier spatial snapshot paths: "
		"CenterX CenterY HalfExtent [Iterations]."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		&BenchmarkSpatialSnapshotsCommand),
	ECVF_Cheat);
}

#endif
