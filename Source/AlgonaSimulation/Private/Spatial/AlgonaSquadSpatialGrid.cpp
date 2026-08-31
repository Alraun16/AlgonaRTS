#include "Spatial/AlgonaSquadSpatialGrid.h"

namespace
{
	constexpr double MinimumSpatialCellSizeCm = 1.0;
}

FAlgonaSquadSpatialGrid::FAlgonaSquadSpatialGrid(double InCellSizeCm)
{
	Reset(InCellSizeCm);
}

void FAlgonaSquadSpatialGrid::Reset(double InCellSizeCm)
{
	CellSizeCm = FMath::Max(InCellSizeCm, MinimumSpatialCellSizeCm);
	Cells.Reset();
}

FIntPoint FAlgonaSquadSpatialGrid::GetCellCoordinates(
	const FVector& WorldPosition) const
{
	return GetCellCoordinates(FVector2D(WorldPosition.X, WorldPosition.Y));
}

FIntPoint FAlgonaSquadSpatialGrid::GetCellCoordinates(
	const FVector2D& WorldPosition) const
{
	return FIntPoint(
		FMath::FloorToInt(WorldPosition.X / CellSizeCm),
		FMath::FloorToInt(WorldPosition.Y / CellSizeCm));
}

FVector2D FAlgonaSquadSpatialGrid::GetCellWorldMin(
	const FIntPoint& Cell) const
{
	return FVector2D(
		static_cast<double>(Cell.X) * CellSizeCm,
		static_cast<double>(Cell.Y) * CellSizeCm);
}

void FAlgonaSquadSpatialGrid::AddSquad(
	int32 SquadId,
	const FVector& Center)
{
	if (SquadId == INDEX_NONE)
	{
		return;
	}

	TArray<int32>& CellSquads = Cells.FindOrAdd(GetCellCoordinates(Center));
	CellSquads.AddUnique(SquadId);
}

void FAlgonaSquadSpatialGrid::RemoveSquad(
	int32 SquadId,
	const FVector& Center)
{
	const FIntPoint Cell = GetCellCoordinates(Center);
	TArray<int32>* CellSquads = Cells.Find(Cell);
	if (!CellSquads)
	{
		return;
	}

	CellSquads->RemoveSingleSwap(SquadId);
	if (CellSquads->IsEmpty())
	{
		Cells.Remove(Cell);
	}
}

bool FAlgonaSquadSpatialGrid::UpdateSquad(
	int32 SquadId,
	const FVector& OldCenter,
	const FVector& NewCenter)
{
	const FIntPoint OldCell =
		GetCellCoordinates(OldCenter);

	const FIntPoint NewCell =
		GetCellCoordinates(NewCenter);

	if (OldCell == NewCell)
	{
		return false;
	}

	RemoveSquad(SquadId, OldCenter);
	AddSquad(SquadId, NewCenter);

	return true;
}

void FAlgonaSquadSpatialGrid::QuerySquadIds(
	const FVector2D& WorldMin,
	const FVector2D& WorldMax,
	TArray<int32>& OutSquadIds) const
{
	OutSquadIds.Reset();

	const FVector2D OrderedMin(
		FMath::Min(WorldMin.X, WorldMax.X),
		FMath::Min(WorldMin.Y, WorldMax.Y));
	const FVector2D OrderedMax(
		FMath::Max(WorldMin.X, WorldMax.X),
		FMath::Max(WorldMin.Y, WorldMax.Y));

	const FIntPoint MinCell = GetCellCoordinates(OrderedMin);
	const FIntPoint MaxCell = GetCellCoordinates(OrderedMax);

	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			if (const TArray<int32>* CellSquads = Cells.Find(FIntPoint(CellX, CellY)))
			{
				OutSquadIds.Append(*CellSquads);
			}
		}
	}
}
