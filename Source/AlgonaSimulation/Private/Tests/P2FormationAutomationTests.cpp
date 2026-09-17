#include "Army/AlgonaFormation.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr float FormationTestTolerance = 0.01f;

	// Прямоугольник охватывающих границ всех слотов должен быть
	// симметричен относительно центра Squad.
	void TestLayoutIsCentered(
		FAutomationTestBase& Test,
		const FAlgonaFormationLayout& Layout)
	{
		if (Layout.Slots.IsEmpty())
		{
			return;
		}

		FVector2f BoundsMin = Layout.Slots[0].LocalOffset;
		FVector2f BoundsMax = BoundsMin;

		for (const FAlgonaFormationSlot& Slot : Layout.Slots)
		{
			BoundsMin.X = FMath::Min(BoundsMin.X, Slot.LocalOffset.X);
			BoundsMin.Y = FMath::Min(BoundsMin.Y, Slot.LocalOffset.Y);
			BoundsMax.X = FMath::Max(BoundsMax.X, Slot.LocalOffset.X);
			BoundsMax.Y = FMath::Max(BoundsMax.Y, Slot.LocalOffset.Y);
		}

		Test.TestEqual(
			TEXT("Bounds are centered on X"),
			BoundsMin.X + BoundsMax.X,
			0.0f,
			FormationTestTolerance);
		Test.TestEqual(
			TEXT("Bounds are centered on Y"),
			BoundsMin.Y + BoundsMax.Y,
			0.0f,
			FormationTestTolerance);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2FormationDefaultRowLengthTest,
	"Algona.P2.Formation.DefaultRowLength",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2FormationDefaultRowLengthTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	TestEqual(TEXT("1 unit"), GetAlgonaDefaultRowLength(1), 5);
	TestEqual(TEXT("20 units"), GetAlgonaDefaultRowLength(20), 5);
	TestEqual(TEXT("21 units"), GetAlgonaDefaultRowLength(21), 10);
	TestEqual(TEXT("50 units"), GetAlgonaDefaultRowLength(50), 10);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2FormationFullRectangleTest,
	"Algona.P2.Formation.FullRectangle",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2FormationFullRectangleTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	// Разные расстояния по осям, чтобы поймать перепутанные X и Y.
	FAlgonaFormationParams Params;
	Params.RowLength = 10;
	Params.SlotSpacing = 150.0f;
	Params.RowSpacing = 200.0f;

	FAlgonaFormationLayout Layout;
	BuildAlgonaFormationLayout(Params, 50, Layout);

	TestEqual(TEXT("Slot count"), Layout.Slots.Num(), 50);
	TestEqual(TEXT("Row count"), Layout.RowCount, 5);

	if (Layout.Slots.Num() != 50)
	{
		return false;
	}

	// Глубина 4 * 200 = 800 см: передняя строка на +400, задняя на -400.
	TestEqual(TEXT("Front row X"), Layout.FrontRowLocalX, 400.0f, FormationTestTolerance);
	TestEqual(TEXT("Slot 0 X"), Layout.Slots[0].LocalOffset.X, 400.0f, FormationTestTolerance);
	TestEqual(TEXT("Slot 49 X"), Layout.Slots[49].LocalOffset.X, -400.0f, FormationTestTolerance);

	// Строка из 10: от центра к краям, правая сторона первой.
	TestEqual(TEXT("Slot 0 is right-center"), Layout.Slots[0].LocalOffset.Y, 75.0f, FormationTestTolerance);
	TestEqual(TEXT("Slot 1 is left-center"), Layout.Slots[1].LocalOffset.Y, -75.0f, FormationTestTolerance);
	TestEqual(TEXT("Slot 2 is right second"), Layout.Slots[2].LocalOffset.Y, 225.0f, FormationTestTolerance);
	TestEqual(TEXT("Slot 9 is left edge"), Layout.Slots[9].LocalOffset.Y, -675.0f, FormationTestTolerance);

	TestEqual(TEXT("Slot 9 is in row 0"), Layout.Slots[9].RowIndex, 0);
	TestEqual(TEXT("Slot 10 starts row 1"), Layout.Slots[10].RowIndex, 1);
	TestEqual(TEXT("Slot 10 is right-center of row 1"), Layout.Slots[10].LocalOffset.Y, 75.0f, FormationTestTolerance);

	TestEqual(
		TEXT("Radius reaches the corner slot"),
		Layout.Radius,
		FMath::Sqrt(400.0f * 400.0f + 675.0f * 675.0f),
		FormationTestTolerance);

	TestLayoutIsCentered(*this, Layout);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2FormationPartialRowTest,
	"Algona.P2.Formation.PartialRow",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2FormationPartialRowTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	FAlgonaFormationParams Params;
	Params.RowLength = 10;
	Params.SlotSpacing = 150.0f;
	Params.RowSpacing = 150.0f;

	FAlgonaFormationLayout Layout;

	// 23 слота: строки 10, 10 и неполная из 3.
	BuildAlgonaFormationLayout(Params, 23, Layout);

	TestEqual(TEXT("Slot count"), Layout.Slots.Num(), 23);
	TestEqual(TEXT("Row count"), Layout.RowCount, 3);

	if (Layout.Slots.Num() != 23)
	{
		return false;
	}

	TestEqual(TEXT("Slot 20 is in row 2"), Layout.Slots[20].RowIndex, 2);
	TestEqual(TEXT("Odd partial row: slot 20 at center"), Layout.Slots[20].LocalOffset.Y, 0.0f, FormationTestTolerance);
	TestEqual(TEXT("Odd partial row: slot 21 right"), Layout.Slots[21].LocalOffset.Y, 150.0f, FormationTestTolerance);
	TestEqual(TEXT("Odd partial row: slot 22 left"), Layout.Slots[22].LocalOffset.Y, -150.0f, FormationTestTolerance);

	// Центр по X — середина между первой и последней строкой.
	TestEqual(TEXT("Front row X"), Layout.FrontRowLocalX, 150.0f, FormationTestTolerance);
	TestEqual(TEXT("Back row X"), Layout.Slots[22].LocalOffset.X, -150.0f, FormationTestTolerance);

	TestLayoutIsCentered(*this, Layout);

	// Один слот стоит точно в центре.
	BuildAlgonaFormationLayout(Params, 1, Layout);
	TestEqual(TEXT("Single slot count"), Layout.Slots.Num(), 1);
	TestEqual(TEXT("Single slot radius"), Layout.Radius, 0.0f, FormationTestTolerance);
	TestEqual(TEXT("Single slot front row X"), Layout.FrontRowLocalX, 0.0f, FormationTestTolerance);

	// Пустая раскладка очищает результат предыдущего вызова.
	BuildAlgonaFormationLayout(Params, 0, Layout);
	TestEqual(TEXT("Empty layout has no slots"), Layout.Slots.Num(), 0);
	TestEqual(TEXT("Empty layout has no rows"), Layout.RowCount, 0);

	// Некорректная длина строки не приводит к зависанию: считается равной 1.
	Params.RowLength = 0;
	BuildAlgonaFormationLayout(Params, 3, Layout);
	TestEqual(TEXT("Zero row length gives one slot per row"), Layout.RowCount, 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2FormationMirrorLayoutTest,
	"Algona.P2.Formation.MirrorLayout",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2FormationMirrorLayoutTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	FAlgonaFormationParams Params;
	Params.RowLength = 10;

	// Неполная последняя строка — случай, ради которого раскладка
	// отражается целиком: ни один Unit не должен менять место.
	FAlgonaFormationLayout Layout;
	BuildAlgonaFormationLayout(Params, 23, Layout);

	const FAlgonaFormationLayout OriginalLayout = Layout;

	TArray<int32> NewSlotForOldSlot;
	MirrorAlgonaFormationLayout(Layout, NewSlotForOldSlot);

	TestEqual(TEXT("Slot count"), Layout.Slots.Num(), OriginalLayout.Slots.Num());
	TestEqual(TEXT("Mapping size"), NewSlotForOldSlot.Num(), OriginalLayout.Slots.Num());

	TArray<int32> UseCount;
	UseCount.Init(0, Layout.Slots.Num());

	for (int32 OldSlotIndex = 0; OldSlotIndex < NewSlotForOldSlot.Num(); ++OldSlotIndex)
	{
		const int32 NewSlotIndex = NewSlotForOldSlot[OldSlotIndex];

		if (!TestTrue(TEXT("New slot is valid"), UseCount.IsValidIndex(NewSlotIndex)))
		{
			return false;
		}

		++UseCount[NewSlotIndex];

		// Слот того же Unit стоит там же в мире: смещение сменило знак,
		// а направление Squad — противоположное.
		TestTrue(
			TEXT("Slot keeps its world position"),
			Layout.Slots[NewSlotIndex].LocalOffset.Equals(
				-OriginalLayout.Slots[OldSlotIndex].LocalOffset,
				FormationTestTolerance));
	}

	for (const int32 Count : UseCount)
	{
		TestEqual(TEXT("Each slot used once"), Count, 1);
	}

	// Обычный порядок слотов сохранён: строки спереди назад, внутри строки
	// от центра к краям.
	for (int32 SlotIndex = 1; SlotIndex < Layout.Slots.Num(); ++SlotIndex)
	{
		const FAlgonaFormationSlot& Previous = Layout.Slots[SlotIndex - 1];
		const FAlgonaFormationSlot& Current = Layout.Slots[SlotIndex];

		TestTrue(TEXT("Rows go front to back"), Previous.RowIndex <= Current.RowIndex);

		if (Previous.RowIndex == Current.RowIndex)
		{
			TestTrue(
				TEXT("Slots go from the row center outwards"),
				FMath::Abs(Previous.LocalOffset.Y) <= FMath::Abs(Current.LocalOffset.Y));
		}
	}

	// Границы раскладки симметричны, поэтому передняя строка стоит на том же
	// расстоянии от центра — но теперь это бывшая задняя, неполная строка.
	TestEqual(
		TEXT("Front row distance from the center"),
		Layout.FrontRowLocalX,
		OriginalLayout.FrontRowLocalX,
		FormationTestTolerance);

	TestTrue(
		TEXT("Front row is the incomplete one"),
		Layout.Slots[0].RowIndex == 0 && Layout.Slots.Num() % Params.RowLength != 0);

	TestLayoutIsCentered(*this, Layout);
	return true;
}

#endif
