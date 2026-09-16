#include "Game/AlgonaPlayerController.h"

#include "Camera/AlgonaRTSCameraActor.h"
#include "Core/AlgonaSimulationSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"

namespace
{
	// Нажатие ЛКМ считается кликом, если мышь сдвинулась меньше, пиксели.
	// Больший сдвиг — будущая рамка выбора.
	constexpr float ClickMaxMousePixels = 6.0f;

	// Эталонная высота Unit для попадания курсором, см. Позже — из типа Unit.
	constexpr double UnitPickHeight = 170.0;

	// Минимальный радиус попадания, чтобы по Unit было легко кликнуть, см.
	constexpr double MinPickRadius = 50.0;

	// Круги выбора: контур под каждым Unit выбранного Squad.
	constexpr int32 SelectionCircleSegments = 24;
	constexpr float SelectionCircleThickness = 3.0f;

	// Круг выбора крупнее самого Unit, чтобы был заметен.
	constexpr double SelectionCircleRadiusScale = 1.5;
	constexpr double SelectionCircleHeightOffset = 3.0;

	// Пересечение луча с вертикальным цилиндром (основание Base, радиус,
	// высота). OutT — расстояние вдоль луча до точки входа.
	bool IntersectRayVerticalCylinder(
		const FVector& Origin,
		const FVector& Direction,
		const FVector& Base,
		double Radius,
		double Height,
		double& OutT)
	{
		double EnterT = 0.0;
		double ExitT = TNumericLimits<double>::Max();

		// По высоте: участок луча между основанием и верхом цилиндра.
		if (FMath::IsNearlyZero(Direction.Z))
		{
			if (Origin.Z < Base.Z || Origin.Z > Base.Z + Height)
			{
				return false;
			}
		}
		else
		{
			double BottomT = (Base.Z - Origin.Z) / Direction.Z;
			double TopT = (Base.Z + Height - Origin.Z) / Direction.Z;
			if (BottomT > TopT)
			{
				Swap(BottomT, TopT);
			}

			EnterT = FMath::Max(EnterT, BottomT);
			ExitT = FMath::Min(ExitT, TopT);
		}

		// В плане: расстояние от оси цилиндра не больше радиуса
		// (квадратное уравнение по t).
		const double OffsetX = Origin.X - Base.X;
		const double OffsetY = Origin.Y - Base.Y;
		const double A = Direction.X * Direction.X + Direction.Y * Direction.Y;
		const double C = OffsetX * OffsetX + OffsetY * OffsetY - Radius * Radius;

		if (A <= UE_DOUBLE_SMALL_NUMBER)
		{
			// Вертикальный луч: либо весь внутри цилиндра, либо мимо.
			if (C > 0.0)
			{
				return false;
			}
		}
		else
		{
			const double B = 2.0 * (OffsetX * Direction.X + OffsetY * Direction.Y);
			const double Discriminant = B * B - 4.0 * A * C;
			if (Discriminant < 0.0)
			{
				return false;
			}

			const double SqrtDiscriminant = FMath::Sqrt(Discriminant);
			EnterT = FMath::Max(EnterT, (-B - SqrtDiscriminant) / (2.0 * A));
			ExitT = FMath::Min(ExitT, (-B + SqrtDiscriminant) / (2.0 * A));
		}

		if (EnterT > ExitT)
		{
			return false;
		}

		OutT = EnterT;
		return true;
	}
}

void AAlgonaPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// Курсор нужен для выбора Squad и приказов.
	bShowMouseCursor = true;
}

void AAlgonaPlayerController::SetRTSCamera(
	AAlgonaRTSCameraActor* InCamera)
{
	CameraActor = InCamera;
	SetViewTarget(CameraActor);
}

void AAlgonaPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	UpdateCameraInput(DeltaTime);
	UpdateSelectionInput();
	DrawSelectedSquads();
}

void AAlgonaPlayerController::UpdateCameraInput(float DeltaTime)
{
	if (!CameraActor)
	{
		return;
	}

	float ForwardInput = 0.0f;
	float RightInput = 0.0f;

	if (IsInputKeyDown(EKeys::W))
	{
		ForwardInput += 1.0f;
	}
	if (IsInputKeyDown(EKeys::S))
	{
		ForwardInput -= 1.0f;
	}
	if (IsInputKeyDown(EKeys::D))
	{
		RightInput += 1.0f;
	}
	if (IsInputKeyDown(EKeys::A))
	{
		RightInput -= 1.0f;
	}

	if (ForwardInput != 0.0f || RightInput != 0.0f)
	{
		CameraActor->MoveGroundFocus(
			ForwardInput,
			RightInput,
			DeltaTime);
	}

	if (IsInputKeyDown(EKeys::MiddleMouseButton))
	{
		float MouseDeltaX = 0.0f;
		float UnusedMouseDeltaY = 0.0f;
		GetInputMouseDelta(MouseDeltaX, UnusedMouseDeltaY);

		if (MouseDeltaX != 0.0f)
		{
			CameraActor->RotateYaw(
				MouseDeltaX * CameraRotationSensitivity);
		}
	}

	const float ZoomInput =
		GetInputAnalogKeyState(EKeys::MouseWheelAxis);

	if (ZoomInput != 0.0f)
	{
		CameraActor->Zoom(ZoomInput);
	}
}

