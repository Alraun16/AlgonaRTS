#include "Presentation/AlgonaArmyPresentationActor.h"

#include "AlgonaPresentationSnapshotSelection.h"
#include "AlgonaPresentationView.h"
#include "Presentation/AlgonaPresentationSettings.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogAlgonaPresentation, Log, All);

namespace
{
	constexpr TCHAR AnimBankPath[] =
		TEXT("/Game/NewFolder/NewAnimBank.NewAnimBank");

	constexpr int32 AnimationIndex = 0;

	// Per-instance GPU interpolation data layout.
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

	// Current debug unit height. When final representation types are added,
	// this becomes a per-type value derived from their actual visual bounds.
	constexpr double ReferenceUnitHeightCm = 170.0;

	constexpr double NearMinProjectedHeightPixels = 16.0;
	constexpr double FarMaxProjectedHeightPixels = 10.0;
	constexpr double MediumMaxVisualStepPixels = 1.0;
	constexpr double TierHysteresis = 0.05;
	constexpr double TeleportDistance = 2000.0;
	constexpr double CullingGuardPixels = 128.0;
	constexpr double CloseCullingGuardScale = 1.5;

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
		UMaterialEditorOnlyData* EditorData = RuntimeMaterial.GetEditorOnlyData();
		if (!EditorData)
		{
			OutError = TEXT("material has no editor-only graph data");
			return false;
		}

