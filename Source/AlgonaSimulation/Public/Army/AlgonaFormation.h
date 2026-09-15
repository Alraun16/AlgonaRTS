#pragma once

#include "CoreMinimal.h"

/** Форма построения Squad. Первая реализация — только прямоугольник. */
enum class EAlgonaFormationShape : uint8
{
	Rectangle
};

/**
 * Параметры построения — вход генератора формы.
 * Каждый Squad хранит свои параметры и может менять их через Reform.
 */
struct ALGONASIMULATION_API FAlgonaFormationParams
{
	EAlgonaFormationShape Shape = EAlgonaFormationShape::Rectangle;

	// Максимальное число слотов в одной строке.
	int32 RowLength = 10;

	// Расстояние между соседними слотами внутри строки, см.
	float SlotSpacing = 150.0f;

	// Расстояние между соседними строками, см.
	float RowSpacing = 150.0f;
};

/**
 * Один слот в локальных координатах Squad.
 * Конвенция UE: X — вперёд по направлению Squad, Y — вправо.
 * (В ГД-документе оси названы наоборот: X — вдоль строки, Y — глубина.)
 */
struct ALGONASIMULATION_API FAlgonaFormationSlot
{
	// Смещение слота от центра Squad, см.
	FVector2f LocalOffset = FVector2f::ZeroVector;

	// Номер строки: 0 — передняя.
	int32 RowIndex = 0;
};

/**
 * Раскладка слотов — выход генератора формы.
 * Индекс в Slots и есть номер слота. Порядок: строки спереди назад,
 * внутри строки от центра к краям, правая сторона первой. Этот же порядок
 * является очередью при проходе через узкое место.
 */
struct ALGONASIMULATION_API FAlgonaFormationLayout
{
	TArray<FAlgonaFormationSlot> Slots;

	int32 RowCount = 0;

	// Расстояние от центра Squad до самого дальнего слота, см.
	float Radius = 0.0f;

	// Положение передней строки по X относительно центра Squad, см.
	// Нужно при Reform, чтобы строки оставались на месте, а центр сдвигался.
	float FrontRowLocalX = 0.0f;
};

/**
 * Длина строки прямоугольника по умолчанию: 5 при <=20 Unit, 10 при >20.
 * Временное правило первой реализации из ГД-документа.
 */
ALGONASIMULATION_API int32 GetAlgonaDefaultRowLength(int32 UnitCount);

/**
 * Строит раскладку из SlotCount слотов по параметрам формы.
 * Чистая функция: не зависит от Mass, Squad и мира.
 * Центр Squad — центр прямоугольника, охватывающего все слоты.
 */
ALGONASIMULATION_API void BuildAlgonaFormationLayout(
	const FAlgonaFormationParams& Params,
	int32 SlotCount,
	FAlgonaFormationLayout& OutLayout);
