#include "Core/AlgonaSimulationSubsystem.h"

#include "Army/AlgonaSquadPath.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "NavMesh/NavMeshPath.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

/*
 * Путь центра Squad по навигации UE (L1, шаг 16).
 *
 * Приказ движения ставит Squad в очередь запросов и сразу пускает его по
 * прямой к цели: приказ не ждёт навигации. За тик считается не больше
 * algona.P2.PathQueriesPerTick путей, поэтому групповой приказ на тысячи
 * Squad не делает один шаг симуляции тяжёлым.
 *
 * Запрос синхронный: NavMesh уже построен, запрос только ищет по нему путь,
 * и делать это на рабочих потоках навигация UE не даёт.
 *
 * Недостижимая цель приказ не выполняет вовсе: идти в ближайшее доступное
 * место значит придумать за игрока другую цель. То же при неудаче запроса
 * (Squad не стоит на карте проходимости) — но сперва запрос повторяется от
 * ближайшей проходимой точки: Squad могли вытолкнуть к стене свои же Unit.
 *
 * Запас от стен даёт не постобработка пути, а сама карта проходимости:
 * Recast при построении сужает проходимую зону на радиус агента, поэтому
 * путь по такой карте держит запас и точками, и отрезками, без единой
 * пробы.
 *
 * Карта на каждый Squad невозможна (каждая — полная перестройка Recast по
 * уровню) и не нужна: точную ширину строя разбирают препятствия для Unit
 * (шаг 17) и проход очередью (шаг 19), а карта даёт лишь запас «не скрестись
 * о стену, когда есть место». Поэтому ступеней несколько: Squad берёт самого
 * широкого объявленного агента, который не шире его самого. Список ступеней
 * читается из SupportedAgents (Config/DefaultEngine.ini) — добавленная туда
 * строка подхватывается без правок кода.
 *
 * Запас — это предпочтение, а не ограничение маршрута: по ГД узкий коридор
 * нельзя запрещать только потому, что он уже строя. Поэтому считаются оба
 * пути, по обычной карте и по широкой, и широкий берётся, только если он не
 * длиннее короткого больше чем на algona.P2.PathWideDetour.
 *
 * Запас на коротком пути (по обычной карте) даёт штатный
 * FNavMeshPath::OffsetFromCorners: он сдвигает точки, прижатые к углам,
 * вдоль ребра коридора на половину ребра, но не больше запрошенного. В
 * узких воротах это ровно их середина, точки в чистом поле не трогаются,
 * а второй проход по видимости выкидывает ставшие лишними точки.
 */

