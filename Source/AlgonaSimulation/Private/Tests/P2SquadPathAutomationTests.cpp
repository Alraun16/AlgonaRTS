#include "Army/AlgonaSquadPath.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr double PathTestTolerance = 0.5;

	// Г-образный путь: 10 м вперёд по X, затем 10 м вправо по Y.
	// Первой точки «позиция центра» в пути нет, как и в настоящем.
	TArray<FVector> MakeCornerPath()
	{
		return TArray<FVector>{
			FVector(1000.0, 0.0, 0.0),
			FVector(1000.0, 1000.0, 0.0)};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SquadPathAimTest,
	"Algona.P2.SquadPath.Aim",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SquadPathAimTest::RunTest(
	const FString& Parameters)
{
	// 1. Путь из одной точки — это прямая к цели: цель и есть точка погони.
	{
		const TArray<FVector> StraightPath{FVector(500.0, 0.0, 0.0)};
		int32 PointIndex = 0;

		const FAlgonaPathFollowResult Follow = FollowAlgonaPath(
			StraightPath,
			FVector::ZeroVector,
			300.0,
			PointIndex);

		TestEqual(TEXT("Straight path: aim is the target"), Follow.AimLocation, StraightPath[0]);
		TestEqual(
			TEXT("Straight path: remaining distance"),
			Follow.RemainingDistance,
			500.0,
			PathTestTolerance);
		TestEqual(TEXT("Straight path: point index"), PointIndex, 0);
	}

	const TArray<FVector> Path = MakeCornerPath();

	// 2. Пока до точки поворота дальше, чем расстояние погони, центр идёт
	// прямо к ней, а остаток пути — вся ломаная.
	{
		int32 PointIndex = 0;

		const FAlgonaPathFollowResult Follow = FollowAlgonaPath(
			Path,
			FVector::ZeroVector,
			300.0,
			PointIndex);

		TestEqual(TEXT("Far from the corner: aim is the corner"), Follow.AimLocation, Path[0]);
		TestEqual(
			TEXT("Remaining distance from the start"),
			Follow.RemainingDistance,
			2000.0,
			PathTestTolerance);
	}

	// 3. У поворота точка погони уже за ним: центр начинает срезать угол,
	// не доезжая до самой точки поворота.
	{
		int32 PointIndex = 0;

		const FAlgonaPathFollowResult Follow = FollowAlgonaPath(
			Path,
			FVector(900.0, 0.0, 0.0),
			300.0,
			PointIndex);

		TestEqual(
			TEXT("Near the corner: aim is past it"),
			Follow.AimLocation,
			FVector(1000.0, 200.0, 0.0));
		TestEqual(
			TEXT("Remaining distance at the corner"),
			Follow.RemainingDistance,
			1100.0,
			PathTestTolerance);
		TestEqual(TEXT("Point index at the corner"), PointIndex, 0);
	}

	// 4. Поворот пройден: номер точки сдвинулся, остаток — только последний
	// отрезок. Центр при этом точки поворота не касался.
	{
		int32 PointIndex = 0;

		const FAlgonaPathFollowResult Follow = FollowAlgonaPath(
			Path,
			FVector(1000.0, 900.0, 0.0),
			300.0,
			PointIndex);

		TestEqual(TEXT("Last segment: aim is the target"), Follow.AimLocation, Path.Last());
		TestEqual(
			TEXT("Remaining distance near the target"),
			Follow.RemainingDistance,
			100.0,
			PathTestTolerance);
		TestEqual(TEXT("Point index after the corner"), PointIndex, 1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SquadPathRunTest,
	"Algona.P2.SquadPath.Run",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SquadPathRunTest::RunTest(
	const FString& Parameters)
{
	// Прогон центра по Г-образному пути с постоянной скоростью: так же, как
	// в UpdateSquadCenters, центр каждый тик идёт к точке погони, а приход
	// определяется остатком пути.
	const TArray<FVector> Path = MakeCornerPath();

	constexpr double StepSeconds = 1.0 / 40.0;
	constexpr double Speed = 450.0;
	constexpr double Lookahead = 400.0;
	constexpr int32 MaxTicks = 1000;

	FVector Center = FVector::ZeroVector;
	int32 PointIndex = 0;
	int32 Ticks = 0;
	double PreviousRemaining = TNumericLimits<double>::Max();
	double MinCornerDistance = TNumericLimits<double>::Max();
	bool bRemainingDecreases = true;

	for (; Ticks < MaxTicks; ++Ticks)
	{
		const FAlgonaPathFollowResult Follow = FollowAlgonaPath(
			Path,
			Center,
			Lookahead,
			PointIndex);

		bRemainingDecreases &= Follow.RemainingDistance < PreviousRemaining;
		PreviousRemaining = Follow.RemainingDistance;

		const double MoveDistance = Speed * StepSeconds;

		if (Follow.RemainingDistance <= MoveDistance)
		{
			Center = Path.Last();
			++Ticks;
			break;
		}

		const FVector Direction =
			(Follow.AimLocation - Center).GetSafeNormal2D();
		Center += Direction * MoveDistance;

		MinCornerDistance = FMath::Min(
			MinCornerDistance,
			FVector::Dist2D(Center, Path[0]));
	}

	TestTrue(TEXT("Remaining distance decreases every tick"), bRemainingDecreases);
	TestEqual(TEXT("Center ends exactly at the target"), Center, Path.Last());

	// Проверено прогоном той же схемы: 151 тик и срез угла 356 см при
	// расстоянии погони 400 см.
	TestTrue(
		FString::Printf(TEXT("Path takes 150-152 ticks (got %d)"), Ticks),
		Ticks >= 150 && Ticks <= 152);
	TestEqual(
		TEXT("Corner is cut by about the lookahead"),
		MinCornerDistance,
		356.3,
		2.0);

	return true;
}

#endif