		if (RuntimeMaterial.bUseMaterialAttributes)
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
				CreateTransientMaterialExpression<UMaterialExpressionConstant3Vector>(
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
			CreateTransientMaterialExpression<UMaterialExpressionWorldPosition>(
				&RuntimeMaterial,
				-900,
				-500);

		if (!WorldPosition)
		{
			OutError = TEXT("cannot create WorldPosition expression");
			return false;
		}

		WorldPosition->WorldPositionShaderOffset = WPT_ExcludeAllShaderOffsets;

		static const TCHAR* InstanceInputNames[GpuInstanceCustomDataFloatCount] =
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
		InstanceDataExpressions.Reserve(GpuInstanceCustomDataFloatCount);

		for (int32 DataIndex = 0; DataIndex < GpuInstanceCustomDataFloatCount; ++DataIndex)
		{
			UMaterialExpressionPerInstanceCustomData* Expression =
				CreateTransientMaterialExpression<UMaterialExpressionPerInstanceCustomData>(
					&RuntimeMaterial,
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
			{TEXT("AlphaNear"), NearAlphaPrimitiveDataIndex},
			{TEXT("AlphaMedium"), MediumAlphaPrimitiveDataIndex}
		};

		TArray<UMaterialExpressionScalarParameter*> AlphaExpressions;
		AlphaExpressions.Reserve(UE_ARRAY_COUNT(AlphaDescs));

		for (int32 Index = 0; Index < UE_ARRAY_COUNT(AlphaDescs); ++Index)
		{
			UMaterialExpressionScalarParameter* Expression =
				CreateTransientMaterialExpression<UMaterialExpressionScalarParameter>(
					&RuntimeMaterial,
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
			CreateTransientMaterialExpression<UMaterialExpressionCustom>(
				&RuntimeMaterial,
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
		RuntimeMaterial.bAlwaysEvaluateWorldPositionOffset = true;

		UMaterialEditingLibrary::SetBaseMaterialUsage(
			&RuntimeMaterial,
			MATUSAGE_InstancedSkinnedMesh,
			true);

		Custom->PostEditChange();
		RuntimeMaterial.PostEditChange();

		const TArray<FString> CompileErrors =
			UMaterialEditingLibrary::RecompileMaterial(&RuntimeMaterial);

		if (!CompileErrors.IsEmpty())
		{
			OutError = FString::Join(CompileErrors, TEXT(" | "));
			return false;
		}

		return true;
	}
#endif
}

AAlgonaArmyPresentationActor::AAlgonaArmyPresentationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	InstancedSkinnedMeshComponent =
		CreateDefaultSubobject<UInstancedSkinnedMeshComponent>(TEXT("ArmySkinnedInstances"));
	InstancedSkinnedMeshComponent->SetupAttachment(SceneRoot);
	InstancedSkinnedMeshComponent->SetMobility(EComponentMobility::Movable);
	InstancedSkinnedMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InstancedSkinnedMeshComponent->SetGenerateOverlapEvents(false);
	InstancedSkinnedMeshComponent->SetCastShadow(false);
	InstancedSkinnedMeshComponent->SetAnimationMinScreenSize(-1.0f);

	// UE 5.8 previous-instance transforms are for motion blur. Presentation
	// carries its explicit interpolation pair in custom data instead.
	InstancedSkinnedMeshComponent->SetHasPerInstancePrevTransforms(false);
}

void AAlgonaArmyPresentationActor::BeginPlay()
{
	Super::BeginPlay();

	AnimBank = LoadObject<UAnimBank>(nullptr, AnimBankPath);
	if (!AnimBank)
	{
		Fail(FString::Printf(TEXT("ISKM Presentation failed: cannot load %s"), AnimBankPath));
		return;
	}

	if (!AnimBank->Asset)
	{
		Fail(TEXT("ISKM Presentation failed: NewAnimBank -> Mapping -> Asset is None"));
		return;
	}

	if (AnimBank->Sequences.IsEmpty())
	{
		Fail(TEXT("ISKM Presentation failed: NewAnimBank has no Sequences"));
		return;
	}

	ValidateInstancingBuildSettings();
}

void AAlgonaArmyPresentationActor::SetPresentationCamera(
	UCameraComponent* InCameraComponent)
{
	PresentationCamera = InCameraComponent;
	bHasCameraView = false;
}

void AAlgonaArmyPresentationActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bRendererFailed)
	{
		return;
	}

	if (!bRendererReady && !PrepareSkinnedRenderer())
	{
		return;
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

	VisibilityRefreshElapsedSeconds += FMath::Max(DeltaSeconds, 0.0f);

	const uint64 CurrentSimulationTick = Simulation->GetSimulationTick();
	const uint64 CurrentRevision = Simulation->GetStateRevision();

	const bool bSimulationChanged =
		!bHasCapturedState || CurrentRevision != LastStateRevision;
	const bool bForceSnapshotSnap =
		bHasCapturedState && CurrentRevision > LastStateRevision + 1;

	const bool bCullingEnabled =
		IsAlgonaP1PresentationCameraCullingEnabled();
	const bool bCullingModeChanged =
		!bHasCullingMode || bCullingEnabled != bLastCullingEnabled;
	const bool bSpatialSnapshotsEnabled =
		IsAlgonaP1SpatialSnapshotsEnabled();
	const bool bSpatialSnapshotModeChanged =
		!bHasSpatialSnapshotMode
		|| bSpatialSnapshotsEnabled != bLastSpatialSnapshotsEnabled;

	// Screen-significance tiers depend on zoom even when camera culling is
	// disabled, so camera changes always invalidate this working set.
	const bool bCameraChanged = HasCameraViewChanged();
	const bool bRefreshForCamera =
		bCameraChanged
		&& VisibilityRefreshElapsedSeconds >= VisibilityRefreshIntervalSeconds;

	// Camera movement requires a new export only for the spatial path. The old
	// full-export path reuses its cached all-unit snapshot and only re-culls it.
	const bool bNeedsSnapshotCapture =
		bSimulationChanged
		|| bCullingModeChanged
		|| bSpatialSnapshotModeChanged
		|| (bCullingEnabled && bSpatialSnapshotsEnabled && bRefreshForCamera);

	if (bNeedsSnapshotCapture)
	{
		CaptureLatestState(*Simulation);
		bHasCapturedState = true;
	}

	if (bSimulationChanged)
	{
		LastStateRevision = CurrentRevision;
	}

	if (bSimulationChanged
		|| bCullingModeChanged
		|| bSpatialSnapshotModeChanged
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
		bLastSpatialSnapshotsEnabled = bSpatialSnapshotsEnabled;
		bHasSpatialSnapshotMode = true;
	}

	const bool bSimulationAdvancedWithoutStateChange =
		!bSimulationChanged && CurrentSimulationTick != LastSimulationTick;

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
	DebugCounterElapsedSeconds += FMath::Max(DeltaSeconds, 0.0f);
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
					TEXT("Presented ISKM: %d   Simulation: %d   UnitPx: %.1f   MediumCadence: %d   MedMaxSpeed: %.0f cm/s   Alpha N/M: %.2f/%.2f"),
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

bool AAlgonaArmyPresentationActor::PrepareSkinnedRenderer()
{
	if (bRendererReady)
	{
		return true;
	}

	if (bRendererFailed || !AnimBank || !InstancedSkinnedMeshComponent)
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
		SequenceProvider = CreateSequenceProvider(AnimBank);
		if (!SequenceProvider)
		{
			Fail(TEXT("ISKM Presentation failed: CreateFromAnimBank returned null"));
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
	InstancedSkinnedMeshComponent->SetSkinnedAssetAndUpdate(AnimBank->Asset.Get(), true);
	InstancedSkinnedMeshComponent->SetTransformProvider(SequenceProvider);
	InstancedSkinnedMeshComponent->SetNumCustomDataFloats(GpuInstanceCustomDataFloatCount);

	if (!BuildGpuInterpolationMaterials())
	{
		return false;
	}

	CompleteBufferedInterpolation();
	bRendererReady = true;

	Succeed(TEXT("ISKM Presentation ready: animation + GPU buffered interpolation"));
	return true;
}

UAnimSequenceTransformProviderData*
AAlgonaArmyPresentationActor::CreateSequenceProvider(UAnimBank* InAnimBank)
{
	if (!InAnimBank)
	{
		return nullptr;
	}

	UClass* ProviderClass = UAnimSequenceTransformProviderData::StaticClass();
	UFunction* Function =
		ProviderClass ? ProviderClass->FindFunctionByName(TEXT("CreateFromAnimBank")) : nullptr;

	if (!ProviderClass || !Function)
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

	UObject* ClassDefaultObject = ProviderClass->GetDefaultObject();
	if (!ClassDefaultObject)
	{
		return nullptr;
	}

	ClassDefaultObject->ProcessEvent(Function, &Params);
	return Params.ReturnValue;
}

bool AAlgonaArmyPresentationActor::ValidateInstancingBuildSettings()
{
	if (!AnimBank || !AnimBank->Asset)
	{
		return false;
	}

	const FSkeletalMeshLODInfo* LOD0 = AnimBank->Asset->GetLODInfo(0);
	if (!LOD0)
	{
		Fail(TEXT("ISKM Presentation failed: skinned asset has no LOD0 info"));
		return false;
	}

	if (!LOD0->BuildSettings.bOptimizeForInstancing)
	{
		Fail(TEXT("ISKM Presentation failed: LOD0 Optimize For Instancing is OFF"));
		return false;
	}

	return true;
}

bool AAlgonaArmyPresentationActor::BuildGpuInterpolationMaterials()
{
#if !WITH_EDITOR
	Fail(TEXT(
		"ISKM Presentation currently needs an Editor build because the proven P1 GPU interpolation path injects transient WPO materials at runtime. Replace this bridge with authored production materials before packaged builds."));
	return false;
#else
	if (!AnimBank || !AnimBank->Asset || !InstancedSkinnedMeshComponent)
	{
		return false;
	}

	RuntimeInterpolationMaterials.Reset();

	const TArray<FSkeletalMaterial>& SourceMaterials = AnimBank->Asset->GetMaterials();
	RuntimeInterpolationMaterials.Reserve(SourceMaterials.Num());

	for (int32 MaterialIndex = 0; MaterialIndex < SourceMaterials.Num(); ++MaterialIndex)
	{
		UMaterialInterface* SourceInterface = SourceMaterials[MaterialIndex].MaterialInterface;
		if (!SourceInterface)
		{
			Fail(FString::Printf(
				TEXT("ISKM Presentation failed: material slot %d is null"),
				MaterialIndex));
			return false;
		}

		UMaterial* SourceBaseMaterial = SourceInterface->GetMaterial();
		if (!SourceBaseMaterial)
		{
			Fail(FString::Printf(
				TEXT("ISKM Presentation failed: material slot %d has no base material"),
				MaterialIndex));
			return false;
		}

		UMaterial* RuntimeBaseMaterial = DuplicateObject<UMaterial>(SourceBaseMaterial, this);
		if (!RuntimeBaseMaterial)
		{
			Fail(FString::Printf(
				TEXT("ISKM Presentation failed: cannot duplicate material slot %d"),
				MaterialIndex));
			return false;
		}

		RuntimeBaseMaterial->SetFlags(RF_Transient);

		FString MaterialError;
		if (!InjectGpuInterpolationIntoMaterial(*RuntimeBaseMaterial, MaterialError))
		{
			Fail(FString::Printf(
				TEXT("ISKM Presentation GPU material %d failed: %s"),
				MaterialIndex,
				*MaterialError));
			return false;
		}

		UMaterialInterface* RuntimeInterface = RuntimeBaseMaterial;

		if (SourceInterface != SourceBaseMaterial)
		{
			UMaterialInstanceConstant* RuntimeInstance =
				NewObject<UMaterialInstanceConstant>(this, NAME_None, RF_Transient);

			if (!RuntimeInstance)
			{
				Fail(TEXT("ISKM Presentation failed: cannot create transient material instance"));
				return false;
			}

			RuntimeInstance->SetParentEditorOnly(RuntimeBaseMaterial, false);
			RuntimeInstance->CopyMaterialUniformParametersEditorOnly(SourceInterface, true);
			RuntimeInstance->PostEditChange();
			RuntimeInterface = RuntimeInstance;
		}

		// READY must not include transient shader compilation in benchmarks.
		RuntimeInterface->EnsureIsComplete();
		RuntimeInterpolationMaterials.Add(RuntimeInterface);
		InstancedSkinnedMeshComponent->SetMaterial(MaterialIndex, RuntimeInterface);
	}

	return true;
#endif
}

void AAlgonaArmyPresentationActor::CaptureLatestState(
	UAlgonaSimulationSubsystem& Simulation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaPresentation_CaptureState);
	CaptureAlgonaPresentationSnapshots(
		Simulation,
		GetWorld(),
		PresentationCamera.Get(),
		MaxPresentedEntities,
		CachedSnapshots);
}

void AAlgonaArmyPresentationActor::RefreshPresentationWorkingSet(
	bool bSimulationChanged,
	bool bForceSnapshotSnap,
	double SimulationStepSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaPresentation_BuildCameraWorkingSet);

	NextPresentedEntityIds.Reset();
	NextCurrentTransforms.Reset();
	NextInterpolationTiers.Reset();
	NextObservedLinearSpeedsCmPerSecond.Reset();

	NextPresentedEntityIds.Reserve(CachedSnapshots.Num());
	NextCurrentTransforms.Reserve(CachedSnapshots.Num());

	LastSimulationStepSeconds = FMath::Max(
		SimulationStepSeconds,
		UE_DOUBLE_SMALL_NUMBER);

	FAlgonaPresentationView View;
	UWorld* World = GetWorld();
	const UCameraComponent* Camera = PresentationCamera.Get();
	const bool bHasValidView =
		World && Camera && View.Build(*World, *Camera);
	const bool bUsePerUnitCulling =
		IsAlgonaP1PresentationCameraCullingEnabled()
		&& bHasValidView;

	ProjectedUnitHeightPixels =
		bHasValidView
			? View.GetProjectedVerticalSizePixels(ReferenceUnitHeightCm)
			: 0.0f;
	const double EffectiveCullingGuardPixels =
		bHasValidView && View.IsPerspectiveProjection()
			? FMath::Max(
				CullingGuardPixels,
				static_cast<double>(ProjectedUnitHeightPixels)
					* CloseCullingGuardScale)
			: CullingGuardPixels;
	GroundPixelsPerWorldUnit =
		bHasValidView ? View.GetGroundPixelsPerWorldUnit() : 0.0f;

	const FQuat MeshFacingCorrection =
		FRotator(0.0f, -90.0f, 0.0f).Quaternion();

	for (const FAlgonaUnitSnapshot& Snapshot : CachedSnapshots)
	{
		if (bUsePerUnitCulling
			&& !View.IsGroundPointVisible(Snapshot.Position, EffectiveCullingGuardPixels))
		{
			continue;
		}

		NextPresentedEntityIds.Add(Snapshot.EntityId);
		NextCurrentTransforms.Emplace(
			(Snapshot.Facing * MeshFacingCorrection).GetNormalized(),
			Snapshot.Position,
			InstanceScale);
	}

	const bool bSameEntityOrder = HasSameEntityOrder(NextPresentedEntityIds);

	if (!bSameEntityOrder)
	{
		TMap<uint32, int32> OldIndexByEntityId;
		OldIndexByEntityId.Reserve(PresentedEntityIds.Num());

		for (int32 OldIndex = 0; OldIndex < PresentedEntityIds.Num(); ++OldIndex)
		{
			OldIndexByEntityId.Add(PresentedEntityIds[OldIndex], OldIndex);
		}

		TArray<FTransform> NewPreviousTransforms;
		TArray<FTransform> NewCurrentTransforms;
		TArray<float> NewObservedLinearSpeedsCmPerSecond;
		TArray<EAlgonaPresentationInterpolationTier> NewTiers;

		NewPreviousTransforms.SetNumUninitialized(NextPresentedEntityIds.Num());
		NewCurrentTransforms.SetNumUninitialized(NextPresentedEntityIds.Num());
		NewObservedLinearSpeedsCmPerSecond.SetNumZeroed(NextPresentedEntityIds.Num());
		NewTiers.SetNumUninitialized(NextPresentedEntityIds.Num());

		TArray<FPrimitiveInstanceId> NewInstanceIds;
		TArray<bool> bOldInstanceRetained;
		const bool bHasExistingWorkingSet = !PresentedEntityIds.IsEmpty();
		bool bIncrementalInstanceUpdateSucceeded =
			bHasExistingWorkingSet
			&& !NextPresentedEntityIds.IsEmpty()
			&& InstancedSkinnedMeshComponent
			&& InstanceIds.Num() == PresentedEntityIds.Num();

		if (bIncrementalInstanceUpdateSucceeded)
		{
			NewInstanceIds.Reserve(NextPresentedEntityIds.Num());
			bOldInstanceRetained.Init(false, PresentedEntityIds.Num());
		}

		bool bAnyInterpolatedTransformChanged = false;

		for (int32 NewIndex = 0; NewIndex < NextPresentedEntityIds.Num(); ++NewIndex)
		{
			const uint32 EntityId = NextPresentedEntityIds[NewIndex];
			const FTransform& SnapshotTransform = NextCurrentTransforms[NewIndex];
			const int32* OldIndexPtr = OldIndexByEntityId.Find(EntityId);

			if (bIncrementalInstanceUpdateSucceeded)
			{
				if (OldIndexPtr && InstanceIds.IsValidIndex(*OldIndexPtr))
				{
					NewInstanceIds.Add(InstanceIds[*OldIndexPtr]);
					bOldInstanceRetained[*OldIndexPtr] = true;
				}
				else
				{
					const FPrimitiveInstanceId NewInstanceId =
						InstancedSkinnedMeshComponent->AddInstance(
							SnapshotTransform,
							AnimationIndex,
							false);

					if (!NewInstanceId.IsValid())
					{
						bIncrementalInstanceUpdateSucceeded = false;
					}
					else
					{
						NewInstanceIds.Add(NewInstanceId);
					}
				}
			}

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
					NewCurrentTransforms[NewIndex] = SnapshotTransform;
					NewPreviousTransforms[NewIndex] = CurrentTransforms[OldIndex];

					if (bForceSnapshotSnap
						|| ShouldSnapInterpolation(
							NewPreviousTransforms[NewIndex],
							NewCurrentTransforms[NewIndex]))
					{
						NewPreviousTransforms[NewIndex] = NewCurrentTransforms[NewIndex];
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
					NewPreviousTransforms[NewIndex] = PreviousTransforms[OldIndex];
					NewCurrentTransforms[NewIndex] = CurrentTransforms[OldIndex];
				}
			}
			else
			{
				// Spawn or camera re-entry: never interpolate from stale data.
				NewPreviousTransforms[NewIndex] = SnapshotTransform;
				NewCurrentTransforms[NewIndex] = SnapshotTransform;
				NewObservedLinearSpeedsCmPerSecond[NewIndex] = 0.0f;
				NewTiers[NewIndex] = DetermineInterpolationTier(
					0.0f,
					EAlgonaPresentationInterpolationTier::Near,
					false);
			}
		}

		if (bIncrementalInstanceUpdateSucceeded)
		{
			for (int32 OldIndex = 0; OldIndex < InstanceIds.Num(); ++OldIndex)
			{
				if (!bOldInstanceRetained[OldIndex]
					&& !InstancedSkinnedMeshComponent->RemoveInstance(InstanceIds[OldIndex]))
				{
					bIncrementalInstanceUpdateSucceeded = false;
					break;
				}
			}

			if (bIncrementalInstanceUpdateSucceeded
				&& (NewInstanceIds.Num() != NextPresentedEntityIds.Num()
					|| InstancedSkinnedMeshComponent->GetInstanceCount()
						!= NextPresentedEntityIds.Num()))
			{
				bIncrementalInstanceUpdateSucceeded = false;
			}
		}

		PresentedEntityIds = MoveTemp(NextPresentedEntityIds);
		PreviousTransforms = MoveTemp(NewPreviousTransforms);
		CurrentTransforms = MoveTemp(NewCurrentTransforms);
		ObservedLinearSpeedsCmPerSecond = MoveTemp(NewObservedLinearSpeedsCmPerSecond);
		InterpolationTiers = MoveTemp(NewTiers);
		RefreshInterpolationTierPresence();

		if (bSimulationChanged)
		{
			bInterpolationActive = bAnyInterpolatedTransformChanged;
			if (bInterpolationActive)
			{
				BeginBufferedInterpolation();
			}
			else
			{
				CompleteBufferedInterpolation();
			}
		}

		if (!bIncrementalInstanceUpdateSucceeded)
		{
			// Initial population, full disappearance, or an unexpected ISKM
			// mutation failure: keep the old full rebuild as a safe fallback.
			RebuildInstances();
			return;
		}

		InstanceIds = MoveTemp(NewInstanceIds);

		// RebuildInstances used to refresh this bound on every working-set
		// change. Preserve that behavior without recreating the instances.
		FBox WorkingSetBounds(EForceInit::ForceInit);
		for (int32 Index = 0; Index < CurrentTransforms.Num(); ++Index)
		{
			WorkingSetBounds += CurrentTransforms[Index].GetLocation();
			if (PreviousTransforms.IsValidIndex(Index))
			{
				WorkingSetBounds += PreviousTransforms[Index].GetLocation();
			}
		}

		if (WorkingSetBounds.IsValid)
		{
			WorkingSetBounds =
				WorkingSetBounds.ExpandBy(FVector(5000.0, 5000.0, 5000.0));
			InstancedSkinnedMeshComponent->SetPrimitiveBoundsOverride(WorkingSetBounds);
		}

		if (bSimulationChanged)
		{
			if (!UploadSimulationStateToInstances())
			{
				return;
			}
		}
		else
		{
			for (int32 Index = 0; Index < InstanceIds.Num(); ++Index)
			{
				if (!UploadInstanceGpuData(Index))
				{
					return;
				}
			}
		}

		return;
	}

	NextInterpolationTiers.SetNumUninitialized(NextPresentedEntityIds.Num());
	NextObservedLinearSpeedsCmPerSecond.SetNumZeroed(NextPresentedEntityIds.Num());

	for (int32 Index = 0; Index < NextPresentedEntityIds.Num(); ++Index)
	{
		const bool bHasPreviousTier = InterpolationTiers.IsValidIndex(Index);
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
			ObservedSpeedCmPerSecond = ObservedLinearSpeedsCmPerSecond[Index];
		}

		NextObservedLinearSpeedsCmPerSecond[Index] = ObservedSpeedCmPerSecond;
		NextInterpolationTiers[Index] = DetermineInterpolationTier(
			ObservedSpeedCmPerSecond,
			bHasPreviousTier
				? InterpolationTiers[Index]
				: EAlgonaPresentationInterpolationTier::Near,
			bHasPreviousTier);
	}

