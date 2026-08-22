#include "Presentation/AlgonaArmyPresentationActor.h"

#include "Core/AlgonaSimulationSubsystem.h"

#include "Camera/CameraComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/StaticMesh.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/ConstructorHelpers.h"

#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	/*
	 * 1 = Presentation обновляет только camera-visible working set.
	 * 0 = старое поведение: Presentation обновляет все snapshots.
	 *
	 * Нужен только для прямого A/B-теста.
	 */
	TAutoConsoleVariable<int32>
		CVarAlgonaP1PresentationCameraCull(
			TEXT("algona.P1.PresentationCameraCull"),
			1,
			TEXT(
				"1 = camera-visible ISM working set, "
				"0 = all exported soldiers."),
			ECVF_Default);
}

AAlgonaArmyPresentationActor::AAlgonaArmyPresentationActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	InstancedMeshComponent =
		CreateDefaultSubobject<UInstancedStaticMeshComponent>(
			TEXT("ArmyInstances"));

	SetRootComponent(InstancedMeshComponent);

	InstancedMeshComponent->SetMobility(
		EComponentMobility::Movable);

	InstancedMeshComponent->SetCollisionEnabled(
		ECollisionEnabled::NoCollision);

	InstancedMeshComponent->SetGenerateOverlapEvents(false);
	InstancedMeshComponent->SetCastShadow(false);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SoldierMesh(
		TEXT("/Game/Archer.Archer"));

	if (SoldierMesh.Succeeded())
	{
		InstancedMeshComponent->SetStaticMesh(
			SoldierMesh.Object);
	}
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

    UWorld* World = GetWorld();
    if (!World || !InstancedMeshComponent)
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

    if (!bHasCapturedState
        || CurrentRevision != LastStateRevision)
    {
        CaptureLatestState(*Simulation);

        LastStateRevision = CurrentRevision;
        bHasCapturedState = true;
        bSimulationChanged = true;
    }

    const bool bCullingEnabled =
        CVarAlgonaP1PresentationCameraCull
            .GetValueOnGameThread() != 0;

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
            bSimulationChanged);

        CacheCurrentCameraView();

        VisibilityRefreshElapsedSeconds = 0.0f;

        bLastCullingEnabled = bCullingEnabled;
        bHasCullingMode = true;
    }

    /*
     * Если после последнего изменившего Transform step
     * прошёл ещё один Simulation tick без нового state,
     * значит движение закончилось.
     *
     * Доводим изображение точно до последнего Target.
     */
    const bool bSimulationAdvancedWithoutStateChange =
        !bSimulationChanged
        && CurrentSimulationTick != LastSimulationTick;

    if (bInterpolationActive)
    {
        if (bSimulationAdvancedWithoutStateChange)
        {
            RenderTransforms = TargetTransforms;

            if (!RenderTransforms.IsEmpty())
            {
                InstancedMeshComponent
                    ->BatchUpdateInstancesTransforms(
                        0,
                        RenderTransforms,
                        true,
                        true,
                        true);
            }

            bInterpolationActive = false;
        }
        else
        {
            ApplyInterpolatedTransforms(
                Simulation->GetInterpolationAlpha());
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
                    TEXT("Presented: %d   Simulation: %d"),
                    InstancedMeshComponent->GetInstanceCount(),
                    CachedSnapshots.Num()));
        }
    }

#endif
}

void AAlgonaArmyPresentationActor::CaptureLatestState(
	UAlgonaSimulationSubsystem& Simulation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaPresentation_CaptureState);

	/*
	 * Важно:
	 * здесь всё ещё разрешено получить все 100k.
	 *
	 * Это сознательно оставлено для P1 experiment:
	 * мы отдельно проверяем стоимость Simulation/export
	 * и стоимость per-frame Presentation.
	 */
	Simulation.ExportSoldierSnapshots(
		CachedSnapshots,
		MaxPresentedEntities);
}

