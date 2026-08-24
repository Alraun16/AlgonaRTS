#include "Presentation/AlgonaArmySkinnedPresentationActor.h"

#include "Core/AlgonaSimulationSubsystem.h"

#include "Animation/AnimBank.h"
#include "Animation/AnimSequenceTransformProviderData.h"
#include "Camera/CameraComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Class.h"

#if WITH_EDITOR
#include "MaterialEditingLibrary.h"
#include "MaterialExpressionIO.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#endif

namespace
{
	constexpr TCHAR AnimBankPath[] =
		TEXT("/Game/NewFolder/NewAnimBank.NewAnimBank");

	constexpr TCHAR CameraCullCVarName[] =
		TEXT("algona.P1.PresentationCameraCull");

	constexpr int32 AnimationIndex = 0;

	// Per-instance GPU data layout.
	constexpr int32 PrevPositionIndex = 0;
	constexpr int32 PrevRotationIndex = 3;
	constexpr int32 PrevScaleIndex = 7;
	constexpr int32 CurrentPositionIndex = 10;
	constexpr int32 CurrentRotationIndex = 13;
	constexpr int32 CurrentScaleIndex = 17;
	constexpr int32 TierIndex = 20;
	constexpr int32 GpuInstanceCustomDataFloatCount = 21;

	// Component-level alpha values. FAR bypasses sub-step interpolation.
	constexpr int32 NearAlphaPrimitiveDataIndex = 0;
	constexpr int32 MediumAlphaPrimitiveDataIndex = 1;

	// Current P1 soldier/camera assumptions used only for Presentation quality.
	// Unreal units are centimetres.
	constexpr double ReferenceUnitHeightCm = 170.0;

	// Screen-space thresholds. The FAR decision also checks the actual
	// per-unit motion between adjacent Simulation snapshots, so a faster
	// unit automatically keeps interpolation farther away.
	// >16 px: full render-frame Slerp. <=10 px may become FAR only when
	// one authoritative Simulation step is <= 1 screen pixel.
	constexpr double NearMinProjectedHeightPixels = 16.0;
	constexpr double FarMaxProjectedHeightPixels = 10.0;
	constexpr double MediumMaxVisualStepPixels = 1.0;
	constexpr double TierHysteresis = 0.05;

