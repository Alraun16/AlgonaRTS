#pragma once

#include "CoreMinimal.h"

/**
 * Превращает переменный frame DeltaTime в фиксированные simulation steps.
 *
 * За один frame выполняется ограниченное число steps. Оставшийся долг
 * сохраняется: clock не выбрасывает authoritative simulation ticks молча.
 */
struct ALGONASIMULATION_API FAlgonaFixedStepAccumulator
{
	FAlgonaFixedStepAccumulator(
		double InFixedStepSeconds = 1.0 / 20.0,
		int32 InMaxStepsPerFrame = 5)
	{
		Configure(InFixedStepSeconds, InMaxStepsPerFrame);
	}

	void Configure(
		double InFixedStepSeconds,
		int32 InMaxStepsPerFrame)
	{
		FixedStepSeconds = FMath::Max(InFixedStepSeconds, 1.0e-6);
		MaxStepsPerFrame = FMath::Max(InMaxStepsPerFrame, 1);
		Reset();
	}

	void Reset()
	{
		AccumulatorSeconds = 0.0;
		MaxObservedBacklogSeconds = 0.0;
		OverloadedFrameCount = 0;
	}

template <typename TStepFunction>
int32 Advance(
	double FrameDeltaSeconds,
	TStepFunction&& ExecuteStep)
{
	AccumulatorSeconds +=
		FMath::Max(FrameDeltaSeconds, 0.0);

	MaxObservedBacklogSeconds = FMath::Max(
		MaxObservedBacklogSeconds,
		AccumulatorSeconds);

	// Небольшой допуск только против погрешности double.
	const double StepToleranceSeconds =
		FixedStepSeconds * 1.0e-9;

	int32 ExecutedSteps = 0;

	while (
		AccumulatorSeconds + StepToleranceSeconds
			>= FixedStepSeconds
		&& ExecutedSteps < MaxStepsPerFrame)
	{
		ExecuteStep(FixedStepSeconds);

		AccumulatorSeconds -= FixedStepSeconds;
		++ExecutedSteps;
	}

	if (FMath::Abs(AccumulatorSeconds)
		<= StepToleranceSeconds)
	{
		AccumulatorSeconds = 0.0;
	}
	else
	{
		AccumulatorSeconds =
			FMath::Max(AccumulatorSeconds, 0.0);
	}

	if (AccumulatorSeconds + StepToleranceSeconds
		>= FixedStepSeconds)
	{
		++OverloadedFrameCount;
	}

	return ExecutedSteps;
}

	double GetFixedStepSeconds() const { return FixedStepSeconds; }
	double GetBacklogSeconds() const { return AccumulatorSeconds; }
	double GetMaxObservedBacklogSeconds() const
	{
		return MaxObservedBacklogSeconds;
	}
	uint64 GetOverloadedFrameCount() const
	{
		return OverloadedFrameCount;
	}

private:
	double FixedStepSeconds;
	double AccumulatorSeconds;
	double MaxObservedBacklogSeconds;

	int32 MaxStepsPerFrame;
	uint64 OverloadedFrameCount;
};