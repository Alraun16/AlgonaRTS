#include "AlgonaSoldierTrait.h"

#include "AlgonaSoldierFragments.h"
#include "Mass/EntityFragments.h"
#include "MassEntityTemplateRegistry.h"
#include "MassActorSubsystem.h"

void UAlgonaSoldierTrait::BuildTemplate(
	FMassEntityTemplateBuildContext& BuildContext,
	const UWorld& World
) const
{
	BuildContext.AddFragment<FTransformFragment>();
	BuildContext.AddFragment<FAlgonaSquadMemberFragment>();
	BuildContext.AddFragment<FMassActorFragment>();

	BuildContext.AddTag<FAlgonaSoldierTag>();
}