	constexpr double TeleportDistance = 2000.0;

#if WITH_EDITOR
	template <typename TExpression>
	TExpression* CreateTransientMaterialExpression(
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

	bool InjectGpuInterpolationIntoMaterial(
		UMaterial& RuntimeMaterial,
		FString& OutError)
	{
		UMaterialEditorOnlyData* EditorData =
			RuntimeMaterial.GetEditorOnlyData();

		if (!EditorData)
		{
			OutError = TEXT("material has no editor-only graph data");
			return false;
		}

		if (RuntimeMaterial.bUseMaterialAttributes)
		{
			OutError =
				TEXT("material uses Material Attributes; test WPO injection does not support that graph yet");
			return false;
		}

		FExpressionInput OriginalWpoInput =
			static_cast<const FExpressionInput&>(
				EditorData->WorldPositionOffset);

		if (!OriginalWpoInput.Expression)
		{
			UMaterialExpressionConstant3Vector* ZeroWpo =
				CreateTransientMaterialExpression<
					UMaterialExpressionConstant3Vector>(
						&RuntimeMaterial,
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
			CreateTransientMaterialExpression<
				UMaterialExpressionWorldPosition>(
					&RuntimeMaterial,
					-900,
					-500);

		if (!WorldPosition)
		{
			OutError = TEXT("cannot create WorldPosition expression");
			return false;
		}

		WorldPosition->WorldPositionShaderOffset =
			WPT_ExcludeAllShaderOffsets;

		static const TCHAR* InstanceInputNames[
			GpuInstanceCustomDataFloatCount] =
		{
			TEXT("PrevPX"), TEXT("PrevPY"), TEXT("PrevPZ"),
			TEXT("PrevQX"), TEXT("PrevQY"), TEXT("PrevQZ"), TEXT("PrevQW"),
			TEXT("PrevSX"), TEXT("PrevSY"), TEXT("PrevSZ"),
			TEXT("CurrPX"), TEXT("CurrPY"), TEXT("CurrPZ"),
			TEXT("CurrQX"), TEXT("CurrQY"), TEXT("CurrQZ"), TEXT("CurrQW"),
			TEXT("CurrSX"), TEXT("CurrSY"), TEXT("CurrSZ"),
			TEXT("InterpTier")
		};

		TArray<UMaterialExpressionPerInstanceCustomData*>
			InstanceDataExpressions;
		InstanceDataExpressions.Reserve(
			GpuInstanceCustomDataFloatCount);

		for (int32 DataIndex = 0;
			DataIndex < GpuInstanceCustomDataFloatCount;
			++DataIndex)
		{
			UMaterialExpressionPerInstanceCustomData* Expression =
				CreateTransientMaterialExpression<
					UMaterialExpressionPerInstanceCustomData>(
						&RuntimeMaterial,
						-900,
						-400 + DataIndex * 40);

			if (!Expression)
			{
				OutError = TEXT("cannot create PerInstanceCustomData expression");
				return false;
			}

			Expression->DataIndex =
				static_cast<uint32>(DataIndex);
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
			{TEXT("AlphaNear"), NearAlphaPrimitiveDataIndex},
			{TEXT("AlphaMedium"), MediumAlphaPrimitiveDataIndex}
		};

		TArray<UMaterialExpressionScalarParameter*> AlphaExpressions;
		AlphaExpressions.Reserve(UE_ARRAY_COUNT(AlphaDescs));

		for (int32 Index = 0;
			Index < UE_ARRAY_COUNT(AlphaDescs);
			++Index)
		{
			UMaterialExpressionScalarParameter* Expression =
				CreateTransientMaterialExpression<
					UMaterialExpressionScalarParameter>(
						&RuntimeMaterial,
						-500,
						-400 + Index * 80);

			if (!Expression)
			{
				OutError = TEXT("cannot create CustomPrimitiveData alpha expression");
				return false;
			}

			Expression->ParameterName =
				FName(AlphaDescs[Index].Name);
			Expression->DefaultValue = 1.0f;
			Expression->bUseCustomPrimitiveData = true;
			Expression->PrimitiveDataIndex =
				static_cast<uint8>(AlphaDescs[Index].PrimitiveDataIndex);
			AlphaExpressions.Add(Expression);
		}

		UMaterialExpressionCustom* Custom =
			CreateTransientMaterialExpression<
				UMaterialExpressionCustom>(
					&RuntimeMaterial,
					100,
					0);

		if (!Custom)
		{
			OutError = TEXT("cannot create Custom interpolation expression");
			return false;
		}

		Custom->Description = TEXT("Algona GPU buffered interpolation");
		Custom->OutputType = CMOT_Float3;
		Custom->Code = TEXT(R"ALGONA(
// FAR uses the latest authoritative instance transform directly.
// CPU classification only assigns FAR when this specific unit's measured
// displacement per Simulation step is <= the one-pixel screen budget.
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

// NEAR gets true shortest-path Slerp. MEDIUM uses normalized Lerp,
// which is cheaper and visually sufficient at 10..16 px unit height.
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

// WorldPos is the already skinned vertex under the CURRENT instance
// transform, excluding Material WPO. Undo only the current rigid instance
// transform so the skeletal animation remains intact, then apply the
// interpolated rigid transform.
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

		for (int32 Index = 0;
			Index < InstanceDataExpressions.Num();
			++Index)
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

		for (int32 Index = 0;
			Index < AlphaExpressions.Num();
			++Index)
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

		if (!AddCustomInput(
			*Custom,
			TEXT("OriginalWPO"),
			OriginalWpoInput))
		{
			OutError = TEXT("cannot connect original WPO input");
			return false;
		}

		EditorData->WorldPositionOffset.Connect(0, Custom);
		RuntimeMaterial.bAlwaysEvaluateWorldPositionOffset = true;

		UMaterialEditingLibrary::SetBaseMaterialUsage(
			&RuntimeMaterial,
			MATUSAGE_InstancedSkinnedMesh,
			true);

		Custom->PostEditChange();
		RuntimeMaterial.PostEditChange();

		const TArray<FString> CompileErrors =
			UMaterialEditingLibrary::RecompileMaterial(
				&RuntimeMaterial);

		if (!CompileErrors.IsEmpty())
		{
			OutError = FString::Join(
				CompileErrors,
				TEXT(" | "));
			return false;
		}

		return true;
	}
#endif
}

AAlgonaArmySkinnedPresentationActor::AAlgonaArmySkinnedPresentationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot =
		CreateDefaultSubobject<USceneComponent>(
			TEXT("Root"));

	SetRootComponent(SceneRoot);

	InstancedSkinnedMeshComponent =
		CreateDefaultSubobject<UInstancedSkinnedMeshComponent>(
			TEXT("ArmySkinnedInstances"));

	InstancedSkinnedMeshComponent->SetupAttachment(SceneRoot);
	InstancedSkinnedMeshComponent->SetMobility(
		EComponentMobility::Movable);
	InstancedSkinnedMeshComponent->SetCollisionEnabled(
		ECollisionEnabled::NoCollision);
	InstancedSkinnedMeshComponent->SetGenerateOverlapEvents(false);
	InstancedSkinnedMeshComponent->SetCastShadow(false);
	InstancedSkinnedMeshComponent->SetAnimationMinScreenSize(-1.0f);

	// UE 5.8 documents this buffer for motion blur, not for visual
	// interpolation. Backend 3 therefore carries its interpolation pair in
	// explicit per-instance custom data consumed by WPO on the GPU.
	InstancedSkinnedMeshComponent->SetHasPerInstancePrevTransforms(false);
}

void AAlgonaArmySkinnedPresentationActor::BeginPlay()
{
	Super::BeginPlay();

	AnimBank =
		LoadObject<UAnimBank>(
			nullptr,
			AnimBankPath);

	if (!AnimBank)
	{
		Fail(
			FString::Printf(
				TEXT("Backend 3 failed: cannot load %s"),
				AnimBankPath));
		return;
	}

	if (!AnimBank->Asset)
	{
		Fail(
			TEXT(
				"Backend 3 failed: NewAnimBank -> Mapping -> Asset is None"));
		return;
	}

	if (AnimBank->Sequences.IsEmpty())
	{
		Fail(
			TEXT(
				"Backend 3 failed: NewAnimBank has no Sequences"));
		return;
	}

	if (!ValidateInstancingBuildSettings())
	{
		return;
	}
}

void AAlgonaArmySkinnedPresentationActor::SetPresentationCamera(
	UCameraComponent* InCameraComponent)
{
	PresentationCamera = InCameraComponent;
	bHasCameraView = false;
}

void AAlgonaArmySkinnedPresentationActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bRendererFailed)
	{
		return;
	}

	if (!bRendererReady)
	{
		if (!PrepareSkinnedRenderer())
		{
			return;
		}
	}

	UWorld* World = GetWorld();
	if (!World || !InstancedSkinnedMeshComponent)
	{
		return;
	}

	UAlgonaSimulationSubsystem* Simulation =
		World->GetSubsystem<UAlgonaSimulationSubsystem>();

	if (!Simulation)
	{
		return;
	}


	VisibilityRefreshElapsedSeconds +=
		FMath::Max(DeltaSeconds, 0.0f);

	const uint64 CurrentSimulationTick =
		Simulation->GetSimulationTick();

	const uint64 CurrentRevision =
		Simulation->GetStateRevision();

	bool bSimulationChanged = false;
	bool bForceSnapshotSnap = false;

	if (!bHasCapturedState
		|| CurrentRevision != LastStateRevision)
	{
		// If more than one changed snapshot elapsed between render frames,
		// we no longer have the exact adjacent pair required by buffered
		// interpolation. Snap instead of stretching a multi-step gap.
		bForceSnapshotSnap =
			bHasCapturedState
			&& CurrentRevision > LastStateRevision + 1;

		CaptureLatestState(*Simulation);

		LastStateRevision = CurrentRevision;
		bHasCapturedState = true;
		bSimulationChanged = true;
	}

	const bool bCullingEnabled =
		IsCameraCullingEnabled();

	const bool bCullingModeChanged =
		!bHasCullingMode
		|| bCullingEnabled != bLastCullingEnabled;

	const bool bCameraChanged =
		bCullingEnabled
		&& HasCameraViewChanged();

	const bool bRefreshForCamera =
		bCameraChanged
		&& VisibilityRefreshElapsedSeconds
			>= VisibilityRefreshIntervalSeconds;

	if (bSimulationChanged
		|| bCullingModeChanged
		|| bRefreshForCamera)
	{
		RefreshPresentationWorkingSet(
			bSimulationChanged,
			bForceSnapshotSnap,
			Simulation->GetFixedStepSeconds());

		if (bRendererFailed)
		{
			return;
		}

		CacheCurrentCameraView();

		VisibilityRefreshElapsedSeconds = 0.0f;

		bLastCullingEnabled = bCullingEnabled;
		bHasCullingMode = true;
	}

	const bool bSimulationAdvancedWithoutStateChange =
		!bSimulationChanged
		&& CurrentSimulationTick != LastSimulationTick;

	if (bInterpolationActive)
	{
		if (bSimulationAdvancedWithoutStateChange)
		{
			CompleteBufferedInterpolation();
		}
		else
		{
			UpdateGpuInterpolationAlphas(
				Simulation->GetInterpolationAlpha(),
				DeltaSeconds);
		}
	}

	LastSimulationTick = CurrentSimulationTick;

#if !UE_BUILD_SHIPPING
	DebugCounterElapsedSeconds +=
		FMath::Max(DeltaSeconds, 0.0f);

	if (DebugCounterElapsedSeconds >= 0.2f)
	{
		DebugCounterElapsedSeconds = 0.0f;

		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(
				42001,
				0.25f,
				FColor::White,
				FString::Printf(
					TEXT(
						"Presented ISKM: %d   Simulation: %d   UnitPx: %.1f   MediumCadence: %d   MedMaxSpeed: %.0f cm/s   Alpha N/M: %.2f/%.2f"),
					InstancedSkinnedMeshComponent->GetInstanceCount(),
					CachedSnapshots.Num(),
					ProjectedUnitHeightPixels,
					MediumFrameCadence,
					MaxMediumObservedSpeedCmPerSecond,
					NearInterpolationAlpha,
					MediumInterpolationAlpha));
		}
	}
#endif
}