namespace
{
	TAutoConsoleVariable<int32> CVarAlgonaP2Path(
		TEXT("algona.P2.Path"),
		1,
		TEXT("1 = squad centers follow navigation paths, 0 = straight lines to the target."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP2PathQueriesPerTick(
		TEXT("algona.P2.PathQueriesPerTick"),
		64,
		TEXT("Max navigation path queries per simulation step. Squads waiting in the queue move straight."),
		ECVF_Default);

	// Верхний предел ступени карты проходимости, см: агенты шире этого не
	// берутся, даже если строй шире. 0 — ступени выключены, все Squad
	// ходят по обычной карте.
	TAutoConsoleVariable<float> CVarAlgonaP2PathAgentRadius(
		TEXT("algona.P2.PathAgentRadius"),
		2000.0f,
		TEXT("Upper limit in cm for the navigation agent a squad may use. 0 keeps every squad on the unit-sized navmesh."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP2PathDebug(
		TEXT("algona.P2.PathDebug"),
		0,
		TEXT("1 = log every built path: points, required clearance, measured side clearance and applied shift."),
		ECVF_Default);

	// Во сколько раз путь с запасом от стен может быть длиннее короткого,
	// чтобы его всё же предпочесть. 1 — всегда кратчайший путь.
	TAutoConsoleVariable<float> CVarAlgonaP2PathWideDetour(
		TEXT("algona.P2.PathWideDetour"),
		1.25f,
		TEXT("How much longer the path with wall clearance may be than the shortest one to still be preferred. 1 = always the shortest."),
		ECVF_Default);

	TAutoConsoleVariable<float> CVarAlgonaP2PathLookahead(
		TEXT("algona.P2.PathLookahead"),
		0.0f,
		TEXT("Distance ahead along the path the squad center aims at, cm (corner smoothing). 0 = from the squad turn radius."),
		ECVF_Default);

	// Насколько далеко от точки приказа может оборваться путь, чтобы цель
	// всё ещё считалась достигнутой, см. Нужно на случай клика у самого
	// края проходимой зоны: там путь обрывается немного
	// раньше цели, и отменять приказ из-за этого не стоит.
	constexpr double UnreachableTargetToleranceCm = 150.0;

	// Насколько близко к цели должен подойти частичный путь по широкой
	// карте, чтобы его всё же принять (в радиусах агента). Ближе — значит
	// дорога пройдена, а не дошли только до самой точки приказа: она у
	// стены, куда широкая карта не достаёт. Дальше — проход для строя
	// закрыт, и нужна обычная карта.
	constexpr double PartialPathTargetFactor = 2.0;

	// Какую долю расстояния погони занимает срез угла: при повороте на 90°
	// центр подходит к точке поворота примерно на 0.9 расстояния погони
	// (проверено прогоном схемы ведения). Отсюда предел расстояния погони
	// по имеющемуся запасу.
	constexpr double PathCornerCutFactor = 0.9;

	// Какой запас путь реально держит: расстояние от его точек до ближайшего
	// конца ворот коридора, то есть до угла препятствия. Больше запрошенного
	// не нужно, меньше — повод вести центр ближе к ломаной.
	double GetPathCornerClearance(
		TConstArrayView<FVector> Points,
		TConstArrayView<FAlgonaPathPortal> Portals,
		double MaxClearance)
	{
		if (Points.IsEmpty() || Portals.IsEmpty())
		{
			return MaxClearance;
		}

		double Clearance = MaxClearance;

		for (const FVector& Point : Points)
		{
			const FVector2D Location(Point.X, Point.Y);

			for (const FAlgonaPathPortal& Portal : Portals)
			{
				Clearance = FMath::Min3(
					Clearance,
					static_cast<double>(FVector2D::Distance(Location, Portal.Left)),
					static_cast<double>(FVector2D::Distance(Location, Portal.Right)));
			}
		}

		return Clearance;
	}

	// Полуширина строя, см: самый дальний слот по боковой оси плюс тело
	// Unit. По ней выбирается ступень карты проходимости.
	double GetSquadHalfWidth(const FAlgonaSquad& Squad)
	{
		double HalfWidth = 0.0;

		for (const FAlgonaFormationSlot& Slot : Squad.FormationLayout.Slots)
		{
			HalfWidth = FMath::Max(
				HalfWidth,
				static_cast<double>(FMath::Abs(Slot.LocalOffset.Y)));
		}

		return HalfWidth + static_cast<double>(Squad.MaxUnitRadius);
	}

	// Допуск при проверке места под Squad: насколько далеко от точки
	// разрешено искать проходимую землю по горизонтали и по высоте, см.
	// По горизонтали — почти ничего (точка должна стоять на земле сама),
	// по высоте с запасом на неровности.
	constexpr double PlacementProjectionExtentCm = 20.0;
	constexpr double PlacementProjectionHeightCm = 300.0;

}

bool UAlgonaSimulationSubsystem::IsSquadPathEnabled()
{
	return CVarAlgonaP2Path.GetValueOnGameThread() != 0;
}

int32 UAlgonaSimulationSubsystem::GetPathQueriesPerTick()
{
	return CVarAlgonaP2PathQueriesPerTick.GetValueOnGameThread();
}

float UAlgonaSimulationSubsystem::GetPathAgentRadius()
{
	return CVarAlgonaP2PathAgentRadius.GetValueOnGameThread();
}

float UAlgonaSimulationSubsystem::GetPathWideDetour()
{
	return CVarAlgonaP2PathWideDetour.GetValueOnGameThread();
}

float UAlgonaSimulationSubsystem::GetPathLookaheadOverride()
{
	return CVarAlgonaP2PathLookahead.GetValueOnGameThread();
}

void UAlgonaSimulationSubsystem::CancelSquadMoveOrder(
	FAlgonaSquad& Squad,
	const TCHAR* Reason)
{
	Squad.bHasMoveTarget = false;
	Squad.bHasFacingTarget = false;
	Squad.PendingRowLength = 0;
	Squad.TargetCenterLocation = Squad.CenterLocation;
	Squad.PathPoints.Reset(1);
	Squad.PathPoints.Add(Squad.CenterLocation);
	Squad.PathPointIndex = 0;
	Squad.PathMaxLookahead = 0.0f;

	if (CVarAlgonaP2PathDebug.GetValueOnGameThread() != 0)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Display,
			TEXT("[P2 PathDebug] squad=%d order cancelled: %s"),
			Squad.SquadId,
			Reason);
	}
}

double UAlgonaSimulationSubsystem::GetSquadPathLookahead(const FAlgonaSquad& Squad)
{
	const float LookaheadOverride = GetPathLookaheadOverride();

	if (LookaheadOverride > 0.0f)
	{
		return static_cast<double>(LookaheadOverride);
	}

	const double MaxYawRate = static_cast<double>(Squad.GetMaxYawRate());

	const double TurnRadius = MaxYawRate > UE_DOUBLE_SMALL_NUMBER
		? static_cast<double>(Squad.CenterMoveSpeed) / MaxYawRate
		: FAlgonaSquad::MinPathLookaheadCm;

	return FMath::Clamp(
		TurnRadius,
		FAlgonaSquad::MinPathLookaheadCm,
		FAlgonaSquad::MaxPathLookaheadCm);
}

void UAlgonaSimulationSubsystem::RequestSquadPath(FAlgonaSquad& Squad)
{
	// Повторная постановка не нужна: запрос в очереди всё равно возьмёт
	// текущую цель Squad, а не ту, с которой он туда попал.
	if (Squad.bPathRequestPending || Squad.SquadId == INDEX_NONE)
	{
		return;
	}

	Squad.bPathRequestPending = true;
	PathRequestQueue.Add(Squad.SquadId);
}

void UAlgonaSimulationSubsystem::ProcessSquadPathRequests()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaSimulation_SquadPaths);

