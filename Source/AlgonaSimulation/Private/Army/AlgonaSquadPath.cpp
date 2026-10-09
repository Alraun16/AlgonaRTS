#include "Army/AlgonaSquadPath.h"

double GetAlgonaPathLength(
	TConstArrayView<FVector> PathPoints,
	int32 FromIndex)
{
	double Length = 0.0;

	for (int32 PointIndex = FMath::Max(FromIndex, 0);
		PointIndex + 1 < PathPoints.Num();
		++PointIndex)
	{
		Length += FVector::Dist2D(
			PathPoints[PointIndex],
			PathPoints[PointIndex + 1]);
	}

	return Length;
}

FAlgonaPathFollowResult FollowAlgonaPath(
	TConstArrayView<FVector> PathPoints,
	const FVector& CenterLocation,
	double Lookahead,
	int32& InOutPointIndex)
{
	FAlgonaPathFollowResult Result;

	const int32 PointCount = PathPoints.Num();

	if (PointCount == 0)
	{
		Result.AimLocation = CenterLocation;
		return Result;
	}

	int32 PointIndex = FMath::Clamp(InOutPointIndex, 0, PointCount - 1);

	// 1. Поворот считается пройденным, когда центр оказался за ним по
	// направлению следующего отрезка. По расстоянию судить нельзя: на
	// срезанной дуге центр до самой точки поворота не доходит.
	while (PointIndex < PointCount - 1)
	{
		const FVector Corner = PathPoints[PointIndex];

		FVector NextDirection = PathPoints[PointIndex + 1] - Corner;
		NextDirection.Z = 0.0;

		if (!NextDirection.Normalize())
		{
			// Вырожденный отрезок: такой точки на пути как будто нет.
			++PointIndex;
			continue;
		}

		FVector FromCorner = CenterLocation - Corner;
		FromCorner.Z = 0.0;

		if (FVector::DotProduct(FromCorner, NextDirection) <= 0.0)
		{
			break;
		}

		++PointIndex;
	}

	InOutPointIndex = PointIndex;

	const double DistanceToPoint =
		FVector::Dist2D(CenterLocation, PathPoints[PointIndex]);

	Result.RemainingDistance =
		DistanceToPoint + GetAlgonaPathLength(PathPoints, PointIndex);

	// 2. Точка погони: отмеряем Lookahead вдоль пути, начиная от центра.
	// Если до текущей точки пути дальше, чем Lookahead, центр идёт прямо
	// к ней — так же, как по прямому пути из одной точки.
	double Budget = Lookahead - DistanceToPoint;
	int32 AimIndex = PointIndex;
	Result.AimLocation = PathPoints[PointIndex];

	while (Budget > 0.0 && AimIndex < PointCount - 1)
	{
		const FVector SegmentStart = PathPoints[AimIndex];
		const FVector SegmentEnd = PathPoints[AimIndex + 1];
		const double SegmentLength = FVector::Dist2D(SegmentStart, SegmentEnd);

		if (SegmentLength <= Budget)
		{
			Budget -= SegmentLength;
			++AimIndex;
			Result.AimLocation = SegmentEnd;
			continue;
		}

		Result.AimLocation = FMath::Lerp(
			SegmentStart,
			SegmentEnd,
			Budget / SegmentLength);
		Budget = 0.0;
	}

	Result.AimLocation.Z = CenterLocation.Z;
	return Result;
}