bool AAlgonaArmySkinnedPresentationActor::PrepareSkinnedRenderer()
{
	if (bRendererReady)
	{
		return true;
	}

	if (bRendererFailed
		|| !AnimBank
		|| !InstancedSkinnedMeshComponent)
	{
		return false;
	}

	if (AnimBank->IsCompiling())
	{
		ProviderReadyFrames = 0;
		return false;
	}

	if (!SequenceProvider)
	{
		SequenceProvider =
			CreateSequenceProvider(AnimBank);

		if (!SequenceProvider)
		{
			Fail(
				TEXT(
					"Backend 3 failed: CreateFromAnimBank returned null"));
			return false;
		}

		ProviderReadyFrames = 0;
		return false;
	}

	if (SequenceProvider->IsCompiling())
	{
		ProviderReadyFrames = 0;
		return false;
	}

	++ProviderReadyFrames;
	if (ProviderReadyFrames < 3)
	{
		return false;
	}

	InstancedSkinnedMeshComponent->ClearInstances();
	InstancedSkinnedMeshComponent->SetTransformProvider(nullptr);

	InstancedSkinnedMeshComponent->SetSkinnedAssetAndUpdate(
		AnimBank->Asset.Get(),
		true);

	InstancedSkinnedMeshComponent->SetTransformProvider(
		SequenceProvider);

	InstancedSkinnedMeshComponent->SetNumCustomDataFloats(
		GpuInstanceCustomDataFloatCount);

	if (!BuildGpuInterpolationMaterials())
	{
		return false;
	}

	CompleteBufferedInterpolation();
	bRendererReady = true;

	Succeed(
		TEXT(
			"Backend 3 ready: ISKM + AnimSequenceTransformProvider + GPU buffered interpolation"));

	return true;
}

UAnimSequenceTransformProviderData*
AAlgonaArmySkinnedPresentationActor::CreateSequenceProvider(
	UAnimBank* InAnimBank)
{
	if (!InAnimBank)
	{
		return nullptr;
	}

	UClass* ProviderClass =
		UAnimSequenceTransformProviderData::StaticClass();

	if (!ProviderClass)
	{
		return nullptr;
	}

	UFunction* Function =
		ProviderClass->FindFunctionByName(
			TEXT("CreateFromAnimBank"));

	if (!Function)
	{
		return nullptr;
	}

	struct FCreateFromAnimBankParams
	{
		UAnimBank* InAnimBank = nullptr;
		UAnimSequenceTransformProviderData* ReturnValue = nullptr;
	};

	FCreateFromAnimBankParams Params;
	Params.InAnimBank = InAnimBank;

	UObject* ClassDefaultObject =
		ProviderClass->GetDefaultObject();

	if (!ClassDefaultObject)
	{
		return nullptr;
	}

	ClassDefaultObject->ProcessEvent(
		Function,
		&Params);

	return Params.ReturnValue;
}

bool AAlgonaArmySkinnedPresentationActor::ValidateInstancingBuildSettings()
{
	if (!AnimBank || !AnimBank->Asset)
	{
		return false;
	}

	const FSkeletalMeshLODInfo* LOD0 =
		AnimBank->Asset->GetLODInfo(0);

	if (!LOD0)
	{
		Fail(
			TEXT(
				"Backend 3 failed: skinned asset has no LOD0 info"));
		return false;
	}

	if (!LOD0->BuildSettings.bOptimizeForInstancing)
	{
		Fail(
			TEXT(
				"Backend 3 failed: LOD0 Optimize For Instancing is OFF"));
		return false;
	}

	return true;
}

bool AAlgonaArmySkinnedPresentationActor::BuildGpuInterpolationMaterials()
{
#if !WITH_EDITOR
	Fail(
		TEXT(
			"Backend 3 GPU interpolation test currently requires an Editor build because it creates transient WPO materials at runtime"));
	return false;
#else
	if (!AnimBank || !AnimBank->Asset || !InstancedSkinnedMeshComponent)
	{
		return false;
	}

	RuntimeInterpolationMaterials.Reset();

	const TArray<FSkeletalMaterial>& SourceMaterials =
		AnimBank->Asset->GetMaterials();

	RuntimeInterpolationMaterials.Reserve(
		SourceMaterials.Num());

	for (int32 MaterialIndex = 0;
		MaterialIndex < SourceMaterials.Num();
		++MaterialIndex)
	{
		UMaterialInterface* SourceInterface =
			SourceMaterials[MaterialIndex].MaterialInterface;

		if (!SourceInterface)
		{
			Fail(
				FString::Printf(
					TEXT("Backend 3 failed: material slot %d is null"),
					MaterialIndex));
			return false;
		}

		UMaterial* SourceBaseMaterial =
			SourceInterface->GetMaterial();

		if (!SourceBaseMaterial)
		{
			Fail(
				FString::Printf(
					TEXT("Backend 3 failed: material slot %d has no base material"),
					MaterialIndex));
			return false;
		}

		UMaterial* RuntimeBaseMaterial =
			DuplicateObject<UMaterial>(
				SourceBaseMaterial,
				this);

		if (!RuntimeBaseMaterial)
		{
			Fail(
				FString::Printf(
					TEXT("Backend 3 failed: cannot duplicate material slot %d"),
					MaterialIndex));
			return false;
		}

		RuntimeBaseMaterial->SetFlags(RF_Transient);

		FString MaterialError;
		if (!InjectGpuInterpolationIntoMaterial(
			*RuntimeBaseMaterial,
			MaterialError))
		{
			Fail(
				FString::Printf(
					TEXT("Backend 3 GPU material %d failed: %s"),
					MaterialIndex,
					*MaterialError));
			return false;
		}

		UMaterialInterface* RuntimeInterface =
			RuntimeBaseMaterial;

		if (SourceInterface != SourceBaseMaterial)
		{
			UMaterialInstanceConstant* RuntimeInstance =
				NewObject<UMaterialInstanceConstant>(
					this,
					NAME_None,
					RF_Transient);

			if (!RuntimeInstance)
			{
				Fail(
					TEXT(
						"Backend 3 failed: cannot create transient material instance"));
				return false;
			}

			RuntimeInstance->SetParentEditorOnly(
				RuntimeBaseMaterial,
				false);

			RuntimeInstance->CopyMaterialUniformParametersEditorOnly(
				SourceInterface,
				true);

			RuntimeInstance->PostEditChange();
			RuntimeInterface = RuntimeInstance;
		}

		// The green READY message must mean shader compilation is already
		// finished, otherwise the first benchmark seconds would include a
		// transient material compile.
		RuntimeInterface->EnsureIsComplete();

		RuntimeInterpolationMaterials.Add(
			RuntimeInterface);

		InstancedSkinnedMeshComponent->SetMaterial(
			MaterialIndex,
			RuntimeInterface);
	}

	return true;
#endif
}

