#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "AlgonaHUD.generated.h"

class AAlgonaPlayerController;

/**
 * Экранный интерфейс RTS: рамка выбора, стрелка предпросмотра приказа
 * и сообщения. Состояние хранит AAlgonaPlayerController.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaHUD final : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawOrderPreviewArrow(const AAlgonaPlayerController& AlgonaController);
};
