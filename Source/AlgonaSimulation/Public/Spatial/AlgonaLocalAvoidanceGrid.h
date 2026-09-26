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
	 * Вызывает Visitor(UnitIndex) для каждого Unit в квадрате клеток
	 * (2 * Rings + 1)² вокруг Location: Rings = 1 — 3×3 (гарантированно видны
	 * все в 150 см), Rings = 2 — 5×5 (в 300 см). Среди кандидатов бывают
	 * далёкие Unit из совпавших корзин тора и сам Unit — расстояние проверяет
	 * вызывающий код.
	 *
	 * Клетки одной строки квадрата — соседние корзины, поэтому строка
	 * обходится одним сплошным куском Entries (или двумя, если переходит
	 * через край тора). Тор не меньше 32 × 32 корзин, так что клетки
	 * квадрата никогда не совпадают друг с другом.
	 */
	template <typename FunctorType>
	void ForEachCandidate(const FVector& Location, int32 Rings, FunctorType&& Visitor) const
	{
		if (Entries.IsEmpty())
		{
			return;
		}

		const int32 CenterCellX = GetCellCoordinate(Location.X);
		const int32 CenterCellY = GetCellCoordinate(Location.Y);
		const uint32 RowCellCount = static_cast<uint32>(2 * Rings + 1);
		const uint32 FirstColumn = static_cast<uint32>(CenterCellX - Rings) & BucketsXMask;

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

		for (int32 OffsetY = -Rings; OffsetY <= Rings; ++OffsetY)
		{
			const uint32 RowStart =
				(static_cast<uint32>(CenterCellY + OffsetY) & BucketsYMask) << BucketsXBits;

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
