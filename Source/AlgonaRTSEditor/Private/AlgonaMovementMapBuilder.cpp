#include "BSPOps.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Polys.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/IConsoleManager.h"
#include "Model.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaMapBuilder, Log, All);

/*
 * Редакторный генератор карты проверок движения P2.
 *
 * Запускается один раз вручную в редакторе (не в PIE) на пустом уровне
 * командой algona.P2.BuildMovementMap, после чего уровень нужно сохранить
 * (Ctrl+S) и построить навигацию (Build -> Build Paths).
 *
 * Карта описана числами здесь, а не расставлена мышью: площадку препятствий
 * можно переделать правкой координат и повторным запуском команды — свои
 * актёры (с префиксом AlgonaP2_) она каждый раз удаляет и создаёт заново.
 *
 * Что получается (см):
 *   - ровная земля 1200x1200 м, верх ровно на Z = 0 (симуляция плоская);
 *   - Г-образная стена — срез угла дугой при повороте пути;
 *   - узкий проход 2 м — запас по бокам пути и будущий проход очередью;
 *   - широкие ворота 10 м — строй проходит целиком;
 *   - круглая скала в открытом месте — обход с двух сторон;
 *   - закрытый двор — цель внутри недостижима, путь получается частичным.
 */
namespace
{
	// Префикс имён всех создаваемых актёров: по нему же они и удаляются
	// при повторном запуске.
	const FString AlgonaActorPrefix = TEXT("AlgonaP2_");

	// Земля и том навигации, см.
	constexpr double GroundSizeCm = 120000.0;
	constexpr double GroundThicknessCm = 20.0;
	constexpr double NavVolumeHeightCm = 2000.0;

	// Стены: толщина и высота, см.
	constexpr double WallThicknessCm = 400.0;
	constexpr double WallHeightCm = 1000.0;

	// Одно препятствие-стена: центр на земле и размеры по X и Y.
	struct FWallBox
	{
		const TCHAR* Name;
		double CenterX;
		double CenterY;
		double SizeX;
		double SizeY;
	};

	// Площадка препятствий вокруг начала координат: игрок при старте
	// смотрит именно туда.
	const FWallBox ObstacleWalls[] =
	{
		// Г-образная стена: длинная часть вдоль X и боковая вдоль Y,
		// угол в точке (10000, 10000).
		{TEXT("WallCornerLong"), -5000.0, 10000.0, 30000.0, WallThicknessCm},
		{TEXT("WallCornerSide"), 10000.0, 2300.0, WallThicknessCm, 15000.0},

		// Узкий проход 2 м по центру стены.
		{TEXT("WallNarrowLeft"), -10100.0, -10000.0, 20000.0, WallThicknessCm},
		{TEXT("WallNarrowRight"), 10100.0, -10000.0, 20000.0, WallThicknessCm},

		// Широкие ворота 10 м.
		{TEXT("WallGateLeft"), -10500.0, -20000.0, 20000.0, WallThicknessCm},
		{TEXT("WallGateRight"), 10500.0, -20000.0, 20000.0, WallThicknessCm},

		// Закрытый двор 13.6 х 13.6 м: внутрь пути нет.
		{TEXT("WallYardNorth"), 25000.0, 7000.0, 14400.0, WallThicknessCm},
		{TEXT("WallYardSouth"), 25000.0, -7000.0, 14400.0, WallThicknessCm},
		{TEXT("WallYardWest"), 18000.0, 0.0, WallThicknessCm, 14000.0},
		{TEXT("WallYardEast"), 32000.0, 0.0, WallThicknessCm, 14000.0},
	};

	// Круглая скала: центр, диаметр и высота, см.
	constexpr double RockCenterX = -25000.0;
	constexpr double RockCenterY = 0.0;
	constexpr double RockDiameterCm = 3000.0;
	constexpr double RockHeightCm = 1200.0;

	UStaticMesh* LoadBasicShape(const TCHAR* AssetPath)
	{
		return LoadObject<UStaticMesh>(nullptr, AssetPath);
	}

