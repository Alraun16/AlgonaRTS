#pragma once

#include "CoreMinimal.h"

/**
 * Мелкая сетка для L3 (локального избегания): быстрый поиск соседей Unit.
 *
 * Перестраивается целиком каждый тик, потому что двигаются все Unit.
 * Таблица корзин — прямоугольник BucketsX × BucketsY (степени двойки,
 * всего примерно по числу Unit), и клетка мира (X, Y) попадает в корзину
 * (X mod BucketsX, Y mod BucketsY): мир сворачивается в тор. Память зависит
 * только от числа Unit, а не от размера карты или разброса армий по ней.
 *
 * Свёртка, а не перемешивающий хеш: соседние клетки мира остаются
 * соседними корзинами, поэтому Unit одного Squad пишут и читают соседние
 * места памяти. Совпадают только клетки, отстоящие ровно на ширину или
 * высоту тора. Это не ошибка: вызывающий код всё равно проверяет
 * настоящее расстояние, просто иногда впустую.
 *
 * Хранение — плоские массивы без выделений на клетку (сортировка подсчётом):
 * Entries содержит номера Unit, сгруппированные по корзинам, а кусок
 * корзины B — это Entries[BucketStarts[B] .. BucketStarts[B + 1]).
 * Раскладка идёт по порядку входного списка, поэтому при списке по
 * возрастанию номеров (как сейчас) номера внутри корзины тоже по
 * возрастанию, и обход одинаков при любом числе потоков.
 */
class ALGONASIMULATION_API FAlgonaLocalAvoidanceGrid
{
public:
	// Клетка размером с интервал строя: в строю обычно один Unit в клетке.
	// Радиус взаимодействия двух обычных Unit (70 см) меньше клетки, поэтому
	// соседа достаточно искать в квадрате 3×3 клеток.
	static constexpr float CellSizeCm = 150.0f;

	/**
	 * Перестраивает сетку. UnitIndices — индексы участвующих Unit
	 * (UnitId - 1) по возрастанию, Positions — позиции всех Unit по этому
	 * индексу.
	 */
	void Rebuild(
		TConstArrayView<int32> UnitIndices,
		TConstArrayView<FVector> Positions,
		bool bParallel);

	/**
	 * Вызывает Visitor(UnitIndex) для каждого Unit в клетке Location и восьми
	 * клетках вокруг. Корзина, в которую попали две из девяти клеток,
	 * обходится один раз. Среди кандидатов могут быть далёкие Unit из
	 * совпавших корзин и сам Unit — расстояние проверяет вызывающий код.
	 */
	template <typename FunctorType>
	void ForEachCandidate(const FVector& Location, FunctorType&& Visitor) const
	{
		if (Entries.IsEmpty())
		{
			return;
		}

		const int32 CenterCellX = GetCellCoordinate(Location.X);
		const int32 CenterCellY = GetCellCoordinate(Location.Y);

		uint32 VisitedBuckets[9];
		int32 VisitedCount = 0;

		for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
		{
			for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
			{
				const uint32 Bucket = GetBucketIndex(
					CenterCellX + OffsetX,
					CenterCellY + OffsetY);

				bool bAlreadyVisited = false;
				for (int32 Index = 0; Index < VisitedCount; ++Index)
				{
					bAlreadyVisited |= VisitedBuckets[Index] == Bucket;
				}

				if (bAlreadyVisited)
				{
					continue;
				}

				VisitedBuckets[VisitedCount++] = Bucket;

				const int32 End = BucketStarts[Bucket + 1];
				for (int32 EntryIndex = BucketStarts[Bucket]; EntryIndex < End; ++EntryIndex)
				{
					Visitor(Entries[EntryIndex]);
				}
			}
		}
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
	static int32 GetCellCoordinate(double WorldCoordinate)
	{
		return FMath::FloorToInt32(WorldCoordinate / CellSizeCm);
	}

	// Свёртка клетки в тор: остаток от деления на степень двойки — побитовое
	// «и». Для отрицательных координат это тоже корректный остаток.
	uint32 GetBucketIndex(int32 CellX, int32 CellY) const
	{
		return ((static_cast<uint32>(CellY) & BucketsYMask) << BucketsXBits)
			| (static_cast<uint32>(CellX) & BucketsXMask);
	}

	// Размеры тора: BucketsX = 2^BucketsXBits, BucketsY = BucketsYMask + 1.
	uint32 BucketsXBits = 0;
	uint32 BucketsXMask = 0;
	uint32 BucketsYMask = 0;
	uint32 BucketMask = 0;

	// Начало куска каждой корзины в Entries; последний элемент — Entries.Num().
	TArray<int32> BucketStarts;

	// Номера Unit, сгруппированные по корзинам.
	TArray<int32> Entries;

	// Рабочие массивы построения, переиспользуются между тиками.
	TArray<uint32> EntryBuckets;
	TArray<int32> BucketWriteCursors;
};
