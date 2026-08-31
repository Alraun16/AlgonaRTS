#include "Spatial/AlgonaSoldierSpatialGrid.h"

namespace
{
	constexpr double MinimumSpatialCellSizeCm = 1.0;
}

FAlgonaSoldierSpatialGrid::FAlgonaSoldierSpatialGrid(
	double InCellSizeCm)
{
	Reset(InCellSizeCm);
}

void FAlgonaSoldierSpatialGrid::Reset(
	double InCellSizeCm)
{
	CellSizeCm = FMath::Max(
		InCellSizeCm,
		MinimumSpatialCellSizeCm);

	Cells.Reset();
	SoldierEntries.Reset();
}

FIntPoint FAlgonaSoldierSpatialGrid::GetCellCoordinates(
	const FVector& WorldPosition) const
{
	return FIntPoint(
		FMath::FloorToInt(WorldPosition.X / CellSizeCm),
		FMath::FloorToInt(WorldPosition.Y / CellSizeCm));
}

void FAlgonaSoldierSpatialGrid::AddSoldier(
	uint32 SoldierId,
	const FVector& WorldPosition)
{
	if (SoldierId == 0)
	{
		return;
	}

	if (SoldierEntries.Num() <= static_cast<int32>(SoldierId))
	{
		SoldierEntries.SetNum(
			static_cast<int32>(SoldierId) + 1);
	}

	FSoldierEntry& Entry = SoldierEntries[SoldierId];

	if (Entry.IndexInCell != INDEX_NONE)
	{
		return;
	}

	Entry.Cell = GetCellCoordinates(WorldPosition);

	TArray<uint32>& CellSoldiers =
		Cells.FindOrAdd(Entry.Cell);

	Entry.IndexInCell =
		CellSoldiers.Add(SoldierId);
}

bool FAlgonaSoldierSpatialGrid::UpdateSoldier(
	uint32 SoldierId,
	const FVector& WorldPosition)
{
	if (SoldierId == 0
		|| !SoldierEntries.IsValidIndex(
			static_cast<int32>(SoldierId)))
	{
		return false;
	}

	FSoldierEntry& Entry = SoldierEntries[SoldierId];

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

	TArray<uint32>* OldCellSoldiers =
		Cells.Find(Entry.Cell);

	if (!OldCellSoldiers
		|| !OldCellSoldiers->IsValidIndex(Entry.IndexInCell))
	{
		return false;
	}

	const int32 RemovedIndex = Entry.IndexInCell;
	const int32 LastIndex = OldCellSoldiers->Num() - 1;
	const uint32 SwappedSoldierId =
		(*OldCellSoldiers)[LastIndex];

	OldCellSoldiers->RemoveAtSwap(
		RemovedIndex,
		1,
		EAllowShrinking::No);

	if (RemovedIndex != LastIndex
		&& SoldierEntries.IsValidIndex(
			static_cast<int32>(SwappedSoldierId)))
	{
		SoldierEntries[SwappedSoldierId].IndexInCell =
			RemovedIndex;
	}

	const FIntPoint OldCell = Entry.Cell;

	if (OldCellSoldiers->IsEmpty())
	{
		Cells.Remove(OldCell);
	}

	TArray<uint32>& NewCellSoldiers =
		Cells.FindOrAdd(NewCell);

	Entry.Cell = NewCell;
	Entry.IndexInCell =
		NewCellSoldiers.Add(SoldierId);

	return true;
}

void FAlgonaSoldierSpatialGrid::QuerySoldierIds(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	TArray<uint32>& OutSoldierIds) const
{
	OutSoldierIds.Reset();

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
			if (const TArray<uint32>* CellSoldiers =
				Cells.Find(FIntPoint(CellX, CellY)))
			{
				OutSoldierIds.Append(*CellSoldiers);
			}
		}
	}
}