	if (!bSimulationChanged)
	{
		ObservedLinearSpeedsCmPerSecond = MoveTemp(NextObservedLinearSpeedsCmPerSecond);
		InterpolationTiers = MoveTemp(NextInterpolationTiers);
		RefreshInterpolationTierPresence();

		for (int32 Index = 0; Index < InstanceIds.Num(); ++Index)
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
	ObservedLinearSpeedsCmPerSecond = MoveTemp(NextObservedLinearSpeedsCmPerSecond);
	InterpolationTiers = MoveTemp(NextInterpolationTiers);
	RefreshInterpolationTierPresence();

	if (PreviousTransforms.Num() != CurrentTransforms.Num()
		|| ObservedLinearSpeedsCmPerSecond.Num() != CurrentTransforms.Num()
		|| InterpolationTiers.Num() != CurrentTransforms.Num())
	{
		Fail(TEXT("ISKM Presentation failed: interpolation buffers lost entity alignment"));
		return;
	}

	bool bAnyInterpolatedTransformChanged = false;

	for (int32 Index = 0; Index < CurrentTransforms.Num(); ++Index)
	{
		if (bForceSnapshotSnap
			|| ShouldSnapInterpolation(PreviousTransforms[Index], CurrentTransforms[Index]))
		{
			PreviousTransforms[Index] = CurrentTransforms[Index];
			continue;
		}

		if (InterpolationTiers[Index] != EAlgonaPresentationInterpolationTier::Far
			&& !PreviousTransforms[Index].Equals(CurrentTransforms[Index], 0.01f))
		{
			bAnyInterpolatedTransformChanged = true;
		}
	}

	bInterpolationActive = bAnyInterpolatedTransformChanged;

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

bool AAlgonaArmyPresentationActor::HasSameEntityOrder(
	const TArray<uint32>& CandidateEntityIds) const
{
	if (PresentedEntityIds.Num() != CandidateEntityIds.Num())
	{
		return false;
	}

	for (int32 Index = 0; Index < CandidateEntityIds.Num(); ++Index)
	{
		if (PresentedEntityIds[Index] != CandidateEntityIds[Index])
		{
			return false;
		}
	}

	return true;
}

bool AAlgonaArmyPresentationActor::HasCameraViewChanged() const
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

	const bool bPerspective =
		Camera->ProjectionMode == ECameraProjectionMode::Perspective;

	if (bPerspective != bLastCameraWasPerspective)
	{
		return true;
	}

	if (bPerspective)
	{
		// Perspective invalidation follows the projection that UE actually uses
		// for this local view, not the camera component transform. The rendered
		// projection can become current one frame after a zoom input.
		UWorld* World = GetWorld();
		FAlgonaPresentationView CurrentView;
		if (!World
			|| !CurrentView.Build(*World, *Camera)
			|| !CurrentView.IsPerspectiveProjection())
		{
			return true;
		}

		const FIntRect& CurrentRect = CurrentView.GetProjectionViewRect();
		if (CurrentRect.Min.X != LastPerspectiveViewRect.Min.X
			|| CurrentRect.Min.Y != LastPerspectiveViewRect.Min.Y
			|| CurrentRect.Max.X != LastPerspectiveViewRect.Max.X
			|| CurrentRect.Max.Y != LastPerspectiveViewRect.Max.Y)
		{
			return true;
		}

		const FMatrix& CurrentMatrix = CurrentView.GetViewProjectionMatrix();
		constexpr double MatrixTolerance = 1.0e-8;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				if (!FMath::IsNearlyEqual(
						CurrentMatrix.M[Row][Column],
						LastPerspectiveViewProjectionMatrix.M[Row][Column],
						MatrixTolerance))
				{
					return true;
				}
			}
		}

