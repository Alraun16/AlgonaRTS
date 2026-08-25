#include "Presentation/AlgonaPresentationSettings.h"

#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarAlgonaP1Presentation(
		TEXT("algona.P1.Presentation"),
		2,
		TEXT(
			"P1 local presentation mode. "
			"0=simulation only, "
			"1=legacy static ISM baseline, "
			"2=instanced skinned mesh presentation. "
			"Restart PIE after changing."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP1PresentationCameraCull(
		TEXT("algona.P1.PresentationCameraCull"),
		1,
		TEXT(
			"1=build a camera-visible presentation working set, "
			"0=present all exported soldiers. Intended for A/B tests."),
		ECVF_Default);
}

EAlgonaP1PresentationMode GetAlgonaP1PresentationMode()
{
	const int32 Value = FMath::Clamp(
		CVarAlgonaP1Presentation.GetValueOnGameThread(),
		0,
		2);

	return static_cast<EAlgonaP1PresentationMode>(Value);
}

bool IsAlgonaP1PresentationCameraCullingEnabled()
{
	return CVarAlgonaP1PresentationCameraCull.GetValueOnGameThread() != 0;
}
