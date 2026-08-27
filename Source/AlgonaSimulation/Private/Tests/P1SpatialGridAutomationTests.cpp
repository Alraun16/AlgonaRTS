#include "Army/AlgonaSquad.h"
#include "Spatial/AlgonaSquadSpatialGrid.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP1SpatialGridTest,
	"Algona.P1.Spatial.UniformGrid",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP1SpatialGridTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	FAlgonaSquadSpatialGrid Grid(5000.0);

	TestTrue(
		TEXT("Origin is cell 0,0"),
		Grid.GetCellCoordinates(FVector::ZeroVector) == FIntPoint(0, 0));
	TestTrue(
		TEXT("Negative coordinates use floor, not truncation"),
		Grid.GetCellCoordinates(FVector(-1.0, -1.0, 0.0)) == FIntPoint(-1, -1));
	TestTrue(
		TEXT("Positive boundary starts the next cell"),
		Grid.GetCellCoordinates(FVector(5000.0, 5000.0, 0.0)) == FIntPoint(1, 1));

	Grid.AddSquad(10, FVector(100.0, 100.0, 0.0));
	Grid.AddSquad(20, FVector(5200.0, 100.0, 0.0));
	Grid.AddSquad(30, FVector(-100.0, -100.0, 0.0));

	TArray<int32> SquadIds;
	Grid.QuerySquadIds(
		FVector2D(0.0, 0.0),
		FVector2D(9999.0, 4999.0),
		SquadIds);
	SquadIds.Sort();

	TestEqual(TEXT("Two occupied cells are returned"), SquadIds.Num(), 2);
	if (SquadIds.Num() == 2)
	{
		TestEqual(TEXT("First squad ID is 10"), SquadIds[0], 10);
		TestEqual(TEXT("Second squad ID is 20"), SquadIds[1], 20);
	}

	Grid.UpdateSquad(
		20,
		FVector(5200.0, 100.0, 0.0),
		FVector(10100.0, 100.0, 0.0));

	Grid.QuerySquadIds(
		FVector2D(0.0, 0.0),
		FVector2D(9999.0, 4999.0),
		SquadIds);
	TestEqual(TEXT("Moved squad left its old query area"), SquadIds.Num(), 1);
	if (SquadIds.Num() == 1)
	{
		TestEqual(TEXT("Squad 10 remains"), SquadIds[0], 10);
	}

	Grid.QuerySquadIds(
		FVector2D(10000.0, 0.0),
		FVector2D(14999.0, 4999.0),
		SquadIds);
	TestEqual(TEXT("Moved squad entered its new cell"), SquadIds.Num(), 1);
	if (SquadIds.Num() == 1)
	{
		TestEqual(TEXT("New cell contains squad 20"), SquadIds[0], 20);
	}

	FAlgonaSquad Squad;
	Squad.MemberCount = 50;
	Squad.FormationWidth = 10;
	Squad.SoldierSpacing = 100.0f;
	Squad.AnchorLocation = FVector(1000.0, 0.0, 0.0);
	Squad.FacingDirection = FVector::ForwardVector;

	TestTrue(
		TEXT("Spatial center is the occupied formation center, not front anchor"),
		Squad.GetSpatialCenter().Equals(FVector(800.0, 0.0, 0.0), 0.01));

	return true;
}

#endif
