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

	// Луч в центре выбранного Squad: сплошной меш, тоже без коллизий.
	BeamComponent = CreateDefaultSubobject<UInstancedStaticMeshComponent>(
		TEXT("SquadCenterBeams"));
	BeamComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BeamComponent->SetCanEverAffectNavigation(false);
	BeamComponent->SetCastShadow(false);
	BeamComponent->SetMobility(EComponentMobility::Movable);
	BeamComponent->SetupAttachment(RingComponent);
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

	UStaticMesh* BeamMesh =
		LoadObject<UStaticMesh>(nullptr, AlgonaSelection::BeamMeshObjectPath);
	UMaterialInterface* BeamMaterial =
		LoadObject<UMaterialInterface>(nullptr, AlgonaSelection::BeamMaterialObjectPath);

	if (!BeamMesh || !BeamMaterial)
	{
		UE_LOG(
			LogAlgonaSelection,
			Error,
			TEXT("[Selection] Cannot load beam mesh or material (%s). Run algona.P2.BuildSquadCenterBeamMaterial in the editor (not in PIE)."),
			AlgonaSelection::BeamMaterialObjectPath);
		return;
	}

	// Габариты меша читаются из него самого: у стандартного цилиндра точка
	// привязки в центре, но полагаться на это незачем — масштаб и подъём
	// считаются так, чтобы низ луча оказался ровно в заданной точке.
	const FBox MeshBox = BeamMesh->GetBoundingBox();
	const FVector MeshSize = MeshBox.GetSize();

	BeamScale = FVector(
		MeshSize.X > UE_DOUBLE_SMALL_NUMBER
			? AlgonaSelection::BeamRadiusCm * 2.0 / MeshSize.X
			: 1.0,
		MeshSize.Y > UE_DOUBLE_SMALL_NUMBER
			? AlgonaSelection::BeamRadiusCm * 2.0 / MeshSize.Y
			: 1.0,
		MeshSize.Z > UE_DOUBLE_SMALL_NUMBER
			? AlgonaSelection::BeamHeightCm / MeshSize.Z
			: 1.0);

	BeamPivotOffsetCm = -MeshBox.Min.Z * BeamScale.Z;

	BeamComponent->SetStaticMesh(BeamMesh);
	BeamComponent->SetMaterial(0, BeamMaterial);
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

void AAlgonaSelectionPresentationActor::SetSquadCenterBeams(
	const TArray<FVector>& BeamBaseLocations)
{
	BeamTransforms.Reset(BeamBaseLocations.Num());

	for (const FVector& BaseLocation : BeamBaseLocations)
	{
		BeamTransforms.Emplace(
			FQuat::Identity,
			BaseLocation + FVector(0.0, 0.0, BeamPivotOffsetCm),
			BeamScale);
	}

	const int32 CurrentCount = BeamComponent->GetInstanceCount();

	if (CurrentCount != BeamTransforms.Num())
	{
		BeamComponent->ClearInstances();
		if (!BeamTransforms.IsEmpty())
		{
			BeamComponent->AddInstances(BeamTransforms, false, false, false);
		}
		return;
	}

	if (!BeamTransforms.IsEmpty())
	{
		BeamComponent->BatchUpdateInstancesTransforms(
			0,
			BeamTransforms,
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
