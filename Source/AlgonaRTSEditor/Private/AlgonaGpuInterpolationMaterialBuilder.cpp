#include "Presentation/AlgonaGpuInterpolationLayout.h"

#include "Animation/AnimBank.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetCommon.h"
#include "HAL/IConsoleManager.h"
#include "MaterialEditingLibrary.h"
#include "MaterialExpressionIO.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaRTSEditor, Log, All);

/*
 * Редакторский генератор материалов GPU buffered interpolation.
 *
 * Запускается один раз вручную в редакторе (не в PIE) консольной командой
 * algona.P1.BuildGpuInterpolationMaterials. Для каждого material slot
 * skinned asset из AnimBank он:
 *   1. копирует базовый материал;
 *   2. встраивает в World Position Offset граф интерполяции;
 *   3. создаёт Material Instance с параметрами исходного слота;
 *   4. сохраняет оба ассета в AlgonaGpuInterpolation::MaterialFolder.
 *
 * Игра (PIE и Standalone) только загружает готовые Material Instance.
 * Граф и HLSL перенесены без изменений из прежнего рантайм-пути P1.
 */
namespace
{
	template <typename TExpression>
	TExpression* CreateMaterialExpression(
		UMaterial* Material,
		int32 NodeX,
		int32 NodeY)
	{
		return Cast<TExpression>(
			UMaterialEditingLibrary::CreateMaterialExpressionEx(
				Material,
				nullptr,
				TExpression::StaticClass(),
				nullptr,
				NodeX,
				NodeY,
				false));
	}

	bool AddCustomInput(
		UMaterialExpressionCustom& Custom,
		const TCHAR* Name,
		UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return false;
		}

