#pragma once

#include "CoreMinimal.h"

/**
 * Ведение центра Squad по ломаной пути (L1).
 *
 * Путь — массив точек; последняя точка это цель приказа. Центр ведёт не
 * сама точка поворота, а точка на пути впереди центра (погоня за точкой):
 * пока центр едет, эта точка скользит по пути, поэтому направление меняется
 * постепенно и центр срезает угол дугой, а не идёт по ломаной.
 *
 * Расстояние до точки погони — Lookahead; оно задаёт крутизну дуги.
 */
struct FAlgonaPathFollowResult
{
	// Куда вести центр сейчас.
	FVector AimLocation = FVector::ZeroVector;

	// Остаток пути до последней точки, см: от центра до текущей точки плюс
	// длина оставшихся отрезков. По нему решаются торможение у цели,
	// упреждающий доворот и применение ширины строя.
	double RemainingDistance = 0.0;
};

/**
 * Точка погони и остаток пути.
 * InOutPointIndex — к какой точке Squad идёт; функция сдвигает его вперёд,
 * когда центр прошёл поворот (на срезанной дуге сама точка поворота
 * остаётся в стороне, поэтому «дошёл» определяется по направлению
 * следующего отрезка, а не по расстоянию).
 */
ALGONASIMULATION_API FAlgonaPathFollowResult FollowAlgonaPath(
	TConstArrayView<FVector> PathPoints,
	const FVector& CenterLocation,
	double Lookahead,
	int32& InOutPointIndex);

/** Длина ломаной от точки FromIndex до последней, см (на плоскости). */
ALGONASIMULATION_API double GetAlgonaPathLength(
	TConstArrayView<FVector> PathPoints,
	int32 FromIndex);

/**
 * Ворота коридора: общее ребро двух соседних полигонов найденного пути.
 * Их концы — углы препятствий, а длина — ширина прохода в этом месте.
 * Нужны для отладочной отрисовки и для оценки того, какой запас от углов
 * путь реально держит.
 */
struct FAlgonaPathPortal
{
	FVector2D Left = FVector2D::ZeroVector;
	FVector2D Right = FVector2D::ZeroVector;
};
