#include "PCH.h"
#include <cmath>
#include "GravityScene.h"
#include "EngineContext.h"
#include "ModelManager.h"

// --- Процедурные параллелепипеды "cube_0".."cube_(N-1)" для сцены из scene_gen.py ---
// ВАЖНО: kCubeVariants должен совпадать с NUM_CUBE_MODELS в scripts/scene_gen.py —
// питон-скрипт раздаёт сущностям имена именно из этого диапазона.
static constexpr int kCubeVariants = 12;

void GravityScene::CreateModels(EngineContext* ctx)
{
    ModelManager* modelManager = ctx->GetModelManager();

    for (int ci = 0; ci < kCubeVariants; ++ci) {
        // Разные пропорции коробки из индекса (детерминированно, полу-размеры 0.3..1.1).
        const float hx = 0.3f + 0.2f * float((ci * 7)  % 5);
        const float hy = 0.3f + 0.2f * float((ci * 3)  % 5);
        const float hz = 0.3f + 0.2f * float((ci * 11) % 5);
        ctx->CreateModel<PosUVNormal>("cube_" + std::to_string(ci), [hx, hy, hz](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
            const float H[3] = { hx, hy, hz };
            // 6 граней. c — угол-начало, U/V — рёбра (в долях полу-размеров, per-axis);
            // cross(U,V) = ВНЕШНЯЯ нормаль → CCW наружу (как у quad). p0=c, p1=c+U, p2=c+U+V, p3=c+V.
            struct FaceDef { float c[3], U[3], V[3], N[3]; };
            static const FaceDef faces[6] = {
                {{ 1,-1, 1}, { 0, 0,-2}, { 0, 2, 0}, { 1, 0, 0}},  // +X
                {{-1,-1,-1}, { 0, 0, 2}, { 0, 2, 0}, {-1, 0, 0}},  // -X
                {{-1, 1, 1}, { 2, 0, 0}, { 0, 0,-2}, { 0, 1, 0}},  // +Y
                {{-1,-1,-1}, { 2, 0, 0}, { 0, 0, 2}, { 0,-1, 0}},  // -Y
                {{-1,-1, 1}, { 2, 0, 0}, { 0, 2, 0}, { 0, 0, 1}},  // +Z
                {{ 1,-1,-1}, {-2, 0, 0}, { 0, 2, 0}, { 0, 0,-1}},  // -Z
            };
            const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
            for (int f = 0; f < 6; ++f) {
                const FaceDef& fd = faces[f];
                // Касательная = нормализованное направление U в мировых пропорциях.
                float tx = fd.U[0]*H[0], ty = fd.U[1]*H[1], tz = fd.U[2]*H[2];
                float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
                if (tl > 0.0f) { tx /= tl; ty /= tl; tz /= tl; }
                const uint32_t vbase = static_cast<uint32_t>(v.size());
                for (int q = 0; q < 4; ++q) {
                    PosUVNormal vert{};
                    vert.x = (fd.c[0] + uv[q][0]*fd.U[0] + uv[q][1]*fd.V[0]) * H[0];
                    vert.y = (fd.c[1] + uv[q][0]*fd.U[1] + uv[q][1]*fd.V[1]) * H[1];
                    vert.z = (fd.c[2] + uv[q][0]*fd.U[2] + uv[q][1]*fd.V[2]) * H[2];
                    // v-down канон (как quad/sphere): хранимый v = 1-параметр. Позиция выше считается
                    // по исходному uv[q] — её НЕ трогаем, флипаем только текстурный v.
                    vert.u = uv[q][0]; vert.v = 1.0f - uv[q][1];
                    vert.nx = fd.N[0]; vert.ny = fd.N[1]; vert.nz = fd.N[2];
                    vert.tx = tx;      vert.ty = ty;      vert.tz = tz;
                    v.push_back(vert);
                }
                idx.push_back(vbase + 0); idx.push_back(vbase + 1); idx.push_back(vbase + 2);
                idx.push_back(vbase + 0); idx.push_back(vbase + 2); idx.push_back(vbase + 3);
            }
        }, AnchorShift::Keep, ResourceTag::CodeOwned);   // процедурные кубы игры — в models.json не идут
        modelManager->SetModelLods(modelManager->ModelIdOf("cube_" + std::to_string(ci)),
                                   { ModelLod{ modelManager->ModelIdOf("quad"), 8.0f },
                                     ModelLod{ modelManager->ModelIdOf("point"), 2.0f } });
    }
}

void GravityScene::DeleteModels(EngineContext* ctx)
{
    ModelManager* modelManager = ctx->GetModelManager();
    for (int ci = 0; ci < kCubeVariants; ++ci)
        modelManager->DeleteModel(modelManager->ModelIdOf("cube_" + std::to_string(ci)), NameSlot::Release);
}