	// Актёр со статическим мешем заданного размера. Меш задаётся до
	// регистрации компонента (SpawnActorDeferred), иначе статический
	// компонент менять нельзя.
	AStaticMeshActor* SpawnMeshBox(
		UWorld& World,
		UStaticMesh* Mesh,
		const FString& Name,
		const FVector& Location,
		const FVector& Scale)
	{
		if (!Mesh)
		{
			return nullptr;
		}

		const FTransform Transform(FRotator::ZeroRotator, Location, Scale);

		AStaticMeshActor* Actor = World.SpawnActorDeferred<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(),
			Transform);

		if (!Actor)
		{
			return nullptr;
		}

		UStaticMeshComponent* MeshComponent = Actor->GetStaticMeshComponent();
		if (MeshComponent)
		{
			MeshComponent->SetStaticMesh(Mesh);
		}

		Actor->FinishSpawning(Transform);
		Actor->SetActorLabel(AlgonaActorPrefix + Name);
		return Actor;
	}

	// Том границ навигации: кисть собирается так же, как это делает
	// редактор при перетаскивании тома на уровень.
	ANavMeshBoundsVolume* SpawnNavMeshBounds(
		UWorld& World,
		const FVector& Location,
		const FVector& Size)
	{
		ANavMeshBoundsVolume* Volume = World.SpawnActor<ANavMeshBoundsVolume>(
			Location,
			FRotator::ZeroRotator);

		if (!Volume)
		{
			return nullptr;
		}

		Volume->PreEditChange(nullptr);

		const EObjectFlags ObjectFlags =
			Volume->GetFlags() & (RF_Transient | RF_Transactional);

		Volume->PolyFlags = 0;
		Volume->Brush = NewObject<UModel>(Volume, NAME_None, ObjectFlags);
		Volume->Brush->Initialize(nullptr, true);
		Volume->Brush->Polys = NewObject<UPolys>(Volume->Brush, NAME_None, ObjectFlags);
		Volume->GetBrushComponent()->Brush = Volume->Brush;

		UCubeBuilder* CubeBuilder = NewObject<UCubeBuilder>(Volume);
		CubeBuilder->X = static_cast<float>(Size.X);
		CubeBuilder->Y = static_cast<float>(Size.Y);
		CubeBuilder->Z = static_cast<float>(Size.Z);
		Volume->BrushBuilder = CubeBuilder;

		CubeBuilder->Build(&World, Volume);
		FBSPOps::csgPrepMovingBrush(Volume);

		Volume->PostEditChange();
		Volume->SetActorLabel(AlgonaActorPrefix + TEXT("NavBounds"));

		// Кисть построена уже после регистрации актёра, поэтому навигации
		// нужно сказать, что границы изменились.
		UNavigationSystemV1* NavigationSystem =
			FNavigationSystem::GetCurrent<UNavigationSystemV1>(&World);

		if (NavigationSystem)
		{
			NavigationSystem->OnNavigationBoundsUpdated(Volume);
		}

		return Volume;
	}

	// Свет и небо: пустой уровень иначе чёрный.
	void SpawnLighting(UWorld& World)
	{
		ADirectionalLight* SunLight = World.SpawnActor<ADirectionalLight>(
			FVector(0.0, 0.0, 20000.0),
			FRotator(-50.0, -30.0, 0.0));

		if (SunLight)
		{
			SunLight->SetActorLabel(AlgonaActorPrefix + TEXT("SunLight"));
			SunLight->SetMobility(EComponentMobility::Stationary);
		}

		ASkyAtmosphere* SkyAtmosphere = World.SpawnActor<ASkyAtmosphere>(
			FVector::ZeroVector,
			FRotator::ZeroRotator);

		if (SkyAtmosphere)
		{
			SkyAtmosphere->SetActorLabel(AlgonaActorPrefix + TEXT("SkyAtmosphere"));
		}

		ASkyLight* SkyLight = World.SpawnActor<ASkyLight>(
			FVector(0.0, 0.0, 20000.0),
			FRotator::ZeroRotator);

		if (SkyLight)
		{
			SkyLight->SetActorLabel(AlgonaActorPrefix + TEXT("SkyLight"));

			USkyLightComponent* SkyLightComponent = SkyLight->GetLightComponent();
			if (SkyLightComponent)
			{
				// Небо снимается в реальном времени: отдельная кубическая
				// карта для тестовой сцены не нужна.
				SkyLightComponent->SetMobility(EComponentMobility::Movable);
				SkyLightComponent->bRealTimeCapture = true;
				SkyLightComponent->MarkRenderStateDirty();
			}
		}

		APlayerStart* PlayerStart = World.SpawnActor<APlayerStart>(
			FVector(0.0, 0.0, 200.0),
			FRotator::ZeroRotator);

		if (PlayerStart)
		{
			PlayerStart->SetActorLabel(AlgonaActorPrefix + TEXT("PlayerStart"));
		}
	}

	// Удаляет актёров прошлого запуска команды.
	int32 DestroyPreviousActors(UWorld& World)
	{
		TArray<AActor*> ToDestroy;

		for (TActorIterator<AActor> ActorIterator(&World); ActorIterator; ++ActorIterator)
		{
			AActor* Actor = *ActorIterator;

			if (Actor && Actor->GetActorLabel().StartsWith(AlgonaActorPrefix))
			{
				ToDestroy.Add(Actor);
			}
		}

		for (AActor* Actor : ToDestroy)
		{
			World.EditorDestroyActor(Actor, false);
		}

		return ToDestroy.Num();
	}

	void BuildMovementMap()
	{
		if (!GEditor || GEditor->PlayWorld)
		{
			UE_LOG(
				LogAlgonaMapBuilder,
				Error,
				TEXT("Run algona.P2.BuildMovementMap in the editor, not in PIE."));
			return;
		}

		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			UE_LOG(LogAlgonaMapBuilder, Error, TEXT("No editor world."));
			return;
		}

		UStaticMesh* CubeMesh = LoadBasicShape(TEXT("/Engine/BasicShapes/Cube.Cube"));
		UStaticMesh* CylinderMesh =
			LoadBasicShape(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));

		if (!CubeMesh || !CylinderMesh)
		{
			UE_LOG(
				LogAlgonaMapBuilder,
				Error,
				TEXT("Engine basic shapes are not available."));
			return;
		}

		const int32 DestroyedCount = DestroyPreviousActors(*World);

		// Земля: плита, верх которой лежит ровно на Z = 0. Базовый куб —
		// 100 см, поэтому масштаб это размер, делённый на 100.
		SpawnMeshBox(
			*World,
			CubeMesh,
			TEXT("Ground"),
			FVector(0.0, 0.0, -GroundThicknessCm * 0.5),
			FVector(
				GroundSizeCm / 100.0,
				GroundSizeCm / 100.0,
				GroundThicknessCm / 100.0));

		int32 WallCount = 0;

		for (const FWallBox& Wall : ObstacleWalls)
		{
			if (SpawnMeshBox(
				*World,
				CubeMesh,
				Wall.Name,
				FVector(Wall.CenterX, Wall.CenterY, WallHeightCm * 0.5),
				FVector(
					Wall.SizeX / 100.0,
					Wall.SizeY / 100.0,
					WallHeightCm / 100.0)))
			{
				++WallCount;
			}
		}

		SpawnMeshBox(
			*World,
			CylinderMesh,
			TEXT("Rock"),
			FVector(RockCenterX, RockCenterY, RockHeightCm * 0.5),
			FVector(
				RockDiameterCm / 100.0,
				RockDiameterCm / 100.0,
				RockHeightCm / 100.0));

		SpawnLighting(*World);

		const ANavMeshBoundsVolume* NavBounds = SpawnNavMeshBounds(
			*World,
			FVector(0.0, 0.0, NavVolumeHeightCm * 0.5 - GroundThicknessCm),
			FVector(GroundSizeCm, GroundSizeCm, NavVolumeHeightCm));

		GEditor->RedrawLevelEditingViewports();

		UE_LOG(
			LogAlgonaMapBuilder,
			Display,
			TEXT("[P2 Map] built: ground %.0fx%.0f m, walls=%d, rock=1, navBounds=%s, removed previous=%d. ")
			TEXT("Save the level (Ctrl+S) and run Build -> Build Paths."),
			GroundSizeCm / 100.0,
			GroundSizeCm / 100.0,
			WallCount,
			NavBounds ? TEXT("yes") : TEXT("FAILED"),
			DestroyedCount);
	}

	FAutoConsoleCommand GAlgonaBuildMovementMapCommand(
		TEXT("algona.P2.BuildMovementMap"),
		TEXT("Editor only (not in PIE): fill the current level with the P2 movement test ground, obstacles and navigation bounds."),
		FConsoleCommandDelegate::CreateStatic(&BuildMovementMap));
}
