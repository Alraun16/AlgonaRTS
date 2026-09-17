#include "Game/AlgonaHUD.h"

#include "Game/AlgonaPlayerController.h"

void AAlgonaHUD::DrawHUD()
{
	Super::DrawHUD();

	const AAlgonaPlayerController* AlgonaController =
		Cast<AAlgonaPlayerController>(GetOwningPlayerController());

	FVector2D BoxMin;
	FVector2D BoxMax;
	if (!AlgonaController || !AlgonaController->GetSelectionBox(BoxMin, BoxMax))
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
