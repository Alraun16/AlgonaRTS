#pragma once

#include "CoreMinimal.h"
#include "Spatial/AlgonaTorusGrid.h"

/**
 * Стены для Unit — статические препятствия локального уровня (P2, шаг 17).
 *
 * Источник данных — граница проходимой зоны навмеша: ровно те линии, которыми
 * редактор рисует край навигационной карты. Recast при построении уже сузил
 * проходимую зону на радиус агента, поэтому эта линия — то место, где должен
 * остановиться центр Unit, касающийся стены плечом. Запас считать не нужно:
 * для обычного Unit он нулевой, для крупного равен его превышению над
 * радиусом агента.
 *
 * Отрезки запекаются один раз (навмеш статичен) и кладутся в ту же
 * FAlgonaTorusGrid, что соседи и Squad, поэтому Unit ищет стены тем же
 * поиском по клеткам.
 */
struct FAlgonaWallSegment
{
	FVector2f Start = FVector2f::ZeroVector;
	FVector2f End = FVector2f::ZeroVector;

	// Единичная нормаль в проходимую сторону. Берётся не из порядка вершин,
	// а от центра полигона навмеша, которому принадлежит отрезок: полигоны
	// выпуклые, поэтому их центр всегда с внутренней стороны каждого ребра.
	// Без нормали направление вырождается — в покое Unit стоит ровно на линии.
	FVector2f Normal = FVector2f::ZeroVector;

	// Высота отрезка: на многоуровневой карте стена моста не мешает тому,
	// кто идёт под мостом.
	float MinZ = 0.0f;
	float MaxZ = 0.0f;
};

/** Размер клетки сетки стен, см: один поиск 3×3 видит всё в пределах 300 см. */
inline constexpr float AlgonaWallCellSizeCm = 300.0f;

/**
 * Насколько глубоко за линией отрезка Unit ещё считается «у этой стены» в
 * обходе и скольжении (намерение), см. Зажим этим числом не пользуется:
 * там решает ближайший элемент границы.
 */
inline constexpr float AlgonaWallMaxDepthCm = 150.0f;

/** Разница высот, при которой стена ещё считается своей для Unit, см. */
inline constexpr float AlgonaWallHeightToleranceCm = 200.0f;

/**
 * Радиус, в котором Unit (или его слот) вообще видит стены, см. Равен клетке
 * сетки: поиск 3×3 гарантированно находит всё в этих пределах.
 */
inline constexpr float AlgonaWallQueryRadiusCm = AlgonaWallCellSizeCm;

/** Квадрат настоящего расстояния от точки до отрезка (не до его линии). */
inline float GetAlgonaWallDistanceSquared(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Point)
{
	const FVector2f Direction = Wall.End - Wall.Start;
	const float LengthSquared = Direction.SizeSquared();

	const float Along = LengthSquared > UE_SMALL_NUMBER
		? FMath::Clamp(
			FVector2f::DotProduct(Point - Wall.Start, Direction) / LengthSquared,
			0.0f,
			1.0f)
		: 0.0f;

	return (Point - (Wall.Start + Direction * Along)).SizeSquared();
}

/**
 * Ближайший к точке элемент границы проходимой зоны.
 *
 * Решает только он: за какой стороной границы точка и куда её вернуть.
 * Складывать поправки от всех отрезков, за линией которых оказалась точка,
 * нельзя — длинная линия чужого отрезка «захватывает» точку издалека, а
 * на мелких скосах углов остаются места, которые не захватывает никто.
 *
 * Если ближайшая точка — общая вершина двух отрезков, сторона решается по
 * их средней нормали: у вершины нормаль одного отрезка врёт.
 */
struct FAlgonaWallNearest
{
	FVector2f ClosestPoint = FVector2f::ZeroVector;

	// Сумма нормалей отрезков, сходящихся в ближайшей точке.
	FVector2f NormalSum = FVector2f::ZeroVector;

	float Distance = TNumericLimits<float>::Max();
	bool bFound = false;
};

/** Учитывает очередной отрезок-кандидат в поиске ближайшего элемента. */
ALGONASIMULATION_API void AccumulateAlgonaNearestWall(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Point,
	FAlgonaWallNearest& InOutNearest);

/**
 * Куда и насколько сдвинуть точку по ближайшему элементу границы, чтобы она
 * лежала в проходимой стороне не ближе Margin. Нулевой вектор — нарушения нет.
 *
 * Перед границей выталкивание радиальное, от ближайшей точки: в проходе
 * Unit расходится с обоими краями, у торца стены обходит угол. За границей —
 * возврат к ближайшей точке границы.
 *
 * Отрицательный Margin — предел, а не цель: точке разрешено зайти за
 * границу не глубже |Margin|, но туда её никто не тянет.
 */
ALGONASIMULATION_API FVector2f GetAlgonaNearestWallCorrection(
	const FAlgonaWallNearest& Nearest,
	const FVector2f& Point,
	float Margin);

