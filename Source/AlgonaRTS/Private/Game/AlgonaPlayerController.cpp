#include "Game/AlgonaPlayerController.h"

#include "Camera/AlgonaRTSCameraActor.h"
#include "Core/AlgonaSimulationSubsystem.h"
#include "Presentation/AlgonaSelectionPresentationActor.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"
#include "SceneView.h"

namespace
{
	// Нажатие ЛКМ считается кликом, если мышь сдвинулась меньше, пиксели.
	// Больший сдвиг — рамка выбора.
	constexpr float ClickMaxMousePixels = 6.0f;

	// Эталонная высота Unit для попадания курсором, см. Позже — из типа Unit.
	constexpr double UnitPickHeight = 170.0;

	// Радиус попадания по Unit, см. Берётся с запасом от интервала строя
	// (больше половины интервала), чтобы клик между строками и рядом с Unit
	// попадал в Squad. Пересечения зон соседних Unit решает выбор
	// ближайшего к камере.
	constexpr double PickRadiusSpacingFactor = 0.55;
	constexpr double MinPickRadius = 50.0;
	constexpr double MaxPickRadius = 300.0;

	// Круги выбора: крупнее самого Unit, чтобы были заметны; чуть над землёй.
	constexpr double SelectionRingRadiusScale = 1.5;

	// Отметка точки приказа вписана в круг в полтора раза шире круга выбора.
	constexpr double OrderMarkerRadiusScale = 1.5;
	constexpr double SelectionRingHeightOffset = 3.0;

	// Мёртвая зона ПКМ, пиксели: пока курсор ближе к точке нажатия,
	// это клик, и угол курсора вокруг цели ещё неустойчив.
	constexpr double OrderDeadZonePixels = 20.0;

	// Ширина строя: один шаг длины строки на столько пикселей сдвига курсора.
	constexpr double RowLengthStepPixels = 40.0;

	// Ограничение длины строки для приказа мышью.
	constexpr int32 MaxOrderRowLength = 50;

	// Стрелка предпросмотра: отступ от передней строки и длина, см.
	constexpr double PreviewArrowOffset = 150.0;
	constexpr double PreviewArrowLength = 400.0;

	bool IsShiftDown(const APlayerController& Controller)
	{
		return Controller.IsInputKeyDown(EKeys::LeftShift)
			|| Controller.IsInputKeyDown(EKeys::RightShift);
	}

	double GetUnitPickRadius(const FAlgonaSquad& Squad)
	{
		const double Spacing = FMath::Max(
			Squad.FormationParams.SlotSpacing,
			Squad.FormationParams.RowSpacing);

		return FMath::Clamp(
			FMath::Max(static_cast<double>(Squad.UnitRadius), Spacing * PickRadiusSpacingFactor),
			MinPickRadius,
			MaxPickRadius);
	}

	// Пересекает ли отрезок AB прямоугольник [Min, Max] (отсечение
	// Лианга–Барски: сужаем допустимый участок отрезка по каждой оси).
	bool SegmentIntersectsBox(
		const FVector2D& A,
		const FVector2D& B,
		const FVector2D& Min,
		const FVector2D& Max)
	{
		const FVector2D Delta = B - A;
		double EnterT = 0.0;
		double ExitT = 1.0;

		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			if (FMath::IsNearlyZero(Delta[Axis]))
			{
				if (A[Axis] < Min[Axis] || A[Axis] > Max[Axis])
				{
					return false;
				}
				continue;
			}

			double T0 = (Min[Axis] - A[Axis]) / Delta[Axis];
			double T1 = (Max[Axis] - A[Axis]) / Delta[Axis];
			if (T0 > T1)
			{
				Swap(T0, T1);
			}

			EnterT = FMath::Max(EnterT, T0);
			ExitT = FMath::Min(ExitT, T1);
			if (EnterT > ExitT)
			{
				return false;
			}
		}

		return true;
	}

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

	// Круги выбора — только у локального игрока.
	if (IsLocalController())
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Owner = this;
		SpawnParameters.ObjectFlags |= RF_Transient;

		SelectionPresentation =
			GetWorld()->SpawnActor<AAlgonaSelectionPresentationActor>(SpawnParameters);
	}
}

