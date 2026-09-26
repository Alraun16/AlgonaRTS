#include "Spatial/AlgonaTorusGrid.h"

#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	// Корзин примерно столько же, сколько записей (ближайшая степень двойки
	// сверху): проход по корзинам короткий, а в корзине в среднем около
	// одной записи.
	constexpr int32 MinBucketCount = 1024;

	// Минимальная порция элементов на одну задачу рабочего потока.
	constexpr int32 GridMinBatchSize = 2048;
}

void FAlgonaTorusGrid::SetupBuckets(float InCellSize, int32 EntryCount)
{
	CellSize = InCellSize > 0.0f ? InCellSize : 1.0f;

	const int32 BucketCount = static_cast<int32>(FMath::RoundUpToPowerOfTwo(
		static_cast<uint32>(FMath::Max(EntryCount, MinBucketCount))));
	BucketMask = static_cast<uint32>(BucketCount - 1);

	// Тор примерно квадратный: ширина получает на один бит больше при
	// нечётном числе бит (131 072 = 512 × 256 корзин).
	const uint32 BucketBits = FMath::FloorLog2(static_cast<uint32>(BucketCount));
	BucketsXBits = (BucketBits + 1) / 2;
	BucketsXMask = (1u << BucketsXBits) - 1;
	BucketsYMask = (1u << (BucketBits - BucketsXBits)) - 1;

	// Массивы только меняют размер: память между тиками переиспользуется.
	BucketStarts.SetNumUninitialized(BucketCount + 1, EAllowShrinking::No);
	BucketWriteCursors.SetNumUninitialized(BucketCount, EAllowShrinking::No);
	FMemory::Memzero(BucketWriteCursors.GetData(), BucketCount * sizeof(int32));
}

void FAlgonaTorusGrid::RebuildFromPoints(
	float InCellSize,
	TConstArrayView<int32> Indices,
	TConstArrayView<FVector> Positions,
	bool bParallel)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaTorusGrid_RebuildFromPoints);

	const int32 EntryCount = Indices.Num();
	SetupBuckets(InCellSize, EntryCount);

	Entries.SetNumUninitialized(EntryCount, EAllowShrinking::No);
	EntryBuckets.SetNumUninitialized(EntryCount, EAllowShrinking::No);

	// В горячих циклах — обычные указатели: обращение к TArray по индексу
	// в конфигурации Development проверяет границы на каждом элементе,
	// и в простых циклах эти проверки стоят дороже самой работы.
	const int32* IndexData = Indices.GetData();
	const FVector* PositionData = Positions.GetData();
	uint32* EntryBucketData = EntryBuckets.GetData();
	int32* CursorData = BucketWriteCursors.GetData();
	int32* StartData = BucketStarts.GetData();
	int32* EntryData = Entries.GetData();

	// 1. Корзина каждого элемента. Проход не пишет в общие данные, поэтому
	// выполняется в несколько потоков без атомарных операций.
	auto ComputeBucket = [this, IndexData, PositionData, EntryBucketData](int32 EntryIndex)
	{
		const FVector& Position = PositionData[IndexData[EntryIndex]];
		EntryBucketData[EntryIndex] = GetBucketIndex(
			GetCellCoordinate(Position.X),
			GetCellCoordinate(Position.Y));
	};

	if (bParallel)
	{
		ParallelFor(
			TEXT("AlgonaTorusGrid_Buckets"),
			EntryCount,
			GridMinBatchSize,
			ComputeBucket);
	}
	else
	{
		for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
		{
			ComputeBucket(EntryIndex);
		}
	}

	// 2. Число записей в каждой корзине. Дальше всё в одном потоке: без
	// атомарных операций и борьбы потоков за кэш это быстрее.
	for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
	{
		++CursorData[EntryBucketData[EntryIndex]];
	}

	// 3. Префиксные суммы: где в Entries начинается каждая корзина.
	const int32 BucketCount = GetBucketCount();
	int32 RunningStart = 0;

	for (int32 Bucket = 0; Bucket < BucketCount; ++Bucket)
	{
		const int32 BucketSize = CursorData[Bucket];
		StartData[Bucket] = RunningStart;
		CursorData[Bucket] = RunningStart;
		RunningStart += BucketSize;
	}

	StartData[BucketCount] = RunningStart;

	// 4. Раскладка номеров по корзинам в порядке входного списка.
	for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
	{
		EntryData[CursorData[EntryBucketData[EntryIndex]]++] = IndexData[EntryIndex];
	}
}

