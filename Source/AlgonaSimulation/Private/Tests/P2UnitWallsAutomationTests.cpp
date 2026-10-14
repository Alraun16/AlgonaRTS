#include "Spatial/AlgonaWallField.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr float WallTestTolerance = 0.01f;

	// Отрезок стены с нормалью в сторону InsidePoint — так же, как при
	// запекании нормаль смотрит в сторону центра полигона-владельца.
	FAlgonaWallSegment MakeWall(
		const FVector2f& Start,
		const FVector2f& End,
		const FVector2f& InsidePoint)
	{
		FAlgonaWallSegment Wall;
		Wall.Start = Start;
		Wall.End = End;

		const FVector2f Tangent = (End - Start).GetSafeNormal();
		Wall.Normal = FVector2f(-Tangent.Y, Tangent.X);

		if (FVector2f::DotProduct(InsidePoint - Start, Wall.Normal) < 0.0f)
		{
			Wall.Normal = -Wall.Normal;
		}

		return Wall;
	}

	// Стена вдоль Y от (0, 0) до (0, 1000), проходимо при отрицательных X.
	FAlgonaWallSegment MakeWall()
	{
		return MakeWall(
			FVector2f(0.0f, 0.0f),
			FVector2f(0.0f, 1000.0f),
			FVector2f(-100.0f, 500.0f));
	}

	// Зажим так же, как в Simulation: ближайший элемент, два прохода.
	FVector2f ClampToWalls(
		TConstArrayView<FAlgonaWallSegment> Walls,
		FVector2f Point,
		float Margin)
	{
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			FAlgonaWallNearest Nearest;

			for (const FAlgonaWallSegment& Wall : Walls)
			{
				AccumulateAlgonaNearestWall(Wall, Point, Nearest);
			}

			const FVector2f Correction =
				GetAlgonaNearestWallCorrection(Nearest, Point, Margin);

			if (Correction.IsNearlyZero())
			{
				break;
			}

			Point += Correction;
		}

		return Point;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2UnitWallCorrectionTest,
	"Algona.P2.UnitWalls.Correction",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2UnitWallCorrectionTest::RunTest(
	const FString& Parameters)
{
	// Сравнение покомпонентно: так в отчёте видно, какая координата разошлась.
	auto TestPoint = [this](
		const TCHAR* What,
		const FVector2f& Actual,
		const FVector2f& Expected)
	{
		TestEqual(*FString::Printf(TEXT("%s (X)"), What), Actual.X, Expected.X, WallTestTolerance);
		TestEqual(*FString::Printf(TEXT("%s (Y)"), What), Actual.Y, Expected.Y, WallTestTolerance);
	};

	TestPoint(
		TEXT("Normal points to the owner polygon side"),
		MakeWall().Normal,
		FVector2f(-1.0f, 0.0f));

	// Числа сверены численной моделью до сборки.
	// Препятствие — прямоугольник [0, 400] x [0, 1000], проходимо снаружи.
	const FAlgonaWallSegment Box[] =
	{
		MakeWall(FVector2f(0.0f, 0.0f), FVector2f(0.0f, 1000.0f), FVector2f(-100.0f, 500.0f)),
		MakeWall(FVector2f(400.0f, 0.0f), FVector2f(400.0f, 1000.0f), FVector2f(500.0f, 500.0f)),
		MakeWall(FVector2f(0.0f, 1000.0f), FVector2f(400.0f, 1000.0f), FVector2f(200.0f, 1100.0f)),
		MakeWall(FVector2f(0.0f, 0.0f), FVector2f(400.0f, 0.0f), FVector2f(200.0f, -100.0f)),
	};

	// 1. Перед стеной обычный Unit (запас 0) не трогается, даже вплотную.
	TestPoint(TEXT("In front: untouched"),
		ClampToWalls(Box, FVector2f(-50.0f, 500.0f), 0.0f), FVector2f(-50.0f, 500.0f));

	// 2. Переступил линию — ровно на неё.
	TestPoint(TEXT("Crossed: back to the line"),
		ClampToWalls(Box, FVector2f(10.0f, 500.0f), 0.0f), FVector2f(0.0f, 500.0f));

	// 3. Глубоко внутри — к ближайшей грани, а не к той, чья линия «длиннее».
	TestPoint(TEXT("Deep inside: to the nearest face"),
		ClampToWalls(Box, FVector2f(150.0f, 500.0f), 0.0f), FVector2f(0.0f, 500.0f));
	TestPoint(TEXT("Nearer the far face: to the far face, not through"),
		ClampToWalls(Box, FVector2f(300.0f, 500.0f), 0.0f), FVector2f(400.0f, 500.0f));

	// 4. Снаружи у угла по диагонали: сторону решает средняя нормаль
	// двух граней в общей вершине, и точку не затягивает внутрь.
	TestPoint(TEXT("Outside at a corner: untouched"),
		ClampToWalls(Box, FVector2f(-20.0f, 1020.0f), 0.0f), FVector2f(-20.0f, 1020.0f));

	// 5. Крупному Unit — запас от самой грани.
	TestPoint(TEXT("Large unit keeps its margin"),
		ClampToWalls(Box, FVector2f(-20.0f, 500.0f), 35.0f), FVector2f(-35.0f, 500.0f));

	// 6. Отрицательный запас — предел, а не цель: переступившего на 10 см
	// он не утаскивает вглубь, а глубже разрешённого возвращает к пределу.
	TestPoint(TEXT("Negative margin: allowed, not pulled in"),
		ClampToWalls(Box, FVector2f(10.0f, 500.0f), -15.0f), FVector2f(10.0f, 500.0f));
	TestPoint(TEXT("Negative margin: deeper than allowed, back to the limit"),
		ClampToWalls(Box, FVector2f(30.0f, 500.0f), -15.0f), FVector2f(15.0f, 500.0f));

	// 7. Внутренний угол Г-образной стены со скосом: проходимая выемка.
	// Именно здесь сложение поправок от всех отрезков оставляло места,
	// которые не возвращал ни один отрезок.
	const FAlgonaWallSegment Inner[] =
	{
		MakeWall(FVector2f(0.0f, 60.0f), FVector2f(0.0f, 1000.0f), FVector2f(100.0f, 500.0f)),
		MakeWall(FVector2f(0.0f, 60.0f), FVector2f(60.0f, 0.0f), FVector2f(100.0f, 100.0f)),
		MakeWall(FVector2f(60.0f, 0.0f), FVector2f(1000.0f, 0.0f), FVector2f(500.0f, 100.0f)),
	};

	TestPoint(TEXT("In the notch: untouched"),
		ClampToWalls(Inner, FVector2f(40.0f, 40.0f), 0.0f), FVector2f(40.0f, 40.0f));
	TestPoint(TEXT("Pushed behind the bevel: back onto it"),
		ClampToWalls(Inner, FVector2f(20.0f, 20.0f), 0.0f), FVector2f(30.0f, 30.0f));
	TestPoint(TEXT("Pushed behind the face near the bevel: back"),
		ClampToWalls(Inner, FVector2f(-10.0f, 70.0f), 0.0f), FVector2f(0.0f, 70.0f));

	// 8. Узкий проход: точно посередине обе стены равноудалены — Unit на
	// месте; вдавленный в одну стену возвращается к ней, а не к другой.
	const FAlgonaWallSegment Strip[] =
	{
		MakeWall(FVector2f(0.0f, -500.0f), FVector2f(0.0f, 500.0f), FVector2f(50.0f, 0.0f)),
		MakeWall(FVector2f(100.0f, -500.0f), FVector2f(100.0f, 500.0f), FVector2f(50.0f, 0.0f)),
	};

	TestPoint(TEXT("Middle of a passage: untouched"),
		ClampToWalls(Strip, FVector2f(50.0f, 0.0f), 0.0f), FVector2f(50.0f, 0.0f));
	TestPoint(TEXT("Pushed into one side: back to it"),
		ClampToWalls(Strip, FVector2f(-8.0f, 0.0f), 0.0f), FVector2f(0.0f, 0.0f));

	// 9. Признак «напротив отрезка» для скольжения.
	TestTrue(
		TEXT("Point opposite the segment"),
		IsAlgonaWallFacingPoint(MakeWall(), FVector2f(-50.0f, 500.0f), 0.0f));
	TestFalse(
		TEXT("Point past the segment end"),
		IsAlgonaWallFacingPoint(MakeWall(), FVector2f(-50.0f, 1100.0f), 0.0f));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2UnitWallSpanTest,
	"Algona.P2.UnitWalls.LateralSpan",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2UnitWallSpanTest::RunTest(
	const FString& Parameters)
{
	// Unit в начале координат идёт по X; «вправо» от его хода — это +Y.
	const FVector2f Position = FVector2f::ZeroVector;
	const FVector2f Forward(1.0f, 0.0f);
	const FVector2f Right(0.0f, 1.0f);
	constexpr float MaxForward = 300.0f;
	constexpr float Inflate = 20.0f;

	float Low = 0.0f;
	float High = 0.0f;
	float ForwardDistance = 0.0f;

	// 1. Стена поперёк пути перекрывает ноль оси: впереди щели нет.
	TestTrue(
		TEXT("Wall across the path is seen"),
		GetAlgonaWallLateralSpan(
			MakeWall(FVector2f(200.0f, -100.0f), FVector2f(200.0f, 100.0f), FVector2f(0.0f, 0.0f)),
			Position,
			Forward,
			Right,
			MaxForward,
			Inflate,
			Low,
			High,
			ForwardDistance));
	TestEqual(TEXT("Across: low"), Low, -120.0f, WallTestTolerance);
	TestEqual(TEXT("Across: high"), High, 120.0f, WallTestTolerance);
	TestEqual(TEXT("Across: distance"), ForwardDistance, 200.0f, WallTestTolerance);
	TestTrue(TEXT("Across: blocks the way"), Low < 0.0f && High > 0.0f);

	// 2. Стена сбоку занимает интервал в стороне и ход не перекрывает —
	// иначе Unit тормозил бы всю дорогу вдоль стены.
	TestTrue(
		TEXT("Wall alongside is seen"),
		GetAlgonaWallLateralSpan(
			MakeWall(FVector2f(-500.0f, 60.0f), FVector2f(500.0f, 60.0f), FVector2f(0.0f, 0.0f)),
			Position,
			Forward,
			Right,
			MaxForward,
			Inflate,
			Low,
			High,
			ForwardDistance));
	TestEqual(TEXT("Alongside: low"), Low, 40.0f, WallTestTolerance);
	TestEqual(TEXT("Alongside: high"), High, 80.0f, WallTestTolerance);
	TestFalse(TEXT("Alongside: does not block"), Low < 0.0f && High > 0.0f);

	// 3. Стена позади не считается вовсе.
	TestFalse(
		TEXT("Wall behind is ignored"),
		GetAlgonaWallLateralSpan(
			MakeWall(FVector2f(-500.0f, -100.0f), FVector2f(-500.0f, 100.0f), FVector2f(0.0f, 0.0f)),
			Position,
			Forward,
			Right,
			MaxForward,
			Inflate,
			Low,
			High,
			ForwardDistance));

	// 4. Проход между двумя стенами: интервалы не пересекаются, поэтому
	// между ними остаётся щель, и она накрывает ноль оси.
	float LeftLow = 0.0f;
	float LeftHigh = 0.0f;
	float RightLow = 0.0f;
	float RightHigh = 0.0f;

	GetAlgonaWallLateralSpan(
		MakeWall(FVector2f(200.0f, -400.0f), FVector2f(200.0f, -120.0f), FVector2f(0.0f, 0.0f)),
		Position,
		Forward,
		Right,
		MaxForward,
		Inflate,
		LeftLow,
		LeftHigh,
		ForwardDistance);
	GetAlgonaWallLateralSpan(
		MakeWall(FVector2f(200.0f, 120.0f), FVector2f(200.0f, 400.0f), FVector2f(0.0f, 0.0f)),
		Position,
		Forward,
		Right,
		MaxForward,
		Inflate,
		RightLow,
		RightHigh,
		ForwardDistance);

	TestEqual(TEXT("Gate: left high"), LeftHigh, -100.0f, WallTestTolerance);
	TestEqual(TEXT("Gate: right low"), RightLow, 100.0f, WallTestTolerance);
	TestTrue(TEXT("Gate: intervals do not overlap"), LeftHigh < RightLow);
	TestTrue(
		TEXT("Gate: the gap covers the way ahead"),
		LeftHigh < 0.0f && RightLow > 0.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2UnitWallLocalityTest,
	"Algona.P2.UnitWalls.Locality",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2UnitWallLocalityTest::RunTest(
	const FString& Parameters)
{
	// Сетка стен свёрнута в тор. При двух отрезках в ней 1024 корзины,
	// 32 × 32, клетка 300 см: период тора — 9600 см. Стена B стоит ровно на
	// период дальше стены A и попадает в те же корзины, то есть приходит
	// кандидатом из клеток вокруг A. Поиск обязан её отбросить: именно такая
	// «чужая» стена переносила Unit из чистого поля на сотни метров.
	constexpr float TorusPeriodCm = 32.0f * AlgonaWallCellSizeCm;

	TArray<FAlgonaWallSegment> Walls;
	Walls.Add(MakeWall(
		FVector2f(0.0f, -100.0f),
		FVector2f(0.0f, 100.0f),
		FVector2f(-100.0f, 0.0f)));
	Walls.Add(MakeWall(
		FVector2f(TorusPeriodCm, -100.0f),
		FVector2f(TorusPeriodCm, 100.0f),
		FVector2f(TorusPeriodCm + 100.0f, 0.0f)));

	FAlgonaWallField Field;
	Field.Build(MoveTemp(Walls));

	auto CountNear = [&Field](const FVector& Location, float& OutStartX)
	{
		int32 Count = 0;
		Field.ForEachSegmentNear(
			Location,
			AlgonaWallQueryRadiusCm,
			[&](const FAlgonaWallSegment& Wall)
		{
			++Count;
			OutStartX = Wall.Start.X;
		});
		return Count;
	};

	float StartX = 0.0f;

	TestEqual(TEXT("Near A: only A is visited"),
		CountNear(FVector(50.0, 0.0, 0.0), StartX), 1);
	TestEqual(TEXT("Near A: the visited wall is A"), StartX, 0.0f, WallTestTolerance);

	TestEqual(TEXT("Near B: only B is visited"),
		CountNear(FVector(TorusPeriodCm + 50.0, 0.0, 0.0), StartX), 1);
	TestEqual(TEXT("Near B: the visited wall is B"), StartX, TorusPeriodCm, WallTestTolerance);

	// Открытое поле между ними: ни одной стены.
	TestEqual(TEXT("Open field: nothing is visited"),
		CountNear(FVector(TorusPeriodCm * 0.5, 0.0, 0.0), StartX), 0);

	// Та же стена, но на другой высоте (мост над головой): не своя.
	TestEqual(TEXT("Another level: nothing is visited"),
		CountNear(FVector(50.0, 0.0, 1000.0), StartX), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
