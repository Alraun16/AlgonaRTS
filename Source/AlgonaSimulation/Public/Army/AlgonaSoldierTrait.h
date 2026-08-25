#pragma once

#include "CoreMinimal.h"
#include "MassEntityTraitBase.h"

#include "AlgonaSoldierTrait.generated.h"

/** Adds only authoritative simulation data required by one soldier entity. */
UCLASS()
class ALGONASIMULATION_API UAlgonaSoldierTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(
		FMassEntityTemplateBuildContext& BuildContext,
		const UWorld& World) const override;
};
