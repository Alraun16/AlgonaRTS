#include "Army/AlgonaSquad.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr float SquadTestTolerance = 0.01f;

	// Squad с UnitId 1..UnitCount и раскладкой по параметрам по умолчанию
	// (оба расстояния 150 см).
	FAlgonaSquad MakeTestSquad(int32 UnitCount)
	{
		FAlgonaSquad Squad;
		Squad.SquadId = 0;

		for (int32 Index = 0; Index < UnitCount; ++Index)
		{
			Squad.AddActiveUnit(static_cast<uint32>(Index + 1));
		}

		Squad.RebuildFormationLayout();
		return Squad;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SquadCompositionTest,
	"Algona.P2.Squad.Composition",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SquadCompositionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	FAlgonaSquad Squad;
	TestEqual(TEXT("First added unit gets slot 0"), Squad.AddActiveUnit(7), 0);
	TestEqual(TEXT("Second added unit gets slot 1"), Squad.AddActiveUnit(8), 1);

	Squad = MakeTestSquad(23);
	uint32 MovedUnitId = 0;

	// Удаление из середины: последний Unit (23) занимает слот 5.
	TestTrue(TEXT("Remove slot 5"), Squad.RemoveActiveUnitAt(5, MovedUnitId));
	TestEqual(TEXT("Last unit moved into the hole"), MovedUnitId, static_cast<uint32>(23));
	TestEqual(TEXT("Slot 5 now holds unit 23"), Squad.ActiveUnitIds[5], static_cast<uint32>(23));
	TestEqual(TEXT("Unit count after removal"), Squad.ActiveUnitIds.Num(), 22);
	TestEqual(TEXT("Other slots are unchanged"), Squad.ActiveUnitIds[6], static_cast<uint32>(7));

	// Удаление последнего слота никого не переставляет.
	TestTrue(TEXT("Remove last slot"), Squad.RemoveActiveUnitAt(21, MovedUnitId));
	TestEqual(TEXT("Nobody moved"), MovedUnitId, static_cast<uint32>(0));
	TestEqual(TEXT("Unit count after last removal"), Squad.ActiveUnitIds.Num(), 21);

	// Неверный слот не меняет состав.
	TestFalse(TEXT("Negative slot is rejected"), Squad.RemoveActiveUnitAt(-1, MovedUnitId));
	TestFalse(TEXT("Slot past the end is rejected"), Squad.RemoveActiveUnitAt(21, MovedUnitId));
	TestEqual(TEXT("Unit count unchanged"), Squad.ActiveUnitIds.Num(), 21);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SquadReformAfterCompositionTest,
	"Algona.P2.Squad.ReformAfterComposition",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SquadReformAfterCompositionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	// 21 Unit: длина строки 10, строки 10 + 10 + 1, передняя строка на +150.
	FAlgonaSquad Squad = MakeTestSquad(21);
	Squad.CenterLocation = FVector(1000.0, 500.0, 0.0);
	Squad.FacingDirection = FVector::ForwardVector;

	TestEqual(TEXT("Initial row length"), Squad.FormationParams.RowLength, 10);
	TestEqual(TEXT("Initial front row X"), Squad.FormationLayout.FrontRowLocalX, 150.0f, SquadTestTolerance);

	const double FrontRowWorldX =
		Squad.CenterLocation.X + Squad.FormationLayout.FrontRowLocalX;
	const uint32 RevisionBefore = Squad.FormationRevision;

	// 20 Unit: правило даёт длину 5, строк 4, передняя строка на +225.
	uint32 MovedUnitId = 0;
	Squad.RemoveActiveUnitAt(20, MovedUnitId);
	Squad.ReformAfterCompositionChange();

	TestEqual(TEXT("Default rule switched row length"), Squad.FormationParams.RowLength, 5);
	TestEqual(TEXT("Row count"), Squad.FormationLayout.RowCount, 4);
	TestEqual(TEXT("Revision increased"), Squad.FormationRevision, RevisionBefore + 1);

	TestEqual(
		TEXT("Front row stays in place"),
		Squad.CenterLocation.X + Squad.FormationLayout.FrontRowLocalX,
		FrontRowWorldX,
		0.01);
	TestEqual(TEXT("Center moved back"), Squad.CenterLocation.X, 925.0, 0.01);
	TestEqual(TEXT("Center did not move sideways"), Squad.CenterLocation.Y, 500.0, 0.01);

	// Та же ситуация для Squad, смотрящего в +Y: сдвиг идёт вдоль направления.
	FAlgonaSquad TurnedSquad = MakeTestSquad(21);
	TurnedSquad.FacingDirection = FVector::RightVector;
	TurnedSquad.RemoveActiveUnitAt(20, MovedUnitId);
	TurnedSquad.ReformAfterCompositionChange();

	TestEqual(TEXT("Turned squad: center shift along facing"), TurnedSquad.CenterLocation.Y, -75.0, 0.01);
	TestEqual(TEXT("Turned squad: no shift across facing"), TurnedSquad.CenterLocation.X, 0.0, 0.01);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SquadRowLengthOrderTest,
	"Algona.P2.Squad.RowLengthOrder",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SquadRowLengthOrderTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	FAlgonaSquad Squad = MakeTestSquad(50);
	Squad.CenterLocation = FVector(300.0, 200.0, 0.0);

	TestEqual(TEXT("Default row length for 50"), Squad.FormationParams.RowLength, 10);
	TestFalse(TEXT("Row length not set by player yet"), Squad.bRowLengthSetByPlayer);

	const uint32 RevisionBefore = Squad.FormationRevision;

	// Приказ меняет раскладку, но центр остаётся на месте.
	TestTrue(TEXT("Row length order changes layout"), Squad.ApplyRowLengthOrder(5));
	TestTrue(TEXT("Row length marked as set by player"), Squad.bRowLengthSetByPlayer);
	TestEqual(TEXT("Row length"), Squad.FormationParams.RowLength, 5);
	TestEqual(TEXT("Row count"), Squad.FormationLayout.RowCount, 10);
	TestEqual(TEXT("Revision increased"), Squad.FormationRevision, RevisionBefore + 1);
	TestTrue(TEXT("Center stays in place"), Squad.CenterLocation.Equals(FVector(300.0, 200.0, 0.0), 0.01));

	// Повторный приказ той же длины Reform не вызывает.
	TestFalse(TEXT("Same row length is not a change"), Squad.ApplyRowLengthOrder(5));
	TestEqual(TEXT("Revision unchanged"), Squad.FormationRevision, RevisionBefore + 1);

	// После ручной установки правило по умолчанию не возвращает длину 10.
	uint32 MovedUnitId = 0;
	Squad.RemoveActiveUnitAt(0, MovedUnitId);
	Squad.ReformAfterCompositionChange();
	TestEqual(TEXT("Player row length survives reform"), Squad.FormationParams.RowLength, 5);

	// Длина строки ограничивается допустимым диапазоном.
	Squad.ApplyRowLengthOrder(0);
	TestEqual(TEXT("Row length clamped to minimum"), Squad.FormationParams.RowLength, AlgonaFormationLimits::MinRowLength);
	Squad.ApplyRowLengthOrder(1000);
	TestEqual(TEXT("Row length clamped to maximum"), Squad.FormationParams.RowLength, AlgonaFormationLimits::MaxRowLength);

	return true;
}

#endif
