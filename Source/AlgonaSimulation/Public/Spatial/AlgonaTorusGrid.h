#pragma once

#include "CoreMinimal.h"

/**
 * Равномерная сетка на плоскости, свёрнутая в тор: быстрый поиск соседей.
 *
 * Перестраивается целиком каждый тик — в проекте любой Unit и любой Squad
 * всегда потенциально подвижен, поэтому точечное обновление не окупается.
 *
 * Таблица корзин — прямоугольник BucketsX × BucketsY (степени двойки, всего
 * примерно по числу элементов), и клетка мира (X, Y) попадает в корзину
 * (X mod BucketsX, Y mod BucketsY): мир сворачивается в тор. Память зависит
 * только от числа элементов, а не от размера карты или разброса армий.
 *
 * Свёртка, а не перемешивающий хеш: соседние клетки мира остаются соседними
 * корзинами, поэтому раскладка и поиск идут по соседним местам памяти.
 * Совпадают только клетки, отстоящие ровно на ширину или высоту тора. Это не
 * ошибка: вызывающий код всё равно проверяет настоящее расстояние.
 *
 * Хранение — плоские массивы без выделений на клетку (сортировка подсчётом):
 * Entries содержит номера элементов, сгруппированные по корзинам, а кусок
 * корзины B — это Entries[BucketStarts[B] .. BucketStarts[B + 1]).
 * Раскладка идёт по порядку входного списка, поэтому при списке по
 * возрастанию номера внутри корзины тоже по возрастанию, и обход одинаков
 * при любом числе потоков.
 *
 * Одна и та же сетка используется для Unit (клетка около интервала строя)
 * и для Squad (крупная клетка, элементы кладутся по прямоугольнику).
 */
class ALGONASIMULATION_API FAlgonaTorusGrid
{
public:
	/**
	 * Перестраивает сетку по точкам. Indices — номера участвующих элементов
	 * по возрастанию, Positions — позиции всех элементов по этому номеру.
	 */
	/**
	 * Radii (может быть пустым) — радиусы элементов. Элемент крупнее
	 * половины клетки кладётся во все клетки, которые накрывает: иначе
	 * сосед с обычным радиусом не нашёл бы его поиском вокруг себя.
	 * Мелкие элементы по-прежнему занимают ровно одну клетку.
	 */
	void RebuildFromPoints(
		float InCellSize,
		TConstArrayView<int32> Indices,
		TConstArrayView<FVector> Positions,
		TConstArrayView<float> Radii,
		bool bParallel);

	/**
	 * Перестраивает сетку по прямоугольникам: элемент кладётся во все клетки,
	 * которые накрывает. Так крупный элемент находится поиском вокруг точки,
	 * даже если его центр далеко.
	 */
	void RebuildFromBounds(
		float InCellSize,
		TConstArrayView<int32> Indices,
		TConstArrayView<FVector2f> BoundsMin,
		TConstArrayView<FVector2f> BoundsMax);

	/**
	 * Вызывает Visitor(Index) для каждого элемента в квадрате клеток
	 * (2 * Rings + 1)² вокруг Location: Rings = 1 — 3×3 (гарантированно видны
	 * все в пределах клетки), Rings = 2 — 5×5 (в пределах двух клеток).
	 * Среди кандидатов бывают далёкие элементы из совпавших корзин тора и сам
	 * элемент — расстояние проверяет вызывающий код.
	 *
	 * Клетки одной строки квадрата — соседние корзины, поэтому строка
	 * обходится одним сплошным куском Entries (или двумя, если переходит
	 * через край тора). Тор не меньше 32 × 32 корзин, так что клетки квадрата
	 * не совпадают друг с другом.
	 */
	template <typename FunctorType>
	void ForEachCandidate(const FVector& Location, int32 Rings, FunctorType&& Visitor) const
	{
		if (Entries.IsEmpty())
		{
			return;
		}

		ForEachCellRange(
			GetCellCoordinate(Location.X) - Rings,
			GetCellCoordinate(Location.Y) - Rings,
			2 * Rings + 1,
			2 * Rings + 1,
			Visitor);
	}

