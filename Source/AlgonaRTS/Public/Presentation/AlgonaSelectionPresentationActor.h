#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AlgonaSelectionPresentationActor.generated.h"

class UInstancedStaticMeshComponent;

namespace AlgonaSelection
{
	// Материал кольца создаётся заранее редакторской командой
	// algona.P2.BuildSelectionRingMaterial; игра только загружает его.
	inline constexpr const TCHAR* RingMaterialFolder =
		TEXT("/Game/Algona/Presentation/Selection");
	inline constexpr const TCHAR* RingMaterialAssetName = TEXT("M_SelectionRing");
	inline constexpr const TCHAR* RingMaterialObjectPath =
		TEXT("/Game/Algona/Presentation/Selection/M_SelectionRing.M_SelectionRing");

	// Стандартная плоскость движка 100 × 100 см с UV 0..1.
	inline constexpr const TCHAR* RingMeshObjectPath =
		TEXT("/Engine/BasicShapes/Plane.Plane");
	inline constexpr double RingMeshSizeCm = 100.0;

	// Внутренний радиус кольца относительно внешнего (толщина кольца).
	inline constexpr float RingInnerRatio = 0.85f;
}

/**
 * Круги выбора под Unit выбранных Squad.
 * Локальное отображение игрока: одна плоскость с кольцевым материалом,
 * размноженная инстансами. Позиции задаёт PlayerController каждый кадр.
 */
UCLASS(NotBlueprintable, Transient)
class ALGONARTS_API AAlgonaSelectionPresentationActor final : public AActor
{
	GENERATED_BODY()

public:
	AAlgonaSelectionPresentationActor();

	virtual void BeginPlay() override;

	/** Заменяет набор кругов. Transform: позиция центра и масштаб по радиусу. */
	void SetRingTransforms(const TArray<FTransform>& RingTransforms);

	/** Масштаб плоскости, при котором внешний радиус кольца равен Radius. */
	static FVector GetRingScale(double Radius);

private:
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> RingComponent = nullptr;
};