void AAlgonaArmyPresentationActor::RefreshPresentationWorkingSet(
	bool bSimulationChanged)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(
        AlgonaPresentation_BuildCameraWorkingSet);

    NextPresentedEntityIds.Reset();
    NextTargetTransforms.Reset();

    NextPresentedEntityIds.Reserve(
        CachedSnapshots.Num());

    NextTargetTransforms.Reserve(
        CachedSnapshots.Num());

    UCameraComponent* Camera =
        PresentationCamera.Get();

    const bool bCameraCullingEnabled =
        CVarAlgonaP1PresentationCameraCull
            .GetValueOnGameThread() != 0;

    const bool bUseCameraCulling =
        bCameraCullingEnabled
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
            World
                ? World->GetFirstPlayerController()
                : nullptr;

        if (PlayerController)
        {
            PlayerController->GetViewportSize(
                ViewportWidth,
                ViewportHeight);

            auto DeprojectToGround =
                [PlayerController](
                    const FVector2D& ScreenPosition,
                    FVector& OutGroundPosition)
                {
                    FVector WorldPosition;
                    FVector WorldDirection;

                    if (!UGameplayStatics::DeprojectScreenToWorld(
                        PlayerController,
                        ScreenPosition,
                        WorldPosition,
                        WorldDirection))
                    {
                        return false;
                    }

                    if (FMath::Abs(WorldDirection.Z)
                        <= KINDA_SMALL_NUMBER)
                    {
                        return false;
                    }

                    const double Distance =
                        -WorldPosition.Z
                        / WorldDirection.Z;

                    if (Distance < 0.0)
                    {
                        return false;
                    }

                    OutGroundPosition =
                        WorldPosition
                        + WorldDirection * Distance;

                    OutGroundPosition.Z = 0.0;

                    return true;
                };

            if (ViewportWidth > 0
                && ViewportHeight > 0)
            {
                bUseGroundViewCulling =
                    DeprojectToGround(
                        FVector2D(0.0, 0.0),
                        GroundTopLeft)
                    && DeprojectToGround(
                        FVector2D(
                            static_cast<double>(ViewportWidth),
                            0.0),
                        GroundTopRight)
                    && DeprojectToGround(
                        FVector2D(
                            0.0,
                            static_cast<double>(ViewportHeight)),
                        GroundBottomLeft);
            }
        }
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

    const FQuat MeshFacingCorrection =
        FRotator(0.0f, -90.0f, 0.0f)
            .Quaternion();

    for (const FAlgonaSoldierSnapshot& Snapshot
        : CachedSnapshots)
    {
        if (bUseGroundViewCulling
            && FMath::Abs(ViewDeterminant)
                > UE_DOUBLE_SMALL_NUMBER)
        {
            const FVector ToSoldier =
                Snapshot.Position - GroundTopLeft;

            const double Horizontal =
                (
                    ToSoldier.X * ViewDown.Y
                    - ToSoldier.Y * ViewDown.X
                )
                / ViewDeterminant;

            const double Vertical =
                (
                    ViewRight.X * ToSoldier.Y
                    - ViewRight.Y * ToSoldier.X
                )
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
            (
                Snapshot.Facing
                * MeshFacingCorrection
            ).GetNormalized();

        NextPresentedEntityIds.Add(
            Snapshot.EntityId);

        NextTargetTransforms.Emplace(
            PresentationRotation,
            Snapshot.Position,
            InstanceScale);
    }

    const bool bSameEntityOrder =
    HasSameEntityOrder(
        NextPresentedEntityIds);

if (!bSameEntityOrder)
{
    /*
     * Visible set изменился.
     *
     * Не сбрасываем интерполяцию всех оставшихся
     * на экране солдат: переносим их старое состояние
     * по стабильному EntityId.
     */
    TMap<uint32, int32> OldIndexByEntityId;

    OldIndexByEntityId.Reserve(
        PresentedEntityIds.Num());

    for (int32 OldIndex = 0;
        OldIndex < PresentedEntityIds.Num();
        ++OldIndex)
    {
        OldIndexByEntityId.Add(
            PresentedEntityIds[OldIndex],
            OldIndex);
    }

    TArray<FTransform> NewPreviousTransforms;
    TArray<FTransform> NewRenderTransforms;

    NewPreviousTransforms.SetNumUninitialized(
        NextPresentedEntityIds.Num());

    NewRenderTransforms.SetNumUninitialized(
        NextPresentedEntityIds.Num());

    bool bAnyTransformChanged = false;

    for (int32 NewIndex = 0;
        NewIndex < NextPresentedEntityIds.Num();
        ++NewIndex)
    {
        const uint32 EntityId =
            NextPresentedEntityIds[NewIndex];

        const FTransform& NewTarget =
            NextTargetTransforms[NewIndex];

        const int32* OldIndexPtr =
            OldIndexByEntityId.Find(EntityId);

        if (OldIndexPtr
            && PreviousTransforms.IsValidIndex(*OldIndexPtr)
            && TargetTransforms.IsValidIndex(*OldIndexPtr)
            && RenderTransforms.IsValidIndex(*OldIndexPtr))
        {
            const int32 OldIndex = *OldIndexPtr;

            /*
             * Уже видимый солдат сохраняет своё
             * текущее визуальное положение.
             */
            NewRenderTransforms[NewIndex] =
                RenderTransforms[OldIndex];

            /*
             * Если пришёл новый Simulation state,
             * старый Target и есть настоящий state N-1.
             *
             * Если изменилась только камера,
             * сохраняем текущую interpolation pair.
             */
            NewPreviousTransforms[NewIndex] =
                bSimulationChanged
                    ? TargetTransforms[OldIndex]
                    : PreviousTransforms[OldIndex];

            if (!NewPreviousTransforms[NewIndex]
                    .Equals(NewTarget, 0.01f))
            {
                bAnyTransformChanged = true;
            }
        }
        else
        {
            /*
             * Солдат только что вошёл в camera working set.
             * Предыдущего визуального состояния у нас нет,
             * поэтому показываем его сразу в актуальной позиции.
             */
            NewPreviousTransforms[NewIndex] =
                NewTarget;

            NewRenderTransforms[NewIndex] =
                NewTarget;
        }
    }

    PresentedEntityIds =
        MoveTemp(NextPresentedEntityIds);

    PreviousTransforms =
        MoveTemp(NewPreviousTransforms);

    TargetTransforms =
        MoveTemp(NextTargetTransforms);

    RenderTransforms =
        MoveTemp(NewRenderTransforms);

    bInterpolationActive =
        bAnyTransformChanged;

    RebuildInstances();
    return;
}

/*
 * Камера обновила working set,
 * но набор entities остался тем же.
 * Simulation state при этом не менялся —
 * interpolation pair не трогаем.
 */
if (!bSimulationChanged)
{
    return;
}

/*
 * Ключевое отличие от старой системы:
 *
 * Previous = прошлый authoritative Simulation state,
 * а НЕ текущее промежуточное Render положение.
 */
PreviousTransforms = TargetTransforms;

bool bAnyTransformChanged = false;

for (int32 Index = 0;
    Index < NextTargetTransforms.Num();
    ++Index)
{
    if (!NextTargetTransforms[Index].Equals(
        TargetTransforms[Index],
        0.01f))
    {
        bAnyTransformChanged = true;
    }
}

TargetTransforms =
    MoveTemp(NextTargetTransforms);

bInterpolationActive =
    bAnyTransformChanged;
}