bool AAlgonaPlayerController::GetSelectionBox(
	FVector2D& OutMin,
	FVector2D& OutMax) const
{
	if (!bBoxSelecting)
	{
		return false;
	}

	OutMin = FVector2D::Min(LeftMousePressPosition, LeftMouseCurrentPosition);
	OutMax = FVector2D::Max(LeftMousePressPosition, LeftMouseCurrentPosition);
	return true;
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
	UpdateOrderInput();
	UpdateSelectionRings();
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
		LeftMouseCurrentPosition = LeftMousePressPosition;
		bLeftMousePressed = true;
		bBoxSelecting = false;
	}

	if (!bLeftMousePressed)
	{
		return;
	}

	// Пока ЛКМ зажата: сдвиг больше порога превращает клик в рамку
	// (и дальше остаётся рамкой, даже если мышь вернулась назад).
	if (bHasMouse)
	{
		LeftMouseCurrentPosition = FVector2D(MouseX, MouseY);
		if (FVector2D::Distance(LeftMousePressPosition, LeftMouseCurrentPosition)
			> ClickMaxMousePixels)
		{
			bBoxSelecting = true;
		}
	}

	if (!WasInputKeyJustReleased(EKeys::LeftMouseButton))
	{
		return;
	}

	// Без Shift выбор заменяется, с Shift Squad добавляются к выбору.
	// Клик по пустой земле или пустая рамка без Shift снимают выбор.
	const bool bAddToSelection = IsShiftDown(*this);

	TArray<int32> PickedSquadIds;

	if (bBoxSelecting)
	{
		FVector2D BoxMin;
		FVector2D BoxMax;
		GetSelectionBox(BoxMin, BoxMax);
		CollectSquadsInScreenBox(BoxMin, BoxMax, PickedSquadIds);
	}
	else
	{
		const int32 PickedSquadId = PickSquadUnderCursor();
		if (PickedSquadId != INDEX_NONE)
		{
			PickedSquadIds.Add(PickedSquadId);
		}
	}

	bLeftMousePressed = false;
	bBoxSelecting = false;

	if (!bAddToSelection)
	{
		SelectedSquadIds.Reset();
	}

	for (const int32 SquadId : PickedSquadIds)
	{
		SelectedSquadIds.AddUnique(SquadId);
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

	// Запас — наибольший возможный радиус попадания.
	const FVector2D BoundsMin(
		FMath::Min(GroundPoint.X, TopPoint.X) - MaxPickRadius,
		FMath::Min(GroundPoint.Y, TopPoint.Y) - MaxPickRadius);
	const FVector2D BoundsMax(
		FMath::Max(GroundPoint.X, TopPoint.X) + MaxPickRadius,
		FMath::Max(GroundPoint.Y, TopPoint.Y) + MaxPickRadius);

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

		double HitT = 0.0;
		if (IntersectRayVerticalCylinder(
				RayOrigin,
				RayDirection,
				UnitPosition,
				GetUnitPickRadius(*Squad),
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

void AAlgonaPlayerController::CollectSquadsInScreenBox(
	const FVector2D& BoxMin,
	const FVector2D& BoxMax,
	TArray<int32>& OutSquadIds) const
{
	const UWorld* World = GetWorld();
	const UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;
	const ULocalPlayer* LocalPlayer = GetLocalPlayer();

	if (!Simulation
		|| !LocalPlayer
		|| !LocalPlayer->ViewportClient
		|| !LocalPlayer->ViewportClient->Viewport)
	{
		return;
	}

	// Матрица камеры берётся один раз на всю проверку, а не на каждый Unit.
	FSceneViewProjectionData ProjectionData;
	if (!LocalPlayer->GetProjectionData(
			LocalPlayer->ViewportClient->Viewport,
			ProjectionData))
	{
		return;
	}

	const FIntRect ViewRect = ProjectionData.GetConstrainedViewRect();
	const FMatrix ViewProjectionMatrix = ProjectionData.ComputeViewProjectionMatrix();

	const FVector CameraRight = PlayerCameraManager
		? PlayerCameraManager->GetCameraRotation().RotateVector(FVector::RightVector)
		: FVector::RightVector;

	// Каждый Unit — экранный отрезок «ноги–голова», утолщённый на экранный
	// радиус Unit. Утолщение учитывается расширением рамки на этот радиус.
	// Squad выбирается по первому попавшему Unit, остальные не проверяются.
	// Проверка идёт только при отпускании ЛКМ, поэтому полный перебор допустим.
	const int32 SquadCount = Simulation->GetSquadCount();

	for (int32 SquadId = 0; SquadId < SquadCount; ++SquadId)
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

			FVector2D FootScreen;
			FVector2D HeadScreen;
			FVector2D SideScreen;

			// Точки за камерой не проецируются — такой Unit не в рамке.
			if (!FSceneView::ProjectWorldToScreen(
					UnitPosition,
					ViewRect,
					ViewProjectionMatrix,
					FootScreen)
				|| !FSceneView::ProjectWorldToScreen(
					UnitPosition + FVector(0.0, 0.0, UnitPickHeight),
					ViewRect,
					ViewProjectionMatrix,
					HeadScreen)
				|| !FSceneView::ProjectWorldToScreen(
					UnitPosition + CameraRight * Squad->UnitRadius,
					ViewRect,
					ViewProjectionMatrix,
					SideScreen))
			{
				continue;
			}

			const double ScreenRadius = FVector2D::Distance(FootScreen, SideScreen);
			const FVector2D Expand(ScreenRadius, ScreenRadius);

			if (SegmentIntersectsBox(
					FootScreen,
					HeadScreen,
					BoxMin - Expand,
					BoxMax + Expand))
			{
				OutSquadIds.Add(SquadId);
				break;
			}
		}
	}
}

void AAlgonaPlayerController::UpdateSelectionRings()
{
	if (!SelectionPresentation)
	{
		return;
	}

	const UWorld* World = GetWorld();
	const UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;

	RingTransforms.Reset();

	if (Simulation)
	{
		for (const int32 SquadId : SelectedSquadIds)
		{
			const FAlgonaSquad* Squad = Simulation->FindSquad(SquadId);
			if (!Squad)
			{
				continue;
			}

			const FVector RingScale = AAlgonaSelectionPresentationActor::GetRingScale(
				Squad->UnitRadius * SelectionRingRadiusScale);

			for (const uint32 UnitId : Squad->ActiveUnitIds)
			{
				FVector UnitPosition;
				if (Simulation->GetUnitPosition(UnitId, UnitPosition))
				{
					RingTransforms.Emplace(
						FQuat::Identity,
						UnitPosition + FVector(0.0, 0.0, SelectionRingHeightOffset),
						RingScale);
				}
			}
		}
	}

	// Предпросмотр приказа: круги слотов в целевой позиции. Слоты строятся
	// тем же генератором формы и той же формулой, что в Simulation.
	const FAlgonaSquad* OrderSquad = Simulation && bRightMousePressed && bOrderDragged
		? Simulation->FindSquad(OrderSquadId)
		: nullptr;

	if (OrderSquad)
	{
		FAlgonaFormationParams PreviewParams = OrderSquad->FormationParams;
		PreviewParams.RowLength = OrderRowLength;

		FAlgonaFormationLayout PreviewLayout;
		BuildAlgonaFormationLayout(
			PreviewParams,
			OrderSquad->ActiveUnitIds.Num(),
			PreviewLayout);

		const FVector RingScale = AAlgonaSelectionPresentationActor::GetRingScale(
			OrderSquad->UnitRadius * SelectionRingRadiusScale);

		for (const FAlgonaFormationSlot& Slot : PreviewLayout.Slots)
		{
			RingTransforms.Emplace(
				FQuat::Identity,
				GetAlgonaSlotWorldLocation(OrderTarget, OrderForward, Slot.LocalOffset)
					+ FVector(0.0, 0.0, SelectionRingHeightOffset),
				RingScale);
		}
	}

	SelectionPresentation->SetRingTransforms(RingTransforms);
}

bool AAlgonaPlayerController::GetCursorGroundPoint(FVector& OutPoint) const
{
	FVector RayOrigin;
	FVector RayDirection;

	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection)
		|| RayDirection.Z >= -UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return false;
	}

	OutPoint = RayOrigin + RayDirection * (-RayOrigin.Z / RayDirection.Z);
	OutPoint.Z = 0.0;
	return true;
}

