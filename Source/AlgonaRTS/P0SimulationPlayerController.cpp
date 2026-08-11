#include "P0SimulationPlayerController.h"

#include "AlgonaSimulationSubsystem.h"
#include "DrawDebugHelpers.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaP0Input, Log, All);

namespace
{
TAutoConsoleVariable<int32> CVarAlgonaP1DebugMouseInput(
	TEXT("algona.P1.DebugMouseInput"),
	1,
	TEXT("Enables debug-only RTS mouse input on AP0SimulationPlayerController."),
	ECVF_Default
);

TAutoConsoleVariable<float> CVarAlgonaP1DebugPickRadius(
	TEXT("algona.P1.DebugPickRadius"),
	300.0f,
	TEXT("2D radius used by debug RTS mouse input when resolving a clicked Mass group."),
	ECVF_Default
);

AP0SimulationPlayerController* GetP0PlayerController(UWorld* World)
{
	return World != nullptr
		? Cast<AP0SimulationPlayerController>(World->GetFirstPlayerController())
		: nullptr;
}

void NetMoveGroupP0Command(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 4)
	{
		UE_LOG(LogAlgonaP0Input, Warning, TEXT("Usage: algona.P0.NetMoveGroup <GroupId> <X> <Y> <Z>"));
		return;
	}

	AP0SimulationPlayerController* PlayerController =
		GetP0PlayerController(World);
	if (PlayerController == nullptr)
	{
		UE_LOG(LogAlgonaP0Input, Warning, TEXT("algona.P0.NetMoveGroup failed: no P0 player controller."));
		return;
	}

	PlayerController->SubmitMoveGroupCommand(
		FSimGroupId(FCString::Atoi(*Args[0])),
		FVector(
			FCString::Atod(*Args[1]),
			FCString::Atod(*Args[2]),
			FCString::Atod(*Args[3])
		)
	);
}

void NetMoveAllP0Command(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 3)
	{
		UE_LOG(LogAlgonaP0Input, Warning, TEXT("Usage: algona.P0.NetMoveAll <X> <Y> <Z>"));
		return;
	}

	AP0SimulationPlayerController* PlayerController =
		GetP0PlayerController(World);
	if (PlayerController == nullptr)
	{
		UE_LOG(LogAlgonaP0Input, Warning, TEXT("algona.P0.NetMoveAll failed: no P0 player controller."));
		return;
	}

	PlayerController->SubmitMoveAllGroupsCommand(
		FVector(
			FCString::Atod(*Args[0]),
			FCString::Atod(*Args[1]),
			FCString::Atod(*Args[2])
		)
	);
}

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0NetMoveGroupCommand(
	TEXT("algona.P0.NetMoveGroup"),
	TEXT("Submits MoveGroup through the owning P0 PlayerController RPC. Usage: algona.P0.NetMoveGroup <GroupId> <X> <Y> <Z>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&NetMoveGroupP0Command)
);

FAutoConsoleCommandWithWorldAndArgs GAlgonaP0NetMoveAllCommand(
	TEXT("algona.P0.NetMoveAll"),
	TEXT("Submits MoveAll through the owning P0 PlayerController RPC. Usage: algona.P0.NetMoveAll <X> <Y> <Z>"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&NetMoveAllP0Command)
);
}

void AP0SimulationPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController()
		&& CVarAlgonaP1DebugMouseInput.GetValueOnGameThread() != 0)
	{
		bShowMouseCursor = true;
		bEnableClickEvents = true;
		bEnableMouseOverEvents = true;
	}
}

void AP0SimulationPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (InputComponent == nullptr)
	{
		return;
	}

	InputComponent->BindKey(
		EKeys::LeftMouseButton,
		IE_Pressed,
		this,
		&AP0SimulationPlayerController::HandleDebugSelectGroup
	);
	InputComponent->BindKey(
		EKeys::RightMouseButton,
		IE_Pressed,
		this,
		&AP0SimulationPlayerController::HandleDebugMoveSelectedGroup
	);
}

void AP0SimulationPlayerController::SubmitMoveGroupCommand(
	FSimGroupId GroupId,
	FVector TargetPosition)
{
	if (HasAuthority())
	{
		ServerSubmitMoveGroupCommand_Implementation(GroupId.Value, TargetPosition);
	}
	else
	{
		ServerSubmitMoveGroupCommand(GroupId.Value, TargetPosition);
	}

	UE_LOG(
		LogAlgonaP0Input,
		Log,
		TEXT("Submitted P0 net MoveGroup request. LocalRole=%d, GroupId=%d, Target=%s"),
		static_cast<int32>(GetLocalRole()),
		GroupId.Value,
		*TargetPosition.ToCompactString()
	);
}

void AP0SimulationPlayerController::SubmitMoveAllGroupsCommand(
	FVector TargetPosition)
{
	if (HasAuthority())
	{
		ServerSubmitMoveAllGroupsCommand_Implementation(TargetPosition);
	}
	else
	{
		ServerSubmitMoveAllGroupsCommand(TargetPosition);
	}

	UE_LOG(
		LogAlgonaP0Input,
		Log,
		TEXT("Submitted P0 net MoveAll request. LocalRole=%d, Target=%s"),
		static_cast<int32>(GetLocalRole()),
		*TargetPosition.ToCompactString()
	);
}

