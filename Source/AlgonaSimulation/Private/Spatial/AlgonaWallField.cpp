#include "Spatial/AlgonaWallField.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	// Совпадение ближайших точек и расстояний, см: общие вершины соседних
	// отрезков в данных навмеша совпадают побитно, допуск — с запасом.
	constexpr float NearestWallTieCm = 0.5f;

	// Ближайшая точка отрезка к Point и положение этой точки на отрезке.
	FVector2f GetClosestPointOnSegment(
		const FAlgonaWallSegment& Wall,
		const FVector2f& Point,
		float& OutAlong,
		float& OutLength)
	{
		const FVector2f Direction = Wall.End - Wall.Start;
		const float LengthSquared = Direction.SizeSquared();

		if (LengthSquared <= UE_SMALL_NUMBER)
		{
			OutAlong = 0.0f;
			OutLength = 0.0f;
			return Wall.Start;
		}

		OutLength = FMath::Sqrt(LengthSquared);
		const FVector2f Tangent = Direction / OutLength;
		OutAlong = FVector2f::DotProduct(Point - Wall.Start, Tangent);

		return Wall.Start + Tangent * FMath::Clamp(OutAlong, 0.0f, OutLength);
	}
}

void AccumulateAlgonaNearestWall(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Point,
	FAlgonaWallNearest& InOutNearest)
{
	float Along = 0.0f;
	float Length = 0.0f;
	const FVector2f Closest = GetClosestPointOnSegment(Wall, Point, Along, Length);
	const float Distance = (Point - Closest).Size();

	// Заметно ближе прежнего — новый ближайший элемент.
	if (!InOutNearest.bFound || Distance < InOutNearest.Distance - NearestWallTieCm)
	{
		InOutNearest.ClosestPoint = Closest;
		InOutNearest.NormalSum = Wall.Normal;
		InOutNearest.Distance = Distance;
		InOutNearest.bFound = true;
		return;
	}

	// Та же точка — это общая вершина двух отрезков: сторона по средней
	// нормали. Равное расстояние до другой точки (середина узкого прохода)
	// ничего не меняет: обе стороны там проходимы.
	if (Distance <= InOutNearest.Distance + NearestWallTieCm
		&& (Closest - InOutNearest.ClosestPoint).Size() < NearestWallTieCm)
	{
		InOutNearest.NormalSum += Wall.Normal;
	}
}

FVector2f GetAlgonaNearestWallCorrection(
	const FAlgonaWallNearest& Nearest,
	const FVector2f& Point,
	float Margin)
{
	if (!Nearest.bFound)
	{
		return FVector2f::ZeroVector;
	}

	// Сумма противоположных нормалей вырождается только при испорченных
	// данных; тогда сторону не определить, и трогать точку нельзя.
	const FVector2f Normal = Nearest.NormalSum.GetSafeNormal();
	if (Normal.IsNearlyZero())
	{
		return FVector2f::ZeroVector;
	}

	const FVector2f Offset = Point - Nearest.ClosestPoint;
	const float Distance = Nearest.Distance;
	const float PositiveMargin = FMath::Max(Margin, 0.0f);

	const bool bBehind =
		Distance > NearestWallTieCm
		&& FVector2f::DotProduct(Offset, Normal) < 0.0f;

	if (bBehind)
	{
		// За границей: обратно к ближайшей её точке, но не глубже
		// разрешённого (отрицательный запас) и с положительным запасом
		// наружу (крупный Unit).
		const float AllowedDepth = FMath::Max(-Margin, 0.0f);

		if (Distance <= AllowedDepth)
		{
			return FVector2f::ZeroVector;
		}

		const FVector2f ToBoundary = -Offset / Distance;
		return ToBoundary * (Distance - AllowedDepth) + Normal * PositiveMargin;
	}

	// Перед границей: радиальный запас от ближайшей точки.
	if (Distance >= PositiveMargin)
	{
		return FVector2f::ZeroVector;
	}

	const FVector2f Direction = Distance > NearestWallTieCm
		? Offset / Distance
		: Normal;

	return Direction * (PositiveMargin - Distance);
}

bool IsAlgonaWallFacingPoint(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Point,
	float Tolerance)
{
	float Along = 0.0f;
	float Length = 0.0f;
	GetClosestPointOnSegment(Wall, Point, Along, Length);

	return Along >= -Tolerance && Along <= Length + Tolerance;
}