	if (PathRequestQueue.IsEmpty())
	{
		return;
	}

	// Пути выключены: очередь просто сбрасывается, Squad идут по прямой.
	if (!IsSquadPathEnabled())
	{
		for (const int32 SquadId : PathRequestQueue)
		{
			if (Squads.IsValidIndex(SquadId))
			{
				Squads[SquadId].bPathRequestPending = false;
			}
		}

		PathRequestQueue.Reset();
		return;
	}

	const int32 Budget = FMath::Max(GetPathQueriesPerTick(), 1);
	const int32 RequestCount = FMath::Min(Budget, PathRequestQueue.Num());

	for (int32 RequestIndex = 0; RequestIndex < RequestCount; ++RequestIndex)
	{
		const int32 SquadId = PathRequestQueue[RequestIndex];

		if (!Squads.IsValidIndex(SquadId))
		{
			continue;
		}

		FAlgonaSquad& Squad = Squads[SquadId];

		if (!Squad.bPathRequestPending)
		{
			continue;
		}

		Squad.bPathRequestPending = false;

		// Приказ мог закончиться, пока Squad стоял в очереди.
		if (!Squad.bHasMoveTarget)
		{
			continue;
		}

		if (BuildSquadPath(Squad))
		{
			ApplySquadPathToOrder(Squad);
		}
	}

	PathRequestQueue.RemoveAt(0, RequestCount, EAllowShrinking::No);
}

