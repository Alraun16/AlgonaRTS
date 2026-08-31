#pragma once

#include "CoreMinimal.h"

/**
 * Sparse 2D uniform grid indexing individual combat units.
 * Every unit belongs to exactly one cell by its current world position.
 */
class ALGONASIMULATION_API FAlgonaUnitSpatialGrid final
{
public:
	explicit FAlgonaUnitSpatialGrid(double InCellSizeCm = 10000.0);

	void Reset(double InCellSizeCm);

	double GetCellSizeCm() const
	{
		return CellSizeCm;
	}

	FIntPoint GetCellCoordinates(const FVector& WorldPosition) const;
	FVector2D GetCellWorldMin(const FIntPoint& Cell) const;

	void AddUnit(
		uint32 UnitId,
		const FVector& WorldPosition);

	/**
	 * Updates the unit cell.
	 * Returns true only when the unit actually crossed a cell boundary.
	 */
	bool UpdateUnit(
		uint32 UnitId,
		const FVector& WorldPosition);

	void QueryUnitIds(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		TArray<uint32>& OutUnitIds) const;

private:
	struct FUnitEntry
	{
		FIntPoint Cell = FIntPoint(0, 0);
		int32 IndexInCell = INDEX_NONE;
	};

	double CellSizeCm = 10000.0;
	TMap<FIntPoint, TArray<uint32>> Cells;

	// UnitId is currently dense and starts at 1.
	// Keeping the cell slot here makes cell changes O(1).
	TArray<FUnitEntry> UnitEntries;
};
