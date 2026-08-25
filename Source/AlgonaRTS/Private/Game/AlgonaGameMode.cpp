#include "Game/AlgonaGameMode.h"

#include "GameFramework/Pawn.h"

AAlgonaGameMode::AAlgonaGameMode()
{
	/*
	 * RTS camera/control живут отдельно от Pawn.
	 * Невидимый базовый Pawn нужен только как формальный объект,
	 * которым PlayerController может владеть.
	 */
	DefaultPawnClass = APawn::StaticClass();
}