void FAlgonaTorusGrid::RebuildFromBounds(
	float InCellSize,
	TConstArrayView<int32> Indices,
	TConstArrayView<FVector2f> BoundsMin,
	TConstArrayView<FVector2f> BoundsMax)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaTorusGrid_RebuildFromBounds);

	const int32 ItemCount = Indices.Num();

	// Элемент занимает столько клеток, сколько накрывает его прямоугольник,
	// поэтому записей больше, чем элементов. Считаются они в первом проходе.
	SetupBuckets(InCellSize, ItemCount);

	int32* CursorData = BucketWriteCursors.GetData();
	int32* StartData = BucketStarts.GetData();

	// Клетки элемента: прямоугольник шире тора кладётся во всю его ширину.
	auto GetCellRange = [this, &BoundsMin, &BoundsMax](
		int32 ItemIndex,
		int32& OutFirstCellX,
		int32& OutFirstCellY,
		int32& OutCellCountX,
		int32& OutCellCountY)
	{
		OutFirstCellX = GetCellCoordinate(BoundsMin[ItemIndex].X);
		OutFirstCellY = GetCellCoordinate(BoundsMin[ItemIndex].Y);
		OutCellCountX = FMath::Min(
			GetCellCoordinate(BoundsMax[ItemIndex].X) - OutFirstCellX + 1,
			static_cast<int32>(BucketsXMask) + 1);
		OutCellCountY = FMath::Min(
			GetCellCoordinate(BoundsMax[ItemIndex].Y) - OutFirstCellY + 1,
			static_cast<int32>(BucketsYMask) + 1);
	};

	// 1. Сколько записей попадёт в каждую корзину.
	int32 EntryCount = 0;

	for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
	{
		int32 FirstCellX, FirstCellY, CellCountX, CellCountY;
		GetCellRange(Indices[ItemIndex], FirstCellX, FirstCellY, CellCountX, CellCountY);

		for (int32 OffsetY = 0; OffsetY < CellCountY; ++OffsetY)
		{
			for (int32 OffsetX = 0; OffsetX < CellCountX; ++OffsetX)
			{
				++CursorData[GetBucketIndex(FirstCellX + OffsetX, FirstCellY + OffsetY)];
				++EntryCount;
			}
		}
	}

	Entries.SetNumUninitialized(EntryCount, EAllowShrinking::No);
	int32* EntryData = Entries.GetData();

	// 2. Префиксные суммы.
	const int32 BucketCount = GetBucketCount();
	int32 RunningStart = 0;

	for (int32 Bucket = 0; Bucket < BucketCount; ++Bucket)
	{
		const int32 BucketSize = CursorData[Bucket];
		StartData[Bucket] = RunningStart;
		CursorData[Bucket] = RunningStart;
		RunningStart += BucketSize;
	}

	StartData[BucketCount] = RunningStart;

	// 3. Раскладка: элемент попадает во все свои клетки.
	for (int32 ItemIndex = 0; ItemIndex < ItemCount; ++ItemIndex)
	{
		const int32 Index = Indices[ItemIndex];
		int32 FirstCellX, FirstCellY, CellCountX, CellCountY;
		GetCellRange(Index, FirstCellX, FirstCellY, CellCountX, CellCountY);

		for (int32 OffsetY = 0; OffsetY < CellCountY; ++OffsetY)
		{
			for (int32 OffsetX = 0; OffsetX < CellCountX; ++OffsetX)
			{
				const uint32 Bucket = GetBucketIndex(FirstCellX + OffsetX, FirstCellY + OffsetY);
				EntryData[CursorData[Bucket]++] = Index;
			}
		}
	}
}