void AAlgonaArmySkinnedPresentationActor::CaptureLatestState(
	UAlgonaSimulationSubsystem& Simulation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSkinnedPresentation_CaptureState);

	Simulation.ExportSoldierSnapshots(
		CachedSnapshots,
		MaxPresentedEntities);
}

void AAlgonaArmySkinnedPresentationActor::RefreshPresentationWorkingSet(
	bool bSimulationChanged,
	bool bForceSnapshotSnap,
	double SimulationStepSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSkinnedPresentation_BuildCameraWorkingSet);

	NextPresentedEntityIds.Reset();
	NextCurrentTransforms.Reset();
	NextInterpolationTiers.Reset();
	NextObservedLinearSpeedsCmPerSecond.Reset();

	NextPresentedEntityIds.Reserve(CachedSnapshots.Num());
	NextCurrentTransforms.Reserve(CachedSnapshots.Num());

	// Presentation derives visual motion rate from the authoritative
	// snapshots themselves. No gameplay movement-speed constant is mirrored
	// here, so different unit speeds need no Presentation rewrite.
	LastSimulationStepSeconds = FMath::Max(
		SimulationStepSeconds,
		UE_DOUBLE_SMALL_NUMBER);

	UCameraComponent* Camera = PresentationCamera.Get();

	const bool bUseCameraCulling =
		IsCameraCullingEnabled()
		&& Camera
		&& Camera->ProjectionMode
			== ECameraProjectionMode::Orthographic;

	FVector GroundTopLeft = FVector::ZeroVector;
	FVector GroundTopRight = FVector::ZeroVector;
	FVector GroundBottomLeft = FVector::ZeroVector;

	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	bool bUseGroundViewCulling = false;

	if (bUseCameraCulling)
	{
		UWorld* World = GetWorld();
		APlayerController* PlayerController =
			World ? World->GetFirstPlayerController() : nullptr;

		if (PlayerController)
		{
			PlayerController->GetViewportSize(
				ViewportWidth,
				ViewportHeight);
		}

		/*
		 * Build the orthographic ground footprint directly from the camera
		 * component that Presentation already depends on.
		 *
		 * Do not use PlayerController deprojection here. The camera actor can
		 * change OrthoWidth earlier in the frame than the controller/camera
		 * manager view-projection is refreshed. Mixing those two states can
		 * build a working set for the old zoom and then cache the new zoom as
		 * already processed.
		 */
		if (ViewportWidth > 0
			&& ViewportHeight > 0
			&& Camera->OrthoWidth > KINDA_SMALL_NUMBER)
		{
			const FVector CameraForward = Camera->GetForwardVector();

			if (FMath::Abs(CameraForward.Z) > KINDA_SMALL_NUMBER)
			{
				const double ViewAspectRatio =
					static_cast<double>(ViewportWidth)
					/ static_cast<double>(ViewportHeight);

				const double HalfViewWidth =
					static_cast<double>(Camera->OrthoWidth) * 0.5;

				const double HalfViewHeight =
					HalfViewWidth / ViewAspectRatio;

				const FVector CameraLocation =
					Camera->GetComponentLocation();

				const FVector CameraRight = Camera->GetRightVector();
				const FVector CameraUp = Camera->GetUpVector();

				auto ProjectOrthoCornerToGround =
					[&](
						double RightOffset,
						double UpOffset,
						FVector& OutGroundPosition)
					{
						const FVector RayOrigin =
							CameraLocation
							+ CameraRight * RightOffset
							+ CameraUp * UpOffset;

						const double Distance =
							-RayOrigin.Z / CameraForward.Z;

						if (Distance < 0.0)
						{
							return false;
						}

						OutGroundPosition =
							RayOrigin + CameraForward * Distance;

						OutGroundPosition.Z = 0.0;
						return true;
					};

				bUseGroundViewCulling =
					ProjectOrthoCornerToGround(
						-HalfViewWidth,
						HalfViewHeight,
						GroundTopLeft)
					&& ProjectOrthoCornerToGround(
						HalfViewWidth,
						HalfViewHeight,
						GroundTopRight)
					&& ProjectOrthoCornerToGround(
						-HalfViewWidth,
						-HalfViewHeight,
						GroundBottomLeft);
			}
		}
	}

	// Orthographic screen significance is global for equal-sized soldiers:
	// apparent size depends on OrthoWidth, not world-space distance to camera.
	ProjectedUnitHeightPixels = 0.0f;
	GroundPixelsPerWorldUnit = 0.0f;

	if (Camera
		&& Camera->ProjectionMode == ECameraProjectionMode::Orthographic
		&& ViewportWidth > 0
		&& Camera->OrthoWidth > KINDA_SMALL_NUMBER)
	{
		GroundPixelsPerWorldUnit =
			static_cast<float>(ViewportWidth) / Camera->OrthoWidth;

		// The 170 cm vertical model is foreshortened by the camera pitch.
		// Dot(WorldUp, CameraUp) gives its vertical screen-plane projection.
		const double VerticalProjection = FMath::Abs(
			FVector::DotProduct(
				FVector::UpVector,
				Camera->GetUpVector()));

		ProjectedUnitHeightPixels = static_cast<float>(
			ReferenceUnitHeightCm
			* VerticalProjection
			* static_cast<double>(GroundPixelsPerWorldUnit));
	}

	const FVector ViewRight =
		GroundTopRight - GroundTopLeft;
	const FVector ViewDown =
		GroundBottomLeft - GroundTopLeft;

	const double ViewDeterminant =
		ViewRight.X * ViewDown.Y
		- ViewRight.Y * ViewDown.X;

	constexpr double CullingGuardPixels = 128.0;

	const double HorizontalGuard =
		ViewportWidth > 0
			? CullingGuardPixels
				/ static_cast<double>(ViewportWidth)
			: 0.0;

	const double VerticalGuard =
		ViewportHeight > 0
			? CullingGuardPixels
				/ static_cast<double>(ViewportHeight)
			: 0.0;

	// Preserve the same mesh-facing correction used by Backend 1 so the
	// A/B comparison changes only the renderer/interpolation path.
	const FQuat MeshFacingCorrection =
		FRotator(0.0f, -90.0f, 0.0f).Quaternion();

	for (const FAlgonaSoldierSnapshot& Snapshot : CachedSnapshots)
	{
		if (bUseGroundViewCulling
			&& FMath::Abs(ViewDeterminant)
				> UE_DOUBLE_SMALL_NUMBER)
		{
			const FVector ToSoldier =
				Snapshot.Position - GroundTopLeft;

			const double Horizontal =
				(ToSoldier.X * ViewDown.Y
					- ToSoldier.Y * ViewDown.X)
				/ ViewDeterminant;

			const double Vertical =
				(ViewRight.X * ToSoldier.Y
					- ViewRight.Y * ToSoldier.X)
				/ ViewDeterminant;

			if (Horizontal < -HorizontalGuard
				|| Horizontal > 1.0 + HorizontalGuard
				|| Vertical < -VerticalGuard
				|| Vertical > 1.0 + VerticalGuard)
			{
				continue;
			}
		}

		const FQuat PresentationRotation =
			(Snapshot.Facing * MeshFacingCorrection).GetNormalized();

		NextPresentedEntityIds.Add(Snapshot.EntityId);
		NextCurrentTransforms.Emplace(
			PresentationRotation,
			Snapshot.Position,
			InstanceScale);
	}

	const bool bSameEntityOrder =
		HasSameEntityOrder(NextPresentedEntityIds);

	if (!bSameEntityOrder)
	{
		TMap<uint32, int32> OldIndexByEntityId;
		OldIndexByEntityId.Reserve(PresentedEntityIds.Num());

		for (int32 OldIndex = 0;
			OldIndex < PresentedEntityIds.Num();
			++OldIndex)
		{
			OldIndexByEntityId.Add(
				PresentedEntityIds[OldIndex],
				OldIndex);
		}

		TArray<FTransform> NewPreviousTransforms;
		TArray<FTransform> NewCurrentTransforms;
		TArray<float> NewObservedLinearSpeedsCmPerSecond;
		TArray<EAlgonaPresentationInterpolationTier> NewTiers;

		NewPreviousTransforms.SetNumUninitialized(
			NextPresentedEntityIds.Num());
		NewCurrentTransforms.SetNumUninitialized(
			NextPresentedEntityIds.Num());
		NewObservedLinearSpeedsCmPerSecond.SetNumZeroed(
			NextPresentedEntityIds.Num());
		NewTiers.SetNumUninitialized(
			NextPresentedEntityIds.Num());

		bool bAnyInterpolatedTransformChanged = false;

		for (int32 NewIndex = 0;
			NewIndex < NextPresentedEntityIds.Num();
			++NewIndex)
		{
			const uint32 EntityId =
				NextPresentedEntityIds[NewIndex];

			const FTransform& SnapshotTransform =
				NextCurrentTransforms[NewIndex];

			const int32* OldIndexPtr =
				OldIndexByEntityId.Find(EntityId);

			if (OldIndexPtr
				&& PreviousTransforms.IsValidIndex(*OldIndexPtr)
				&& CurrentTransforms.IsValidIndex(*OldIndexPtr)
				&& InterpolationTiers.IsValidIndex(*OldIndexPtr))
			{
				const int32 OldIndex = *OldIndexPtr;

				float ObservedSpeedCmPerSecond = 0.0f;

				if (bSimulationChanged && !bForceSnapshotSnap)
				{
					ObservedSpeedCmPerSecond = static_cast<float>(
						FVector::Dist(
							CurrentTransforms[OldIndex].GetLocation(),
							SnapshotTransform.GetLocation())
						/ LastSimulationStepSeconds);
				}
				else if (ObservedLinearSpeedsCmPerSecond.IsValidIndex(OldIndex))
				{
					ObservedSpeedCmPerSecond =
						ObservedLinearSpeedsCmPerSecond[OldIndex];
				}

				NewObservedLinearSpeedsCmPerSecond[NewIndex] =
					ObservedSpeedCmPerSecond;

				const EAlgonaPresentationInterpolationTier NewTier =
					DetermineInterpolationTier(
						ObservedSpeedCmPerSecond,
						InterpolationTiers[OldIndex],
						true);

				NewTiers[NewIndex] = NewTier;

				if (bSimulationChanged)
				{
					NewCurrentTransforms[NewIndex] =
						SnapshotTransform;
					NewPreviousTransforms[NewIndex] =
						CurrentTransforms[OldIndex];

					if (bForceSnapshotSnap
						|| ShouldSnapInterpolation(
							NewPreviousTransforms[NewIndex],
							NewCurrentTransforms[NewIndex]))
					{
						NewPreviousTransforms[NewIndex] =
							NewCurrentTransforms[NewIndex];
					}
					else if (NewTier != EAlgonaPresentationInterpolationTier::Far
						&& !NewPreviousTransforms[NewIndex].Equals(
							NewCurrentTransforms[NewIndex],
							0.01f))
					{
						bAnyInterpolatedTransformChanged = true;
					}
				}
				else
				{
					NewPreviousTransforms[NewIndex] =
						PreviousTransforms[OldIndex];
					NewCurrentTransforms[NewIndex] =
						CurrentTransforms[OldIndex];
				}
			}
			else
			{
				// Spawn or camera re-entry: never interpolate from stale data.
				NewPreviousTransforms[NewIndex] =
					SnapshotTransform;
				NewCurrentTransforms[NewIndex] =
					SnapshotTransform;
				NewObservedLinearSpeedsCmPerSecond[NewIndex] = 0.0f;
				NewTiers[NewIndex] =
					DetermineInterpolationTier(
						0.0f,
						EAlgonaPresentationInterpolationTier::Near,
						false);
			}
		}

		PresentedEntityIds = MoveTemp(NextPresentedEntityIds);
		PreviousTransforms = MoveTemp(NewPreviousTransforms);
		CurrentTransforms = MoveTemp(NewCurrentTransforms);
		ObservedLinearSpeedsCmPerSecond =
			MoveTemp(NewObservedLinearSpeedsCmPerSecond);
		InterpolationTiers = MoveTemp(NewTiers);
		RefreshInterpolationTierPresence();

		if (bSimulationChanged)
		{
			bInterpolationActive =
				bAnyInterpolatedTransformChanged;

			if (bInterpolationActive)
			{
				BeginBufferedInterpolation();
			}
			else
			{
				CompleteBufferedInterpolation();
			}
		}

		RebuildInstances();
		return;
	}

	NextInterpolationTiers.SetNumUninitialized(
		NextPresentedEntityIds.Num());
	NextObservedLinearSpeedsCmPerSecond.SetNumZeroed(
		NextPresentedEntityIds.Num());

	for (int32 Index = 0;
		Index < NextPresentedEntityIds.Num();
		++Index)
	{
		const bool bHasPreviousTier =
			InterpolationTiers.IsValidIndex(Index);

		float ObservedSpeedCmPerSecond = 0.0f;

		if (bSimulationChanged
			&& !bForceSnapshotSnap
			&& CurrentTransforms.IsValidIndex(Index))
		{
			ObservedSpeedCmPerSecond = static_cast<float>(
				FVector::Dist(
					CurrentTransforms[Index].GetLocation(),
					NextCurrentTransforms[Index].GetLocation())
				/ LastSimulationStepSeconds);
		}
		else if (ObservedLinearSpeedsCmPerSecond.IsValidIndex(Index))
		{
			ObservedSpeedCmPerSecond =
				ObservedLinearSpeedsCmPerSecond[Index];
		}

		NextObservedLinearSpeedsCmPerSecond[Index] =
			ObservedSpeedCmPerSecond;

		NextInterpolationTiers[Index] =
			DetermineInterpolationTier(
				ObservedSpeedCmPerSecond,
				bHasPreviousTier
					? InterpolationTiers[Index]
					: EAlgonaPresentationInterpolationTier::Near,
				bHasPreviousTier);
	}

	if (!bSimulationChanged)
	{
		ObservedLinearSpeedsCmPerSecond =
			MoveTemp(NextObservedLinearSpeedsCmPerSecond);
		InterpolationTiers =
			MoveTemp(NextInterpolationTiers);
		RefreshInterpolationTierPresence();

		for (int32 Index = 0;
			Index < InstanceIds.Num();
			++Index)
		{
			if (!UploadInstanceGpuData(Index))
			{
				return;
			}
		}
		return;
	}

	PreviousTransforms = CurrentTransforms;
	CurrentTransforms = MoveTemp(NextCurrentTransforms);
	ObservedLinearSpeedsCmPerSecond =
		MoveTemp(NextObservedLinearSpeedsCmPerSecond);
	InterpolationTiers = MoveTemp(NextInterpolationTiers);
	RefreshInterpolationTierPresence();

	if (PreviousTransforms.Num() != CurrentTransforms.Num()
		|| ObservedLinearSpeedsCmPerSecond.Num() != CurrentTransforms.Num()
		|| InterpolationTiers.Num() != CurrentTransforms.Num())
	{
		Fail(
			TEXT(
				"Backend 3 failed: interpolation buffers lost entity alignment"));
		return;
	}

	bool bAnyInterpolatedTransformChanged = false;

	for (int32 Index = 0;
		Index < CurrentTransforms.Num();
		++Index)
	{
		if (bForceSnapshotSnap
			|| ShouldSnapInterpolation(
				PreviousTransforms[Index],
				CurrentTransforms[Index]))
		{
			PreviousTransforms[Index] = CurrentTransforms[Index];
			continue;
		}

		if (InterpolationTiers[Index] != EAlgonaPresentationInterpolationTier::Far
			&& !PreviousTransforms[Index].Equals(
				CurrentTransforms[Index],
				0.01f))
		{
			bAnyInterpolatedTransformChanged = true;
		}
	}

	bInterpolationActive =
		bAnyInterpolatedTransformChanged;

	if (!UploadSimulationStateToInstances())
	{
		return;
	}

	if (bInterpolationActive)
	{
		BeginBufferedInterpolation();
	}
	else
	{
		CompleteBufferedInterpolation();
	}
}

