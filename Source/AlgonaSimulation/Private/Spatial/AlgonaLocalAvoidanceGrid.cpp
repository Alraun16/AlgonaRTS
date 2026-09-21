#include "Spatial/AlgonaLocalAvoidanceGrid.h"

#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	// Корзин примерно столько же, сколько Unit (ближайшая степень двойки
	// сверху): проход по корзинам короткий, а в корзине в среднем около
	// одного Unit.
	constexpr int32 MinBucketCount = 1024;

	// Минимальная порция Unit на одну задачу рабочего потока.
	constexpr int32 GridMinBatchSize = 2048;
}

void FAlgonaLocalAvoidanceGrid::Rebuild(
	TConstArrayView<int32> UnitIndices,
	TConstArrayView<FVector> Positions,
	bool bParallel)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_BuildLocalAvoidanceGrid);

	const int32 EntryCount = UnitIndices.Num();

	const int32 BucketCount = static_cast<int32>(FMath::RoundUpToPowerOfTwo(
		static_cast<uint32>(FMath::Max(EntryCount, MinBucketCount))));
	BucketMask = static_cast<uint32>(BucketCount - 1);

	// Тор примерно квадратный: ширина получает на один бит больше при
	// нечётном числе бит (131 072 = 512 × 256 корзин, при клетке 150 см
	// это 768 × 384 м).
	const uint32 BucketBits = FMath::FloorLog2(static_cast<uint32>(BucketCount));
	BucketsXBits = (BucketBits + 1) / 2;
	BucketsXMask = (1u << BucketsXBits) - 1;
	BucketsYMask = (1u << (BucketBits - BucketsXBits)) - 1;

	// Массивы только меняют размер: память между тиками переиспользуется.
	BucketStarts.SetNumUninitialized(BucketCount + 1, EAllowShrinking::No);
	BucketWriteCursors.SetNumUninitialized(BucketCount, EAllowShrinking::No);
	Entries.SetNumUninitialized(EntryCount, EAllowShrinking::No);
	EntryBuckets.SetNumUninitialized(EntryCount, EAllowShrinking::No);

	// В горячих циклах — обычные указатели: обращение к TArray по индексу
	// в конфигурации Development проверяет границы на каждом элементе,
	// и в простых циклах эти проверки стоят дороже самой работы.
	const int32* UnitIndexData = UnitIndices.GetData();
	const FVector* PositionData = Positions.GetData();
	uint32* EntryBucketData = EntryBuckets.GetData();
	int32* CursorData = BucketWriteCursors.GetData();
	int32* StartData = BucketStarts.GetData();
	int32* EntryData = Entries.GetData();

	// 1. Корзина каждого Unit. Проход не пишет в общие данные, поэтому
	// выполняется в несколько потоков без атомарных операций.
	auto ComputeBucket = [this, UnitIndexData, PositionData, EntryBucketData](int32 EntryIndex)
	{
		const FVector& Position = PositionData[UnitIndexData[EntryIndex]];
		EntryBucketData[EntryIndex] = GetBucketIndex(
			GetCellCoordinate(Position.X),
			GetCellCoordinate(Position.Y));
	};

	if (bParallel)
	{
		ParallelFor(
			TEXT("AlgonaSimulation_LocalAvoidanceGrid"),
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

	// 2. Число Unit в каждой корзине. Дальше всё в одном потоке: без
	// атомарных операций и борьбы потоков за кэш это быстрее.
	FMemory::Memzero(CursorData, BucketCount * sizeof(int32));

	for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
	{
		++CursorData[EntryBucketData[EntryIndex]];
	}

	// 3. Префиксные суммы: где в Entries начинается каждая корзина.
	int32 RunningStart = 0;

	for (int32 Bucket = 0; Bucket < BucketCount; ++Bucket)
	{
		const int32 BucketSize = CursorData[Bucket];
		StartData[Bucket] = RunningStart;
		CursorData[Bucket] = RunningStart;
		RunningStart += BucketSize;
	}

	StartData[BucketCount] = RunningStart;

	// 4. Раскладка номеров Unit по корзинам по порядку входного списка.
	// Внутри корзины номера сразу идут по возрастанию, поэтому обход
	// соседей одинаков при любом числе потоков и отдельная сортировка
	// не нужна.
	for (int32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
	{
		EntryData[CursorData[EntryBucketData[EntryIndex]]++] = UnitIndexData[EntryIndex];
	}
}
