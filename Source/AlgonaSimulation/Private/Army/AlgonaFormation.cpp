#include "Army/AlgonaFormation.h"

namespace
{
	// Правило формы: сколько слотов получает строка RowIndex,
	// если разложить осталось RemainingSlots. Всегда не меньше 1.
	int32 GetRowSlotCount(
		const FAlgonaFormationParams& Params,
		int32 RowIndex,
		int32 RemainingSlots)
	{
		(void)RowIndex;

		switch (Params.Shape)
		{
		case EAlgonaFormationShape::Rectangle:
		default:
			return FMath::Min(
				FMath::Max(Params.RowLength, 1),
				RemainingSlots);
		}
	}

	// Боковое смещение (Y) слота с порядковым номером OrderInRow в строке
	// из RowSlotCount слотов. Нумерация от центра к краям, правая сторона
	// первой. В шагах SlotSpacing:
	//   3 слота: 0, +1, -1
	//   4 слота: +0.5, -0.5, +1.5, -1.5
	// Любая строка получается симметричной, поэтому неполная строка
	// автоматически центрирована относительно оси Squad.
	float GetRowSlotLateralOffset(
		int32 OrderInRow,
		int32 RowSlotCount,
		float SlotSpacing)
	{
		const bool bOddRow = (RowSlotCount % 2) != 0;

		float StepsFromCenter = 0.0f;
		bool bRightSide = true;

		if (bOddRow)
		{
			// Слот 0 — точно по центру, дальше пары справа/слева.
			StepsFromCenter = static_cast<float>((OrderInRow + 1) / 2);
			bRightSide = (OrderInRow % 2) != 0;
		}
		else
		{
			// Центр строки между двумя слотами.
			StepsFromCenter = static_cast<float>(OrderInRow / 2) + 0.5f;
			bRightSide = (OrderInRow % 2) == 0;
		}

		const float Offset = StepsFromCenter * SlotSpacing;
		return bRightSide ? Offset : -Offset;
	}
}

int32 GetAlgonaDefaultRowLength(int32 UnitCount)
{
	return UnitCount <= 20 ? 5 : 10;
}

void BuildAlgonaFormationLayout(
	const FAlgonaFormationParams& Params,
	int32 SlotCount,
	FAlgonaFormationLayout& OutLayout)
{
	// Массив слотов переиспользуется между вызовами Reform.
	OutLayout.Slots.Reset();
	OutLayout.RowCount = 0;
	OutLayout.Radius = 0.0f;
	OutLayout.FrontRowLocalX = 0.0f;

	if (SlotCount <= 0)
	{
		return;
	}

	OutLayout.Slots.Reserve(SlotCount);

	// 1. Слоты строка за строкой. Передняя строка стоит на X = 0,
	// каждая следующая — на RowSpacing дальше назад.
	int32 RowIndex = 0;

	while (OutLayout.Slots.Num() < SlotCount)
	{
		const int32 RowSlotCount = GetRowSlotCount(
			Params,
			RowIndex,
			SlotCount - OutLayout.Slots.Num());

		const float RowX =
			-static_cast<float>(RowIndex) * Params.RowSpacing;

		for (int32 OrderInRow = 0;
			OrderInRow < RowSlotCount;
			++OrderInRow)
		{
			FAlgonaFormationSlot& Slot =
				OutLayout.Slots.AddDefaulted_GetRef();

			Slot.LocalOffset = FVector2f(
				RowX,
				GetRowSlotLateralOffset(
					OrderInRow,
					RowSlotCount,
					Params.SlotSpacing));
			Slot.RowIndex = RowIndex;
		}

		++RowIndex;
	}

	OutLayout.RowCount = RowIndex;

	// 2. Центр Squad — центр прямоугольника, охватывающего все слоты.
	// Он меняется только при появлении или исчезновении целой строки.
	FVector2f BoundsMin = OutLayout.Slots[0].LocalOffset;
	FVector2f BoundsMax = BoundsMin;

	for (const FAlgonaFormationSlot& Slot : OutLayout.Slots)
	{
		BoundsMin.X = FMath::Min(BoundsMin.X, Slot.LocalOffset.X);
		BoundsMin.Y = FMath::Min(BoundsMin.Y, Slot.LocalOffset.Y);
		BoundsMax.X = FMath::Max(BoundsMax.X, Slot.LocalOffset.X);
		BoundsMax.Y = FMath::Max(BoundsMax.Y, Slot.LocalOffset.Y);
	}

	const FVector2f Center = (BoundsMin + BoundsMax) * 0.5f;

	// 3. Смещения пересчитываются относительно центра, заодно считается радиус.
	float RadiusSquared = 0.0f;

	for (FAlgonaFormationSlot& Slot : OutLayout.Slots)
	{
		Slot.LocalOffset -= Center;
		RadiusSquared = FMath::Max(
			RadiusSquared,
			Slot.LocalOffset.SizeSquared());
	}

	OutLayout.Radius = FMath::Sqrt(RadiusSquared);

	// Передняя строка до сдвига стояла на X = 0.
	OutLayout.FrontRowLocalX = -Center.X;
}
