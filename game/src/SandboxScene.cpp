#include "PCH.h"
#include "SandboxScene.h"
#include "EngineContext.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "FontManager.h"
#include "UI_Yoga.h"
#include "PositionStructure.h"
#include "Colliders.h"
#include "DebugColliderSystem.h"

void SandboxScene::CreateDebugColliders(EngineContext* ctx, const SceneName& scene_name)
{
    ObjectManager* objectManager = ctx->GetObjectManager();
    SceneData* scene = objectManager->GetActiveScene();
    if (!scene) return;
    if (!ctx->GetMaterialManager()->GetMaterial("debug_collider")) return;

    // Авто-формы модели для fallback-прохода физики: по OBB на каждый сабмеш из его локального
    // AABB. Считаем здесь, потому что словарь моделей живёт в Engine, которую Physics не линкует
    // (см. ColliderQuery::ModelColliders).
    ModelManager* mm = ctx->GetModelManager();
    std::vector<DebugColliderSystem::DebugShape> shapes = DebugColliderSystem::CollectDebugShapes(
        *objectManager, scene,
        [mm](ModelId id) -> std::vector<Collider> {
            std::vector<Collider> boxes;
            const ModelData* model = mm->FindModel(id);
            if (!model) return boxes;
            boxes.reserve(model->submeshes.size());
            for (const SubMeshData& sm : model->submeshes)
                boxes.push_back(Collider::Box(sm.aabb_half, sm.aabb_center));
            return boxes;
        });
    if (shapes.empty()) return;

    for (const DebugColliderSystem::DebugShape& s : shapes) {
        const char* model_name = (s.kind == ShapeKind::Box) ? "debug_box" : "debug_sphere";
        LocalMatrixProxy16 lm{};   // SoA-локаль: в CreateEntity едет как прокси (как PositionProxy16)
        for (int i = 0; i < 16; ++i) lm.m[i] = s.local[i];
        ctx->CreateEntity(scene_name,
            RenderableProxy::Single(ctx->GetModelManager()->InternModel(model_name),
                                    { ctx->GetMaterialManager()->InternMaterial("debug_collider") }, false),
            PositionProxy16{},          // перезапишется композицией parent × local
            ParentComponent{ s.owner },
            lm,
            DebugColliderTag{},
            EditorHiddenComponent{},    // движковый тег: не показывать в списке объектов UI
            GeneratedComponent{});      // сгенерировано кодом → не сериализуется, пересоздаётся генератором
    }
}

void SandboxScene::CreateModels(EngineContext* ctx)
{
    // --- "two_quads": ОДИН меш из двух НЕСВЯЗАННЫХ островов (ни общих вершин, ни общих рёбер). ---
    // Процедурный путь кладёт всю геометрию генератора в ОДИН сабмеш (ModelManager::CreateModel),
    // так что это ровно «1 меш, 2 разъединённые части»: один индирект-дроу на оба квада.
    // Индексы острова смещены на его vbase — нумерация идёт от вершины 0 МОДЕЛИ, не острова.
    // Цена объединения: bounding sphere считается по всем вершинам сразу и накрывает зазор между
    // квадами (каллинг от этого лишь консервативнее, мис-каллить не начнёт), а авто-бокс коллайдера
    // по сабмешу станет монолитным — обе части в одном ящике.
    ctx->CreateModel<PosUVNormal>("two_quads", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
        const float cx[2] = { -1.0f, 1.0f };   // зазор 1.0 между квадами — разрыв виден глазом
        for (int q = 0; q < 2; ++q) {
            const uint32_t vbase = static_cast<uint32_t>(v.size());
            // Квад 1×1 в плоскости XY, нормаль +Z, обход CCW наружу. v-down канон (как quad в
            // Engine.cpp): v=0 у геометрического ВЕРХА, иначе текстура встанет вверх ногами.
            v.push_back({ cx[q] - 0.5f, -0.5f, 0.0f,  0,1,  0,0,1,  1,0,0 });
            v.push_back({ cx[q] + 0.5f, -0.5f, 0.0f,  1,1,  0,0,1,  1,0,0 });
            v.push_back({ cx[q] + 0.5f,  0.5f, 0.0f,  1,0,  0,0,1,  1,0,0 });
            v.push_back({ cx[q] - 0.5f,  0.5f, 0.0f,  0,0,  0,0,1,  1,0,0 });
            i.insert(i.end(), { vbase + 0, vbase + 1, vbase + 2,
                                vbase + 0, vbase + 2, vbase + 3 });
        }
    }, AnchorShift::Keep, ResourceTag::CodeOwned);
}

void SandboxScene::DeleteModels(EngineContext* ctx)
{
    ModelManager* modelManager = ctx->GetModelManager();
    modelManager->DeleteModel(modelManager->ModelIdOf("two_quads"), NameSlot::Release);
}

void SandboxScene::BuildUI(EngineContext* ctx)
{
    // --- UI: декларативное дерево через Yoga (flex-раскладка → энтити). Строим ПОСЛЕ LoadScene, в
    //     активной сцене. Engine::PrepareFunc зовёт ui->Emit каждый кадр (пересчёт по dirty). ---
    FontData* uifont = ctx->GetFontManager()->GetFont("default");
    if (!uifont) return;

    UI_Yoga*      ui    = ctx->GetUIYoga();
    FontManager*  fm    = ctx->GetFontManager();
    // Ассеты узла — по имени (как в Renderable); резолвит их сборка батчей.
    const std::string uimat = "ui_mat";
    const std::string quad  = "ui_quad";

    // Экран: колонка, дети прижаты к низу и по центру по горизонтали.
    UIStyle screen; screen.dir = UIDir::Column; screen.justify = UIJustify::End; screen.align = UIAlign::Center;
    UI_Yoga::Node root = ui->Root(screen);

    // Панель у нижнего края: колонка, внутренний отступ + зазор между строками, по центру.
    UIStyle panelS; panelS.dir = UIDir::Column; panelS.align = UIAlign::Center;
    panelS.padding = 1.0f; panelS.gap = 8.0f; panelS.margin = 160.0f;
    UI_Yoga::Node panel = ui->Box(root, panelS, uimat, quad);

    // Две текстовые строки (intrinsic-размер из метрик шрифта).
    UIStyle textS;
    ui->Text(panel, textS, "Hello U Hello U Hello U Hello\n U Hello U Hello U Hello UI",    uimat, quad, uifont, fm);
    ui->Text(panel, textS, "Yoga layout", uimat, quad, uifont, fm);

    // Кнопка на материале с ДВУМЯ albedo-вариантами (m_hover из манифеста сцены).
    // Переключения пока нет: узел показывает дефолт (вариант 0). Смысл узла — проверка,
    // что вариативный материал в UI-проходе рисуется как обычный.
    // Размер задан в px явно: у Box нет интринсика (в отличие от Text), при Auto он схлопнется.
    UIStyle btnS;
    btnS.wmode = UISize::Points; btnS.w = 256.0f;
    btnS.hmode = UISize::Points; btnS.h = 74.0f;
    ui->Box(panel, btnS, "m_hover", quad);
}
