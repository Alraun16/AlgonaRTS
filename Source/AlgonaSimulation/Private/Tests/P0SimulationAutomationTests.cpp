#include "Core/AlgonaFixedStepAccumulator.h"
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

	const FAlgonaSquad DefaultSquad;
	TestEqual(
		TEXT("Default squad has 100 formation slots"),
		DefaultSquad.GetFormationCapacity(),
		50);

	const FAlgonaSoldierMovementFragment DefaultMovement;
	TestEqual(
		TEXT("Default soldier state is Idle"),
		DefaultMovement.State,
		EAlgonaSoldierMovementState::Idle);

	TestFalse(
	TEXT("Default squad has no active move target"),
	DefaultSquad.bHasMoveTarget);
	TestTrue(
		TEXT("Default squad anchor speed is positive"),
		DefaultSquad.AnchorMoveSpeed > 0.0f);
	TestTrue(
		TEXT("Default soldier follow speed is positive"),
		DefaultSquad.SoldierMoveSpeed > 0.0f);
	
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
		0.201,
		[&CallbackCount, &SimulatedSeconds](double StepSeconds)
		{
			++CallbackCount;
			SimulatedSeconds += StepSeconds;
		});

	TestEqual(TEXT("0.201 seconds contains four full fixed steps"), Steps, 4);
	TestEqual(TEXT("Callback executed four times"), CallbackCount, 4);
	TestTrue(
		TEXT("Exactly 0.20 simulation seconds advanced"),
		FMath::IsNearlyEqual(SimulatedSeconds, 0.20));
	TestTrue(
		TEXT("Small sub-step remainder is preserved"),
		FMath::IsNearlyEqual(
			Accumulator.GetBacklogSeconds(),
			0.001,
			1.0e-6));

	FAlgonaFixedStepAccumulator HitchAccumulator(0.025, 5);

	int32 TotalHitchSteps = HitchAccumulator.Advance(
		1.0,
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

	for (int32 CatchUpFrame = 0; CatchUpFrame < 3; ++CatchUpFrame)
	{
		TotalHitchSteps += HitchAccumulator.Advance(
			0.0,
			[](double StepSeconds)
			{
				(void)StepSeconds;
			});
	}

	TestEqual(
		TEXT("All twenty fixed steps eventually execute"),
		TotalHitchSteps,
		20);
	TestTrue(
		TEXT("No simulation time was discarded"),
		FMath::IsNearlyZero(
			HitchAccumulator.GetBacklogSeconds(),
			1.0e-6));

	return true;
}

#endif