		return false;
	}

	return !Camera->GetComponentTransform().Equals(LastCameraTransform, 0.01f)
		|| !FMath::IsNearlyEqual(Camera->OrthoWidth, LastCameraOrthoWidth, 0.01f)
		|| !FMath::IsNearlyEqual(Camera->AspectRatio, LastCameraAspectRatio, 0.001f);
}

void AAlgonaArmyPresentationActor::CacheCurrentCameraView()
{
	const UCameraComponent* Camera = PresentationCamera.Get();
	if (!Camera)
	{
		bHasCameraView = false;
		return;
	}

	const bool bPerspective =
		Camera->ProjectionMode == ECameraProjectionMode::Perspective;

	if (bPerspective)
	{
		// Cache the same rendered projection state used by the perspective
		// visibility path so a later projection update cannot be mistaken for
		// an already processed camera transform.
		UWorld* World = GetWorld();
		FAlgonaPresentationView CurrentView;
		if (!World
			|| !CurrentView.Build(*World, *Camera)
			|| !CurrentView.IsPerspectiveProjection())
		{
			bHasCameraView = false;
			return;
		}

		LastPerspectiveViewProjectionMatrix =
			CurrentView.GetViewProjectionMatrix();
		LastPerspectiveViewRect = CurrentView.GetProjectionViewRect();
		bLastCameraWasPerspective = true;
		bHasCameraView = true;
		return;
	}

	LastCameraTransform = Camera->GetComponentTransform();
	LastCameraOrthoWidth = Camera->OrthoWidth;
	LastCameraAspectRatio = Camera->AspectRatio;
	bLastCameraWasPerspective = false;
	bHasCameraView = true;
}