void AAlgonaPlayerController::UpdateSelectionInput()
{
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	const bool bHasMouse = GetMousePosition(MouseX, MouseY);

	if (WasInputKeyJustPressed(EKeys::LeftMouseButton) && bHasMouse)
	{
		LeftMousePressPosition = FVector2D(MouseX, MouseY);
		bLeftMousePressed = true;
	}

	if (!bLeftMousePressed
		|| !WasInputKeyJustReleased(EKeys::LeftMouseButton))
	{
		return;
	}

	bLeftMousePressed = false;

	// Сдвиг больше порога — рамка выбора (следующая часть шага).
	if (!bHasMouse
		|| FVector2D::Distance(LeftMousePressPosition, FVector2D(MouseX, MouseY))
			> ClickMaxMousePixels)
	{
		return;
	}

	// Клик: без Shift выбор заменяется, с Shift Squad добавляется к выбору.
	// Клик по пустой земле без Shift снимает выбор.
	const bool bAddToSelection =
		IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift);

	const int32 PickedSquadId = PickSquadUnderCursor();

	if (!bAddToSelection)
	{
		SelectedSquadIds.Reset();
	}

	if (PickedSquadId != INDEX_NONE)
	{
		SelectedSquadIds.AddUnique(PickedSquadId);
	}
}

int32 AAlgonaPlayerController::PickSquadUnderCursor() const
{
	const UWorld* World = GetWorld();
	const UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;

	FVector RayOrigin;
	FVector RayDirection;

	if (!Simulation
		|| !DeprojectMousePositionToWorld(RayOrigin, RayDirection)
		|| RayDirection.Z >= -UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return INDEX_NONE;
	}

	// Кандидаты: Unit в ячейках Unit Grid вдоль участка луча от высоты
	// Unit до земли (земля пока плоская, Z = 0).
	const double GroundT = -RayOrigin.Z / RayDirection.Z;
	const double TopT = FMath::Max(
		(UnitPickHeight - RayOrigin.Z) / RayDirection.Z,
		0.0);

	if (GroundT < 0.0)
	{
		return INDEX_NONE;
	}

	const FVector GroundPoint = RayOrigin + RayDirection * GroundT;
	const FVector TopPoint = RayOrigin + RayDirection * TopT;

	const FVector2D BoundsMin(
		FMath::Min(GroundPoint.X, TopPoint.X) - MinPickRadius * 2.0,
		FMath::Min(GroundPoint.Y, TopPoint.Y) - MinPickRadius * 2.0);
	const FVector2D BoundsMax(
		FMath::Max(GroundPoint.X, TopPoint.X) + MinPickRadius * 2.0,
		FMath::Max(GroundPoint.Y, TopPoint.Y) + MinPickRadius * 2.0);

	TArray<uint32> CandidateUnitIds;
	Simulation->QueryUnitIdsInBounds(BoundsMin, BoundsMax, CandidateUnitIds);

	// Из всех Unit, которые пересекает луч, выбирается ближайший к камере.
	int32 PickedSquadId = INDEX_NONE;
	double NearestT = TNumericLimits<double>::Max();

	for (const uint32 UnitId : CandidateUnitIds)
	{
		FVector UnitPosition;
		if (!Simulation->GetUnitPosition(UnitId, UnitPosition))
		{
			continue;
		}

		const int32 SquadId = Simulation->GetUnitSquadId(UnitId);
		const FAlgonaSquad* Squad = Simulation->FindSquad(SquadId);
		if (!Squad)
		{
			continue;
		}

		const double PickRadius = FMath::Max(
			static_cast<double>(Squad->UnitRadius),
			MinPickRadius);

		double HitT = 0.0;
		if (IntersectRayVerticalCylinder(
				RayOrigin,
				RayDirection,
				UnitPosition,
				PickRadius,
				UnitPickHeight,
				HitT)
			&& HitT < NearestT)
		{
			NearestT = HitT;
			PickedSquadId = SquadId;
		}
	}

	return PickedSquadId;
}

void AAlgonaPlayerController::DrawSelectedSquads() const
{
	const UWorld* World = GetWorld();
	const UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;

	if (!Simulation)
	{
		return;
	}

	// Временная отрисовка отладочными линиями: достаточно для небольшого
	// выбора. Для выбора рамкой многих Squad круги перейдут на инстансы.
	for (const int32 SquadId : SelectedSquadIds)
	{
		const FAlgonaSquad* Squad = Simulation->FindSquad(SquadId);
		if (!Squad)
		{
			continue;
		}

		for (const uint32 UnitId : Squad->ActiveUnitIds)
		{
			FVector UnitPosition;
			if (!Simulation->GetUnitPosition(UnitId, UnitPosition))
			{
				continue;
			}

			DrawDebugCircle(
				World,
				UnitPosition + FVector(0.0, 0.0, SelectionCircleHeightOffset),
				Squad->UnitRadius * SelectionCircleRadiusScale,
				SelectionCircleSegments,
				FColor::Red,
				false,
				-1.0f,
				0,
				SelectionCircleThickness,
				FVector::ForwardVector,
				FVector::RightVector,
				false);
		}
	}
}
