#include "Game/AlgonaGameMode.h"

// P2 gameplay input controller; RTS camera remains a separate actor.
#include "Game/AlgonaPlayerController.h"

#include "GameFramework/Pawn.h"

AAlgonaGameMode::AAlgonaGameMode()
{
	/*
	 * RTS camera/control live separately from Pawn. The invisible base Pawn is
	 * only the formal possession target; P2 input lives in PlayerController.
	 */
	DefaultPawnClass = APawn::StaticClass();
	PlayerControllerClass = AAlgonaPlayerController::StaticClass();
}
