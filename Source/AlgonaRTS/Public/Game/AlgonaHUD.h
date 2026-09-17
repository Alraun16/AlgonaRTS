#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "AlgonaHUD.generated.h"

/**
 * Экранный интерфейс RTS. Пока рисует только рамку выбора,
 * состояние которой хранит AAlgonaPlayerController.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaHUD final : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