bool AAlgonaArmySkinnedPresentationActor::HasSameEntityOrder(
	const TArray<uint32>& CandidateEntityIds) const
{
	if (PresentedEntityIds.Num() != CandidateEntityIds.Num())
	{
		return false;
	}

	for (int32 Index = 0;
		Index < CandidateEntityIds.Num();
		++Index)
	{
		if (PresentedEntityIds[Index] != CandidateEntityIds[Index])
		{
			return false;
		}
	}

	return true;
}

bool AAlgonaArmySkinnedPresentationActor::HasCameraViewChanged() const
{
	const UCameraComponent* Camera = PresentationCamera.Get();

	if (!Camera)
	{
		return false;
	}

	if (!bHasCameraView)
	{
		return true;
	}

	if (!Camera->GetComponentTransform().Equals(
		LastCameraTransform,
		0.01f))
	{
		return true;
	}

	if (!FMath::IsNearlyEqual(
		Camera->OrthoWidth,
		LastCameraOrthoWidth,
		0.01f))
	{
		return true;
	}

	return !FMath::IsNearlyEqual(
		Camera->AspectRatio,
		LastCameraAspectRatio,
		0.001f);
}

void AAlgonaArmySkinnedPresentationActor::CacheCurrentCameraView()
{
	const UCameraComponent* Camera = PresentationCamera.Get();

	if (!Camera)
	{
		bHasCameraView = false;
		return;
	}

	LastCameraTransform = Camera->GetComponentTransform();
	LastCameraOrthoWidth = Camera->OrthoWidth;
	LastCameraAspectRatio = Camera->AspectRatio;
	bHasCameraView = true;
}

