#include "Army/AlgonaFormation.h"

// Formation generation is intentionally self-contained and deterministic.
namespace
{
	constexpr float MinimumSpacingCm = 1.0f;
}

FAlgonaFormationLayout FAlgonaFormationGenerator::BuildRectangle(
	int32 ActiveUnitCount,
	float SlotSpacingCm,
	int32 RequestedMaxSlotsPerRow)
{
	FAlgonaFormationLayout Layout;
	Layout.Shape = EAlgonaFormationShape::Rectangle;
	Layout.SlotSpacingCm = FMath::Max(SlotSpacingCm, MinimumSpacingCm);

	if (ActiveUnitCount <= 0)
	{
		Layout.MaxSlotsPerRow = FMath::Max(RequestedMaxSlotsPerRow, 1);
		return Layout;
	}

	const int32 DefaultRowLength = ChooseDefaultRectangleRowLength(ActiveUnitCount);
	Layout.MaxSlotsPerRow = FMath::Clamp(
		RequestedMaxSlotsPerRow > 0 ? RequestedMaxSlotsPerRow : DefaultRowLength,
		1,
		ActiveUnitCount);

	const int32 RowCount = FMath::DivideAndRoundUp(
		ActiveUnitCount,
		Layout.MaxSlotsPerRow);

	Layout.Slots.Reserve(ActiveUnitCount);

	TArray<float> RowX;
	RowX.Reserve(Layout.MaxSlotsPerRow);

	float MaxAbsX = 0.0f;
	float MaxAbsY = 0.0f;
	float MaxRadiusSquared = 0.0f;
	int32 RemainingUnits = ActiveUnitCount;

	// The row centers themselves are centered around the squad anchor, so the
	// anchor is the geometric center rather than the old front-row reference.
	for (int32 RowIndex = 0; RowIndex < RowCount; ++RowIndex)
	{
		const int32 UnitsInRow = FMath::Min(
			Layout.MaxSlotsPerRow,
			RemainingUnits);
		RemainingUnits -= UnitsInRow;

		AppendCenteredRowPositions(
			UnitsInRow,
			Layout.SlotSpacingCm,
			RowX);

		const float LocalY =
			(static_cast<float>(RowCount - 1) * 0.5f
				- static_cast<float>(RowIndex))
			* Layout.SlotSpacingCm;

		for (const float LocalX : RowX)
		{
			FAlgonaFormationSlot& Slot = Layout.Slots.AddDefaulted_GetRef();
			Slot.LocalPosition = FVector2D(LocalX, LocalY);

			MaxAbsX = FMath::Max(MaxAbsX, FMath::Abs(LocalX));
			MaxAbsY = FMath::Max(MaxAbsY, FMath::Abs(LocalY));
			MaxRadiusSquared = FMath::Max(
				MaxRadiusSquared,
				Slot.LocalPosition.SizeSquared());
		}
	}

	Layout.HalfExtentsCm = FVector2D(MaxAbsX, MaxAbsY);
	Layout.RadiusCm = FMath::Sqrt(MaxRadiusSquared);
	return Layout;
}

void FAlgonaFormationGenerator::BuildMirrorSlotMap(
	FAlgonaFormationLayout& Layout)
{
	const int32 SlotCount = Layout.Slots.Num();
	Layout.MirrorSlotIndices.Init(INDEX_NONE, SlotCount);

	// Pair unassigned slots. Writing both directions at once makes the map an
	// involution, which keeps a second 180-degree flip exactly reversible.
	for (int32 SourceIndex = 0; SourceIndex < SlotCount; ++SourceIndex)
	{
		if (Layout.MirrorSlotIndices[SourceIndex] != INDEX_NONE)
		{
			continue;
		}

		const FVector2D DesiredPosition =
			-Layout.Slots[SourceIndex].LocalPosition;

		int32 BestTargetIndex = INDEX_NONE;
		double BestDistanceSquared = TNumericLimits<double>::Max();

		for (int32 CandidateIndex = 0;
			CandidateIndex < SlotCount;
			++CandidateIndex)
		{
			if (Layout.MirrorSlotIndices[CandidateIndex] != INDEX_NONE)
			{
				continue;
			}

			const double DistanceSquared =
				(DesiredPosition
					- Layout.Slots[CandidateIndex].LocalPosition)
				.SizeSquared();

			if (DistanceSquared < BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				BestTargetIndex = CandidateIndex;
			}
		}

		if (BestTargetIndex == INDEX_NONE)
		{
			BestTargetIndex = SourceIndex;
		}

		Layout.MirrorSlotIndices[SourceIndex] = BestTargetIndex;
		Layout.MirrorSlotIndices[BestTargetIndex] = SourceIndex;
	}

	Layout.MirrorRevision = Layout.FormationRevision;
}

int32 FAlgonaFormationGenerator::ChooseDefaultRectangleRowLength(
	int32 ActiveUnitCount)
{
	if (ActiveUnitCount <= 0)
	{
		return 1;
	}

	return FMath::Min(ActiveUnitCount, ActiveUnitCount <= 20 ? 5 : 10);
}

void FAlgonaFormationGenerator::AppendCenteredRowPositions(
	int32 SlotCount,
	float SlotSpacingCm,
	TArray<float>& OutLocalX)
{
	OutLocalX.Reset();
	if (SlotCount <= 0)
	{
		return;
	}

	// Odd rows start with the exact center slot. Even rows start with the two
	// slots nearest the center. Both then expand left/right toward the edges.
	if ((SlotCount & 1) != 0)
	{
		OutLocalX.Add(0.0f);
		for (int32 PairIndex = 1; OutLocalX.Num() < SlotCount; ++PairIndex)
		{
			const float Offset = static_cast<float>(PairIndex) * SlotSpacingCm;
			OutLocalX.Add(-Offset);
			if (OutLocalX.Num() < SlotCount)
			{
				OutLocalX.Add(Offset);
			}
		}
		return;
	}

	for (int32 PairIndex = 0; OutLocalX.Num() < SlotCount; ++PairIndex)
	{
		const float Offset =
			(static_cast<float>(PairIndex) + 0.5f) * SlotSpacingCm;
		OutLocalX.Add(-Offset);
		if (OutLocalX.Num() < SlotCount)
		{
			OutLocalX.Add(Offset);
		}
	}
}
