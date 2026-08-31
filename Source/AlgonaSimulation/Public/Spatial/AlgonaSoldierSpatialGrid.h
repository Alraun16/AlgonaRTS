#pragma once

#include "CoreMinimal.h"

/**
 * Sparse 2D uniform grid indexing individual soldiers.
 * Every soldier belongs to exactly one cell by its current world position.
 */
class ALGONASIMULATION_API FAlgonaSoldierSpatialGrid final
{
public:
	explicit FAlgonaSoldierSpatialGrid(double InCellSizeCm = 10000.0);

	void Reset(double InCellSizeCm);

	double GetCellSizeCm() const
	{
		return CellSizeCm;
	}

	FIntPoint GetCellCoordinates(const FVector& WorldPosition) const;

	void AddSoldier(
		uint32 SoldierId,
		const FVector& WorldPosition);

	/**
	 * Updates the soldier cell.
	 * Returns true only when the soldier actually crossed a cell boundary.
	 */
	bool UpdateSoldier(
		uint32 SoldierId,
		const FVector& WorldPosition);

	void QuerySoldierIds(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		TArray<uint32>& OutSoldierIds) const;

private:
	struct FSoldierEntry
	{
		FIntPoint Cell = FIntPoint(0, 0);
		int32 IndexInCell = INDEX_NONE;
	};

	double CellSizeCm = 10000.0;

	TMap<FIntPoint, TArray<uint32>> Cells;

	// SoldierId is currently dense and starts at 1.
	// Keeping the cell slot here makes cell changes O(1).
	TArray<FSoldierEntry> SoldierEntries;
};