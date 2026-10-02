#include "PCH.h"
#include <cmath>
#include "GravitySystem.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "PositionStructure.h"
#include "EngineProfiler.h"
#include "GameComponents.h"
#include "GameShaderSet.h"

// Гравитация центров, ЗАДАННЫХ СЦЕНОЙ: притягивает не безымянная константа, а сущность с
// GravityComponent — центр там, где её Transform, сила = её gm (см. BaseComponents.h). Центров
// может быть несколько, ускорения складываются; ни одного — кубы летят по инерции.
// gm ОБЯЗАН равняться GRAVITY_CONST * CENTRAL_MASS из scripts/scene_gen/scene_gen.py
// (сейчас 1 * 5000 = 5000): скорость круговой орбиты sqrt(GM/r) считает генератор — при другом gm
// орбиты станут эллиптичными/раскрутятся. Форму орбиты задаёт ТОЛЬКО gm.
// Гравитация ОБЪЁМНАЯ: ускорение считается по полному радиусу |(x,y,z)| и меняет все три
// компоненты скорости. Раньше она была плоской (XZ-радиус, только vx/vz) — под секции с
// заметной высотой это не годится: генератор ставит кубу скорость круговой орбиты по ПОЛНОМУ
// радиусу, её плоскость наклонена, и без y-составляющей ускорения куб просто улетал бы по
// прямой вверх/вниз от своего кольца. Плата — диск с ненулевой высотой живёт своей жизнью:
// кубы качаются через y == 0, как и положено наклонным орбитам.
static constexpr float kGravSoft = 1e-3f;   // защита от деления на ~0 у самого центра
static constexpr float kGravSoft2 = kGravSoft * kGravSoft;   // зажим r2 значением, без ветки

void GravitySystem::Tick(ObjectManager* om)
{
    SceneData* scene = om->GetActiveScene();
    if (!scene) return;

    const GravityWorldComponent* gravity_world = nullptr;
    om->ForEach<GravityWorldComponent>(scene, [&gravity_world](GravityWorldComponent& world) {
        gravity_world = &world;
    });
    if (!gravity_world) return;

    SimulateGravity(om, scene, gravity_world->sim_dt);
    ReturnJets(om, scene, gravity_world->jet_return_distance);
}

void GravitySystem::SimulateGravity(ObjectManager* om, SceneData* scene, float sim_dt)
{
    PROF_SCOPE(Sim, "  simulate_gravity (Game)");

    // Проход 1 — центры. Их единицы, и снимаем мы их ОДИН раз: иначе обход миллиона кубов
    // пришлось бы делать по разу на центр. Сам центр симуляция не двигает — он стоит там, куда
    // его поставила сцена (или редактор).
    gravity_sources.clear();
    om->ForEach<Positions, GravityComponent>(scene,
        [this](SoAElement<Positions> pos_el, GravityComponent& G)
    {
        Positions& P = pos_el.container();
        const size_t i = pos_el.i();
        gravity_sources.push_back({ P.w[i], P.d[i], P.h[i], G.gm, G.id,
                                    std::max(G.core_radius * G.core_radius, kGravSoft2) });
    });

    {
        float centers[GameShaderSet::MAX_GRAVITY_CENTERS][3];
        const size_t n = std::min(gravity_sources.size(), GameShaderSet::MAX_GRAVITY_CENTERS);
        for (size_t k = 0; k < n; ++k) {
            centers[k][0] = gravity_sources[k].x;
            centers[k][1] = gravity_sources[k].y;
            centers[k][2] = gravity_sources[k].z;
        }
        GameShaderSet::PublishGravityCenters(centers, n);
    }

    // Проход 2 — притягиваемые: сначала скорость по каждому источнику, потом интеграция позиций.
    // Два отдельных цикла без ветвлений и с колонками в локальных __restrict — только в такой
    // форме тело векторизуется (замерено на 800k, sandbox/GravityVecProbe.cpp). Поэтому и обход —
    // ForEachArchetype: поэлементная форма ForEach невекторизуема в принципе.
    const std::vector<GravitySource>& sources = gravity_sources;
    om->ForEachArchetype<Positions, Velocities>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<Velocities, void>* vel_arr,
            const std::vector<Entity>& ents)
    {
        Positions&  P = pos_arr->data;
        Velocities& V = vel_arr->data;
        const int n = static_cast<int>(ents.size());
        if (n == 0) return;

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();
        const float dt = sim_dt;

        // Состав компонентов у всех сущностей архетипа один, поэтому джет ли это — решает первая.
        const bool attracted = !om->Has<JetComponent>(scene, ents[0]);

        if (attracted) for (const GravitySource& s : sources) {
            const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm, r2_floor = s.r2_floor;
            VEC_HOT("gravity_step");
            for (int i = 0; i < n; ++i) {
                const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
                const float rr = dx * dx + dy * dy + dz * dz;
                // Зажим = max, его векторизатор берёт. Внутри шара r2 = R^2, и k*d = gm*d/R^3 —
                // ровно линейное ускорение однородного шара; снаружи прежнее gm/r^2.
                const float r2 = rr < r2_floor ? r2_floor : rr;
                const float k  = gm / (r2 * std::sqrt(r2));
                vx[i] += dx * k * dt;
                vy[i] += dy * k * dt;
                vz[i] += dz * k * dt;
            }
        }

        // Позиции (wdh) скоростями (xyz)
        VEC_HOT("integrate_positions");
        for (int i = 0; i < n; ++i) {
            pw[i] += vx[i] * dt;
            pd[i] += vy[i] * dt;
            ph[i] += vz[i] * dt;
        }
    });
}

void GravitySystem::ReturnJets(ObjectManager* om, SceneData* scene, float return_distance)
{
    PROF_SCOPE(Sim, "  return_jets (Game)");
    if (gravity_sources.empty()) return;

    // Центр и его объекты связаны ключом: объекты центра — джеты с center == его id. Отсюда и
    // форма обхода — [центры][их объекты]: внешний цикл по центрам, внутренний выбирает объекты
    // своего центра. Скорость не трогаем: вернувшийся куб снова летит тем же курсом.
    const float return2 = return_distance * return_distance;
    om->ForEachArchetype<Positions, JetComponent>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<JetComponent>* jet_arr)
    {
        Positions& P = pos_arr->data;
        const std::vector<JetComponent>& J = jet_arr->data;
        const size_t n = J.size();
        for (const GravitySource& c : gravity_sources) {
            for (size_t i = 0; i < n; ++i) {
                if (J[i].center != c.id) continue;
                const float dx = P.w[i] - c.x, dy = P.d[i] - c.y, dz = P.h[i] - c.z;
                if (dx * dx + dy * dy + dz * dz <= return2) continue;
                P.w[i] = c.x;
                P.d[i] = c.y;
                P.h[i] = c.z;
            }
        }
    });
}
