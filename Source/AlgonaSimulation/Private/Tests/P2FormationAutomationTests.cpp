// Pure formation tests do not need a World or Mass entity manager.
#include "Army/AlgonaFormation.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2RectangleFormationTest,
	"Algona.P2.Formation.RectangleRows",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2RectangleFormationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FAlgonaFormationLayout Layout =
		FAlgonaFormationGenerator::BuildRectangle(17, 100.0f);

	TestEqual(TEXT("17 units use default row length 5"), Layout.MaxSlotsPerRow, 5);
	TestEqual(TEXT("17 units generate 17 slots"), Layout.Num(), 17);

	// Front row comes first; inside the row the order expands center -> edges.
	const float ExpectedFirstRowX[] = {0.0f, -100.0f, 100.0f, -200.0f, 200.0f};
	for (int32 Index = 0; Index < 5; ++Index)
	{
		TestTrue(
			*FString::Printf(TEXT("Front-row X order %d"), Index),
			FMath::IsNearlyEqual(
				Layout.Slots[Index].LocalPosition.X,
				ExpectedFirstRowX[Index]));
		TestTrue(
			*FString::Printf(TEXT("Front-row Y %d"), Index),
			FMath::IsNearlyEqual(
				Layout.Slots[Index].LocalPosition.Y,
				150.0f));
	}

	// The incomplete final row has two slots nearest the common center axis.
	TestTrue(
		TEXT("Final partial row left slot is centered"),
		FMath::IsNearlyEqual(Layout.Slots[15].LocalPosition.X, -50.0f));
	TestTrue(
		TEXT("Final partial row right slot is centered"),
		FMath::IsNearlyEqual(Layout.Slots[16].LocalPosition.X, 50.0f));
	TestTrue(
		TEXT("Final row is the rear row"),
		FMath::IsNearlyEqual(Layout.Slots[15].LocalPosition.Y, -150.0f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2LargeRectangleDefaultWidthTest,
	"Algona.P2.Formation.DefaultWidth",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2LargeRectangleDefaultWidthTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FAlgonaFormationLayout Small =
		FAlgonaFormationGenerator::BuildRectangle(20, 100.0f);
	const FAlgonaFormationLayout Large =
		FAlgonaFormationGenerator::BuildRectangle(21, 100.0f);

	TestEqual(TEXT("20 units default to rows of 5"), Small.MaxSlotsPerRow, 5);
	TestEqual(TEXT("21 units default to rows of 10"), Large.MaxSlotsPerRow, 10);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2RequestedRectangleWidthTest,
	"Algona.P2.Formation.RequestedWidth",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2RequestedRectangleWidthTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	const FAlgonaFormationLayout Layout =
		FAlgonaFormationGenerator::BuildRectangle(50, 100.0f, 7);

	TestEqual(TEXT("Explicit rectangle width is preserved"), Layout.MaxSlotsPerRow, 7);
	TestEqual(TEXT("Explicit width still generates every slot"), Layout.Num(), 50);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2MirrorFormationTest,
	"Algona.P2.Formation.Mirror50",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2MirrorFormationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FAlgonaFormationLayout Layout =
		FAlgonaFormationGenerator::BuildRectangle(50, 100.0f);
	FAlgonaFormationGenerator::BuildMirrorSlotMap(Layout);

	TestEqual(
		TEXT("Mirror map covers every 50-unit slot"),
		Layout.MirrorSlotIndices.Num(),
		50);
	TestEqual(
		TEXT("Mirror map revision matches formation"),
		Layout.MirrorRevision,
		Layout.FormationRevision);

	TSet<int32> UniqueTargets;
	for (int32 SlotIndex = 0; SlotIndex < Layout.Num(); ++SlotIndex)
	{
		const int32 MirroredSlotIndex = Layout.MirrorSlotIndices[SlotIndex];
		TestTrue(
			*FString::Printf(TEXT("Mirror target %d is valid"), SlotIndex),
			Layout.IsValidSlot(MirroredSlotIndex));

		if (!Layout.IsValidSlot(MirroredSlotIndex))
		{
			continue;
		}

		UniqueTargets.Add(MirroredSlotIndex);
		TestEqual(
			*FString::Printf(TEXT("Mirror %d is reversible"), SlotIndex),
			Layout.MirrorSlotIndices[MirroredSlotIndex],
			SlotIndex);

		const FVector2D PositionSum =
			Layout.Slots[SlotIndex].LocalPosition
			+ Layout.Slots[MirroredSlotIndex].LocalPosition;
		TestTrue(
			*FString::Printf(TEXT("Mirror %d preserves world target after 180 flip"), SlotIndex),
			PositionSum.IsNearlyZero(0.01f));
	}

	TestEqual(TEXT("All mirror assignments are unique"), UniqueTargets.Num(), 50);
	return true;
}

#endif
