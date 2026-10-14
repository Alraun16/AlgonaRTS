/**
 * Статические препятствия для Unit (P2, шаг 17).
 *
 * Стены берутся из границы проходимой зоны навмеша — ровно тех линий,
 * которыми редактор рисует край навигационной карты. Recast при построении
 * уже сузил проходимую зону на радиус агента, поэтому эта линия и есть
 * место, где центр Unit останавливается у стены: обычному Unit запас не
 * нужен вовсе, крупному нужен ровно на столько, насколько он шире сужения.
 *
 * Запекание одноразовое: навмеш статичен. Пересчёт — по сигналу навигации о
 * законченном построении и командой algona.P2.RebuildWalls.
 *
 * Источник — полигоны навмеша, а не готовый список рёбер плитки: у каждого
 * полигона берутся его рёбра-стены (`GetPolyWallSegments`, рёбра без
 * соседа), и нормаль каждого отрезка смотрит в сторону центра полигона,
 * которому он принадлежит. Полигоны навмеша выпуклые, поэтому их центр
 * всегда с внутренней стороны каждого ребра — сторона проходимости
 * получается точной для каждого отрезка в отдельности, без догадок о
 * порядке вершин.
 *
 * Прежний способ (рёбра плитки через `GetEdgesInTile`, нормаль по порядку
 * вершин, соглашение голосованием по выборке) отклонён: рёбра плитки — это
 * рёбра детальной сетки, и порядок их вершин не обязан совпадать от отрезка
 * к отрезку. Голосование ловило только общий переворот; отдельные
 * перевёрнутые отрезки затягивали Unit в стену.
 */

#include "Core/AlgonaSimulationSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/RecastNavMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	TAutoConsoleVariable<int32> CVarAlgonaP2Walls(
		TEXT("algona.P2.Walls"),
		1,
		TEXT("1 = units treat the navmesh boundary as impassable walls."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2WallMargin(
		TEXT("algona.P2.WallMargin"),
		0.0f,
		TEXT("Extra clearance from walls on top of the geometric one, cm. ")
		TEXT("Compensates the voxel precision of the navmesh boundary; ")
		TEXT("negative values let units stand closer than the navmesh border."),
		ECVF_Default);

	// Насколько запас разрешено уводить в минус, см. Recast сужает проходимую
	// зону на целое число клеток (при клетке 50 см и радиусе агента 35 это
	// 50 см, а не 35), поэтому Unit встаёт на полтора десятка сантиметров
	// дальше от стены, чем касается её телом. Минус позволяет это убрать,
	// но не больше клетки — дальше Unit полез бы в саму геометрию.
	constexpr float MinWallMarginCm = -50.0f;

	// Короче этого отрезок границы не несёт информации, см.
	constexpr float MinWallSegmentLengthCm = 1.0f;

	// Пересчёт стен вручную: после правки геометрии уровня в редакторе,
	// когда навигация пересобралась без сигнала.
	FAutoConsoleCommandWithWorld GRebuildWallsCommand(
		TEXT("algona.P2.RebuildWalls"),
		TEXT("Rebake unit walls from the navmesh boundary."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UAlgonaSimulationSubsystem* Simulation =
				World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr)
			{
				Simulation->RebuildWallField();
			}
		}));
}

bool UAlgonaSimulationSubsystem::AreWallsEnabled()
{
	return CVarAlgonaP2Walls.GetValueOnGameThread() != 0;
}

float UAlgonaSimulationSubsystem::GetWallExtraMargin()
{
	return FMath::Max(
		CVarAlgonaP2WallMargin.GetValueOnGameThread(),
		MinWallMarginCm);
}

void UAlgonaSimulationSubsystem::OnNavigationGenerationFinished(ANavigationData* NavData)
{
	(void)NavData;

	// Навмеш пересобран: запечённые стены устарели.
	RebuildWallField();
}

