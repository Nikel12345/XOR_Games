#ifndef MAIN_FRACTAL_SURFACE_HLSL
#define MAIN_FRACTAL_SURFACE_HLSL

// Textured main-суржейс для объектов, ЯКОРЁННЫХ ВО ФРАКТАЛЕ (mygame, sp AnchorObject):
// полная копия main_pass/surface.hlsl (тот же свет/POM/слоты/параметры материала) + туман
// фрактального кадра. FRACTAL_FOG включает в базе (main_pass.frag.hlsl) буфер кадра губки
// (4-й fs_buffer → t9 space2, после Camera t8 текстурного пролога) и гашение цвета/эмиссии
// формулой тумана menger.frag.hlsl — объекты растворяются в той же атмосфере, что и губка.
#define FRACTAL_FOG 1

#include "main_pass/surface.hlsl"

#endif
