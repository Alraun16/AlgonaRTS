#include "Spatial/AlgonaUnitSpatialGrid.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2UnitSpatialGridTest,
	"Algona.P2.Spatial.UnitUniformGrid",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2UnitSpatialGridTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	FAlgonaUnitSpatialGrid Grid(10000.0);

	Grid.AddUnit(
		1,
		FVector(100.0, 100.0, 0.0));

	Grid.AddUnit(
		2,
		FVector(10100.0, 100.0, 0.0));

	Grid.AddUnit(
		3,
		FVector(200.0, 100.0, 0.0));

	TArray<uint32> UnitIds;

	Grid.QueryUnitIds(
		FVector2D(0.0, 0.0),
		FVector2D(9999.0, 9999.0),
		UnitIds);

	UnitIds.Sort();

	TestEqual(
		TEXT("First cell initially contains two units"),
		UnitIds.Num(),
		2);

	TestTrue(
		TEXT("Unit 1 crossed into another cell"),
		Grid.UpdateUnit(
			1,
			FVector(20100.0, 100.0, 0.0)));

	// Unit 3 was swapped inside the old cell when unit 1 left.
	// Moving it afterwards verifies that its stored slot was repaired.
	TestTrue(
		TEXT("Swapped unit keeps a valid cell slot"),
		Grid.UpdateUnit(
			3,
			FVector(10100.0, 200.0, 0.0)));

	Grid.QueryUnitIds(
		FVector2D(0.0, 0.0),
		FVector2D(9999.0, 9999.0),
		UnitIds);

	TestEqual(
		TEXT("Original cell is empty"),
		UnitIds.Num(),
		0);

	return true;
}

#endif
