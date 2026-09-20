#include "Game/AlgonaHUD.h"

#include "Game/AlgonaPlayerController.h"

#include "Engine/Canvas.h"

namespace
{
	// Отметка точки приказа: кружок в центре и четыре стрелки к нему.
	// Радиус кружка — доля от общего радиуса отметки.
	constexpr double OrderMarkerCircleRatio = 0.3;
	constexpr int32 OrderMarkerCircleSegments = 16;

	// Зазор между остриём стрелки и кружком, доля радиуса отметки.
	constexpr double OrderMarkerArrowGap = 0.15;
	constexpr double OrderMarkerArrowHeadRatio = 0.18;

	const FLinearColor OrderMarkerColor(1.0f, 0.15f, 0.15f, 0.9f);
	constexpr float OrderMarkerThickness = 2.0f;
}

void AAlgonaHUD::DrawHUD()
{
	Super::DrawHUD();

	const AAlgonaPlayerController* AlgonaController =
		Cast<AAlgonaPlayerController>(GetOwningPlayerController());

	if (!AlgonaController)
	{
		return;
	}

	DrawOrderPreviewArrow(*AlgonaController);
	DrawOrderTargetMarkers(*AlgonaController);

	if (AlgonaController->ShouldShowSingleSquadOnlyMessage())
	{
		const FString Message =
			TEXT("Direction and width can be set for one squad only");
		float TextWidth = 0.0f;
		float TextHeight = 0.0f;
		const float TextScale = 1.5f;
		GetTextSize(Message, TextWidth, TextHeight, nullptr, TextScale);
		DrawText(
			Message,
			FLinearColor::Red,
			(Canvas->ClipX - TextWidth) * 0.5f,
			Canvas->ClipY * 0.15f,
			nullptr,
			TextScale);
	}

	FVector2D BoxMin;
	FVector2D BoxMax;
	if (!AlgonaController->GetSelectionBox(BoxMin, BoxMax))
	{
		return;
	}

	// Рамка выбора: полупрозрачная заливка и контур.
	const FVector2D BoxSize = BoxMax - BoxMin;
	DrawRect(
		FLinearColor(1.0f, 0.1f, 0.1f, 0.12f),
		BoxMin.X,
		BoxMin.Y,
		BoxSize.X,
		BoxSize.Y);

	const FLinearColor LineColor(1.0f, 0.15f, 0.15f, 0.9f);
	const float LineThickness = 1.5f;
	DrawLine(BoxMin.X, BoxMin.Y, BoxMax.X, BoxMin.Y, LineColor, LineThickness);
	DrawLine(BoxMax.X, BoxMin.Y, BoxMax.X, BoxMax.Y, LineColor, LineThickness);
	DrawLine(BoxMax.X, BoxMax.Y, BoxMin.X, BoxMax.Y, LineColor, LineThickness);
	DrawLine(BoxMin.X, BoxMax.Y, BoxMin.X, BoxMin.Y, LineColor, LineThickness);
}

void AAlgonaHUD::DrawOrderPreviewArrow(
	const AAlgonaPlayerController& AlgonaController)
{
	FVector Start;
	FVector End;
	if (!AlgonaController.GetOrderPreviewArrow(Start, End))
	{
		return;
	}

	// Наконечник строится в мире (на плоскости земли), затем все точки
	// проецируются на экран.
	const FVector Direction = (End - Start).GetSafeNormal();
	const double HeadLength = (End - Start).Length() * 0.3;
	const FVector HeadLeft =
		End - Direction.RotateAngleAxis(-30.0, FVector::UpVector) * HeadLength;
	const FVector HeadRight =
		End - Direction.RotateAngleAxis(30.0, FVector::UpVector) * HeadLength;

	const FVector ScreenPoints[] =
	{
		Project(Start, true),
		Project(End, true),
		Project(HeadLeft, true),
		Project(HeadRight, true)
	};

	// Z = 0 — точка за камерой (Project с отсечением по ближней плоскости).
	for (const FVector& Point : ScreenPoints)
	{
		if (Point.Z <= 0.0)
		{
			return;
		}
	}

	const FLinearColor ArrowColor = FLinearColor::Red;
	const float ArrowThickness = 3.0f;
	DrawLine(ScreenPoints[0].X, ScreenPoints[0].Y, ScreenPoints[1].X, ScreenPoints[1].Y, ArrowColor, ArrowThickness);
	DrawLine(ScreenPoints[1].X, ScreenPoints[1].Y, ScreenPoints[2].X, ScreenPoints[2].Y, ArrowColor, ArrowThickness);
	DrawLine(ScreenPoints[1].X, ScreenPoints[1].Y, ScreenPoints[3].X, ScreenPoints[3].Y, ArrowColor, ArrowThickness);
}