bool GetAlgonaWallLateralSpan(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Position,
	const FVector2f& Forward,
	const FVector2f& Right,
	float MaxForward,
	float Inflate,
	float& OutLow,
	float& OutHigh,
	float& OutForwardDistance)
{
	// Концы отрезка в системе Unit: вдоль хода и поперёк него.
	const FVector2f ToStart = Wall.Start - Position;
	const FVector2f ToEnd = Wall.End - Position;

	const float StartForward = FVector2f::DotProduct(ToStart, Forward);
	const float EndForward = FVector2f::DotProduct(ToEnd, Forward);
	const float StartRight = FVector2f::DotProduct(ToStart, Right);
	const float EndRight = FVector2f::DotProduct(ToEnd, Right);

	// Обрезка по полосе хода [0, MaxForward]: вне неё стена не мешает.
	float LowT = 0.0f;
	float HighT = 1.0f;

	const float ForwardDelta = EndForward - StartForward;

	if (FMath::Abs(ForwardDelta) <= UE_KINDA_SMALL_NUMBER)
	{
		// Стена строго поперёк хода: либо вся в полосе, либо вся вне.
		if (StartForward < 0.0f || StartForward > MaxForward)
		{
			return false;
		}
	}
	else
	{
		const float NearT = (0.0f - StartForward) / ForwardDelta;
		const float FarT = (MaxForward - StartForward) / ForwardDelta;

		LowT = FMath::Max(LowT, FMath::Min(NearT, FarT));
		HighT = FMath::Min(HighT, FMath::Max(NearT, FarT));

		if (LowT > HighT)
		{
			return false;
		}
	}

	const float LowRight = StartRight + (EndRight - StartRight) * LowT;
	const float HighRight = StartRight + (EndRight - StartRight) * HighT;

	OutLow = FMath::Min(LowRight, HighRight) - Inflate;
	OutHigh = FMath::Max(LowRight, HighRight) + Inflate;

	const float LowForward = StartForward + ForwardDelta * LowT;
	const float HighForward = StartForward + ForwardDelta * HighT;

	OutForwardDistance = FMath::Max(FMath::Min(LowForward, HighForward), 0.0f);

	return true;
}

void FAlgonaWallField::Build(TArray<FAlgonaWallSegment>&& InSegments)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaWallField_Build);

	Segments = MoveTemp(InSegments);

	if (Segments.IsEmpty())
	{
		Grid.Reset();
		CoarseOccupancy.Reset();
		return;
	}

	// Сетка поиска: отрезок кладётся во все клетки, которые накрывает его
	// габаритный прямоугольник. Отрезки границы навмеша короткие (они идут
	// по рёбрам полигонов), поэтому лишних клеток почти нет.
	const int32 SegmentCount = Segments.Num();

	TArray<int32> Indices;
	TArray<FVector2f> BoundsMin;
	TArray<FVector2f> BoundsMax;

	Indices.SetNumUninitialized(SegmentCount);
	BoundsMin.SetNumUninitialized(SegmentCount);
	BoundsMax.SetNumUninitialized(SegmentCount);

	CoarseOccupancy.Init(false, CoarseSize * CoarseSize);

	for (int32 Index = 0; Index < SegmentCount; ++Index)
	{
		const FAlgonaWallSegment& Wall = Segments[Index];

		Indices[Index] = Index;
		BoundsMin[Index] = FVector2f::Min(Wall.Start, Wall.End);
		BoundsMax[Index] = FVector2f::Max(Wall.Start, Wall.End);

		const int32 FirstCellX = GetCoarseCell(BoundsMin[Index].X);
		const int32 FirstCellY = GetCoarseCell(BoundsMin[Index].Y);
		const int32 LastCellX = GetCoarseCell(BoundsMax[Index].X);
		const int32 LastCellY = GetCoarseCell(BoundsMax[Index].Y);

		for (int32 CellY = FirstCellY; CellY <= LastCellY; ++CellY)
		{
			for (int32 CellX = FirstCellX; CellX <= LastCellX; ++CellX)
			{
				CoarseOccupancy[GetCoarseBitIndex(CellX, CellY)] = true;
			}
		}
	}

	Grid.RebuildFromBounds(AlgonaWallCellSizeCm, Indices, BoundsMin, BoundsMax);
}

bool FAlgonaWallField::HasSegmentsNearBounds(
	const FVector2f& BoundsMin,
	const FVector2f& BoundsMax) const
{
	if (CoarseOccupancy.IsEmpty())
	{
		return false;
	}

	const int32 FirstCellX = GetCoarseCell(BoundsMin.X);
	const int32 FirstCellY = GetCoarseCell(BoundsMin.Y);

	// Больше периода свёртки проверять нечего: дальше клетки повторяются.
	const int32 LastCellX = FMath::Min(
		GetCoarseCell(BoundsMax.X),
		FirstCellX + CoarseSize - 1);
	const int32 LastCellY = FMath::Min(
		GetCoarseCell(BoundsMax.Y),
		FirstCellY + CoarseSize - 1);

	for (int32 CellY = FirstCellY; CellY <= LastCellY; ++CellY)
	{
		for (int32 CellX = FirstCellX; CellX <= LastCellX; ++CellX)
		{
			if (CoarseOccupancy[GetCoarseBitIndex(CellX, CellY)])
			{
				return true;
			}
		}
	}

	return false;
}
