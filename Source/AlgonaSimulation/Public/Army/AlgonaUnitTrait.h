#pragma once

#include "CoreMinimal.h"
#include "MassEntityTraitBase.h"

#include "AlgonaUnitTrait.generated.h"

/** Adds only authoritative simulation data required by one unit entity. */
UCLASS()
class ALGONASIMULATION_API UAlgonaUnitTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(
		FMassEntityTemplateBuildContext& BuildContext,
		const UWorld& World) const override;
};
