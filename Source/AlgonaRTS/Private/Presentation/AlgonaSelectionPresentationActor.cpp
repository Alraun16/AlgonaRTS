#include "Presentation/AlgonaSelectionPresentationActor.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaSelection, Log, All);

AAlgonaSelectionPresentationActor::AAlgonaSelectionPresentationActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// Круги — чистая визуализация: без коллизий, теней и навигации.
	RingComponent = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
		TEXT("SelectionRings"));
	RingComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RingComponent->SetCanEverAffectNavigation(false);
	RingComponent->SetCastShadow(false);
	RingComponent->SetMobility(EComponentMobility::Movable);
	RootComponent = RingComponent;
}

void AAlgonaSelectionPresentationActor::BeginPlay()
{
	Super::BeginPlay();

	// Актор стоит в начале координат, поэтому локальные transform инстансов
	// совпадают с мировыми.
	SetActorTransform(FTransform::Identity);

	UStaticMesh* RingMesh =
		LoadObject<UStaticMesh>(nullptr, AlgonaSelection::RingMeshObjectPath);
	UMaterialInterface* RingMaterial =
		LoadObject<UMaterialInterface>(nullptr, AlgonaSelection::RingMaterialObjectPath);

	if (!RingMesh || !RingMaterial)
	{
		UE_LOG(
			LogAlgonaSelection,
			Error,
			TEXT("[Selection] Cannot load ring mesh or material (%s). Run algona.P2.BuildSelectionRingMaterial in the editor (not in PIE)."),
			AlgonaSelection::RingMaterialObjectPath);
		return;
	}

	RingComponent->SetStaticMesh(RingMesh);
	RingComponent->SetMaterial(0, RingMaterial);
}

void AAlgonaSelectionPresentationActor::SetRingTransforms(
	const TArray<FTransform>& RingTransforms)
{
	const int32 CurrentCount = RingComponent->GetInstanceCount();

	// Число кругов меняется только при смене выбора или состава Squad —
	// тогда набор пересоздаётся. Иначе обновляются transform на месте.
	if (CurrentCount != RingTransforms.Num())
	{
		RingComponent->ClearInstances();
		if (!RingTransforms.IsEmpty())
		{
			RingComponent->AddInstances(RingTransforms, false, false, false);
		}
		return;
	}

	if (!RingTransforms.IsEmpty())
	{
		RingComponent->BatchUpdateInstancesTransforms(
			0,
			RingTransforms,
			false,
			true,
			true);
	}
}

FVector AAlgonaSelectionPresentationActor::GetRingScale(double Radius)
{
	const double Scale = Radius * 2.0 / AlgonaSelection::RingMeshSizeCm;
	return FVector(Scale, Scale, 1.0);
}