void AAlgonaPlayerController::UpdateOrderInput()
{
	const UWorld* World = GetWorld();
	const UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	const bool bHasMouse = GetMousePosition(MouseX, MouseY);

	// Нажатие ПКМ: цель — точка земли под курсором.
	if (WasInputKeyJustPressed(EKeys::RightMouseButton))
	{
		FVector GroundPoint;
		if (!Simulation
			|| SelectedSquadIds.IsEmpty()
			|| !bHasMouse
			|| !GetCursorGroundPoint(GroundPoint))
		{
			return;
		}

		bRightMousePressed = true;
		bOrderDragged = false;
		bRotationAnchored = false;
		bWidthMode = false;
		RightMousePressPosition = FVector2D(MouseX, MouseY);
		OrderTarget = GroundPoint;
		OrderSquadId = INDEX_NONE;

		// Поворот и ширина — только для одного Squad. Начальный вектор
		// направления — от текущего центра Squad к цели.
		if (SelectedSquadIds.Num() == 1)
		{
			if (const FAlgonaSquad* Squad = Simulation->FindSquad(SelectedSquadIds[0]))
			{
				OrderSquadId = Squad->SquadId;
				OrderRowLength = Squad->FormationParams.RowLength;

				const FVector ToTarget =
					(OrderTarget - Squad->CenterLocation).GetSafeNormal2D();
				OrderForward = ToTarget.IsNearlyZero()
					? Squad->GetForwardDirection2D()
					: ToTarget;
			}
		}
	}

	if (!bRightMousePressed)
	{
		return;
	}

	const double ScreenDistance = bHasMouse
		? FVector2D::Distance(RightMousePressPosition, FVector2D(MouseX, MouseY))
		: 0.0;

	if (ScreenDistance > OrderDeadZonePixels)
	{
		bOrderDragged = true;
	}

	const FAlgonaSquad* OrderSquad =
		Simulation ? Simulation->FindSquad(OrderSquadId) : nullptr;

	if (OrderSquad && bHasMouse)
	{
		const bool bShiftDown = IsShiftDown(*this);

		if (bShiftDown && !bWidthMode)
		{
			// Вход в режим ширины: направление фиксируется.
			bWidthMode = true;
			bOrderDragged = true;
			WidthInitialScreenDistance = ScreenDistance;
			WidthInitialRowLength = OrderRowLength;
		}
		else if (!bShiftDown && bWidthMode)
		{
			// Выход из режима ширины: вращение привязывается заново к
			// текущему направлению, поэтому скачка нет.
			bWidthMode = false;
			bRotationAnchored = false;
		}

		if (bWidthMode)
		{
			// Дальше от цели — шире строй, ближе — уже. Центр не сдвигается.
			const int32 RowLengthSteps = FMath::RoundToInt32(
				(ScreenDistance - WidthInitialScreenDistance) / RowLengthStepPixels);

			OrderRowLength = FMath::Clamp(
				WidthInitialRowLength + RowLengthSteps,
				AlgonaFormationLimits::MinRowLength,
				FMath::Max(
					FMath::Min(OrderSquad->ActiveUnitIds.Num(), MaxOrderRowLength),
					AlgonaFormationLimits::MinRowLength));
		}
		else if (ScreenDistance > OrderDeadZonePixels)
		{
			// Вращение: угол курсора вокруг цели на плоскости земли.
			// Направление = начальный вектор, повёрнутый на разницу углов
			// (без накопления приращений).
			FVector CursorGroundPoint;
			if (GetCursorGroundPoint(CursorGroundPoint))
			{
				const FVector FromTarget = CursorGroundPoint - OrderTarget;
				const double CursorAngleDegrees = FMath::RadiansToDegrees(
					FMath::Atan2(FromTarget.Y, FromTarget.X));

				if (!bRotationAnchored)
				{
					bRotationAnchored = true;
					RotationInitialForward = OrderForward;
					RotationInitialAngleDegrees = CursorAngleDegrees;
				}
				else
				{
					const double DeltaDegrees = FRotator::NormalizeAxis(
						CursorAngleDegrees - RotationInitialAngleDegrees);

					OrderForward = RotationInitialForward
						.RotateAngleAxis(DeltaDegrees, FVector::UpVector)
						.GetSafeNormal2D();
				}
			}
		}
	}

	if (WasInputKeyJustReleased(EKeys::RightMouseButton))
	{
		SubmitMoveOrder();
		bRightMousePressed = false;
		bOrderDragged = false;
		bRotationAnchored = false;
		bWidthMode = false;
		OrderSquadId = INDEX_NONE;
	}
}

