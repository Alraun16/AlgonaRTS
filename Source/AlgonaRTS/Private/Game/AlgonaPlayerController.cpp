#include "Game/AlgonaPlayerController.h"

#include "Camera/AlgonaRTSCameraActor.h"
#include "InputCoreTypes.h"

void AAlgonaPlayerController::SetRTSCamera(
	AAlgonaRTSCameraActor* InCamera)
{
	CameraActor = InCamera;
	SetViewTarget(CameraActor);
}

void AAlgonaPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

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