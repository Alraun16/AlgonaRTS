#include "Army/AlgonaSquad.h"

FVector FAlgonaSquad::GetForwardDirection2D() const
{
	const FVector Forward = FacingDirection.GetSafeNormal2D();
	return Forward.IsNearlyZero() ? FVector::ForwardVector : Forward;
}

float FAlgonaSquad::GetMoveAcceleration() const
{
	return CenterMoveSpeed / AccelerationSeconds;
}

float FAlgonaSquad::GetMoveDeceleration() const
{
	return GetMoveAcceleration() * DecelerationFactor;
}

float FAlgonaSquad::GetYawAcceleration() const
{
	return GetMaxYawRate() / AccelerationSeconds;
}

float FAlgonaSquad::GetMaxYawRate() const
{
	// Угловая скорость = линейная скорость крайнего слота / его радиус.
	// Squad из одного Unit (радиус около нуля) поворачивается как Unit
	// на расстоянии одного интервала строя.
	const float Radius = FMath::Max(
		FormationLayout.Radius,
		FormationParams.SlotSpacing);

	return TurnSpeedFactor * CenterMoveSpeed / Radius;
}

bool FAlgonaSquad::RemoveActiveUnitAt(
	int32 SlotIndex,
	uint32& OutMovedUnitId)
{
	OutMovedUnitId = 0;

	if (!ActiveUnitIds.IsValidIndex(SlotIndex))
	{
		return false;
	}

	const int32 LastSlotIndex = ActiveUnitIds.Num() - 1;
	if (SlotIndex != LastSlotIndex)
	{
		OutMovedUnitId = ActiveUnitIds[LastSlotIndex];
	}

	// Последний активный Unit занимает освободившийся слот. Отряд логически
	// уменьшается с конца, а слоты остальных Unit не меняются.
	ActiveUnitIds.RemoveAtSwap(
		SlotIndex,
		1,
		EAllowShrinking::No);

	return true;
}

int32 FAlgonaSquad::AddActiveUnit(uint32 UnitId)
{
	return ActiveUnitIds.Add(UnitId);
}

void FAlgonaSquad::RebuildFormationLayout()
{
	if (!bRowLengthSetByPlayer)
	{
		FormationParams.RowLength =
			GetAlgonaDefaultRowLength(ActiveUnitIds.Num());
	}

	BuildAlgonaFormationLayout(
		FormationParams,
		ActiveUnitIds.Num(),
		FormationLayout);

	++FormationRevision;
}

void FAlgonaSquad::ReformAfterCompositionChange()
{
	const float OldFrontRowLocalX = FormationLayout.FrontRowLocalX;

	RebuildFormationLayout();

	// Передняя строка остаётся в той же точке мира:
	// Center + Forward * FrontRowLocalX не меняется.
	const double CenterShift = static_cast<double>(
		OldFrontRowLocalX - FormationLayout.FrontRowLocalX);

	CenterLocation += GetForwardDirection2D() * CenterShift;
}

bool FAlgonaSquad::ApplyRowLengthOrder(int32 RowLength)
{
	const int32 NewRowLength = FMath::Clamp(
		RowLength,
		AlgonaFormationLimits::MinRowLength,
		AlgonaFormationLimits::MaxRowLength);

	bRowLengthSetByPlayer = true;

	if (NewRowLength == FormationParams.RowLength)
	{
		return false;
	}

	FormationParams.RowLength = NewRowLength;
	RebuildFormationLayout();
	return true;
}
