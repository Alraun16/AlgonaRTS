#include "Army/AlgonaUnitSteering.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr float SteeringTestDeltaTime = 0.025f;
	constexpr float SteeringTestMaxSpeed = 375.0f;
	constexpr float SteeringTestTolerance = 0.01f;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SteeringDesiredVelocityTest,
	"Algona.P2.Steering.DesiredVelocity",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SteeringDesiredVelocityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	using namespace AlgonaUnitSteering;

	// Далёкий неподвижный слот: полная скорость в сторону слота.
	FVector2f Velocity = ComputeDesiredVelocity(
		FVector2f(1000.0f, 0.0f), FVector2f::ZeroVector,
		SteeringTestMaxSpeed, SteeringTestDeltaTime);
	TestEqual(TEXT("Far slot: full speed"), Velocity.X, SteeringTestMaxSpeed, SteeringTestTolerance);
	TestEqual(TEXT("Far slot: no sideways speed"), Velocity.Y, 0.0f, SteeringTestTolerance);

	// Слот в 5 см: торможение, sqrt(2 * 800 * 5) = 89.44 см/с.
	Velocity = ComputeDesiredVelocity(
		FVector2f(0.0f, 5.0f), FVector2f::ZeroVector,
		SteeringTestMaxSpeed, SteeringTestDeltaTime);
	TestEqual(TEXT("Near slot: arrival braking"), Velocity.Y, 89.4427f, SteeringTestTolerance);

	// Слот в 0.5 см: скорость ровно на один тик, без перелёта.
	Velocity = ComputeDesiredVelocity(
		FVector2f(0.5f, 0.0f), FVector2f::ZeroVector,
		SteeringTestMaxSpeed, SteeringTestDeltaTime);
	TestEqual(TEXT("Very near slot: lands exactly in one tick"), Velocity.X * SteeringTestDeltaTime, 0.5f, 0.001f);

	// Движущийся слот без отставания: Unit идёт со скоростью слота.
	Velocity = ComputeDesiredVelocity(
		FVector2f::ZeroVector, FVector2f(300.0f, 0.0f),
		SteeringTestMaxSpeed, SteeringTestDeltaTime);
	TestEqual(TEXT("Moving slot: follows slot speed"), Velocity.X, 300.0f, SteeringTestTolerance);

	// Отставание на марше: скорость не превышает максимальную.
	Velocity = ComputeDesiredVelocity(
		FVector2f(1000.0f, 0.0f), FVector2f(300.0f, 0.0f),
		SteeringTestMaxSpeed, SteeringTestDeltaTime);
	TestEqual(TEXT("Catching up: capped at max speed"), Velocity.Size(), SteeringTestMaxSpeed, SteeringTestTolerance);

	// Нулевая скорость Squad: Unit не двигается.
	Velocity = ComputeDesiredVelocity(
		FVector2f(100.0f, 0.0f), FVector2f::ZeroVector,
		0.0f, SteeringTestDeltaTime);
	TestEqual(TEXT("Zero max speed: no movement"), Velocity.Size(), 0.0f, SteeringTestTolerance);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlgonaP2SteeringYawTest,
	"Algona.P2.Steering.Yaw",
	EAutomationTestFlags_ApplicationContextMask
		| EAutomationTestFlags::SmokeFilter);

bool FAlgonaP2SteeringYawTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	using namespace AlgonaUnitSteering;

	const float MaxStep =
		FMath::DegreesToRadians(MaxTurnRateDegrees) * SteeringTestDeltaTime;

	// Большой поворот ограничен шагом: 540 градусов/с * 0.025 с = 13.5 градуса.
	float Yaw = StepYawTowards(0.0f, static_cast<float>(UE_HALF_PI), MaxStep);
	TestEqual(TEXT("Turn is limited per tick"), FMath::RadiansToDegrees(Yaw), 13.5f, SteeringTestTolerance);

	// Малый поворот выполняется сразу.
	Yaw = StepYawTowards(0.0f, 0.1f, MaxStep);
	TestEqual(TEXT("Small turn completes"), Yaw, 0.1f, 0.0001f);

	// Через границу -180/180 поворот идёт кратчайшим путём: 170 -> -170 это +20 градусов.
	const float LargeStep = FMath::DegreesToRadians(30.0f);
	Yaw = StepYawTowards(FMath::DegreesToRadians(170.0f), FMath::DegreesToRadians(-170.0f), LargeStep);
	TestEqual(TEXT("Shortest turn across 180"), FMath::RadiansToDegrees(Yaw), -170.0f, SteeringTestTolerance);

	// 170 -> -150 это +40 градусов, шаг 30: результат 200 = -160 градусов.
	Yaw = StepYawTowards(FMath::DegreesToRadians(170.0f), FMath::DegreesToRadians(-150.0f), LargeStep);
	TestEqual(TEXT("Limited turn across 180"), FMath::RadiansToDegrees(Yaw), -160.0f, SteeringTestTolerance);

	// Уже смотрит в цель: угол не меняется.
	Yaw = StepYawTowards(1.0f, 1.0f, MaxStep);
	TestEqual(TEXT("Same yaw stays unchanged"), Yaw, 1.0f, 0.0001f);

	return true;
}

#endif
