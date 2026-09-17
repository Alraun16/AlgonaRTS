#include "Game/AlgonaHUD.h"

#include "Game/AlgonaPlayerController.h"

#include "Engine/Canvas.h"

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
