#pragma once

#include "CoreMinimal.h"
#include "MassEntityTraitBase.h"

#include "AlgonaSoldierTrait.generated.h"

UCLASS()
class ALGONASIMULATION_API UAlgonaSoldierTrait : public UMassEntityTraitBase
{
	GENERATED_BODY()

public:
	virtual void BuildTemplate(
		FMassEntityTemplateBuildContext& BuildContext,
		const UWorld& World
	) const override;
};