EAlgonaPresentationInterpolationTier
AAlgonaArmyPresentationActor::DetermineInterpolationTier(
	float LinearSpeedCmPerSecond,
	EAlgonaPresentationInterpolationTier PreviousTier,
	bool bHasPreviousTier) const
{
	const UCameraComponent* Camera = PresentationCamera.Get();

	if (!Camera
		|| Camera->ProjectionMode != ECameraProjectionMode::Orthographic
		|| ProjectedUnitHeightPixels <= 0.0f
		|| GroundPixelsPerWorldUnit <= 0.0f)
	{
		return EAlgonaPresentationInterpolationTier::Near;
	}

	const double UnitPixels = static_cast<double>(ProjectedUnitHeightPixels);
	const double SafeSpeedCmPerSecond =
		FMath::Max(static_cast<double>(LinearSpeedCmPerSecond), 0.0);
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

	switch (PreviousTier)
	{
	case EAlgonaPresentationInterpolationTier::Near:
		if (UnitPixels >= NearMinProjectedHeightPixels * (1.0 - TierHysteresis))
		{
			return EAlgonaPresentationInterpolationTier::Near;
		}
		break;

	case EAlgonaPresentationInterpolationTier::Medium:
	{
		const bool bClearlyNear =
			UnitPixels >= NearMinProjectedHeightPixels * (1.0 + TierHysteresis);
		const bool bClearlyFar =
			UnitPixels <= FarMaxProjectedHeightPixels * (1.0 - TierHysteresis)
			&& SnapshotStepPixels <= MediumMaxVisualStepPixels * (1.0 - TierHysteresis);

		if (!bClearlyNear && !bClearlyFar)
		{
			return EAlgonaPresentationInterpolationTier::Medium;
		}
		break;
	}

	case EAlgonaPresentationInterpolationTier::Far:
		if (UnitPixels <= FarMaxProjectedHeightPixels * (1.0 + TierHysteresis)
			&& SnapshotStepPixels <= MediumMaxVisualStepPixels * (1.0 + TierHysteresis))
		{
			return EAlgonaPresentationInterpolationTier::Far;
		}
		break;
	}

	return ClassifyRaw(UnitPixels);
}

