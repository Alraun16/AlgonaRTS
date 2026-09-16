#include "Army/AlgonaUnitTrait.h"

#include "Army/AlgonaUnitFragments.h"
#include "MassEntityTemplateRegistry.h"

void UAlgonaUnitTrait::BuildTemplate(
	FMassEntityTemplateBuildContext& BuildContext,
	const UWorld& World) const
{
	(void)World;

	// Только идентичность Unit. Состояние движения хранится в плоских
	// массивах Simulation Subsystem.
	BuildContext.AddFragment<FAlgonaUnitIdFragment>();
	BuildContext.AddTag<FAlgonaUnitTag>();
}