bool AAlgonaArmyPresentationActor::HasSameEntityOrder(
	const TArray<uint32>& CandidateEntityIds) const
{
	if (PresentedEntityIds.Num()
		!= CandidateEntityIds.Num())
	{
		return false;
	}

	for (int32 Index = 0;
		Index < CandidateEntityIds.Num();
		++Index)
	{
		if (PresentedEntityIds[Index]
			!= CandidateEntityIds[Index])
		{
			return false;
		}
	}

	return true;
}

bool AAlgonaArmyPresentationActor::HasCameraViewChanged() const
{
	const UCameraComponent* Camera =
		PresentationCamera.Get();

	if (!Camera)
	{
		return false;
	}

	if (!bHasCameraView)
	{
		return true;
	}

	if (!Camera
		->GetComponentTransform()
		.Equals(
			LastCameraTransform,
			0.01f))
	{
		return true;
	}

	if (!Camera
	   ->GetComponentTransform()
	   .Equals(
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

void AAlgonaArmyPresentationActor::CacheCurrentCameraView()
{
	const UCameraComponent* Camera =
		PresentationCamera.Get();

	if (!Camera)
	{
		bHasCameraView = false;
		return;
	}

	LastCameraTransform =
		Camera->GetComponentTransform();

	LastCameraOrthoWidth =
		Camera->OrthoWidth;
	
	LastCameraAspectRatio =
		Camera->AspectRatio;
	
	bHasCameraView = true;
}

void AAlgonaArmyPresentationActor::RebuildInstances()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaPresentation_RebuildInstances);

	InstancedMeshComponent->ClearInstances();

	if (RenderTransforms.IsEmpty())
	{
		return;
	}

	InstancedMeshComponent->PreAllocateInstancesMemory(
		RenderTransforms.Num());

	InstancedMeshComponent->AddInstances(
		RenderTransforms,
		false,
		true,
		false);
}

void AAlgonaArmyPresentationActor::ApplyInterpolatedTransforms(
	double InterpolationAlpha)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(
		AlgonaPresentation_InterpolateAndUpload);

	if (PreviousTransforms.Num()
			!= TargetTransforms.Num()
		|| RenderTransforms.Num()
			!= TargetTransforms.Num())
	{
		bInterpolationActive = false;
		return;
	}

	const float Alpha =
		FMath::Clamp(
			static_cast<float>(InterpolationAlpha),
			0.0f,
			1.0f);

	for (int32 Index = 0;
		Index < TargetTransforms.Num();
		++Index)
	{
		const FVector Location =
			FMath::Lerp(
				PreviousTransforms[Index]
					.GetLocation(),
				TargetTransforms[Index]
					.GetLocation(),
				Alpha);

		const FQuat Rotation =
			FQuat::Slerp(
				PreviousTransforms[Index]
					.GetRotation(),
				TargetTransforms[Index]
					.GetRotation(),
				Alpha)
			.GetNormalized();

		RenderTransforms[Index] =
			FTransform(
				Rotation,
				Location,
				TargetTransforms[Index]
					.GetScale3D());
	}

	if (!RenderTransforms.IsEmpty())
	{
		InstancedMeshComponent
			->BatchUpdateInstancesTransforms(
				0,
				RenderTransforms,
				true,
				true,
				true);
	}
}