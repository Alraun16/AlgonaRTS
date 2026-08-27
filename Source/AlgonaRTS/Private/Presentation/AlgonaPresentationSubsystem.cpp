#include "Presentation/AlgonaPresentationSubsystem.h"

#include "AlgonaPresentationView.h"
#include "AlgonaPresentationSnapshotSelection.h"
#include "Camera/AlgonaRTSCameraActor.h"
#include "Presentation/AlgonaArmyPresentationActor.h"
#include "Presentation/AlgonaLegacyIsmPresentationActor.h"
#include "Presentation/AlgonaPresentationSettings.h"
#include "Core/AlgonaSimulationSubsystem.h"

#include "Camera/CameraComponent.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

namespace
{
#if !UE_BUILD_SHIPPING
	TAutoConsoleVariable<int32> CVarAlgonaP1DebugSpatialGrid(
		TEXT("algona.P1.DebugSpatialGrid"),
		0,
		TEXT("Draw the squad spatial uniform grid and 2D cell coordinates. 0=off, 1=on."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarAlgonaP1DebugSnapshotMetrics(
		TEXT("algona.P1.DebugSnapshotMetrics"),
		1,
		TEXT("Draw snapshot pipeline timings and selection counts. 0=off, 1=on."),
		ECVF_Default);
#endif
}

bool UAlgonaPresentationSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Cast<UWorld>(Outer);
	if (!World)
	{
		return false;
	}

	const bool bPlayableWorld =
		World->WorldType == EWorldType::Game
		|| World->WorldType == EWorldType::PIE;

	return bPlayableWorld && World->GetNetMode() != NM_DedicatedServer;
}

void UAlgonaPresentationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (GEngine)
	{
		GEngine->SetMaxFPS(165.0f);
	}

	// Keep the two basic benchmark overlays enabled in normal Editor and
	// Development runs. Shipping may suppress engine stats independently.
	if (UGameViewportClient* GameViewport = InWorld.GetGameViewport())
	{
		TArray<FString> EnabledStats;
		if (const TArray<FString>* CurrentStats = GameViewport->GetEnabledStats())
		{
			EnabledStats = *CurrentStats;
		}

		EnabledStats.AddUnique(TEXT("FPS"));
		EnabledStats.AddUnique(TEXT("Unit"));
		GameViewport->SetEnabledStats(EnabledStats);
	}

	if (ArmyPresentationActor || LegacyPresentationActor || CameraActor)
	{
		return;
	}

	const EAlgonaP1PresentationMode PresentationMode =
		GetAlgonaP1PresentationMode();

	if (PresentationMode == EAlgonaP1PresentationMode::SimulationOnly)
	{
		return;
	}

	FActorSpawnParameters CameraSpawnParameters;
	CameraSpawnParameters.Name = TEXT("AlgonaRTSCamera");
	CameraSpawnParameters.ObjectFlags |= RF_Transient;
	CameraSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	CameraActor = InWorld.SpawnActor<AAlgonaRTSCameraActor>(
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		CameraSpawnParameters);

	if (!CameraActor)
	{
		return;
	}

#if !UE_BUILD_SHIPPING
	if (!SpatialGridDebugDrawHandle.IsValid())
	{
		SpatialGridDebugDrawHandle = UDebugDrawService::Register(
			TEXT("Game"),
			FDebugDrawDelegate::CreateUObject(
				this,
				&UAlgonaPresentationSubsystem::DrawSpatialGridDebug));
	}
#endif

	FActorSpawnParameters PresentationSpawnParameters;
	PresentationSpawnParameters.ObjectFlags |= RF_Transient;
	PresentationSpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	if (PresentationMode == EAlgonaP1PresentationMode::LegacyStaticIsm)
	{
		PresentationSpawnParameters.Name = TEXT("AlgonaLegacyIsmPresentation");
		LegacyPresentationActor =
			InWorld.SpawnActor<AAlgonaLegacyIsmPresentationActor>(
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				PresentationSpawnParameters);

		if (!LegacyPresentationActor)
		{
			CameraActor->Destroy();
			CameraActor = nullptr;
			return;
		}

		LegacyPresentationActor->SetPresentationCamera(
			CameraActor->GetCameraComponent());
		LegacyPresentationActor->AddTickPrerequisiteActor(CameraActor);
		return;
	}

	PresentationSpawnParameters.Name = TEXT("AlgonaArmyPresentation");
	ArmyPresentationActor =
		InWorld.SpawnActor<AAlgonaArmyPresentationActor>(
			FVector::ZeroVector,
			FRotator::ZeroRotator,
			PresentationSpawnParameters);

	if (!ArmyPresentationActor)
	{
		CameraActor->Destroy();
		CameraActor = nullptr;
		return;
	}