bool UAlgonaSimulationSubsystem::BuildSquadPath(FAlgonaSquad& Squad)
{
	UWorld* World = GetWorld();
	UNavigationSystemV1* NavigationSystem = UNavigationSystemV1::GetCurrent(World);
	const ANavigationData* UnitNavData = NavigationSystem
		? NavigationSystem->GetDefaultNavDataInstance()
		: nullptr;

	if (!UnitNavData)
	{
		if (!bMissingNavigationLogged)
		{
			bMissingNavigationLogged = true;

			UE_LOG(
				LogAlgonaSimulation,
				Warning,
				TEXT("[P2 Path] no navigation data in the world: squads move in straight lines. ")
				TEXT("Add a NavMeshBoundsVolume to the level to enable paths."));
		}

		return false;
	}

	// Ступень карты проходимости: самый широкий объявленный агент, который
	// не шире строя. Карта такого агента уже сужена на его радиус, поэтому
	// путь по ней держит запас от стен сам, без постобработки.
	const double SquadHalfWidth = GetSquadHalfWidth(Squad);
	const double MaxAgentRadius = static_cast<double>(GetPathAgentRadius());
	const double MaxTierRadius = FMath::Min(SquadHalfWidth, MaxAgentRadius);

	const ANavigationData* SquadNavData = nullptr;
	double SquadAgentRadius = 0.0;

	for (const FNavDataConfig& Agent : NavigationSystem->GetSupportedAgents())
	{
		const double AgentRadius = static_cast<double>(Agent.AgentRadius);

		if (AgentRadius > MaxTierRadius || AgentRadius <= SquadAgentRadius)
		{
			continue;
		}

		const ANavigationData* AgentNavData = NavigationSystem->GetNavDataForProps(
			FNavAgentProperties(Agent.AgentRadius, Agent.AgentHeight));

		// Карты под такого агента в мире может не быть: навигация тогда
		// возвращает обычную, и запаса от неё ждать нечего.
		if (AgentNavData && AgentNavData != UnitNavData)
		{
			SquadNavData = AgentNavData;
			SquadAgentRadius = AgentRadius;
		}
	}

	// Короткий путь по обычной карте: он есть всегда, когда путь вообще
	// есть, и служит мерой для пути с запасом.
	TArray<FVector> Points;
	TArray<FAlgonaPathPortal> Portals;
	FVector PathStartLocation = Squad.CenterLocation;
	bool bPartialPath = false;

	// По обычной карте запас просит сам путь; по широкой он уже заложен в
	// карту, и просить его ещё раз значило бы складывать два запаса.
	bool bPathFound = QuerySquadPathPoints(
		Squad,
		*UnitNavData,
		Squad.CenterLocation,
		SquadHalfWidth,
		Points,
		Portals,
		PathStartLocation,
		bPartialPath);

	if (!bPathFound)
	{
		// Squad мог оказаться вне карты проходимости: в давке свои же Unit
		// выталкивают его к стене. Пробуем от ближайшей проходимой точки.
		const FVector ProjectionExtent(
			PlacementProjectionExtentCm,
			PlacementProjectionExtentCm,
			PlacementProjectionHeightCm);

		FNavLocation ProjectedStart;

		if (NavigationSystem->ProjectPointToNavigation(
			Squad.CenterLocation,
			ProjectedStart,
			ProjectionExtent,
			UnitNavData))
		{
			bPathFound = QuerySquadPathPoints(
				Squad,
				*UnitNavData,
				ProjectedStart.Location,
				SquadHalfWidth,
				Points,
				Portals,
				PathStartLocation,
				bPartialPath);
		}
	}

	// Пути нет вовсе: приказ не выполняется. Прямая отсюда вела бы сквозь
	// стены, а идти в другое место за игрока мы не вправе.
	if (!bPathFound)
	{
		CancelSquadMoveOrder(Squad, TEXT("no path"));
		return false;
	}

	// Путь обрывается заметно раньше цели: цель недостижима.
	if (bPartialPath
		&& FVector::Dist2D(Points.Last(), Squad.TargetCenterLocation)
			> UnreachableTargetToleranceCm)
	{
		CancelSquadMoveOrder(Squad, TEXT("target unreachable"));
		return false;
	}

	const double ShortLength =
		FVector::Dist2D(Squad.CenterLocation, Points[0])
		+ GetAlgonaPathLength(Points, 0);

	// Путь с запасом от стен. Берём его, только если крюк невелик: иначе
	// Squad обходил бы полкарты ради прохода, в который он помещается.
	bool bWideNavData = false;
	double WideLength = 0.0;

	if (SquadNavData)
	{
		TArray<FVector> WidePoints;
		bool bWidePartial = false;

		TArray<FAlgonaPathPortal> WidePortals;
		FVector WideStartLocation = Squad.CenterLocation;


		if (QuerySquadPathPoints(
			Squad,
			*SquadNavData,
			Squad.CenterLocation,
			0.0,
			WidePoints,
			WidePortals,
			WideStartLocation,
			bWidePartial))
		{
			// Частичный путь принимается, только если не дошёл до самой
			// точки приказа: она ближе к стене, чем достаёт широкая карта.
			const bool bReachesTarget = !bWidePartial
				|| FVector::Dist2D(WidePoints.Last(), Squad.TargetCenterLocation)
					<= SquadAgentRadius * PartialPathTargetFactor;

			if (bReachesTarget)
			{
				if (bWidePartial)
				{
					WidePoints.Add(Squad.TargetCenterLocation);
				}

				WideLength =
					FVector::Dist2D(Squad.CenterLocation, WidePoints[0])
					+ GetAlgonaPathLength(WidePoints, 0);

				if (WideLength <= ShortLength * static_cast<double>(GetPathWideDetour()))
				{
					Points = MoveTemp(WidePoints);
					Portals = MoveTemp(WidePortals);
					bPartialPath = bWidePartial;
					bWideNavData = true;
				}
			}
		}
	}

	if (!bWideNavData)
	{
		SquadAgentRadius = 0.0;
	}

	// Симуляция плоская: все точки приводятся к высоте центра Squad.
	for (FVector& Point : Points)
	{
		Point.Z = Squad.CenterLocation.Z;
	}

	// Цель приказа — конец пути. Навигация ставит её на проходимое место:
	// при достижимой цели это та же точка, при недостижимой — ближайшая
	// доступная, и Squad останавливается там, а не идёт в препятствие.
	Squad.TargetCenterLocation = Points.Last();
	Squad.PathPoints = MoveTemp(Points);
	Squad.PathPointIndex = 0;

	// Срез угла дугой не должен съедать запас: он ограничен тем запасом,
	// который путь реально держит — радиусом агента на широкой карте или
	// расстоянием до углов коридора на обычной. Если запаса нет вовсе,
	// срез минимальный, и центр идёт почти по ломаной.
	const double PathClearance = bWideNavData
		? SquadAgentRadius
		: GetPathCornerClearance(Squad.PathPoints, Portals, SquadHalfWidth);

	Squad.PathMaxLookahead = static_cast<float>(FMath::Max(
		PathClearance / PathCornerCutFactor,
		FAlgonaSquad::MinPathLookaheadCm));

	const int32 PathDebug = CVarAlgonaP2PathDebug.GetValueOnGameThread();

	// Ворота коридора для отрисовки: по ним видно, задан ли крюк самим
	// коридором поиска или его делает сборка пути.
	if (PathDebug >= 2)
	{
		DebugPathPortals.Reset(Portals.Num() * 2);

		for (const FAlgonaPathPortal& Portal : Portals)
		{
			DebugPathPortals.Emplace(
				Portal.Left.X,
				Portal.Left.Y,
				Squad.CenterLocation.Z);
			DebugPathPortals.Emplace(
				Portal.Right.X,
				Portal.Right.Y,
				Squad.CenterLocation.Z);
		}
	}

	if (PathDebug != 0)
	{
		UE_LOG(
			LogAlgonaSimulation,
			Display,
			TEXT("[P2 PathDebug] squad=%d points=%d halfWidth=%.0f agent=%.0f partial=%d short=%.0f wide=%.0f maxLookahead=%.0f"),
			Squad.SquadId,
			Squad.PathPoints.Num(),
			SquadHalfWidth,
			SquadAgentRadius,
			bPartialPath ? 1 : 0,
			ShortLength,
			WideLength,
			Squad.PathMaxLookahead);
	}

	return true;
}