		FCustomInput& Input = Custom.Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(0, Expression);
		return true;
	}

	bool AddCustomInput(
		UMaterialExpressionCustom& Custom,
		const TCHAR* Name,
		const FExpressionInput& ExpressionInput)
	{
		if (!ExpressionInput.Expression)
		{
			return false;
		}

		FCustomInput& Input = Custom.Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input = ExpressionInput;
		return true;
	}

	// Встраивает интерполяцию в WPO материала. Исходный WPO сохраняется
	// и прибавляется к результату.
	bool InjectGpuInterpolationIntoMaterial(
		UMaterial& Material,
		FString& OutError)
	{
		UMaterialEditorOnlyData* EditorData = Material.GetEditorOnlyData();
		if (!EditorData)
		{
			OutError = TEXT("material has no editor-only graph data");
			return false;
		}

		if (Material.bUseMaterialAttributes)
		{
			OutError =
				TEXT("material uses Material Attributes; current WPO bridge does not support that graph yet");
			return false;
		}

		FExpressionInput OriginalWpoInput =
			static_cast<const FExpressionInput&>(EditorData->WorldPositionOffset);

		if (!OriginalWpoInput.Expression)
		{
			UMaterialExpressionConstant3Vector* ZeroWpo =
				CreateMaterialExpression<UMaterialExpressionConstant3Vector>(
					&Material,
					-900,
					900);

			if (!ZeroWpo)
			{
				OutError = TEXT("cannot create zero WPO expression");
				return false;
			}

			const FVector3f ConstantWpo =
				EditorData->WorldPositionOffset.UseConstant
					? EditorData->WorldPositionOffset.Constant
					: FVector3f::ZeroVector;

			ZeroWpo->Constant = FLinearColor(
				ConstantWpo.X,
				ConstantWpo.Y,
				ConstantWpo.Z,
				1.0f);

			OriginalWpoInput.Connect(0, ZeroWpo);
		}

		UMaterialExpressionWorldPosition* WorldPosition =
			CreateMaterialExpression<UMaterialExpressionWorldPosition>(
				&Material,
				-900,
				-500);

		if (!WorldPosition)
		{
			OutError = TEXT("cannot create WorldPosition expression");
			return false;
		}

		WorldPosition->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;

		// Имена входов Custom-узла в порядке индексов раскладки.
		static const TCHAR* InstanceInputNames[AlgonaGpuInterpolation::CustomDataFloatCount] =
		{
			TEXT("PrevPX"), TEXT("PrevPY"), TEXT("PrevPZ"),
			TEXT("PrevQX"), TEXT("PrevQY"), TEXT("PrevQZ"), TEXT("PrevQW"),
			TEXT("PrevSX"), TEXT("PrevSY"), TEXT("PrevSZ"),
			TEXT("CurrPX"), TEXT("CurrPY"), TEXT("CurrPZ"),
			TEXT("CurrQX"), TEXT("CurrQY"), TEXT("CurrQZ"), TEXT("CurrQW"),
			TEXT("CurrSX"), TEXT("CurrSY"), TEXT("CurrSZ"),
			TEXT("InterpTier")
		};

		TArray<UMaterialExpressionPerInstanceCustomData*> InstanceDataExpressions;
		InstanceDataExpressions.Reserve(AlgonaGpuInterpolation::CustomDataFloatCount);

		for (int32 DataIndex = 0;
			DataIndex < AlgonaGpuInterpolation::CustomDataFloatCount;
			++DataIndex)
		{
			UMaterialExpressionPerInstanceCustomData* Expression =
				CreateMaterialExpression<UMaterialExpressionPerInstanceCustomData>(
					&Material,
					-900,
					-400 + DataIndex * 40);

			if (!Expression)
			{
				OutError = TEXT("cannot create PerInstanceCustomData expression");
				return false;
			}

			Expression->DataIndex = static_cast<uint32>(DataIndex);
			Expression->ConstDefaultValue = 0.0f;
			InstanceDataExpressions.Add(Expression);
		}

		struct FAlphaExpressionDesc
		{
			const TCHAR* Name;
			int32 PrimitiveDataIndex;
		};

		static const FAlphaExpressionDesc AlphaDescs[] =
		{
			{TEXT("AlphaNear"), AlgonaGpuInterpolation::NearAlphaPrimitiveDataIndex},
			{TEXT("AlphaMedium"), AlgonaGpuInterpolation::MediumAlphaPrimitiveDataIndex}
		};

		TArray<UMaterialExpressionScalarParameter*> AlphaExpressions;
		AlphaExpressions.Reserve(UE_ARRAY_COUNT(AlphaDescs));

		for (int32 Index = 0; Index < UE_ARRAY_COUNT(AlphaDescs); ++Index)
		{
			UMaterialExpressionScalarParameter* Expression =
				CreateMaterialExpression<UMaterialExpressionScalarParameter>(
					&Material,
					-500,
					-400 + Index * 80);

			if (!Expression)
			{
				OutError = TEXT("cannot create CustomPrimitiveData alpha expression");
				return false;
			}

			Expression->ParameterName = FName(AlphaDescs[Index].Name);
			Expression->DefaultValue = 1.0f;
			Expression->bUseCustomPrimitiveData = true;
			Expression->PrimitiveDataIndex =
				static_cast<uint8>(AlphaDescs[Index].PrimitiveDataIndex);
			AlphaExpressions.Add(Expression);
		}

		UMaterialExpressionCustom* Custom =
			CreateMaterialExpression<UMaterialExpressionCustom>(
				&Material,
				100,
				0);

		if (!Custom)
		{
			OutError = TEXT("cannot create custom interpolation expression");
			return false;
		}

		Custom->Description = TEXT("Algona GPU buffered interpolation");
		Custom->OutputType = CMOT_Float3;
		Custom->Code = TEXT(R"ALGONA(
// FAR uses the latest authoritative instance transform directly.
[branch]
if (InterpTier >= 1.5)
{
    return OriginalWPO;
}

float Alpha = (InterpTier < 0.5) ? AlphaNear : AlphaMedium;
Alpha = saturate(Alpha);

float3 PrevPos = float3(PrevPX, PrevPY, PrevPZ);
float4 PrevQ = normalize(float4(PrevQX, PrevQY, PrevQZ, PrevQW));
float3 PrevScale = float3(PrevSX, PrevSY, PrevSZ);
float3 CurrPos = float3(CurrPX, CurrPY, CurrPZ);
float4 CurrQ = normalize(float4(CurrQX, CurrQY, CurrQZ, CurrQW));
float3 CurrScale = float3(CurrSX, CurrSY, CurrSZ);

float4 TargetQ = CurrQ;
float CosTheta = dot(PrevQ, TargetQ);
if (CosTheta < 0.0)
{
    TargetQ = -TargetQ;
    CosTheta = -CosTheta;
}
CosTheta = clamp(CosTheta, -1.0, 1.0);

float4 InterpQ;

// NEAR uses true shortest-path Slerp. MEDIUM uses normalized Lerp.
if (InterpTier >= 0.5 || CosTheta > 0.9995)
{
    InterpQ = normalize(lerp(PrevQ, TargetQ, Alpha));
}
else
{
    float Theta = acos(CosTheta);
    float SinTheta = max(sin(Theta), 0.00001);
    float A = sin((1.0 - Alpha) * Theta) / SinTheta;
    float B = sin(Alpha * Theta) / SinTheta;
    InterpQ = normalize(PrevQ * A + TargetQ * B);
}

float3 InterpPos = lerp(PrevPos, CurrPos, Alpha);
float3 InterpScale = lerp(PrevScale, CurrScale, Alpha);

// WorldPos is already skinned under the CURRENT rigid instance transform.
// Undo only that rigid transform, then apply the interpolated rigid transform.
float3 FromCurrentOrigin = WorldPos - CurrPos;
float4 InvCurrQ = float4(-CurrQ.xyz, CurrQ.w);
float3 CurrentLocalRotated =
    FromCurrentOrigin
    + 2.0 * cross(
        InvCurrQ.xyz,
        cross(InvCurrQ.xyz, FromCurrentOrigin)
        + InvCurrQ.w * FromCurrentOrigin);

float3 SafeCurrScale = float3(
    abs(CurrScale.x) > 0.0001 ? CurrScale.x : 1.0,
    abs(CurrScale.y) > 0.0001 ? CurrScale.y : 1.0,
    abs(CurrScale.z) > 0.0001 ? CurrScale.z : 1.0);
float3 LocalPosition = CurrentLocalRotated / SafeCurrScale;
float3 InterpLocal = LocalPosition * InterpScale;

float3 InterpRotated =
    InterpLocal
    + 2.0 * cross(
        InterpQ.xyz,
        cross(InterpQ.xyz, InterpLocal)
        + InterpQ.w * InterpLocal);

float3 InterpWorld = InterpPos + InterpRotated;
return OriginalWPO + (InterpWorld - WorldPos);
)ALGONA");

		if (!AddCustomInput(*Custom, TEXT("WorldPos"), WorldPosition))
		{
			OutError = TEXT("cannot connect WorldPosition input");
			return false;
		}

		for (int32 Index = 0; Index < InstanceDataExpressions.Num(); ++Index)
		{
			if (!AddCustomInput(
				*Custom,
				InstanceInputNames[Index],
				InstanceDataExpressions[Index]))
			{
				OutError = TEXT("cannot connect instance data input");
				return false;
			}
		}

		for (int32 Index = 0; Index < AlphaExpressions.Num(); ++Index)
		{
			if (!AddCustomInput(
				*Custom,
				AlphaDescs[Index].Name,
				AlphaExpressions[Index]))
			{
				OutError = TEXT("cannot connect alpha input");
				return false;
			}
		}

		if (!AddCustomInput(*Custom, TEXT("OriginalWPO"), OriginalWpoInput))
		{
			OutError = TEXT("cannot connect original WPO input");
			return false;
		}

		EditorData->WorldPositionOffset.Connect(0, Custom);
		Material.bAlwaysEvaluateWorldPositionOffset = true;

		// Флаг использования ставится здесь и сохраняется в ассете: собранная
		// игра не умеет включать usage-флаги материала во время выполнения.
		UMaterialEditingLibrary::SetBaseMaterialUsage(
			&Material,
			MATUSAGE_InstancedSkinnedMesh,
			true);

		Custom->PostEditChange();
		Material.PostEditChange();

		const TArray<FString> CompileErrors =
			UMaterialEditingLibrary::RecompileMaterial(&Material);

		if (!CompileErrors.IsEmpty())
		{
			OutError = FString::Join(CompileErrors, TEXT(" | "));
			return false;
		}

		return true;
	}

	bool IsAssetPathOccupied(const FString& AssetName)
	{
		const FString PackageName =
			AlgonaGpuInterpolation::GetPackageName(AssetName);

		return FPackageName::DoesPackageExist(PackageName)
			|| FindPackage(nullptr, *PackageName) != nullptr;
	}

	bool SaveNewAsset(UObject& Asset, FString& OutError)
	{
		UPackage* Package = Asset.GetPackage();
		if (!Package)
		{
			OutError = TEXT("asset has no package");
			return false;
		}

		FAssetRegistryModule::AssetCreated(&Asset);
		Package->MarkPackageDirty();

		const FString Filename = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, &Asset, *Filename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("cannot save %s"), *Filename);
			return false;
		}

		return true;
	}

	// Создаёт и сохраняет пару ассетов (базовый материал + Material Instance)
	// для одного material slot.
	bool BuildMaterialSlot(
		int32 MaterialSlotIndex,
		UMaterialInterface& SourceInterface,
		FString& OutError)
	{
		UMaterial* SourceBaseMaterial = SourceInterface.GetMaterial();
		if (!SourceBaseMaterial)
		{
			OutError = TEXT("slot has no base material");
			return false;
		}

		// 1. Копия базового материала с встроенной интерполяцией.
		const FString BaseAssetName =
			AlgonaGpuInterpolation::GetBaseMaterialAssetName(MaterialSlotIndex);

		UPackage* BasePackage = CreatePackage(
			*AlgonaGpuInterpolation::GetPackageName(BaseAssetName));

		UMaterial* GeneratedBase = DuplicateObject<UMaterial>(
			SourceBaseMaterial,
			BasePackage,
			FName(*BaseAssetName));

		if (!GeneratedBase)
		{
			OutError = TEXT("cannot duplicate base material");
			return false;
		}

		GeneratedBase->ClearFlags(RF_Transient);
		GeneratedBase->SetFlags(RF_Public | RF_Standalone | RF_Transactional);

		if (!InjectGpuInterpolationIntoMaterial(*GeneratedBase, OutError)
			|| !SaveNewAsset(*GeneratedBase, OutError))
		{
			return false;
		}

		// 2. Material Instance, который загружает игра. Если слот был
		// Material Instance, переносим его uniform-параметры, как делал
		// прежний рантайм-путь.
		const FString InstanceAssetName =
			AlgonaGpuInterpolation::GetMaterialInstanceAssetName(MaterialSlotIndex);

		UPackage* InstancePackage = CreatePackage(
			*AlgonaGpuInterpolation::GetPackageName(InstanceAssetName));

		UMaterialInstanceConstant* GeneratedInstance =
			NewObject<UMaterialInstanceConstant>(
				InstancePackage,
				FName(*InstanceAssetName),
				RF_Public | RF_Standalone | RF_Transactional);

		if (!GeneratedInstance)
		{
			OutError = TEXT("cannot create material instance");
			return false;
		}

		GeneratedInstance->SetParentEditorOnly(GeneratedBase, false);

		if (&SourceInterface != SourceBaseMaterial)
		{
			GeneratedInstance->CopyMaterialUniformParametersEditorOnly(
				&SourceInterface,
				true);
		}

		GeneratedInstance->PostEditChange();

		return SaveNewAsset(*GeneratedInstance, OutError);
	}

	void BuildGpuInterpolationMaterials()
	{
		if (GEditor && GEditor->PlayWorld)
		{
			UE_LOG(
				LogAlgonaRTSEditor,
				Error,
				TEXT("[GpuInterpolation] Stop PIE before building materials."));
			return;
		}

		UAnimBank* AnimBank =
			LoadObject<UAnimBank>(nullptr, AlgonaGpuInterpolation::AnimBankPath);

		if (!AnimBank || !AnimBank->Asset)
		{
			UE_LOG(
				LogAlgonaRTSEditor,
				Error,
				TEXT("[GpuInterpolation] Cannot load AnimBank skinned asset: %s"),
				AlgonaGpuInterpolation::AnimBankPath);
			return;
		}

		const TArray<FSkeletalMaterial>& SourceMaterials =
			AnimBank->Asset->GetMaterials();

		if (SourceMaterials.IsEmpty())
		{
			UE_LOG(
				LogAlgonaRTSEditor,
				Error,
				TEXT("[GpuInterpolation] Skinned asset has no material slots."));
			return;
		}

		// Перезапись не поддерживается намеренно: сначала проверяем все пути,
		// чтобы не оставить наполовину сгенерированный набор.
		TArray<FString> OccupiedAssets;

		for (int32 SlotIndex = 0; SlotIndex < SourceMaterials.Num(); ++SlotIndex)
		{
			const FString AssetNames[] =
			{
				AlgonaGpuInterpolation::GetBaseMaterialAssetName(SlotIndex),
				AlgonaGpuInterpolation::GetMaterialInstanceAssetName(SlotIndex)
			};

			for (const FString& AssetName : AssetNames)
			{
				if (IsAssetPathOccupied(AssetName))
				{
					OccupiedAssets.Add(
						AlgonaGpuInterpolation::GetPackageName(AssetName));
				}
			}
		}

		if (!OccupiedAssets.IsEmpty())
		{
			UE_LOG(
				LogAlgonaRTSEditor,
				Error,
				TEXT("[GpuInterpolation] Assets already exist: %s. Delete them in the Content Browser (or restart the editor if already deleted) and run the command again."),
				*FString::Join(OccupiedAssets, TEXT(", ")));
			return;
		}

		for (int32 SlotIndex = 0; SlotIndex < SourceMaterials.Num(); ++SlotIndex)
		{
			UMaterialInterface* SourceInterface =
				SourceMaterials[SlotIndex].MaterialInterface;

			FString Error = TEXT("material slot is null");

			if (!SourceInterface
				|| !BuildMaterialSlot(SlotIndex, *SourceInterface, Error))
			{
				UE_LOG(
					LogAlgonaRTSEditor,
					Error,
					TEXT("[GpuInterpolation] Material slot %d failed: %s"),
					SlotIndex,
					*Error);
				return;
			}

			UE_LOG(
				LogAlgonaRTSEditor,
				Display,
				TEXT("[GpuInterpolation] Material slot %d saved: %s (source %s)"),
				SlotIndex,
				*AlgonaGpuInterpolation::GetObjectPath(
					AlgonaGpuInterpolation::GetMaterialInstanceAssetName(SlotIndex)),
				*SourceInterface->GetPathName());
		}

		UE_LOG(
			LogAlgonaRTSEditor,
			Display,
			TEXT("[GpuInterpolation] Done: %d material slot(s) in %s"),
			SourceMaterials.Num(),
			AlgonaGpuInterpolation::MaterialFolder);
	}

	FAutoConsoleCommand GAlgonaBuildGpuInterpolationMaterialsCommand(
		TEXT("algona.P1.BuildGpuInterpolationMaterials"),
		TEXT("Editor only (not in PIE): generate and save army GPU interpolation materials for the AnimBank skinned asset."),
		FConsoleCommandDelegate::CreateStatic(
			&BuildGpuInterpolationMaterials));
}