bool AAlgonaArmySkinnedPresentationActor::IsCameraCullingEnabled() const
{
	const IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(
			CameraCullCVarName);

	return !Variable || Variable->GetInt() != 0;
}

EAlgonaPresentationInterpolationTier
AAlgonaArmySkinnedPresentationActor::DetermineInterpolationTier(
	float LinearSpeedCmPerSecond,
	EAlgonaPresentationInterpolationTier PreviousTier,
	bool bHasPreviousTier) const
{
	const UCameraComponent* Camera = PresentationCamera.Get();

	// This P1 experiment uses the orthographic RTS camera. If another camera
	// type is substituted, prefer full quality rather than guessing a tier.
	if (!Camera
		|| Camera->ProjectionMode != ECameraProjectionMode::Orthographic
		|| ProjectedUnitHeightPixels <= 0.0f
		|| GroundPixelsPerWorldUnit <= 0.0f)
	{
		return EAlgonaPresentationInterpolationTier::Near;
	}

	const double UnitPixels =
		static_cast<double>(ProjectedUnitHeightPixels);

	const double SafeSpeedCmPerSecond = FMath::Max(
		static_cast<double>(LinearSpeedCmPerSecond),
		0.0);

	// Actual screen displacement of this unit during one authoritative
	// Simulation step. No gameplay movement-speed constant is duplicated here.
	const double SnapshotStepPixels =
		SafeSpeedCmPerSecond
		* LastSimulationStepSeconds
		* static_cast<double>(GroundPixelsPerWorldUnit);

	auto ClassifyRaw = [SnapshotStepPixels](double HeightPixels)
	{
		if (HeightPixels >= NearMinProjectedHeightPixels)
		{
			return EAlgonaPresentationInterpolationTier::Near;
		}

		// FAR is allowed only when both the soldier itself is tiny and its
		// entire 40 Hz snapshot jump is at most one screen pixel. A faster
		// unit therefore remains MEDIUM farther out automatically.
		if (HeightPixels <= FarMaxProjectedHeightPixels
			&& SnapshotStepPixels <= MediumMaxVisualStepPixels)
		{
			return EAlgonaPresentationInterpolationTier::Far;
		}

		return EAlgonaPresentationInterpolationTier::Medium;
	};

	if (!bHasPreviousTier)
	{
		return ClassifyRaw(UnitPixels);
	}

	// Screen-space hysteresis. FAR additionally uses motion hysteresis so a
	// unit with speed near the one-pixel boundary does not flap tiers.
	switch (PreviousTier)
	{
	case EAlgonaPresentationInterpolationTier::Near:
		if (UnitPixels >=
			NearMinProjectedHeightPixels * (1.0 - TierHysteresis))
		{
			return EAlgonaPresentationInterpolationTier::Near;
		}
		break;

	case EAlgonaPresentationInterpolationTier::Medium:
	{
		const bool bClearlyNear =
			UnitPixels >=
				NearMinProjectedHeightPixels * (1.0 + TierHysteresis);

		const bool bClearlyFar =
			UnitPixels <=
				FarMaxProjectedHeightPixels * (1.0 - TierHysteresis)
			&& SnapshotStepPixels <=
				MediumMaxVisualStepPixels * (1.0 - TierHysteresis);

		if (!bClearlyNear && !bClearlyFar)
		{
			return EAlgonaPresentationInterpolationTier::Medium;
		}
		break;
	}

	case EAlgonaPresentationInterpolationTier::Far:
		if (UnitPixels <=
				FarMaxProjectedHeightPixels * (1.0 + TierHysteresis)
			&& SnapshotStepPixels <=
				MediumMaxVisualStepPixels * (1.0 + TierHysteresis))
		{
			return EAlgonaPresentationInterpolationTier::Far;
		}
		break;
	}

	return ClassifyRaw(UnitPixels);
}