bool UAlgonaSimulationSubsystem::QuerySquadPathPoints(
	const FAlgonaSquad& Squad,
	const ANavigationData& NavData,
	const FVector& StartLocation,
	double Clearance,
	TArray<FVector>& OutPoints,
	TArray<FAlgonaPathPortal>& OutPortals,
	FVector& OutStartLocation,
	bool& bOutPartial)
{
	bOutPartial = false;

	// Частичный путь разрешён, и цель не обязана быть проходимой: приказ
	// «идти в стену» или за закрытые ворота доводит Squad до ближайшего
	// достижимого места, а не отменяется и не ведёт сквозь препятствие.
	FPathFindingQuery Query(
		nullptr,
		NavData,
		StartLocation,
		Squad.TargetCenterLocation);
	Query.SetAllowPartialPaths(true);
	Query.SetRequireNavigableEndLocation(false);

	// Коридор нужен, чтобы пересобрать путь по его воротам.
	Query.NavDataFlags = ERecastPathFlags::GenerateCorridor;

	UNavigationSystemV1* NavigationSystem =
		UNavigationSystemV1::GetCurrent(GetWorld());

	if (!NavigationSystem)
	{
		return false;
	}

	const FPathFindingResult Result = NavigationSystem->FindPathSync(Query);
	bOutPartial = Result.IsPartial();

	if (!Result.IsSuccessful() || !Result.Path.IsValid())
	{
		// Обычная причина — Squad стоит вне этой карты проходимости: по
		// широкой карте это нормально (строй не влезает), по обычной это
		// повод посмотреть на уровень.
		if (!bPathQueryFailedLogged)
		{
			bPathQueryFailedLogged = true;

			UE_LOG(
				LogAlgonaSimulation,
				Warning,
				TEXT("[P2 Path] path query failed for squad %d: %s -> %s. ")
				TEXT("The squad moves straight. Check that it stands inside the NavMeshBoundsVolume."),
				Squad.SquadId,
				*Squad.CenterLocation.ToCompactString(),
				*Squad.TargetCenterLocation.ToCompactString());
		}

		return false;
	}

	// Запас от углов: штатная обработка пути навигации. Сдвигает только те
	// точки, что прижаты к концам рёбер коридора, и не дальше середины
	// ребра — в узких воротах это и есть их середина.
	FNavMeshPath* NavMeshPath = Result.Path->CastPath<FNavMeshPath>();

	if (NavMeshPath && Clearance > 0.0)
	{
		NavMeshPath->OffsetFromCorners(Clearance);
	}

	const TArray<FNavPathPoint>& NavPoints = Result.Path->GetPathPoints();

	// Первая точка пути — сама позиция центра, она не нужна. Если других
	// точек нет, пути как такового тоже нет.
	if (NavPoints.Num() < 2)
	{
		return false;
	}

	OutPoints.Reset(NavPoints.Num() - 1);

	for (int32 PointIndex = 1; PointIndex < NavPoints.Num(); ++PointIndex)
	{
		OutPoints.Add(NavPoints[PointIndex].Location);
	}

	// Ворота коридора: нужны для отладочной отрисовки и для оценки того,
	// какой запас путь реально держит.
	OutPortals.Reset();

	if (NavMeshPath)
	{
		const TArray<FNavigationPortalEdge>& Edges =
			NavMeshPath->GetPathCorridorEdges();

		OutPortals.Reserve(Edges.Num());

		for (const FNavigationPortalEdge& Edge : Edges)
		{
			FAlgonaPathPortal& Portal = OutPortals.AddDefaulted_GetRef();
			Portal.Left = FVector2D(Edge.Left.X, Edge.Left.Y);
			Portal.Right = FVector2D(Edge.Right.X, Edge.Right.Y);
		}
	}

	OutStartLocation = NavPoints[0].Location;
	return true;
}

