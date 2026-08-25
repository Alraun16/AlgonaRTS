#pragma once

#include "CoreMinimal.h"

/**
 * Converts variable frame time into deterministic fixed simulation steps.
 * Work per frame is capped, but authoritative simulation time is never
 * silently discarded: any remaining debt stays in the accumulator.
 */
struct ALGONASIMULATION_API FAlgonaFixedStepAccumulator
{
	FAlgonaFixedStepAccumulator(
		double InFixedStepSeconds = 1.0 / 40.0,
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
		AccumulatorSeconds += FMath::Max(FrameDeltaSeconds, 0.0);

		MaxObservedBacklogSeconds = FMath::Max(
			MaxObservedBacklogSeconds,
			AccumulatorSeconds);

		const double StepToleranceSeconds =
			FixedStepSeconds * 1.0e-9;

		int32 ExecutedSteps = 0;

		while (AccumulatorSeconds + StepToleranceSeconds >= FixedStepSeconds
			&& ExecutedSteps < MaxStepsPerFrame)
		{
			ExecuteStep(FixedStepSeconds);
			AccumulatorSeconds -= FixedStepSeconds;
			++ExecutedSteps;
		}

		if (FMath::Abs(AccumulatorSeconds) <= StepToleranceSeconds)
		{
			AccumulatorSeconds = 0.0;
		}
		else
		{
			AccumulatorSeconds = FMath::Max(AccumulatorSeconds, 0.0);
		}

		if (AccumulatorSeconds + StepToleranceSeconds >= FixedStepSeconds)
		{
			++OverloadedFrameCount;
		}

		return ExecutedSteps;
	}

	double GetFixedStepSeconds() const
	{
		return FixedStepSeconds;
	}

	double GetBacklogSeconds() const
	{
		return AccumulatorSeconds;
	}

	double GetInterpolationAlpha() const
	{
		return FMath::Clamp(
			AccumulatorSeconds / FixedStepSeconds,
			0.0,
			1.0);
	}

	double GetMaxObservedBacklogSeconds() const
	{
		return MaxObservedBacklogSeconds;
	}

	uint64 GetOverloadedFrameCount() const
	{
		return OverloadedFrameCount;
	}

private:
	double FixedStepSeconds = 1.0 / 40.0;
	double AccumulatorSeconds = 0.0;
	double MaxObservedBacklogSeconds = 0.0;
	int32 MaxStepsPerFrame = 5;
	uint64 OverloadedFrameCount = 0;
};
