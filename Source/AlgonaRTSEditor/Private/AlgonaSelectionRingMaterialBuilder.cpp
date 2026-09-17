#include "Presentation/AlgonaSelectionPresentationActor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaSelectionEditor, Log, All);

/*
 * Редакторский генератор материала круга выбора.
 *
 * Запускается один раз вручную в редакторе (не в PIE) консольной командой
 * algona.P2.BuildSelectionRingMaterial. Материал рисует на плоскости
 * красное кольцо: Unlit (не зависит от освещения), Masked (пиксели вне
 * кольца отбрасываются), с флагом использования на инстансах.
 */
namespace
{
	template <typename TExpression>
	TExpression* CreateExpression(UMaterial* Material, int32 NodeX, int32 NodeY)
	{
		return Cast<TExpression>(
			UMaterialEditingLibrary::CreateMaterialExpression(
				Material,
				TExpression::StaticClass(),
				NodeX,
				NodeY));
	}

	void BuildSelectionRingMaterial()
	{
		if (GEditor && GEditor->PlayWorld)
		{
			UE_LOG(
				LogAlgonaSelectionEditor,
				Error,
				TEXT("[Selection] Stop PIE before building the ring material."));
			return;
		}

		const FString PackageName = FString::Printf(
			TEXT("%s/%s"),
			AlgonaSelection::RingMaterialFolder,
			AlgonaSelection::RingMaterialAssetName);

		if (FPackageName::DoesPackageExist(PackageName)
			|| FindPackage(nullptr, *PackageName))
		{
			UE_LOG(
				LogAlgonaSelectionEditor,
				Error,
				TEXT("[Selection] Asset already exists: %s. Delete it in the Content Browser (or restart the editor if already deleted) and run the command again."),
				*PackageName);
			return;
		}

		UPackage* Package = CreatePackage(*PackageName);
		UMaterial* Material = NewObject<UMaterial>(
			Package,
			FName(AlgonaSelection::RingMaterialAssetName),
			RF_Public | RF_Standalone | RF_Transactional);

		Material->SetShadingModel(MSM_Unlit);
		Material->BlendMode = BLEND_Masked;
		Material->TwoSided = true;

		// Цвет кольца.
		UMaterialExpressionConstant3Vector* Color =
			CreateExpression<UMaterialExpressionConstant3Vector>(Material, -400, -200);
		Color->Constant = FLinearColor(1.0f, 0.05f, 0.05f);

		// Маска из стандартных узлов (без HLSL):
		//   D    = расстояние от центра плоскости в долях радиуса (0 в центре, 1 на краю);
		//   Mask = HalfWidth - |D - Mid|.
		// Mask > 0 только внутри кольца [RingInnerRatio, 1]; при пороге
		// отсечения 0 всё остальное отбрасывается.
		const float Mid = (1.0f + AlgonaSelection::RingInnerRatio) * 0.5f;
		const float HalfWidth = (1.0f - AlgonaSelection::RingInnerRatio) * 0.5f;

		UMaterialExpressionTextureCoordinate* TexCoord =
			CreateExpression<UMaterialExpressionTextureCoordinate>(Material, -1200, 100);

		UMaterialExpressionConstant2Vector* Center =
			CreateExpression<UMaterialExpressionConstant2Vector>(Material, -1200, 250);
		Center->R = 0.5f;
		Center->G = 0.5f;

		UMaterialExpressionDistance* DistanceToCenter =
			CreateExpression<UMaterialExpressionDistance>(Material, -1000, 150);
		DistanceToCenter->A.Connect(0, TexCoord);
		DistanceToCenter->B.Connect(0, Center);

		UMaterialExpressionMultiply* NormalizedDistance =
			CreateExpression<UMaterialExpressionMultiply>(Material, -800, 150);
		NormalizedDistance->A.Connect(0, DistanceToCenter);
		NormalizedDistance->ConstB = 2.0f;

		UMaterialExpressionSubtract* OffsetFromMid =
			CreateExpression<UMaterialExpressionSubtract>(Material, -600, 150);
		OffsetFromMid->A.Connect(0, NormalizedDistance);
		OffsetFromMid->ConstB = Mid;

		UMaterialExpressionAbs* AbsOffset =
			CreateExpression<UMaterialExpressionAbs>(Material, -400, 150);
		AbsOffset->Input.Connect(0, OffsetFromMid);

		UMaterialExpressionSubtract* RingMask =
			CreateExpression<UMaterialExpressionSubtract>(Material, -200, 150);
		RingMask->ConstA = HalfWidth;
		RingMask->B.Connect(0, AbsOffset);

		Material->OpacityMaskClipValue = 0.0f;

		UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
		EditorData->EmissiveColor.Connect(0, Color);
		EditorData->OpacityMask.Connect(0, RingMask);

		// Флаг использования сохраняется в ассете: собранная игра
		// не умеет включать его во время выполнения.
		UMaterialEditingLibrary::SetBaseMaterialUsage(
			Material,
			MATUSAGE_InstancedStaticMeshes,
			true);

		Material->PostEditChange();

		const TArray<FString> CompileErrors =
			UMaterialEditingLibrary::RecompileMaterial(Material);
		if (!CompileErrors.IsEmpty())
		{
			UE_LOG(
				LogAlgonaSelectionEditor,
				Error,
				TEXT("[Selection] Ring material compile failed: %s"),
				*FString::Join(CompileErrors, TEXT(" | ")));
			return;
		}

		FAssetRegistryModule::AssetCreated(Material);
		Package->MarkPackageDirty();

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, Material, *Filename, SaveArgs))
		{
			UE_LOG(
				LogAlgonaSelectionEditor,
				Error,
				TEXT("[Selection] Cannot save %s"),
				*Filename);
			return;
		}

		UE_LOG(
			LogAlgonaSelectionEditor,
			Display,
			TEXT("[Selection] Ring material saved: %s"),
			AlgonaSelection::RingMaterialObjectPath);
	}

	FAutoConsoleCommand GAlgonaBuildSelectionRingMaterialCommand(
		TEXT("algona.P2.BuildSelectionRingMaterial"),
		TEXT("Editor only (not in PIE): generate and save the selection ring material."),
		FConsoleCommandDelegate::CreateStatic(&BuildSelectionRingMaterial));
}
