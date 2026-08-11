#include "AlgonaSimulationTypes.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAlgonaP0SimulationTypesTest, "Algona.P0.Simulation.Types", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::SmokeFilter);

bool FAlgonaP0SimulationTypesTest::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Position fragment is a Mass fragment"),
		FAlgonaSimPositionFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()
		)
	);
	TestTrue(
		TEXT("Velocity fragment is a Mass fragment"),
		FAlgonaSimVelocityFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()
		)
	);
	TestTrue(
		TEXT("Group fragment is a Mass fragment"),
		FAlgonaSimGroupFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()
		)
	);
	TestTrue(
		TEXT("State fragment is a Mass fragment"),
		FAlgonaSimStateFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()
		)
	);
	TestTrue(
		TEXT("LOD fragment is a Mass fragment"),
		FAlgonaSimLODFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()
		)
	);
	TestTrue(
		TEXT("Navigation fragment is a Mass fragment"),
		FAlgonaSimNavigationFragment::StaticStruct()->IsChildOf(
			FMassFragment::StaticStruct()
		)
	);

	TestFalse(TEXT("Default group id is invalid"), FSimGroupId().IsValid());
	TestTrue(TEXT("Positive group id is valid"), FSimGroupId(1).IsValid());

	FAlgonaSimCommand Command;
	Command.GroupId = FSimGroupId(7);
	Command.TargetPosition = FVector(100.0, 200.0, 0.0);
	TestEqual(TEXT("Command group id is stored"), Command.GroupId.Value, 7);
	TestEqual(TEXT("Command target is stored"), Command.TargetPosition.X, 100.0);
	TestEqual(
		TEXT("Default command facing mode follows movement"),
		Command.MoveFacingMode,
		EAlgonaMoveFacingMode::FaceMovement
	);

	FAlgonaSimBenchmarkSnapshot Snapshot;
	TestEqual(TEXT("Default benchmark fixed-step time is zero"), Snapshot.LastFixedStepMilliseconds, 0.0);
	TestFalse(TEXT("Default benchmark deadline is not missed"), Snapshot.bLastFixedStepMissedDeadline);
	TestEqual(TEXT("Default benchmark submitted command sequence is zero"), Snapshot.LastSubmittedCommandSequence, uint64(0));
	TestEqual(TEXT("Default benchmark applied command sequence is zero"), Snapshot.LastAppliedCommandSequence, uint64(0));
	TestEqual(TEXT("Default benchmark applied command group is invalid"), Snapshot.LastAppliedCommandGroupId, INDEX_NONE);
	TestEqual(TEXT("Default benchmark queued path count is zero"), Snapshot.QueuedPathRequestCount, 0);
	TestEqual(TEXT("Default benchmark in-flight path count is zero"), Snapshot.InFlightPathRequestCount, 0);
	TestEqual(TEXT("Default benchmark completed path count is zero"), Snapshot.CompletedPathRequests, uint64(0));
	TestEqual(TEXT("Default benchmark stale path result count is zero"), Snapshot.RejectedStalePathResults, uint64(0));
	TestEqual(TEXT("Default spatial grid build time is zero"), Snapshot.LastSpatialGridBuildMilliseconds, 0.0);
	TestEqual(TEXT("Default spatial grid entity count is zero"), Snapshot.LastSpatialGridEntityCount, 0);
	TestEqual(TEXT("Default neighbor query count is zero"), Snapshot.LastNeighborQueryCount, 0);
	TestEqual(TEXT("Default neighbor query LOD skip count is zero"), Snapshot.LastNeighborQueriesSkippedByLOD, 0);
	TestEqual(TEXT("Default high LOD entity count is zero"), Snapshot.HighLODEntityCount, 0);
	TestEqual(TEXT("Default medium LOD entity count is zero"), Snapshot.MediumLODEntityCount, 0);
	TestEqual(TEXT("Default low LOD entity count is zero"), Snapshot.LowLODEntityCount, 0);
	TestEqual(TEXT("Default formation LOD skip count is zero"), Snapshot.LastFormationSkippedGroupsByLOD, 0);
	TestEqual(TEXT("Default formation target update count is zero"), Snapshot.LastFormationTargetUpdatedEntities, 0);
	TestEqual(TEXT("Default formation target LOD skip count is zero"), Snapshot.LastFormationTargetSkippedEntitiesByLOD, 0);
	TestEqual(TEXT("Default neighbor average candidates is zero"), Snapshot.LastNeighborAverageCandidates, 0.0);
	TestEqual(TEXT("Default avoidance time is zero"), Snapshot.LastAvoidanceMilliseconds, 0.0);
	TestEqual(TEXT("Default avoidance application count is zero"), Snapshot.LastAvoidanceAppliedCount, 0);
	TestEqual(TEXT("Default SimLOD measurement step count is zero"), Snapshot.SimLODMeasurementStepCount, uint64(0));
	TestEqual(TEXT("Default SimLOD measured formation time is zero"), Snapshot.SimLODMeasurementFormationMilliseconds, 0.0);
	TestEqual(TEXT("Default SimLOD measured neighbor query count is zero"), Snapshot.SimLODMeasurementNeighborQueries, uint64(0));
	TestEqual(TEXT("Default hard blocker count is zero"), Snapshot.ActiveHardBlockerCount, 0);
	TestEqual(TEXT("Default route invalidation count is zero"), Snapshot.LastRouteInvalidationCount, 0);
	TestEqual(TEXT("Default repath request count is zero"), Snapshot.TotalRepathsRequested, uint64(0));
	TestEqual(TEXT("Default stuck group count is zero"), Snapshot.LastStuckGroupCount, 0);
	TestEqual(TEXT("Default hard-blocked movement count is zero"), Snapshot.LastHardBlockerMovementBlockedEntities, 0);
	TestEqual(TEXT("Default total hard-blocked movement count is zero"), Snapshot.TotalHardBlockerMovementBlockedEntities, uint64(0));

	FAlgonaGroupNavigationState NavigationState;
	TestEqual(
		TEXT("Default P1 navigation status is idle"),
		NavigationState.Status,
		EAlgonaGroupNavigationStatus::Idle
	);
	TestEqual(TEXT("Default P1 route generation is zero"), NavigationState.RouteGeneration, uint64(0));
	TestFalse(TEXT("Default P1 route is invalid"), NavigationState.Route.bValid);
	TestEqual(TEXT("Default P1 route has no points"), NavigationState.Route.Points.Num(), 0);
	TestFalse(TEXT("Default P1 start projection is false"), NavigationState.bLastStartProjectionSucceeded);
	TestFalse(TEXT("Default P1 destination projection is false"), NavigationState.bLastDestinationProjectionSucceeded);
	TestEqual(TEXT("Default P1 last repath reason is empty"), NavigationState.LastRepathReason.Len(), 0);
	TestEqual(TEXT("Default P1 stuck ticks is zero"), NavigationState.StuckTicks, 0);
	TestEqual(TEXT("Default P1 last repath tick is zero"), NavigationState.LastRepathTick, uint64(0));
	TestEqual(TEXT("Default P1 progress sample tick is zero"), NavigationState.LastProgressSampleTick, uint64(0));
	TestEqual(TEXT("Default P1 progress route point is invalid"), NavigationState.LastProgressRoutePointIndex, INDEX_NONE);
	TestEqual(TEXT("Default P1 command start time is unset"), NavigationState.CommandStartTimeSeconds, -1.0);
	TestEqual(TEXT("Default P1 bridge entry time is unset"), NavigationState.BridgeEntryTimeSeconds, -1.0);
	TestEqual(TEXT("Default P1 bridge exit time is unset"), NavigationState.BridgeExitTimeSeconds, -1.0);
	TestEqual(TEXT("Default P1 route arrival time is unset"), NavigationState.RouteArrivalTimeSeconds, -1.0);
	TestEqual(TEXT("Default P1 idle time is unset"), NavigationState.IdleTimeSeconds, -1.0);
	TestEqual(TEXT("Default P1 route length is zero"), NavigationState.RouteLength, 0.0);
	TestFalse(TEXT("Default P1 choke state is false"), NavigationState.bWasInsideChoke);

	FAlgonaFormationState FormationState;
	TestEqual(TEXT("Default formation columns is one"), FormationState.OriginalColumns, 1);
	TestEqual(TEXT("Default formation rows is one"), FormationState.OriginalRows, 1);
	TestEqual(TEXT("Default formation slot count is zero"), FormationState.SlotCount, 0);
	TestTrue(
		TEXT("Default formation travel direction is forward"),
		FormationState.TravelDirection.Equals(FVector::ForwardVector)
	);
	TestTrue(
		TEXT("Default formation layout forward is forward"),
		FormationState.LayoutForward.Equals(FVector::ForwardVector)
	);
	TestTrue(
		TEXT("Default formation layout right is right"),
		FormationState.LayoutRight.Equals(FVector::RightVector)
	);
	TestTrue(
		TEXT("Default formation facing is forward"),
		FormationState.FacingDirection.Equals(FVector::ForwardVector)
	);
	TestEqual(TEXT("Default formation corridor width is zero"), FormationState.CorridorWidth, 0.0f);
	TestEqual(TEXT("Default required footprint half-width is zero"), FormationState.RequiredFootprintHalfWidth, 0.0f);
	TestEqual(TEXT("Default safe anchor offset is zero"), FormationState.SafeAnchorLateralOffset, 0.0f);
	TestEqual(TEXT("Default corridor profile front is zero"), FormationState.CorridorProfileFrontOffset, 0.0f);
	TestEqual(TEXT("Default corridor profile rear is zero"), FormationState.CorridorProfileRearOffset, 0.0f);
	TestEqual(TEXT("Default formation forward speed scale is one"), FormationState.ForwardSpeedScale, 1.0f);
	TestEqual(TEXT("Default required lateral speed is zero"), FormationState.MaxRequiredLateralSpeed, 0.0f);
	TestEqual(TEXT("Default speed-limited member count is zero"), FormationState.SpeedLimitedMembers, 0);
	TestEqual(TEXT("Default formation corridor valid samples is zero"), FormationState.CorridorValidSamples, 0);
	TestFalse(TEXT("Default formation is not locally deformed"), FormationState.bLocallyDeformed);
	TestEqual(TEXT("Default formation phase is normal"), FormationState.Phase, EAlgonaFormationMemberPhase::Normal);
	TestEqual(TEXT("Default formation funnel count is zero"), FormationState.MembersFunneling, 0);
	TestEqual(TEXT("Default formation choke count is zero"), FormationState.MembersInsideChoke, 0);
	TestEqual(TEXT("Default formation re-expansion count is zero"), FormationState.MembersReExpanding, 0);
	TestEqual(TEXT("Default formation soft-overlap count is zero"), FormationState.SoftOverlapMembers, 0);
	TestEqual(TEXT("Default formation settled ticks is zero"), FormationState.SettledTicks, 0);

	FAlgonaSimNavigationFragment NavigationFragment;
	TestEqual(TEXT("Default original slot row is invalid"), NavigationFragment.OriginalSlotRow, INDEX_NONE);
	TestEqual(TEXT("Default original slot column is invalid"), NavigationFragment.OriginalSlotColumn, INDEX_NONE);
	TestEqual(TEXT("Default local spacing is zero"), NavigationFragment.LastLocalSpacing, 0.0f);
	TestEqual(TEXT("Default member longitudinal offset is zero"), NavigationFragment.MemberLongitudinalOffset, 0.0f);
	TestEqual(TEXT("Default corridor constraint offset is zero"), NavigationFragment.CorridorConstraintLongitudinalOffset, 0.0f);
	TestEqual(TEXT("Default narrow-boundary distance is zero"), NavigationFragment.DistanceToNarrowBoundary, 0.0f);
	TestEqual(TEXT("Default required lateral distance is zero"), NavigationFragment.RequiredLateralDistance, 0.0f);
	TestEqual(TEXT("Default member required lateral speed is zero"), NavigationFragment.RequiredLateralSpeed, 0.0f);
	TestEqual(TEXT("Default member forward speed scale is one"), NavigationFragment.RecommendedForwardSpeedScale, 1.0f);
	TestEqual(TEXT("Default member formation phase is normal"), NavigationFragment.FormationPhase, EAlgonaFormationMemberPhase::Normal);
	TestFalse(TEXT("Default member does not soft-overlap"), NavigationFragment.bSoftOverlap);

	FAlgonaFormationCorridorSample CorridorSample;
	TestFalse(TEXT("Default corridor sample is invalid"), CorridorSample.bValid);
	TestEqual(TEXT("Default corridor sample longitudinal offset is zero"), CorridorSample.LongitudinalOffset, 0.0f);
	TestEqual(TEXT("Default corridor sample width is zero"), CorridorSample.Width, 0.0f);

	FAlgonaDebugPresentationEntity DebugPresentationEntity;
	TestEqual(TEXT("Default debug presentation group is invalid"), DebugPresentationEntity.GroupId, INDEX_NONE);
	TestEqual(TEXT("Default debug presentation cube size is twenty-five"), DebugPresentationEntity.CubeSize, 25.0f);
	TestTrue(
		TEXT("Default debug presentation facing is forward"),
		DebugPresentationEntity.FacingDirection.Equals(FVector::ForwardVector)
	);

	FAlgonaSimGroupState GroupState;
	TestEqual(
		TEXT("Default group move facing mode follows movement"),
		GroupState.MoveFacingMode,
		EAlgonaMoveFacingMode::FaceMovement
	);
	TestEqual(TEXT("Default group navigation LOD is high"), GroupState.NavigationLOD, EAlgonaSimLOD::High);
	TestEqual(TEXT("Default group navigation stride is one"), GroupState.NavigationUpdateStride, uint8(1));
	TestEqual(TEXT("Default group high LOD member count is zero"), GroupState.HighLODMemberCount, 0);
	TestEqual(TEXT("Default group minimum same-group spacing is sixty"), GroupState.MinSameGroupSpacing, 60.0f);
	TestEqual(TEXT("Default group debug cube size is twenty-five"), GroupState.DebugPresentationCubeSize, 25.0f);
	TestEqual(TEXT("Default debug formation columns are unset"), GroupState.DebugFormationColumns, 0);
	TestEqual(TEXT("Default debug formation rows are unset"), GroupState.DebugFormationRows, 0);

	return true;
}

#endif
