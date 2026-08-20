#include "AlgonaP0PresentationExperiment.h"

#include "HAL/IConsoleManager.h"

namespace
{
	TAutoConsoleVariable<int32> CVarAlgonaP0PresentationBackend(
		TEXT("algona.P0.PresentationBackend"),
		0,
		TEXT(
			"Temporary P0 comparison backend. "
			"0=legacy stationary Mass Representation, "
			"1=separate ISM candidate, "
			"2=simulation only. Restart PIE after changing."),
		ECVF_Default);
}

EAlgonaP0PresentationBackend GetAlgonaP0PresentationBackend()
{
	const int32 BackendValue = FMath::Clamp(
		CVarAlgonaP0PresentationBackend.GetValueOnGameThread(),
		0,
		2);

	return static_cast<EAlgonaP0PresentationBackend>(BackendValue);
}