bool UAlgonaSimulationSubsystem::IsSquadPlacementFree(
	const FVector& Center,
	const FVector& Forward,
	const FVector2D& HalfExtent,
	TArray<uint32>& ScratchUnitIds)
{
	const FVector Right =
		FVector::CrossProduct(FVector::UpVector, Forward).GetSafeNormal();

	// 1. Площадка целиком на проходимой земле: проверяются центр и четыре
	// угла прямоугольника. Так Squad не встанет в стену и не свесится за
	// её край — для 2000 Squad это 10 000 запросов один раз на спавн.
	UWorld* World = GetWorld();
	const UNavigationSystemV1* NavigationSystem =
		UNavigationSystemV1::GetCurrent(World);
	const ANavigationData* NavData = NavigationSystem
		? NavigationSystem->GetDefaultNavDataInstance()
		: nullptr;

	if (NavData)
	{
		const FVector ProjectionExtent(
			PlacementProjectionExtentCm,
			PlacementProjectionExtentCm,
			PlacementProjectionHeightCm);

		FNavLocation Projected;

		if (!NavigationSystem->ProjectPointToNavigation(
			Center,
			Projected,
			ProjectionExtent,
			NavData))
		{
			return false;
		}

		for (int32 CornerIndex = 0; CornerIndex < 4; ++CornerIndex)
		{
			const double ForwardSign = (CornerIndex & 1) != 0 ? 1.0 : -1.0;
			const double RightSign = (CornerIndex & 2) != 0 ? 1.0 : -1.0;

			const FVector Corner = Center
				+ Forward * (ForwardSign * HalfExtent.X)
				+ Right * (RightSign * HalfExtent.Y);

			if (!NavigationSystem->ProjectPointToNavigation(
				Corner,
				Projected,
				ProjectionExtent,
				NavData))
			{
				return false;
			}
		}
	}
	else if (!bPlacementWithoutNavigationLogged)
	{
		bPlacementWithoutNavigationLogged = true;

		UE_LOG(
			LogAlgonaSimulation,
			Warning,
			TEXT("[P2 Spawn] no navigation data in the world: squads are placed ")
			TEXT("without the walkable-ground check."));
	}

	// 2. В площадке нет чужих Unit. Unit Grid даёт кандидатов по своим
	// крупным ячейкам, поэтому каждого кандидата проверяем по месту.
	const double QueryRadius = FMath::Sqrt(
		HalfExtent.X * HalfExtent.X + HalfExtent.Y * HalfExtent.Y);

	QueryUnitIdsInBounds(
		FVector2D(Center.X - QueryRadius, Center.Y - QueryRadius),
		FVector2D(Center.X + QueryRadius, Center.Y + QueryRadius),
		ScratchUnitIds);

	for (const uint32 UnitId : ScratchUnitIds)
	{
		FVector UnitPosition = FVector::ZeroVector;

		if (!GetUnitPosition(UnitId, UnitPosition))
		{
			continue;
		}

		const FVector Offset = UnitPosition - Center;

		if (FMath::Abs(FVector::DotProduct(Offset, Forward)) <= HalfExtent.X
			&& FMath::Abs(FVector::DotProduct(Offset, Right)) <= HalfExtent.Y)
		{
			return false;
		}
	}

	return true;
}

void UAlgonaSimulationSubsystem::ApplySquadPathToOrder(FAlgonaSquad& Squad)
{
	if (Squad.PathPoints.IsEmpty())
	{
		return;
	}

	// Режим движения решается по длине настоящего пути, а не по прямой до
	// цели: путь в обход препятствия может быть втрое длиннее.
	const double PathLength =
		FVector::Dist2D(Squad.CenterLocation, Squad.PathPoints[0])
		+ GetAlgonaPathLength(Squad.PathPoints, 0);

	Squad.MoveMode = ChooseSquadMoveMode(Squad, PathLength);

	// Конечное направление без явного приказа — направление последнего
	// отрезка пути: Squad приходит в цель так, как в неё вошёл.
	if (!Squad.bFinalFacingFromOrder)
	{
		const FVector SegmentStart = Squad.PathPoints.Num() > 1
			? Squad.PathPoints.Last(1)
			: Squad.CenterLocation;

		const FVector Direction =
			(Squad.PathPoints.Last() - SegmentStart).GetSafeNormal2D();

		if (!Direction.IsNearlyZero())
		{
			Squad.FinalFacingDirection = Direction;
		}
	}

	Squad.bFinalTurnStarted = false;
}
