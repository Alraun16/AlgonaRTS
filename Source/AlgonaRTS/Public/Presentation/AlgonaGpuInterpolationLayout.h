#pragma once

#include "CoreMinimal.h"

/**
 * Общий контракт GPU buffered interpolation.
 *
 * Используется в двух местах, которые обязаны совпадать:
 * - AAlgonaArmyPresentationActor записывает данные инстансов по этим индексам;
 * - редакторский генератор (модуль AlgonaRTSEditor) строит граф материала,
 *   читающий те же индексы, и сохраняет готовые ассеты по этим путям.
 * При изменении раскладки материалы нужно сгенерировать заново.
 */
namespace AlgonaGpuInterpolation
{
	inline constexpr TCHAR AnimBankPath[] =
		TEXT("/Game/NewFolder/NewAnimBank.NewAnimBank");

	// Раскладка per-instance custom data: предыдущий и текущий transform + tier.
	inline constexpr int32 PrevPositionIndex = 0;
	inline constexpr int32 PrevRotationIndex = 3;
	inline constexpr int32 PrevScaleIndex = 7;
	inline constexpr int32 CurrentPositionIndex = 10;
	inline constexpr int32 CurrentRotationIndex = 13;
	inline constexpr int32 CurrentScaleIndex = 17;
	inline constexpr int32 TierIndex = 20;
	inline constexpr int32 CustomDataFloatCount = 21;

	// Alpha интерполяции на уровне компонента (custom primitive data).
	// FAR alpha не использует: он показывает последний transform напрямую.
	inline constexpr int32 NearAlphaPrimitiveDataIndex = 0;
	inline constexpr int32 MediumAlphaPrimitiveDataIndex = 1;

	// Готовые материалы лежат в одной папке, по одному на material slot
	// skinned asset из AnimBank. Игра загружает только Material Instance.
	inline constexpr TCHAR MaterialFolder[] =
		TEXT("/Game/Algona/Presentation/GpuInterpolation");

	inline FString GetBaseMaterialAssetName(int32 MaterialSlotIndex)
	{
		return FString::Printf(TEXT("M_ArmyGpuInterp_Mat%d"), MaterialSlotIndex);
	}

	inline FString GetMaterialInstanceAssetName(int32 MaterialSlotIndex)
	{
		return FString::Printf(TEXT("MI_ArmyGpuInterp_Mat%d"), MaterialSlotIndex);
	}

	inline FString GetPackageName(const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s"), MaterialFolder, *AssetName);
	}

	inline FString GetObjectPath(const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s.%s"), MaterialFolder, *AssetName, *AssetName);
	}
}