bool AAlgonaArmyPresentationActor::ShouldSnapInterpolation(
	const FTransform& Previous,
	const FTransform& Current) const
{
	return FVector::DistSquared(Previous.GetLocation(), Current.GetLocation())
		> FMath::Square(TeleportDistance);
}

bool AAlgonaArmyPresentationActor::RebuildInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaPresentation_RebuildInstances);

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

	AnimationIndicesScratch.Init(AnimationIndex, CurrentTransforms.Num());
	FBox WorkingSetBounds(EForceInit::ForceInit);
	for (int32 Index = 0; Index < CurrentTransforms.Num(); ++Index)
	{
		WorkingSetBounds += CurrentTransforms[Index].GetLocation();
		if (PreviousTransforms.IsValidIndex(Index))
		{
			WorkingSetBounds += PreviousTransforms[Index].GetLocation();
		}
	}

	if (WorkingSetBounds.IsValid)
	{
		WorkingSetBounds = WorkingSetBounds.ExpandBy(FVector(5000.0, 5000.0, 5000.0));
		InstancedSkinnedMeshComponent->SetPrimitiveBoundsOverride(WorkingSetBounds);
	}

	InstanceIds = InstancedSkinnedMeshComponent->AddInstances(
		CurrentTransforms,
		AnimationIndicesScratch,
		true,
		false);

	const int32 CreatedCount = InstancedSkinnedMeshComponent->GetInstanceCount();
	if (CreatedCount != CurrentTransforms.Num()
		|| InstanceIds.Num() != CurrentTransforms.Num())
	{
		Fail(FString::Printf(
			TEXT("ISKM Presentation failed: created %d / %d instances, ids=%d"),
			CreatedCount,
			CurrentTransforms.Num(),
			InstanceIds.Num()));
		return false;
	}

	for (int32 Index = 0; Index < InstanceIds.Num(); ++Index)
	{
		if (!UploadInstanceGpuData(Index))
		{
			return false;
		}
	}

	return true;
}