/** Расстояние от точки до линии стены по нормали: больше нуля — впереди неё. */
inline float GetAlgonaWallSignedOffset(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Point)
{
	return FVector2f::DotProduct(Point - Wall.Start, Wall.Normal);
}

/**
 * Точка напротив отрезка: её проекция попадает на сам отрезок (с допуском
 * Tolerance), а не за его край. За краем стеной занимается соседний отрезок,
 * а Unit свободно обходит угол.
 */
ALGONASIMULATION_API bool IsAlgonaWallFacingPoint(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Point,
	float Tolerance);

/**
 * Отрезок стены на поперечной оси Unit: занятый интервал [Low, High] и
 * расстояние, через которое Unit до него дойдёт.
 *
 * Стена обрезается по ходу Unit до полосы [0, MaxForward]: стена сбоку
 * занимает интервал в стороне и не мешает, а стена поперёк пути перекрывает
 * ноль оси. Так тот же выбор щели, что между соседями, работает и у стен.
 * false — в полосе стены нет.
 */
ALGONASIMULATION_API bool GetAlgonaWallLateralSpan(
	const FAlgonaWallSegment& Wall,
	const FVector2f& Position,
	const FVector2f& Forward,
	const FVector2f& Right,
	float MaxForward,
	float Inflate,
	float& OutLow,
	float& OutHigh,
	float& OutForwardDistance);

/** Запечённые стены и сетка поиска по ним. */
class ALGONASIMULATION_API FAlgonaWallField
{
public:
	/** Принимает готовый список отрезков и строит по нему сетку. */
	void Build(TArray<FAlgonaWallSegment>&& InSegments);

	void Reset()
	{
		Segments.Reset();
		Grid.Reset();
	}

	bool IsEmpty() const
	{
		return Segments.IsEmpty();
	}

	int32 GetSegmentCount() const
	{
		return Segments.Num();
	}

	/** Все отрезки — только для отладочной отрисовки. */
	TConstArrayView<FAlgonaWallSegment> GetSegments() const
	{
		return Segments;
	}

	/**
	 * Вызывает Visitor(Segment) для каждого отрезка не дальше Radius от
	 * Location (настоящее расстояние до отрезка) и на её высоте.
	 *
	 * Другого доступа к отрезкам по месту нет намеренно. Сетка свёрнута в
	 * тор, и среди её кандидатов всегда есть отрезки с другого конца карты,
	 * попавшие в те же корзины. Модель «решает ближайший элемент» без этой
	 * проверки переносила Unit из чистого поля в стену за сотни метров.
	 */
	template <typename FunctorType>
	void ForEachSegmentNear(
		const FVector& Location,
		float Radius,
		FunctorType&& Visitor) const
	{
		const FVector2f Point(
			static_cast<float>(Location.X),
			static_cast<float>(Location.Y));
		const float Height = static_cast<float>(Location.Z);
		const float RadiusSquared = Radius * Radius;

		// Колец столько, чтобы поиск гарантированно накрыл Radius.
		const int32 Rings = FMath::Max(
			1,
			FMath::CeilToInt32(Radius / AlgonaWallCellSizeCm));

		Grid.ForEachCandidate(Location, Rings, [&](int32 Index)
		{
			const FAlgonaWallSegment& Wall = Segments[Index];

			if (Height < Wall.MinZ - AlgonaWallHeightToleranceCm
				|| Height > Wall.MaxZ + AlgonaWallHeightToleranceCm
				|| GetAlgonaWallDistanceSquared(Wall, Point) > RadiusSquared)
			{
				return;
			}

			Visitor(Wall);
		});
	}

	/**
	 * Есть ли стены рядом с прямоугольником. Грубая проверка по редкой
	 * решётке занятости: ею Squad один раз за тик решает, платить ли его
	 * Unit за стены вообще. Ложное «да» безвредно, ложного «нет» не бывает.
	 */
	bool HasSegmentsNearBounds(const FVector2f& BoundsMin, const FVector2f& BoundsMax) const;

private:
	// Решётка занятости: клетка 20 м, таблица 256 × 256 бит (8 КБ).
	// Свёрнута в тор, как и основная сетка, но её период — 5.1 км, поэтому
	// на картах такого размера совпадений клеток нет вовсе.
	static constexpr float CoarseCellSizeCm = 2000.0f;
	static constexpr int32 CoarseBits = 8;
	static constexpr int32 CoarseSize = 1 << CoarseBits;

	static int32 GetCoarseBitIndex(int32 CellX, int32 CellY)
	{
		constexpr uint32 CoarseMask = static_cast<uint32>(CoarseSize) - 1;

		return static_cast<int32>(
			((static_cast<uint32>(CellY) & CoarseMask) << CoarseBits)
			| (static_cast<uint32>(CellX) & CoarseMask));
	}

	static int32 GetCoarseCell(float WorldCoordinate)
	{
		return FMath::FloorToInt32(WorldCoordinate / CoarseCellSizeCm);
	}

	TArray<FAlgonaWallSegment> Segments;
	FAlgonaTorusGrid Grid;
	TBitArray<> CoarseOccupancy;
};