bool AAlgonaArmySkinnedPresentationActor::ShouldSnapInterpolation(
	const FTransform& Previous,
	const FTransform& Current) const
{
	return FVector::DistSquared(
		Previous.GetLocation(),
		Current.GetLocation())
		> FMath::Square(TeleportDistance);
}

bool AAlgonaArmySkinnedPresentationActor::RebuildInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSkinnedPresentation_RebuildInstances);

	if (!InstancedSkinnedMeshComponent)
	{
		return false;
	}

	InstancedSkinnedMeshComponent->ClearInstances();
	InstanceIds.Reset();

	if (CurrentTransforms.IsEmpty())
	{
		return true;
	}

	AnimationIndicesScratch.Init(
		AnimationIndex,
		CurrentTransforms.Num());

	FBox WorkingSetBounds(EForceInit::ForceInit);

	for (int32 Index = 0;
		Index < CurrentTransforms.Num();
		++Index)
	{
		WorkingSetBounds += CurrentTransforms[Index].GetLocation();

		if (PreviousTransforms.IsValidIndex(Index))
		{
			WorkingSetBounds += PreviousTransforms[Index].GetLocation();
		}
	}

	if (WorkingSetBounds.IsValid)
	{
		WorkingSetBounds = WorkingSetBounds.ExpandBy(
			FVector(5000.0, 5000.0, 5000.0));

		InstancedSkinnedMeshComponent->SetPrimitiveBoundsOverride(
			WorkingSetBounds);
	}

	InstanceIds =
		InstancedSkinnedMeshComponent->AddInstances(
			CurrentTransforms,
			AnimationIndicesScratch,
			true,
			false);

	const int32 CreatedCount =
		InstancedSkinnedMeshComponent->GetInstanceCount();

	if (CreatedCount != CurrentTransforms.Num()
		|| InstanceIds.Num() != CurrentTransforms.Num())
	{
		Fail(
			FString::Printf(
				TEXT(
					"Backend 3 failed: created %d / %d instances, ids=%d"),
				CreatedCount,
				CurrentTransforms.Num(),
				InstanceIds.Num()));
		return false;
	}

	for (int32 Index = 0;
		Index < InstanceIds.Num();
		++Index)
	{
		if (!UploadInstanceGpuData(Index))
		{
			return false;
		}
	}

	return true;
}

bool AAlgonaArmySkinnedPresentationActor::UploadSimulationStateToInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaSkinnedPresentation_UploadSimulationState);

	if (!InstancedSkinnedMeshComponent
		|| InstanceIds.Num() != CurrentTransforms.Num())
	{
		Fail(
			TEXT(
				"Backend 3 failed: simulation/instance working-set mismatch"));
		return false;
	}

	for (int32 Index = 0;
		Index < InstanceIds.Num();
		++Index)
	{
		if (!InstancedSkinnedMeshComponent->SetInstanceTransform(
			InstanceIds[Index],
			CurrentTransforms[Index],
			false))
		{
			Fail(
				FString::Printf(
					TEXT(
						"Backend 3 failed: SetInstanceTransform failed at %d"),
					Index));
			return false;
		}

		if (!UploadInstanceGpuData(Index))
		{
			return false;
		}
	}

	return true;
}

