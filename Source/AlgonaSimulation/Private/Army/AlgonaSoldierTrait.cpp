#include "Army/AlgonaSoldierTrait.h"

#include "Army/AlgonaSoldierFragments.h"
#include "Mass/EntityFragments.h"
#include "MassEntityTemplateRegistry.h"

void UAlgonaSoldierTrait::BuildTemplate(
	FMassEntityTemplateBuildContext& BuildContext,
	const UWorld& World) const
{
	(void)World;

	BuildContext.AddFragment<FTransformFragment>();
	BuildContext.AddFragment<FAlgonaSoldierIdFragment>();
	BuildContext.AddFragment<FAlgonaSquadMemberFragment>();
	BuildContext.AddFragment<FAlgonaSoldierMovementFragment>();
	BuildContext.AddTag<FAlgonaSoldierTag>();
}
