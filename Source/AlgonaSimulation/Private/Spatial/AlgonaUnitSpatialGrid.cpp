#include "Spatial/AlgonaUnitSpatialGrid.h"

namespace
{
	constexpr double MinimumSpatialCellSizeCm = 1.0;
}

FAlgonaUnitSpatialGrid::FAlgonaUnitSpatialGrid(
	double InCellSizeCm)
{
	Reset(InCellSizeCm);
}

void FAlgonaUnitSpatialGrid::Reset(
	double InCellSizeCm)
{
	CellSizeCm = FMath::Max(
		InCellSizeCm,
		MinimumSpatialCellSizeCm);

	Cells.Reset();
	UnitEntries.Reset();
}

FIntPoint FAlgonaUnitSpatialGrid::GetCellCoordinates(
	const FVector& WorldPosition) const
{
	return FIntPoint(
		FMath::FloorToInt(WorldPosition.X / CellSizeCm),
		FMath::FloorToInt(WorldPosition.Y / CellSizeCm));
}

FVector2D FAlgonaUnitSpatialGrid::GetCellWorldMin(
	const FIntPoint& Cell) const
{
	return FVector2D(
		static_cast<double>(Cell.X) * CellSizeCm,
		static_cast<double>(Cell.Y) * CellSizeCm);
}

void FAlgonaUnitSpatialGrid::AddUnit(
	uint32 UnitId,
	const FVector& WorldPosition)
{
	if (UnitId == 0)
	{
		return;
	}

	if (UnitEntries.Num() <= static_cast<int32>(UnitId))
	{
		UnitEntries.SetNum(
			static_cast<int32>(UnitId) + 1);
	}

	FUnitEntry& Entry = UnitEntries[UnitId];

	if (Entry.IndexInCell != INDEX_NONE)
	{
		return;
	}

	Entry.Cell = GetCellCoordinates(WorldPosition);

	TArray<uint32>& CellUnits =
		Cells.FindOrAdd(Entry.Cell);

	Entry.IndexInCell =
		CellUnits.Add(UnitId);
}

bool FAlgonaUnitSpatialGrid::UpdateUnit(
	uint32 UnitId,
	const FVector& WorldPosition)
{
	if (UnitId == 0
		|| !UnitEntries.IsValidIndex(
			static_cast<int32>(UnitId)))
	{
		return false;
	}

	FUnitEntry& Entry = UnitEntries[UnitId];

	if (Entry.IndexInCell == INDEX_NONE)
	{
		return false;
	}

	const FIntPoint NewCell =
		GetCellCoordinates(WorldPosition);

	if (NewCell == Entry.Cell)
	{
		return false;
	}

	TArray<uint32>* OldCellUnits =
		Cells.Find(Entry.Cell);

	if (!OldCellUnits
		|| !OldCellUnits->IsValidIndex(Entry.IndexInCell))
	{
		return false;
	}

	const int32 RemovedIndex = Entry.IndexInCell;
	const int32 LastIndex = OldCellUnits->Num() - 1;
	const uint32 SwappedUnitId =
		(*OldCellUnits)[LastIndex];

	OldCellUnits->RemoveAtSwap(
		RemovedIndex,
		1,
		EAllowShrinking::No);

	if (RemovedIndex != LastIndex
		&& UnitEntries.IsValidIndex(
			static_cast<int32>(SwappedUnitId)))
	{
		UnitEntries[SwappedUnitId].IndexInCell =
			RemovedIndex;
	}

	const FIntPoint OldCell = Entry.Cell;

	if (OldCellUnits->IsEmpty())
	{
		Cells.Remove(OldCell);
	}

	TArray<uint32>& NewCellUnits =
		Cells.FindOrAdd(NewCell);

	Entry.Cell = NewCell;
	Entry.IndexInCell =
		NewCellUnits.Add(UnitId);

	return true;
}

void FAlgonaUnitSpatialGrid::QueryUnitIds(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	TArray<uint32>& OutUnitIds) const
{
	OutUnitIds.Reset();

	const FVector2D OrderedMin(
		FMath::Min(WorldMin.X, WorldMax.X),
		FMath::Min(WorldMin.Y, WorldMax.Y));

	const FVector2D OrderedMax(
		FMath::Max(WorldMin.X, WorldMax.X),
		FMath::Max(WorldMin.Y, WorldMax.Y));

	const FIntPoint MinCell(
		FMath::FloorToInt(OrderedMin.X / CellSizeCm),
		FMath::FloorToInt(OrderedMin.Y / CellSizeCm));

	const FIntPoint MaxCell(
		FMath::FloorToInt(OrderedMax.X / CellSizeCm),
		FMath::FloorToInt(OrderedMax.Y / CellSizeCm));

	for (int32 CellY = MinCell.Y;
		CellY <= MaxCell.Y;
		++CellY)
	{
		for (int32 CellX = MinCell.X;
			CellX <= MaxCell.X;
			++CellX)
		{
			if (const TArray<uint32>* CellUnits =
				Cells.Find(FIntPoint(CellX, CellY)))
			{
				OutUnitIds.Append(*CellUnits);
			}
		}
	}
}
