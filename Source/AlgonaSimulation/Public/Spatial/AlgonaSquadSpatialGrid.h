#pragma once

#include "CoreMinimal.h"

/**
 * Sparse 2D uniform grid used as the squad-level spatial broadphase.
 * Only occupied cells are stored; every squad belongs to exactly one cell by
 * its current formation center.
 */
class ALGONASIMULATION_API FAlgonaSquadSpatialGrid final
{
public:
	explicit FAlgonaSquadSpatialGrid(double InCellSizeCm = 5000.0);

	void Reset(double InCellSizeCm);

	double GetCellSizeCm() const
	{
		return CellSizeCm;
	}

	FIntPoint GetCellCoordinates(const FVector& WorldPosition) const;
	FIntPoint GetCellCoordinates(const FVector2D& WorldPosition) const;
	FVector2D GetCellWorldMin(const FIntPoint& Cell) const;

	void AddSquad(int32 SquadId, const FVector& Center);
	void RemoveSquad(int32 SquadId, const FVector& Center);
	bool UpdateSquad(
		int32 SquadId,
		const FVector& OldCenter,
		const FVector& NewCenter);

	void QuerySquadIds(
		const FVector2D& WorldMin,
		const FVector2D& WorldMax,
		TArray<int32>& OutSquadIds) const;

private:
	double CellSizeCm = 5000.0;
	TMap<FIntPoint, TArray<int32>> Cells;
};
