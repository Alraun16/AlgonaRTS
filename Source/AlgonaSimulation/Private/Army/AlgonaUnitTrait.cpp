#include "Army/AlgonaUnitTrait.h"

#include "Army/AlgonaUnitFragments.h"
#include "Mass/EntityFragments.h"
#include "MassEntityTemplateRegistry.h"

void UAlgonaUnitTrait::BuildTemplate(
	FMassEntityTemplateBuildContext& BuildContext,
	const UWorld& World) const
{
	(void)World;

	BuildContext.AddFragment<FTransformFragment>();
	BuildContext.AddFragment<FAlgonaUnitIdFragment>();
	BuildContext.AddFragment<FAlgonaSquadMemberFragment>();
	BuildContext.AddFragment<FAlgonaUnitMovementFragment>();
	BuildContext.AddTag<FAlgonaUnitTag>();
}
