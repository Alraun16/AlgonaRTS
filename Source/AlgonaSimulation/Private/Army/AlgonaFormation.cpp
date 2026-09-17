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

void MirrorAlgonaFormationLayout(
	FAlgonaFormationLayout& Layout,
	TArray<int32>& OutNewSlotForOldSlot)
{
	const int32 SlotCount = Layout.Slots.Num();
	OutNewSlotForOldSlot.Reset(SlotCount);

	if (SlotCount == 0)
	{
		return;
	}

	// 1. Отражение: смещения меняют знак, строки меняются местами.
	// Прямоугольник границ симметричен относительно центра, поэтому
	// центр Squad остаётся тем же, а положения слотов в мире не меняются.
	TArray<FAlgonaFormationSlot> MirroredSlots = Layout.Slots;

	for (FAlgonaFormationSlot& Slot : MirroredSlots)
	{
		Slot.LocalOffset = -Slot.LocalOffset;
		Slot.RowIndex = Layout.RowCount - 1 - Slot.RowIndex;
	}

	// 2. Обычный порядок слотов: строки спереди назад, внутри строки
	// от центра к краям, правая сторона первой.
	TArray<int32> Order;
	Order.Reserve(SlotCount);

	for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
	{
		Order.Add(SlotIndex);
	}

	Order.Sort([&MirroredSlots](int32 A, int32 B)
	{
		const FAlgonaFormationSlot& SlotA = MirroredSlots[A];
		const FAlgonaFormationSlot& SlotB = MirroredSlots[B];

		if (SlotA.RowIndex != SlotB.RowIndex)
		{
			return SlotA.RowIndex < SlotB.RowIndex;
		}

		const float DistanceA = FMath::Abs(SlotA.LocalOffset.Y);
		const float DistanceB = FMath::Abs(SlotB.LocalOffset.Y);

		if (DistanceA != DistanceB)
		{
			return DistanceA < DistanceB;
		}

		return SlotA.LocalOffset.Y > SlotB.LocalOffset.Y;
	});

	// 3. Перенумерация. Вызывающий код переставляет ActiveUnitIds и SlotIndex
	// по OutNewSlotForOldSlot — Unit при этом не двигаются.
	OutNewSlotForOldSlot.SetNum(SlotCount);
	TArray<FAlgonaFormationSlot> OrderedSlots;
	OrderedSlots.Reserve(SlotCount);

	float FrontRowLocalX = MirroredSlots[0].LocalOffset.X;

	for (int32 NewSlotIndex = 0; NewSlotIndex < SlotCount; ++NewSlotIndex)
	{
		const int32 OldSlotIndex = Order[NewSlotIndex];
		OutNewSlotForOldSlot[OldSlotIndex] = NewSlotIndex;
		OrderedSlots.Add(MirroredSlots[OldSlotIndex]);

		FrontRowLocalX = FMath::Max(
			FrontRowLocalX,
			MirroredSlots[OldSlotIndex].LocalOffset.X);
	}

	Layout.Slots = MoveTemp(OrderedSlots);
	Layout.FrontRowLocalX = FrontRowLocalX;
}
