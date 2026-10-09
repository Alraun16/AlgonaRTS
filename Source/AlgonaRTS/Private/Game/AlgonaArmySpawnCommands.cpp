#include "Core/AlgonaSimulationSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

/*
 * Команды спавна армии — инструмент проверок, не игровая механика.
 *
 * Армия больше не создаётся при старте мира (algona.P0.UnitCount=0):
 * отряды досыпаются командой туда, куда смотрит камера. Так проверку
 * можно собрать на подготовленной карте — поставить отряды по разные
 * стороны стены, добавить третий в коридор — и не ждать создания сотен
 * тысяч Unit, когда они не нужны.
 *
 * Команды живут в игровом модуле, потому что им нужна камера игрока:
 * Simulation о локальном игроке ничего не знает и получает уже готовые
 * точку и направление.
 */
#if !UE_BUILD_SHIPPING

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaSpawn, Log, All);

namespace
{
	// Размер отряда по умолчанию, если в команде он не задан.
	constexpr int32 DefaultSpawnSquadSize = 50;

	// Направление строя привязывается к ближайшей оси мира: камера RTS
	// смотрит по диагонали (CameraYaw 45°), а геометрия карты стоит по
	// осям, и диагональный строй рядом с прямой стеной только мешает
	// смотреть на движение. Явный угол в команде привязку отменяет.
	FVector SnapToWorldAxis(const FVector& Direction)
	{
		return FMath::Abs(Direction.X) >= FMath::Abs(Direction.Y)
			? FVector(FMath::Sign(Direction.X), 0.0, 0.0)
			: FVector(0.0, FMath::Sign(Direction.Y), 0.0);
	}

	// Точка на земле, куда смотрит камера игрока, и её направление на
	// плоскости. Земля пока плоская, Z = 0.
	bool GetCameraGroundPoint(
		const UWorld& World,
		FVector& OutPoint,
		FVector& OutForward)
	{
		const APlayerController* PlayerController =
			World.GetFirstPlayerController();
		const APlayerCameraManager* Camera = PlayerController
			? PlayerController->PlayerCameraManager
			: nullptr;

		if (!Camera)
		{
			return false;
		}

		const FVector CameraLocation = Camera->GetCameraLocation();
		const FVector CameraDirection = Camera->GetCameraRotation().Vector();

		OutForward = CameraDirection.GetSafeNormal2D();
		if (OutForward.IsNearlyZero())
		{
			OutForward = FVector::ForwardVector;
		}

		// Пересечение луча камеры с землёй; если камера смотрит вверх или
		// вдоль земли, берём точку под самой камерой.
		if (CameraDirection.Z < -UE_DOUBLE_KINDA_SMALL_NUMBER)
		{
			OutPoint = CameraLocation
				+ CameraDirection * (CameraLocation.Z / -CameraDirection.Z);
		}
		else
		{
			OutPoint = CameraLocation;
		}

		OutPoint.Z = 0.0;
		return true;
	}

	void SpawnArmyCommand(
		const TArray<FString>& Arguments,
		UWorld* World)
	{
		if (!World)
		{
			return;
		}

		UAlgonaSimulationSubsystem* Simulation =
			World->GetSubsystem<UAlgonaSimulationSubsystem>();

		if (!Simulation)
		{
			return;
		}

		if (Arguments.IsEmpty())
		{
			UE_LOG(
				LogAlgonaSpawn,
				Display,
				TEXT("Usage: algona.P2.Spawn <units> [squad size] [facing yaw, degrees]. ")
				TEXT("The army is placed where the camera looks."));
			return;
		}

		const int32 UnitCount = FMath::Clamp(
			FCString::Atoi(*Arguments[0]),
			1,
			500000);

		const int32 SquadSize = Arguments.Num() > 1
			? FMath::Clamp(FCString::Atoi(*Arguments[1]), 1, 1000)
			: DefaultSpawnSquadSize;

		FVector Origin = FVector::ZeroVector;
		FVector Forward = FVector::ForwardVector;

		if (!GetCameraGroundPoint(*World, Origin, Forward))
		{
			UE_LOG(
				LogAlgonaSpawn,
				Warning,
				TEXT("No player camera: the army is placed at the world origin."));
		}

		// Отряды смотрят от камеры (игрок видит их со спины, как своё
		// войско), но по ближайшей оси мира. Третий аргумент — угол строя
		// в градусах, если нужно именно своё направление.
		Forward = Arguments.Num() > 2
			? FRotator(0.0, FCString::Atof(*Arguments[2]), 0.0).Vector()
			: SnapToWorldAxis(Forward);

		Simulation->SpawnArmy(UnitCount, SquadSize, Origin, Forward);
	}

	void ClearArmyCommand(UWorld* World)
	{
		UAlgonaSimulationSubsystem* Simulation = World
			? World->GetSubsystem<UAlgonaSimulationSubsystem>()
			: nullptr;

		if (Simulation)
		{
			Simulation->ClearArmy();
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GAlgonaP2SpawnCommand(
		TEXT("algona.P2.Spawn"),
		TEXT("Spawns <units> more units in squads of [squad size] where the camera looks, facing the nearest world axis away from the camera ([yaw] in degrees overrides it). Cells that are not fully on walkable ground or already occupied are skipped."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SpawnArmyCommand),
		ECVF_Cheat);

	FAutoConsoleCommandWithWorld GAlgonaP2ClearCommand(
		TEXT("algona.P2.Clear"),
		TEXT("Removes every unit and squad."),
		FConsoleCommandWithWorldDelegate::CreateStatic(&ClearArmyCommand),
		ECVF_Cheat);
}

#endif
