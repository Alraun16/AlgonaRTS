#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "AlgonaSimulationSubsystem.generated.h"

class UMassEntitySubsystem;

/**
 * Central entry point for the Algona gameplay simulation.
 *
 * P0.1:
 * Tick -> RunSimulationStep -> simulation state / Mass.
 *
 * P0.2 will replace the direct per-frame simulation call
 * with a fixed-step accumulator.
 */
UCLASS()
class ALGONASIMULATION_API UAlgonaSimulationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

private:
	void RunSimulationStep(float DeltaTime);

	UMassEntitySubsystem* MassEntitySubsystem = nullptr;
	
};