void UAlgonaSimulationSubsystem::RebuildWallField()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_RebuildWallField);

	const double StartSeconds = FPlatformTime::Seconds();

	WallField.Reset();
	WallAgentRadiusCm = 0.0f;
	WallCellSizeCm = 0.0f;

	UWorld* World = GetWorld();
	UNavigationSystemV1* NavigationSystem = UNavigationSystemV1::GetCurrent(World);

	if (!NavigationSystem)
	{
		return;
	}

	// Стены для Unit — граница карты обычного агента: именно она сужена на
	// радиус Unit. Ступени Squad для этого не годятся.
	const ANavigationData* NavData = NavigationSystem->GetDefaultNavDataInstance();
	const ARecastNavMesh* RecastNavMesh = Cast<ARecastNavMesh>(NavData);

	if (!RecastNavMesh)
	{
		// Навигация ещё не готова: признак «запечено» не ставится, и попытка
		// повторится на следующем шаге.
		return;
	}

	bWallFieldBuilt = true;

	// Фактическое сужение, а не объявленный радиус агента: Recast сужает
	// проходимую зону на целое число клеток
	// (walkableRadius = ceil(AgentRadius / CellSize)), поэтому при клетке
	// 50 см и радиусе 35 граница отстоит от стены на 50 см, а не на 35.
	// Запас Unit считается от этого числа, иначе между телом и камнем
	// остаётся воздух.
	WallCellSizeCm = FMath::Max(
		RecastNavMesh->GetCellSize(ENavigationDataResolution::Default),
		UE_KINDA_SMALL_NUMBER);
	WallAgentRadiusCm = FMath::CeilToFloat(
		NavData->GetConfig().AgentRadius / WallCellSizeCm) * WallCellSizeCm;

	// Пересборка навмеша делает запечённые стены неверными. Подписка
	// одноразовая: делегат динамический, поэтому живёт слабой ссылкой и
	// при выгрузке мира отвязывается сам.
	if (!bWallRebuildHookBound)
	{
		bWallRebuildHookBound = true;
		NavigationSystem->OnNavigationGenerationFinishedDelegate.AddDynamic(
			this,
			&UAlgonaSimulationSubsystem::OnNavigationGenerationFinished);
	}

	TArray<FNavTileRef> TileRefs;
	RecastNavMesh->GetAllNavMeshTiles(TileRefs);

	TArray<FAlgonaWallSegment> Segments;
	TArray<FNavPoly> TilePolys;
	TArray<FNavigationPortalEdge> PolyEdges;
	int32 TilesWithData = 0;
	int32 PolyCount = 0;

	for (const FNavTileRef TileRef : TileRefs)
	{
		// GetAllNavMeshTiles отдаёт ссылки и на пустые слоты пула плиток (у
		// них есть salt, но нет данных). GetPolysInTile заголовок плитки
		// проверяет и для пустого слота ничего не возвращает.
		TilePolys.Reset();
		RecastNavMesh->GetPolysInTile(TileRef, TilePolys);

		if (TilePolys.IsEmpty())
		{
			continue;
		}

		++TilesWithData;
		PolyCount += TilePolys.Num();

		for (const FNavPoly& Poly : TilePolys)
		{
			// Рёбра полигона: у стены нет соседа (ToRef == 0), остальное —
			// проходы в соседние полигоны. Частично прикрытое соседом ребро
			// на стыке плиток движок сам режет на проход и стену.
			PolyEdges.Reset();
			RecastNavMesh->GetPolyWallSegments(Poly.Ref, nullptr, nullptr, PolyEdges);

			const FVector2f PolyCenter(
				static_cast<float>(Poly.Center.X),
				static_cast<float>(Poly.Center.Y));

			for (const FNavigationPortalEdge& Edge : PolyEdges)
			{
				if (Edge.ToRef != INVALID_NAVNODEREF)
				{
					continue;
				}

				const FVector2f Start(
					static_cast<float>(Edge.Left.X),
					static_cast<float>(Edge.Left.Y));
				const FVector2f End(
					static_cast<float>(Edge.Right.X),
					static_cast<float>(Edge.Right.Y));

				const FVector2f Direction = End - Start;
				const float Length = Direction.Size();

				if (Length < MinWallSegmentLengthCm)
				{
					continue;
				}

				FAlgonaWallSegment& Wall = Segments.AddDefaulted_GetRef();
				Wall.Start = Start;
				Wall.End = End;

				// Нормаль — в сторону центра своего полигона.
				const FVector2f Tangent = Direction / Length;
				FVector2f Normal(-Tangent.Y, Tangent.X);

				if (FVector2f::DotProduct(PolyCenter - Start, Normal) < 0.0f)
				{
					Normal = -Normal;
				}

				Wall.Normal = Normal;
				Wall.MinZ = static_cast<float>(FMath::Min(Edge.Left.Z, Edge.Right.Z));
				Wall.MaxZ = static_cast<float>(FMath::Max(Edge.Left.Z, Edge.Right.Z));
			}
		}
	}

	if (Segments.IsEmpty())
	{
		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("[P2 Walls] navmesh has no boundary edges: units have no static obstacles."));
		return;
	}

	const int32 SegmentCount = Segments.Num();
	WallField.Build(MoveTemp(Segments));

	UE_LOG(
		LogAlgonaSimulation,
		Display,
		TEXT("[P2 Walls] baked %d boundary segments from %d polygons in %d navmesh tiles in %.1f ms ")
		TEXT("(eroded by %.0f cm at cell %.0f cm)."),
		SegmentCount,
		PolyCount,
		TilesWithData,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0,
		WallAgentRadiusCm,
		WallCellSizeCm);
}
