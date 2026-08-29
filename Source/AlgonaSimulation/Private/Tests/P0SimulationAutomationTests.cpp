// Core fixed-step and authoritative data types kept stable across P2.
#include "Core/AlgonaFixedStepAccumulator.h"
#include "Army/AlgonaFormation.h"
#include "Army/AlgonaSoldierFragments.h"
#include "Army/AlgonaSoldierSnapshot.h"
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

	// Mass storage contract: P2 adds state inside the existing fragments rather
	// than switching stationary/moving soldiers between archetypes.
	TestTrue(
		TEXT("Soldier ID is a Mass fragment"),
		FAlgonaSoldierIdFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()));
	TestTrue(
		TEXT("Squad membership is a Mass fragment"),
		FAlgonaSquadMemberFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()));
	TestTrue(
		TEXT("Movement state is a Mass fragment"),
		FAlgonaSoldierMovementFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()));

	FAlgonaSquad DefaultSquad;
	TestTrue(
		TEXT("Default soldier spacing is 150 cm"),
		FMath::IsNearlyEqual(DefaultSquad.SoldierSpacingCm, 150.0f));
	DefaultSquad.Formation =
		FAlgonaFormationGenerator::BuildRectangle(
			50,
			DefaultSquad.SoldierSpacingCm);
	TestEqual(
		TEXT("Generated default-size squad has 50 formation slots"),
		DefaultSquad.GetFormationCapacity(),
		50);

	const FAlgonaSoldierMovementFragment DefaultMovement;
	TestEqual(
		TEXT("Default soldier state is Idle"),
		DefaultMovement.State,
		EAlgonaSoldierMovementState::Idle);
	TestEqual(
		TEXT("Default soldier formation state is Active"),
		FAlgonaSquadMemberFragment().FormationState,
		EAlgonaSoldierFormationState::Active);

	TestFalse(
		TEXT("Default squad has no active move target"),
		DefaultSquad.bHasMoveTarget);
	TestTrue(
		TEXT("Default walk speed is positive"),
		DefaultSquad.WalkSpeedCmPerSecond > 0.0f);
	TestTrue(
		TEXT("Default run speed exceeds walk speed"),
		DefaultSquad.RunSpeedCmPerSecond > DefaultSquad.WalkSpeedCmPerSecond);
	TestEqual(
		TEXT("Default run speed is 5 m/s"),
		DefaultSquad.RunSpeedCmPerSecond,
		500.0f);
	TestEqual(
		TEXT("Active soldier catch-up limit is 6 m/s"),
		DefaultSquad.GetActiveSoldierSpeedLimit(),
		600.0f);

	const FAlgonaSoldierSnapshot DefaultSnapshot;
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