bool AAlgonaArmySkinnedPresentationActor::UploadInstanceGpuData(
	int32 Index)
{
	if (!InstancedSkinnedMeshComponent
		|| !InstanceIds.IsValidIndex(Index)
		|| !PreviousTransforms.IsValidIndex(Index)
		|| !CurrentTransforms.IsValidIndex(Index)
		|| !InterpolationTiers.IsValidIndex(Index))
	{
		Fail(
			TEXT(
				"Backend 3 failed: invalid GPU interpolation instance data"));
		return false;
	}

	float Data[GpuInstanceCustomDataFloatCount] = {};

	const FTransform& Previous = PreviousTransforms[Index];
	const FTransform& Current = CurrentTransforms[Index];

	const FVector PrevPosition = Previous.GetLocation();
	const FQuat PrevRotation = Previous.GetRotation().GetNormalized();
	const FVector PrevScale = Previous.GetScale3D();

	const FVector CurrentPosition = Current.GetLocation();
	const FQuat CurrentRotation = Current.GetRotation().GetNormalized();
	const FVector CurrentScale = Current.GetScale3D();

	Data[PrevPositionIndex + 0] = static_cast<float>(PrevPosition.X);
	Data[PrevPositionIndex + 1] = static_cast<float>(PrevPosition.Y);
	Data[PrevPositionIndex + 2] = static_cast<float>(PrevPosition.Z);

	Data[PrevRotationIndex + 0] = static_cast<float>(PrevRotation.X);
	Data[PrevRotationIndex + 1] = static_cast<float>(PrevRotation.Y);
	Data[PrevRotationIndex + 2] = static_cast<float>(PrevRotation.Z);
	Data[PrevRotationIndex + 3] = static_cast<float>(PrevRotation.W);

	Data[PrevScaleIndex + 0] = static_cast<float>(PrevScale.X);
	Data[PrevScaleIndex + 1] = static_cast<float>(PrevScale.Y);
	Data[PrevScaleIndex + 2] = static_cast<float>(PrevScale.Z);

	Data[CurrentPositionIndex + 0] = static_cast<float>(CurrentPosition.X);
	Data[CurrentPositionIndex + 1] = static_cast<float>(CurrentPosition.Y);
	Data[CurrentPositionIndex + 2] = static_cast<float>(CurrentPosition.Z);

	Data[CurrentRotationIndex + 0] = static_cast<float>(CurrentRotation.X);
	Data[CurrentRotationIndex + 1] = static_cast<float>(CurrentRotation.Y);
	Data[CurrentRotationIndex + 2] = static_cast<float>(CurrentRotation.Z);
	Data[CurrentRotationIndex + 3] = static_cast<float>(CurrentRotation.W);

	Data[CurrentScaleIndex + 0] = static_cast<float>(CurrentScale.X);
	Data[CurrentScaleIndex + 1] = static_cast<float>(CurrentScale.Y);
	Data[CurrentScaleIndex + 2] = static_cast<float>(CurrentScale.Z);

	Data[TierIndex] =
		static_cast<float>(
			static_cast<uint8>(InterpolationTiers[Index]));

	if (!InstancedSkinnedMeshComponent->SetCustomData(
		InstanceIds[Index],
		TConstArrayView<float>(
			Data,
			GpuInstanceCustomDataFloatCount)))
	{
		Fail(
			FString::Printf(
				TEXT("Backend 3 failed: SetCustomData failed at %d"),
				Index));
		return false;
	}

	return true;
}

void AAlgonaArmySkinnedPresentationActor::BeginBufferedInterpolation()
{
	NearInterpolationAlpha = 0.0f;
	MediumInterpolationAlpha = 0.0f;
	MediumFramesSinceAlphaUpdate = 0;

	PushGpuInterpolationAlphas();
}

void AAlgonaArmySkinnedPresentationActor::CompleteBufferedInterpolation()
{
	NearInterpolationAlpha = 1.0f;
	MediumInterpolationAlpha = 1.0f;
	MediumFramesSinceAlphaUpdate = 0;
	bInterpolationActive = false;
	PushGpuInterpolationAlphas();
}

void AAlgonaArmySkinnedPresentationActor::UpdateGpuInterpolationAlphas(
	double InterpolationAlpha,
	float DeltaSeconds)
{
	const float Alpha = FMath::Clamp(
		static_cast<float>(InterpolationAlpha),
		0.0f,
		1.0f);

	bool bShouldPushGpuAlpha = false;

	// NEAR: full render-frame interpolation.
	if (bHasNearInterpolationTier)
	{
		NearInterpolationAlpha = Alpha;
		bShouldPushGpuAlpha = true;
	}

	// MEDIUM: choose cadence from actual frame time and current zoom.
	// The held visual movement is capped at roughly one screen pixel.
	if (bHasMediumInterpolationTier)
	{
		const double SafeDeltaSeconds =
			FMath::Max(static_cast<double>(DeltaSeconds), 0.000001);

		const double PixelsPerRenderFrame =
			static_cast<double>(MaxMediumObservedSpeedCmPerSecond)
			* SafeDeltaSeconds
			* static_cast<double>(GroundPixelsPerWorldUnit);

		if (MaxMediumObservedSpeedCmPerSecond > KINDA_SMALL_NUMBER
			&& PixelsPerRenderFrame > UE_DOUBLE_SMALL_NUMBER)
		{
			MediumFrameCadence = FMath::Clamp(
				FMath::FloorToInt(
					MediumMaxVisualStepPixels / PixelsPerRenderFrame),
				1,
				8);
		}
		else
		{
			MediumFrameCadence = 1;
		}

		++MediumFramesSinceAlphaUpdate;

		if (MediumFramesSinceAlphaUpdate >= MediumFrameCadence)
		{
			MediumInterpolationAlpha = Alpha;
			MediumFramesSinceAlphaUpdate = 0;
			bShouldPushGpuAlpha = true;
		}
	}

	// FAR intentionally has no sub-step interpolation. Its base ISKM
	// transform advances at the authoritative 40 Hz snapshot rate.
	// If the whole orthographic working set is FAR, there is no per-frame
	// CustomPrimitiveData upload at all.
	if (bShouldPushGpuAlpha)
	{
		PushGpuInterpolationAlphas();
	}
}

void AAlgonaArmySkinnedPresentationActor::PushGpuInterpolationAlphas()
{
	if (!InstancedSkinnedMeshComponent)
	{
		return;
	}

	float AlphaData[2] =
	{
		NearInterpolationAlpha,
		MediumInterpolationAlpha
	};

	InstancedSkinnedMeshComponent->SetCustomPrimitiveDataFloatArray(
		NearAlphaPrimitiveDataIndex,
		TConstArrayView<float>(AlphaData, UE_ARRAY_COUNT(AlphaData)));
}

void AAlgonaArmySkinnedPresentationActor::RefreshInterpolationTierPresence()
{
	bHasNearInterpolationTier = false;
	bHasMediumInterpolationTier = false;
	MaxMediumObservedSpeedCmPerSecond = 0.0f;

	for (int32 Index = 0; Index < InterpolationTiers.Num(); ++Index)
	{
		const EAlgonaPresentationInterpolationTier Tier =
			InterpolationTiers[Index];

		if (Tier == EAlgonaPresentationInterpolationTier::Near)
		{
			bHasNearInterpolationTier = true;
		}
		else if (Tier == EAlgonaPresentationInterpolationTier::Medium)
		{
			bHasMediumInterpolationTier = true;

			if (ObservedLinearSpeedsCmPerSecond.IsValidIndex(Index))
			{
				MaxMediumObservedSpeedCmPerSecond = FMath::Max(
					MaxMediumObservedSpeedCmPerSecond,
					ObservedLinearSpeedsCmPerSecond[Index]);
			}
		}
	}
}

void AAlgonaArmySkinnedPresentationActor::Fail(
	const FString& Message)
{
	bRendererFailed = true;
	SetActorTickEnabled(false);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			49001,
			30.0f,
			FColor::Red,
			Message);
	}

	UE_LOG(
		LogTemp,
		Error,
		TEXT("%s"),
		*Message);
}

void AAlgonaArmySkinnedPresentationActor::Succeed(
	const FString& Message)
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(
			49001,
			10.0f,
			FColor::Green,
			Message);
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("%s"),
		*Message);
}