	ArmyPresentationActor->SetPresentationCamera(
		CameraActor->GetCameraComponent());
	ArmyPresentationActor->AddTickPrerequisiteActor(CameraActor);
}

void UAlgonaPresentationSubsystem::Deinitialize()
{
#if !UE_BUILD_SHIPPING
	ClearAlgonaPresentationSnapshotMetrics(GetWorld());

	if (SpatialGridDebugDrawHandle.IsValid())
	{
		UDebugDrawService::Unregister(SpatialGridDebugDrawHandle);
		SpatialGridDebugDrawHandle = FDelegateHandle();
	}
#endif

	if (IsValid(ArmyPresentationActor))
	{
		ArmyPresentationActor->Destroy();
	}
	ArmyPresentationActor = nullptr;

	if (IsValid(LegacyPresentationActor))
	{
		LegacyPresentationActor->Destroy();
	}
	LegacyPresentationActor = nullptr;

	if (IsValid(CameraActor))
	{
		CameraActor->Destroy();
	}
	CameraActor = nullptr;

	Super::Deinitialize();
}

void UAlgonaPresentationSubsystem::DrawSpatialGridDebug(
	UCanvas* Canvas,
	APlayerController* PlayerController)
{
#if !UE_BUILD_SHIPPING
	const bool bDrawSpatialGrid =
		CVarAlgonaP1DebugSpatialGrid.GetValueOnAnyThread() != 0;
	const bool bDrawSnapshotMetrics =
		CVarAlgonaP1DebugSnapshotMetrics.GetValueOnAnyThread() != 0;

	if ((!bDrawSpatialGrid && !bDrawSnapshotMetrics)
		|| !Canvas
		|| !PlayerController)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || PlayerController->GetWorld() != World)
	{
		return;
	}

	if (bDrawSnapshotMetrics)
	{
		FAlgonaPresentationSnapshotMetrics Metrics;
		if (GetAlgonaPresentationSnapshotMetrics(World, Metrics))
		{
			UFont* MetricFont = GEngine ? GEngine->GetSmallFont() : nullptr;
			if (MetricFont)
			{
				const FVector2D MetricScale(1.0, 1.0);
				const FLinearColor MetricColor(1.0f, 0.0f, 0.0f, 1.0f);
				const FLinearColor MetricShadow(0.0f, 0.0f, 0.0f, 0.9f);
				constexpr float RightMargin = 14.0f;
				constexpr float BottomMargin = 20.0f;
				constexpr float LineSpacing = 2.0f;

				const TCHAR* ModeText = Metrics.bSpatialPathUsed
					? TEXT("GRID")
					: (Metrics.bSpatialSnapshotsRequested ? TEXT("FULL fallback") : TEXT("FULL"));

				TArray<FString> Lines;
				Lines.Reserve(6);
				Lines.Add(FString::Printf(
					TEXT("Snapshots %s [%d]"),
					ModeText,
					Metrics.bSpatialSnapshotsRequested ? 1 : 0));
				Lines.Add(FString::Printf(
					TEXT("Total: %.3f ms   ema %.3f"),
					Metrics.LastTotalMilliseconds,
					Metrics.AverageTotalMilliseconds));
				Lines.Add(FString::Printf(
					TEXT("Export: %.3f ms   ema %.3f"),
					Metrics.LastExportMilliseconds,
					Metrics.AverageExportMilliseconds));
				Lines.Add(FString::Printf(
					TEXT("Grid: %.3f ms   SquadTest: %.3f ms"),
					Metrics.LastGridQueryMilliseconds,
					Metrics.LastSquadTestMilliseconds));
				if (Metrics.bSpatialPathUsed)
				{
					Lines.Add(FString::Printf(
						TEXT("Squads V/C/T: %d / %d / %d"),
						Metrics.VisibleSquadCount,
						Metrics.CandidateSquadCount,
						Metrics.TotalSquadCount));
				}
				else
				{
					Lines.Add(FString::Printf(
						TEXT("Squads V/C/T: - / - / %d"),
						Metrics.TotalSquadCount));
				}
				Lines.Add(FString::Printf(
					TEXT("Soldiers exported: %d / %d"),
					Metrics.ExportedSoldierCount,
					Metrics.TotalSoldierCount));

				float LineHeight = 0.0f;
				for (const FString& Line : Lines)
				{
					const FVector2D Size = Canvas->K2_TextSize(MetricFont, Line, MetricScale);
					LineHeight = FMath::Max(LineHeight, static_cast<float>(Size.Y));
				}

				const float BlockHeight =
					Lines.Num() * LineHeight
					+ FMath::Max(Lines.Num() - 1, 0) * LineSpacing;
				float DrawY = Canvas->ClipY - BottomMargin - BlockHeight;

				for (const FString& Line : Lines)
				{
					const FVector2D Size = Canvas->K2_TextSize(MetricFont, Line, MetricScale);
					const FVector2D Position(
						Canvas->ClipX - RightMargin - Size.X,
						DrawY);

					Canvas->K2_DrawText(
						MetricFont,
						Line,
						Position,
						MetricScale,
						MetricColor,
						0.0f,
						MetricShadow,
						FVector2D(1.0, 1.0),
						false,
						false,
						false,
						FLinearColor::Transparent);

					DrawY += LineHeight + LineSpacing;
				}
			}
		}
	}

	if (!bDrawSpatialGrid || !CameraActor)
	{
		return;
	}

	UAlgonaSimulationSubsystem* Simulation =
		World->GetSubsystem<UAlgonaSimulationSubsystem>();
	UCameraComponent* Camera = CameraActor->GetCameraComponent();
	if (!Simulation || !Camera)
	{
		return;
	}

	FAlgonaPresentationView View;
	FVector2D ViewMin = FVector2D::ZeroVector;
	FVector2D ViewMax = FVector2D::ZeroVector;
	if (!View.Build(*World, *Camera)
		|| !View.GetGroundBounds(0.0, ViewMin, ViewMax))
	{
		return;
	}

	const FIntPoint MinCell = Simulation->GetSpatialGridCellCoordinates(
		FVector(ViewMin.X, ViewMin.Y, 0.0));
	const FIntPoint MaxCell = Simulation->GetSpatialGridCellCoordinates(
		FVector(ViewMax.X, ViewMax.Y, 0.0));
	const double CellSize = Simulation->GetSpatialGridCellSizeCm();

	if (CellSize <= 0.0)
	{
		return;
	}

	const FLinearColor GridColor(1.0f, 1.0f, 1.0f, 0.35f);
	const FLinearColor TextColor(1.0f, 1.0f, 1.0f, 0.85f);
	const FLinearColor ShadowColor(0.0f, 0.0f, 0.0f, 0.8f);
	constexpr float LineThickness = 1.0f;
	constexpr double DrawHeight = 2.0;

	auto ProjectToCanvas = [Canvas](const FVector& WorldPoint)
	{
		const FVector Projected = Canvas->K2_Project(WorldPoint);
		return FVector2D(Projected.X, Projected.Y);
	};

	const double MinWorldX = static_cast<double>(MinCell.X) * CellSize;
	const double MinWorldY = static_cast<double>(MinCell.Y) * CellSize;
	const double MaxWorldX = static_cast<double>(MaxCell.X + 1) * CellSize;
	const double MaxWorldY = static_cast<double>(MaxCell.Y + 1) * CellSize;

	// Each boundary is drawn once, so debug cost scales with visible rows and
	// columns rather than drawing four duplicate edges for every cell.
	for (int32 CellX = MinCell.X; CellX <= MaxCell.X + 1; ++CellX)
	{
		const double WorldX = static_cast<double>(CellX) * CellSize;
		Canvas->K2_DrawLine(
			ProjectToCanvas(FVector(WorldX, MinWorldY, DrawHeight)),
			ProjectToCanvas(FVector(WorldX, MaxWorldY, DrawHeight)),
			LineThickness,
			GridColor);
	}

	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y + 1; ++CellY)
	{
		const double WorldY = static_cast<double>(CellY) * CellSize;
		Canvas->K2_DrawLine(
			ProjectToCanvas(FVector(MinWorldX, WorldY, DrawHeight)),
			ProjectToCanvas(FVector(MaxWorldX, WorldY, DrawHeight)),
			LineThickness,
			GridColor);
	}

	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font)
	{
		return;
	}

	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			const FIntPoint Cell(CellX, CellY);
			const FVector2D CellWorldMin =
				Simulation->GetSpatialGridCellWorldMin(Cell);
			const double HalfCellSize = CellSize * 0.5;
			const FVector2D ScreenCorner = ProjectToCanvas(
				FVector(
					CellWorldMin.X + HalfCellSize,
					CellWorldMin.Y + HalfCellSize,
					DrawHeight));

			Canvas->K2_DrawText(
				Font,
				FString::Printf(TEXT("(%d, %d)"), CellX, CellY),
				ScreenCorner,
				FVector2D(1.4, 1.4),
				TextColor,
				0.0f,
				ShadowColor,
				FVector2D(1.0, 1.0),
				true,
				true,
				false,
				FLinearColor::Transparent);
		}
	}
#else
	(void)Canvas;
	(void)PlayerController;
#endif
}
