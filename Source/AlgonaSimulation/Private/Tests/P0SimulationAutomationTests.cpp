#include "Core/AlgonaFixedStepAccumulator.h"
#include "Army/AlgonaUnitFragments.h"
#include "Army/AlgonaUnitSnapshot.h"
#include "Army/AlgonaSquad.h"

#include "Mass/EntityElementTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP0SimulationTypesTest,
	"Algona.P0.Simulation.Types",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP0SimulationTypesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	TestTrue(
		TEXT("Unit ID is a Mass fragment"),
		FAlgonaUnitIdFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()));

	TestTrue(
		TEXT("Squad membership is a Mass fragment"),
		FAlgonaSquadMemberFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()));

	TestTrue(
		TEXT("Movement state is a Mass fragment"),
		FAlgonaUnitMovementFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()));

	const FAlgonaSquad DefaultSquad;
	TestEqual(
		TEXT("Default squad has no slots until its formation is built"),
		DefaultSquad.FormationLayout.Slots.Num(),
		0);
	TestEqual(
		TEXT("Default squad has no active units"),
		DefaultSquad.ActiveUnitIds.Num(),
		0);

	const FAlgonaUnitMovementFragment DefaultMovement;
	TestEqual(
		TEXT("Default unit state is Idle"),
		DefaultMovement.State,
		EAlgonaUnitMovementState::Idle);

	TestFalse(
		TEXT("Default squad has no active move target"),
		DefaultSquad.bHasMoveTarget);
	TestTrue(
		TEXT("Default squad center speed is positive"),
		DefaultSquad.CenterMoveSpeed > 0.0f);
	TestTrue(
		TEXT("Default unit speed factor lets units catch up with the squad"),
		DefaultSquad.UnitSpeedFactor > 1.0f);

	const FAlgonaUnitSnapshot DefaultSnapshot;
	TestEqual(
		TEXT("Default snapshot has no session ID"),
		DefaultSnapshot.EntityId,
		uint32(0));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP0FixedStepAccumulatorTest,
	"Algona.P0.Simulation.FixedStepAccumulator",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP0FixedStepAccumulatorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FAlgonaFixedStepAccumulator Accumulator(0.025, 5);

	int32 CallbackCount = 0;
	double SimulatedSeconds = 0.0;

	const int32 Steps = Accumulator.Advance(
		0.101,
		[&CallbackCount, &SimulatedSeconds](double StepSeconds)
		{
			++CallbackCount;
			SimulatedSeconds += StepSeconds;
		});

	TestEqual(
		TEXT("0.101 seconds contains four full fixed steps"),
		Steps,
		4);
	TestEqual(
		TEXT("Callback executed four times"),
		CallbackCount,
		4);
	TestTrue(
		TEXT("Exactly 0.10 simulation seconds advanced"),
		FMath::IsNearlyEqual(SimulatedSeconds, 0.10));
	TestTrue(
		TEXT("Small sub-step remainder is preserved"),
		FMath::IsNearlyEqual(
			Accumulator.GetBacklogSeconds(),
			0.001,
			1.0e-6));

	FAlgonaFixedStepAccumulator HitchAccumulator(0.025, 5);
	int32 TotalHitchSteps = HitchAccumulator.Advance(
		0.25,
		[](double StepSeconds)
		{
			(void)StepSeconds;
		});

	TestEqual(
		TEXT("Large hitch executes at most five steps in first frame"),
		TotalHitchSteps,
		5);
	TestTrue(
		TEXT("Unprocessed simulation time remains as backlog"),
		HitchAccumulator.GetBacklogSeconds() > 0.0);
	TestTrue(
		TEXT("Overload is counted"),
		HitchAccumulator.GetOverloadedFrameCount() > 0);

	TotalHitchSteps += HitchAccumulator.Advance(
		0.0,
		[](double StepSeconds)
		{
			(void)StepSeconds;
		});

	TestEqual(
		TEXT("All ten fixed steps eventually execute"),
		TotalHitchSteps,
		10);
	TestTrue(
		TEXT("No simulation time was discarded"),
		FMath::IsNearlyZero(
			HitchAccumulator.GetBacklogSeconds(),
			1.0e-6));

	return true;
}

#endif