void AP0SimulationPlayerController::ServerSubmitMoveGroupCommand_Implementation(
	int32 GroupId,
	FVector TargetPosition)
{
	if (UWorld* World = GetWorld())
	{
		if (UAlgonaSimulationSubsystem* SimulationSubsystem =
			World->GetSubsystem<UAlgonaSimulationSubsystem>())
		{
			SimulationSubsystem->SubmitMoveGroupCommand(
				FSimGroupId(GroupId),
				TargetPosition
			);
		}
	}
}

void AP0SimulationPlayerController::HandleDebugSelectGroup()
{
	if (!IsLocalController()
		|| CVarAlgonaP1DebugMouseInput.GetValueOnGameThread() == 0
		|| !IsDebugMouseModifierDown())
	{
		return;
	}

	FVector WorldPosition;
	if (!ResolveDebugCursorWorldPosition(WorldPosition))
	{
		UE_LOG(LogAlgonaP0Input, Warning, TEXT("P1 debug select failed: no cursor world position."));
		return;
	}

	UWorld* World = GetWorld();
	UAlgonaSimulationSubsystem* SimulationSubsystem =
		World != nullptr
			? World->GetSubsystem<UAlgonaSimulationSubsystem>()
			: nullptr;
	if (SimulationSubsystem == nullptr)
	{
		return;
	}

	FVector NearestPosition;
	FSimGroupId SelectedGroup;
	const double PickRadius = static_cast<double>(
		FMath::Max(CVarAlgonaP1DebugPickRadius.GetValueOnGameThread(), 1.0f)
	);
	if (!SimulationSubsystem->FindNearestGroupAtPosition(
			WorldPosition,
			PickRadius,
			SelectedGroup,
			&NearestPosition))
	{
		UE_LOG(
			LogAlgonaP0Input,
			Log,
			TEXT("P1 debug select found no group. Cursor=%s, Radius=%.1f"),
			*WorldPosition.ToCompactString(),
			PickRadius
		);
		return;
	}

	DebugSelectedGroup = SelectedGroup;
	UE_LOG(
		LogAlgonaP0Input,
		Log,
		TEXT("P1 debug selected group. GroupId=%d, Cursor=%s, NearestEntity=%s"),
		DebugSelectedGroup.Value,
		*WorldPosition.ToCompactString(),
		*NearestPosition.ToCompactString()
	);

	DrawDebugSphere(
		World,
		NearestPosition + FVector(0.0, 0.0, 100.0),
		75.0f,
		12,
		FColor::Green,
		false,
		2.0f
	);
	DrawDebugString(
		World,
		NearestPosition + FVector(0.0, 0.0, 220.0),
		FString::Printf(TEXT("Group %d"), DebugSelectedGroup.Value),
		nullptr,
		FColor::Green,
		2.0f,
		true
	);
}

void AP0SimulationPlayerController::HandleDebugMoveSelectedGroup()
{
	if (!IsLocalController()
		|| CVarAlgonaP1DebugMouseInput.GetValueOnGameThread() == 0
		|| !IsDebugMouseModifierDown())
	{
		return;
	}

	if (!DebugSelectedGroup.IsValid())
	{
		UE_LOG(LogAlgonaP0Input, Warning, TEXT("P1 debug move ignored: no selected group."));
		return;
	}

	FVector WorldPosition;
	if (!ResolveDebugCursorWorldPosition(WorldPosition))
	{
		UE_LOG(LogAlgonaP0Input, Warning, TEXT("P1 debug move failed: no cursor world position."));
		return;
	}

	SubmitMoveGroupCommand(DebugSelectedGroup, WorldPosition);
	UE_LOG(
		LogAlgonaP0Input,
		Log,
		TEXT("P1 debug submitted selected MoveGroup. GroupId=%d, Target=%s"),
		DebugSelectedGroup.Value,
		*WorldPosition.ToCompactString()
	);

	if (UWorld* World = GetWorld())
	{
		DrawDebugSphere(
			World,
			WorldPosition + FVector(0.0, 0.0, 60.0),
			90.0f,
			12,
			FColor::Yellow,
			false,
			2.0f
		);
	}
}

bool AP0SimulationPlayerController::IsDebugMouseModifierDown() const
{
	return IsInputKeyDown(EKeys::LeftShift)
		|| IsInputKeyDown(EKeys::RightShift);
}

bool AP0SimulationPlayerController::ResolveDebugCursorWorldPosition(
	FVector& OutWorldPosition) const
{
	FHitResult HitResult;
	if (GetHitResultUnderCursor(ECC_Visibility, false, HitResult)
		&& HitResult.bBlockingHit)
	{
		OutWorldPosition = HitResult.ImpactPoint;
		return true;
	}

	FVector RayOrigin;
	FVector RayDirection;
	if (!DeprojectMousePositionToWorld(RayOrigin, RayDirection)
		|| FMath::IsNearlyZero(RayDirection.Z))
	{
		return false;
	}

	const double T = -RayOrigin.Z / RayDirection.Z;
	if (T < 0.0)
	{
		return false;
	}

	OutWorldPosition = RayOrigin + RayDirection * T;
	return true;
}

void AP0SimulationPlayerController::ServerSubmitMoveAllGroupsCommand_Implementation(
	FVector TargetPosition)
{
	if (UWorld* World = GetWorld())
	{
		if (UAlgonaSimulationSubsystem* SimulationSubsystem =
			World->GetSubsystem<UAlgonaSimulationSubsystem>())
		{
			SimulationSubsystem->SubmitMoveAllGroupsCommand(TargetPosition);
		}
	}
}