void AAlgonaHUD::DrawOrderTargetMarkers(
	const AAlgonaPlayerController& AlgonaController)
{
	TArray<FAlgonaOrderTargetMarker> Markers;
	AlgonaController.GetOrderTargetMarkers(Markers);

	for (const FAlgonaOrderTargetMarker& Marker : Markers)
	{
		// Кружок в центре и четыре стрелки, указывающие на него снаружи.
		// Всё строится в мире и проецируется на экран, поэтому отметка
		// уменьшается с расстоянием вместе с юнитами.
		const double CircleRadius = Marker.Radius * OrderMarkerCircleRatio;

		FVector2D PreviousCirclePoint;
		bool bCircleVisible = true;

		for (int32 Step = 0; Step <= OrderMarkerCircleSegments; ++Step)
		{
			const double Angle =
				2.0 * UE_DOUBLE_PI * Step / OrderMarkerCircleSegments;

			const FVector WorldPoint = Marker.Location
				+ FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0) * CircleRadius;

			const FVector ScreenPoint = Project(WorldPoint, true);
			if (ScreenPoint.Z <= 0.0)
			{
				bCircleVisible = false;
				break;
			}

			const FVector2D CirclePoint(ScreenPoint.X, ScreenPoint.Y);

			if (Step > 0)
			{
				DrawLine(
					PreviousCirclePoint.X,
					PreviousCirclePoint.Y,
					CirclePoint.X,
					CirclePoint.Y,
					OrderMarkerColor,
					OrderMarkerThickness);
			}

			PreviousCirclePoint = CirclePoint;
		}

		if (!bCircleVisible)
		{
			continue;
		}

		// Четыре стрелки по осям мира: хвост на краю отметки, остриё у кружка.
		static const FVector ArrowDirections[] =
		{
			FVector::ForwardVector,
			-FVector::ForwardVector,
			FVector::RightVector,
			-FVector::RightVector
		};

		for (const FVector& Direction : ArrowDirections)
		{
			const FVector Side = FVector::CrossProduct(FVector::UpVector, Direction);

			const FVector TailLocation = Marker.Location + Direction * Marker.Radius;
			const FVector TipLocation = Marker.Location
				+ Direction * (CircleRadius + Marker.Radius * OrderMarkerArrowGap);
			const double HeadSize = Marker.Radius * OrderMarkerArrowHeadRatio;

			const FVector WorldPoints[] =
			{
				TailLocation,
				TipLocation,
				TipLocation + Direction * HeadSize + Side * HeadSize,
				TipLocation + Direction * HeadSize - Side * HeadSize
			};

			FVector2D ScreenPoints[UE_ARRAY_COUNT(WorldPoints)];
			bool bArrowVisible = true;

			for (int32 Index = 0; Index < UE_ARRAY_COUNT(WorldPoints); ++Index)
			{
				const FVector ScreenPoint = Project(WorldPoints[Index], true);
				if (ScreenPoint.Z <= 0.0)
				{
					bArrowVisible = false;
					break;
				}

				ScreenPoints[Index] = FVector2D(ScreenPoint.X, ScreenPoint.Y);
			}

			if (!bArrowVisible)
			{
				continue;
			}

			// Древко и две линии остриё-назад.
			DrawLine(ScreenPoints[0].X, ScreenPoints[0].Y, ScreenPoints[1].X, ScreenPoints[1].Y, OrderMarkerColor, OrderMarkerThickness);
			DrawLine(ScreenPoints[1].X, ScreenPoints[1].Y, ScreenPoints[2].X, ScreenPoints[2].Y, OrderMarkerColor, OrderMarkerThickness);
			DrawLine(ScreenPoints[1].X, ScreenPoints[1].Y, ScreenPoints[3].X, ScreenPoints[3].Y, OrderMarkerColor, OrderMarkerThickness);
		}
	}
}
