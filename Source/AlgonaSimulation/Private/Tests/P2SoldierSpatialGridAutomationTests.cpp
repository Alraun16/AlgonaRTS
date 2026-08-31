#include "Spatial/AlgonaSoldierSpatialGrid.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SoldierSpatialGridTest,
	"Algona.P2.Spatial.SoldierUniformGrid",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SoldierSpatialGridTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	FAlgonaSoldierSpatialGrid Grid(10000.0);

	Grid.AddSoldier(
		1,
		FVector(100.0, 100.0, 0.0));

	Grid.AddSoldier(
		2,
		FVector(10100.0, 100.0, 0.0));

	Grid.AddSoldier(
		3,
		FVector(200.0, 100.0, 0.0));

	TArray<uint32> SoldierIds;

	Grid.QuerySoldierIds(
		FVector2D(0.0, 0.0),
		FVector2D(9999.0, 9999.0),
		SoldierIds);

	SoldierIds.Sort();

	TestEqual(
		TEXT("First cell initially contains two soldiers"),
		SoldierIds.Num(),
		2);

	TestTrue(
		TEXT("Soldier 1 crossed into another cell"),
		Grid.UpdateSoldier(
			1,
			FVector(20100.0, 100.0, 0.0)));

	// Soldier 3 was swapped inside the old cell when soldier 1 left.
	// Moving it afterwards verifies that its stored slot was repaired.
	TestTrue(
		TEXT("Swapped soldier keeps a valid cell slot"),
		Grid.UpdateSoldier(
			3,
			FVector(10100.0, 200.0, 0.0)));

	Grid.QuerySoldierIds(
		FVector2D(0.0, 0.0),
		FVector2D(9999.0, 9999.0),
		SoldierIds);

	TestEqual(
		TEXT("Original cell is empty"),
		SoldierIds.Num(),
		0);

	return true;
}

#endif