void AAlgonaPlayerController::SubmitMoveOrder()
{
	const UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;

	if (!Simulation || SelectedSquadIds.IsEmpty())
	{
		return;
	}

	// Пока приказы идут в Simulation напрямую; в мультиплеере — через
	// запрос клиента серверу, дальше та же очередь команд.
	const FAlgonaSquad* OrderSquad = Simulation->FindSquad(OrderSquadId);

	if (OrderSquad && bOrderDragged)
	{
		// В приказ уходит ровно последнее показанное состояние.
		const int32 RowLength =
			OrderRowLength != OrderSquad->FormationParams.RowLength ? OrderRowLength : 0;

		Simulation->SubmitMoveSquadCommand(
			OrderSquadId,
			OrderTarget,
			OrderForward,
			RowLength);
		return;
	}

	// Клик ПКМ (или несколько Squad): направление — по пути,
	// раскладку точек группы считает Simulation.
	Simulation->SubmitMoveGroupCommand(SelectedSquadIds, OrderTarget);
}

bool AAlgonaPlayerController::GetOrderPreviewArrow(
	FVector& OutStart,
	FVector& OutEnd) const
{
	const UWorld* World = GetWorld();
	const UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;
	const FAlgonaSquad* OrderSquad =
		Simulation && bRightMousePressed && bOrderDragged
			? Simulation->FindSquad(OrderSquadId)
			: nullptr;

	if (!OrderSquad)
	{
		return false;
	}

	// Стрелка стоит перед передней строкой. Положение передней строки
	// зависит от длины строки, поэтому берётся из раскладки предпросмотра.
	FAlgonaFormationParams PreviewParams = OrderSquad->FormationParams;
	PreviewParams.RowLength = OrderRowLength;

	FAlgonaFormationLayout PreviewLayout;
	BuildAlgonaFormationLayout(
		PreviewParams,
		OrderSquad->ActiveUnitIds.Num(),
		PreviewLayout);

	OutStart = OrderTarget
		+ OrderForward * (PreviewLayout.FrontRowLocalX + PreviewArrowOffset)
		+ FVector(0.0, 0.0, SelectionRingHeightOffset);
	OutEnd = OutStart + OrderForward * PreviewArrowLength;
	return true;
}