bool AAlgonaArmyPresentationActor::UploadSimulationStateToInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AlgonaPresentation_UploadSimulationState);

	if (!InstancedSkinnedMeshComponent
		|| InstanceIds.Num() != CurrentTransforms.Num())
	{
		Fail(TEXT("ISKM Presentation failed: simulation/instance working-set mismatch"));
		return false;
	}

	for (int32 Index = 0; Index < InstanceIds.Num(); ++Index)
	{
		if (!InstancedSkinnedMeshComponent->SetInstanceTransform(
			InstanceIds[Index],
			CurrentTransforms[Index],
			false))
		{
			Fail(FString::Printf(
				TEXT("ISKM Presentation failed: SetInstanceTransform failed at %d"),
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

bool AAlgonaArmyPresentationActor::UploadInstanceGpuData(int32 Index)
{
	if (!InstancedSkinnedMeshComponent
		|| !InstanceIds.IsValidIndex(Index)
		|| !PreviousTransforms.IsValidIndex(Index)
		|| !CurrentTransforms.IsValidIndex(Index)
		|| !InterpolationTiers.IsValidIndex(Index))
	{
		Fail(TEXT("ISKM Presentation failed: invalid GPU interpolation instance data"));
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
	Data[TierIndex] = static_cast<float>(static_cast<uint8>(InterpolationTiers[Index]));

	if (!InstancedSkinnedMeshComponent->SetCustomData(
		InstanceIds[Index],
		TConstArrayView<float>(Data, GpuInstanceCustomDataFloatCount)))
	{
		Fail(FString::Printf(
			TEXT("ISKM Presentation failed: SetCustomData failed at %d"),
			Index));
		return false;
	}

	return true;
}

void AAlgonaArmyPresentationActor::BeginBufferedInterpolation()
{
	NearInterpolationAlpha = 0.0f;
	MediumInterpolationAlpha = 0.0f;
	MediumFramesSinceAlphaUpdate = 0;
	PushGpuInterpolationAlphas();
}

void AAlgonaArmyPresentationActor::CompleteBufferedInterpolation()
{
	NearInterpolationAlpha = 1.0f;
	MediumInterpolationAlpha = 1.0f;
	MediumFramesSinceAlphaUpdate = 0;
	bInterpolationActive = false;
	PushGpuInterpolationAlphas();
}

void AAlgonaArmyPresentationActor::UpdateGpuInterpolationAlphas(
	double InterpolationAlpha,
	float DeltaSeconds)
{
	const float Alpha = FMath::Clamp(
		static_cast<float>(InterpolationAlpha),
		0.0f,
		1.0f);

	bool bShouldPushGpuAlpha = false;

	if (bHasNearInterpolationTier)
	{
		NearInterpolationAlpha = Alpha;
		bShouldPushGpuAlpha = true;
	}

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
				FMath::FloorToInt(MediumMaxVisualStepPixels / PixelsPerRenderFrame),
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

	// FAR advances only on authoritative 40 Hz snapshots. If the whole view
	// is FAR there is no per-frame interpolation-alpha upload.
	if (bShouldPushGpuAlpha)
	{
		PushGpuInterpolationAlphas();
	}
}

void AAlgonaArmyPresentationActor::PushGpuInterpolationAlphas()
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

void AAlgonaArmyPresentationActor::RefreshInterpolationTierPresence()
{
	bHasNearInterpolationTier = false;
	bHasMediumInterpolationTier = false;
	MaxMediumObservedSpeedCmPerSecond = 0.0f;

	for (int32 Index = 0; Index < InterpolationTiers.Num(); ++Index)
	{
		const EAlgonaPresentationInterpolationTier Tier = InterpolationTiers[Index];

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

void AAlgonaArmyPresentationActor::Fail(const FString& Message)
{
	bRendererFailed = true;
	SetActorTickEnabled(false);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(49001, 30.0f, FColor::Red, Message);
	}

	UE_LOG(LogAlgonaPresentation, Error, TEXT("%s"), *Message);
}

void AAlgonaArmyPresentationActor::Succeed(const FString& Message)
{
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(49001, 10.0f, FColor::Green, Message);
	}

	UE_LOG(LogAlgonaPresentation, Display, TEXT("%s"), *Message);
}