	/** То же для прямоугольника мира: все элементы в накрытых им клетках. */
	template <typename FunctorType>
	void ForEachCandidateInBounds(
		const FVector2f& BoundsMin,
		const FVector2f& BoundsMax,
		FunctorType&& Visitor) const
	{
		if (Entries.IsEmpty())
		{
			return;
		}

		const int32 FirstCellX = GetCellCoordinate(BoundsMin.X);
		const int32 FirstCellY = GetCellCoordinate(BoundsMin.Y);

		ForEachCellRange(
			FirstCellX,
			FirstCellY,
			GetCellCoordinate(BoundsMax.X) - FirstCellX + 1,
			GetCellCoordinate(BoundsMax.Y) - FirstCellY + 1,
			Visitor);
	}

	/** Освобождает содержимое: сетка становится пустой. */
	void Reset()
	{
		Entries.Reset();
	}

	int32 GetEntryCount() const
	{
		return Entries.Num();
	}

	int32 GetBucketCount() const
	{
		return static_cast<int32>(BucketMask) + 1;
	}

private:
	int32 GetCellCoordinate(double WorldCoordinate) const
	{
		return FMath::FloorToInt32(WorldCoordinate / static_cast<double>(CellSize));
	}

	// Свёртка клетки в тор: остаток от деления на степень двойки — побитовое
	// «и». Для отрицательных координат это тоже корректный остаток.
	uint32 GetBucketIndex(int32 CellX, int32 CellY) const
	{
		return ((static_cast<uint32>(CellY) & BucketsYMask) << BucketsXBits)
			| (static_cast<uint32>(CellX) & BucketsXMask);
	}

	// Обход прямоугольника клеток: строка за строкой, каждая — одним или
	// двумя сплошными кусками Entries.
	template <typename FunctorType>
	void ForEachCellRange(
		int32 FirstCellX,
		int32 FirstCellY,
		int32 CellCountX,
		int32 CellCountY,
		FunctorType& Visitor) const
	{
		const uint32 RowCellCount = static_cast<uint32>(
			FMath::Min(CellCountX, static_cast<int32>(BucketsXMask) + 1));
		const int32 RowCount =
			FMath::Min(CellCountY, static_cast<int32>(BucketsYMask) + 1);
		const uint32 FirstColumn = static_cast<uint32>(FirstCellX) & BucketsXMask;

		const int32* StartData = BucketStarts.GetData();
		const int32* EntryData = Entries.GetData();

		auto VisitBucketRange = [StartData, EntryData, &Visitor](uint32 FirstBucket, uint32 BucketCount)
		{
			const int32 End = StartData[FirstBucket + BucketCount];
			for (int32 EntryIndex = StartData[FirstBucket]; EntryIndex < End; ++EntryIndex)
			{
				Visitor(EntryData[EntryIndex]);
			}
		};

		for (int32 RowOffset = 0; RowOffset < RowCount; ++RowOffset)
		{
			const uint32 RowStart =
				(static_cast<uint32>(FirstCellY + RowOffset) & BucketsYMask) << BucketsXBits;

			if (FirstColumn + RowCellCount - 1 <= BucketsXMask)
			{
				VisitBucketRange(RowStart + FirstColumn, RowCellCount);
			}
			else
			{
				// Строка переходит через край тора: два куска.
				const uint32 FirstPart = BucketsXMask + 1 - FirstColumn;
				VisitBucketRange(RowStart + FirstColumn, FirstPart);
				VisitBucketRange(RowStart, RowCellCount - FirstPart);
			}
		}
	}

	// Готовит таблицу корзин под ожидаемое число записей.
	void SetupBuckets(float InCellSize, int32 EntryCount);

	float CellSize = 100.0f;

	// Размеры тора: BucketsX = 2^BucketsXBits, BucketsY = BucketsYMask + 1.
	uint32 BucketsXBits = 0;
	uint32 BucketsXMask = 0;
	uint32 BucketsYMask = 0;
	uint32 BucketMask = 0;

	// Начало куска каждой корзины в Entries; последний элемент — Entries.Num().
	TArray<int32> BucketStarts;

	// Номера элементов, сгруппированные по корзинам.
	TArray<int32> Entries;

	// Рабочие массивы построения, переиспользуются между тиками.
	TArray<uint32> EntryBuckets;
	TArray<int32> BucketWriteCursors;
};