bool AAlgonaPlayerController::ShouldShowSingleSquadOnlyMessage() const
{
	return bRightMousePressed && bOrderDragged && SelectedSquadIds.Num() > 1;
}

void AAlgonaPlayerController::GetOrderTargetMarkers(
	TArray<FAlgonaOrderTargetMarker>& OutMarkers) const
{
	const UWorld* World = GetWorld();
	const UAlgonaSimulationSubsystem* Simulation =
		World ? World->GetSubsystem<UAlgonaSimulationSubsystem>() : nullptr;

	if (!Simulation)
	{
		return;
	}

	// Отметка видна, пока Squad выбран и идёт к цели.
	for (const int32 SquadId : SelectedSquadIds)
	{
		const FAlgonaSquad* Squad = Simulation->FindSquad(SquadId);
		if (!Squad || !Squad->bHasMoveTarget)
		{
			continue;
		}

		FAlgonaOrderTargetMarker& Marker = OutMarkers.AddDefaulted_GetRef();
		Marker.Location = Squad->TargetCenterLocation
			+ FVector(0.0, 0.0, SelectionRingHeightOffset);

		// Отметка со стрелками вписана в круг в полтора раза шире
		// круга выбора под Unit.
		Marker.Radius = Squad->UnitRadius
			* SelectionRingRadiusScale
			* OrderMarkerRadiusScale;
	}
}
