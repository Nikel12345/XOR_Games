#include "PCH.h"
#include "FractalShaderSet.h"
#include "FractalUpdateSet.h"
#include "EngineContext.h"
#include "ShaderManager.h"
#include "DefaultRenderPassSet.h"   // RP::LightCountPushData

// Единая точка регистрации СВОИХ push-констант — ОДИН РАЗ на инициализации (см. заголовок).
// Sp приезжают из shaders.json своей сцены; какая из них загружена сейчас — здесь не важно.
void FractalShaderSet::RegisterShaderFuncs(EngineContext* ctx)
{
    namespace RP = DefaultRenderPassNamespace;
    ShaderManager* sm = ctx->GetShaderManager();

    // Пуши движковых sp (ShadowCaster/Wireframe) регистрирует сам движок вместе с их
    // созданием — Engine::InitDefaultShaders. Игре остаются только её собственные.
    //
    // Фрактальные фоны: у каждой сцены свой sp, имена разные — регистрируем обе функции разом,
    // разбор имени сцены больше не нужен. Данные эти sp строят сами (время/итерации), состояние
    // прохода им не нужно — берём сырую форму и raw игнорируем (fragment slot 0 → b0, space3).
    sm->CreatePushInstruction("Fractal", PushStage::Fragment, [](const PushConstantBinder& b, const PushInput&) {
        FractalUpdateSet::FractalPushData d{};   // max_steps по умолчанию — потолок рэймарча
        d.time = (float)SDL_GetTicks() / 1000.0f;
        b.Push(d);
    });

    sm->CreatePushInstruction("Mandelbrot", PushStage::Fragment, [](const PushConstantBinder& b, const PushInput&) {
        FractalUpdateSet::FractalPushData d{};
        d.time = (float)SDL_GetTicks() / 1000.0f;
        // Потолок итераций пикселя = длина референс-орбиты: глубже неё дельты всё
        // равно не уходят (ре-базирование заворачивает m на начало орбиты).
        d.max_steps = FractalUpdateSet::MANDELBROT_ORBIT_MAX;
        b.Push(d);
    });

    // AnchorObject (кубы-якоря на движковой лайтинг-базе) тут НЕ значится намеренно: счётчик
    // светов приезжает ему типовым пушем — маркер стоит в движковом прологе, который его fs
    // включает. Регистрировать по имени программы нужно только СВОИ константы, как выше.
}
