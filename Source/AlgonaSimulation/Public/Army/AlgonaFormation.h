#pragma once

// Core math containers used by the renderer-neutral formation description.
#include "CoreMinimal.h"

/**
 * Shape identifier kept in the layout so a squad can later switch between
 * different formation generators without changing soldier-side storage.
 */
enum class EAlgonaFormationShape : uint8
{
	Rectangle
};

/** One local formation slot. Local X is right; local Y is forward. */
struct ALGONASIMULATION_API FAlgonaFormationSlot
{
	FVector2D LocalPosition = FVector2D::ZeroVector;

	// Reserved for future typed slots. Zero means the ordinary first-pass slot.
	uint8 SlotType = 0;
};

/**
 * Current generated geometry of one squad formation.
 * Slots are ordered front row -> back row and center -> edges inside each row.
 */
struct ALGONASIMULATION_API FAlgonaFormationLayout
{
	int32 Num() const
	{
		return Slots.Num();
	}

	bool IsValidSlot(int32 SlotIndex) const
	{
		return Slots.IsValidIndex(SlotIndex);
	}

	EAlgonaFormationShape Shape = EAlgonaFormationShape::Rectangle;
	int32 MaxSlotsPerRow = 10;
	float SlotSpacingCm = 150.0f;

	TArray<FAlgonaFormationSlot> Slots;
	float RadiusCm = 0.0f;
	FVector2D HalfExtentsCm = FVector2D::ZeroVector;

	// Increased only when slot geometry changes.
	uint32 FormationRevision = 1;

	// Lazy 180-degree assignment cache. It is valid only for MirrorRevision.
	TArray<int32> MirrorSlotIndices;
	uint32 MirrorRevision = 0;
};

/** Pure deterministic formation generation; no World, Mass or Presentation. */
class ALGONASIMULATION_API FAlgonaFormationGenerator final
{
public:
	static FAlgonaFormationLayout BuildRectangle(
		int32 ActiveUnitCount,
		float SlotSpacingCm,
		int32 RequestedMaxSlotsPerRow = 0);

	/**
	 * Builds an involutive slot map for an instant 180-degree formation flip.
	 * For symmetric layouts each source slot maps exactly to local -Position.
	 */
	static void BuildMirrorSlotMap(FAlgonaFormationLayout& Layout);

private:
	static int32 ChooseDefaultRectangleRowLength(int32 ActiveUnitCount);
	static void AppendCenteredRowPositions(
		int32 SlotCount,
		float SlotSpacingCm,
		TArray<float>& OutLocalX);
};
