#include "PCH.h"
#include <cmath>
#include "Game.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "EngineContext.h"
#include "ObjectManager.h"
#include "CameraManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "MaterialManager.h"
#include "InputManager.h"
#include "ThreadController.h"
#include "LightDataModule.h"
#include "UI_ImGui.h"
#include "TexturesPresets.h"
#include "DefaultShaderSet.h"
#include "GameShaderSet.h"
#include "MaterialParams.h"
#include "BufferManager.h"
#include "DefaultRenderPassSet.h"
#include "PositionStructure.h"
#include "FontManager.h"
#include "UI_DataModule.h"
#include "UI_Yoga.h"
#include "EngineProfiler.h"
#include "Engine.h"
// Свой тип params регистрируется отсюда же одной записью, движок для этого не правится:
//   ParamsSpecRegistry::Materials().Register(MakeParamsSpec<MyParams>("MyType", {...}));
// см. ParamsSpec.h
#include "GameComponents.h"   // игровые компоненты: объявление + своя регистрация в реестре
#include "Colliders.h"
#include "ContactSystem.h"
#include "DebugColliderSystem.h"

// Стартовая сцена: имя ОДНО и то же для ECS и для файлов — папка сцены зовётся так же
// (saved_scene/scene1, см. kScenesRoot). Литерал в одном месте: разъедься имя сцены с именем
// папки — LoadScene молча грузил бы пустоту, а генератор навесился бы на чужую сцену.
static const char* const kStartScene = "scene2";

Game::Game(Engine* engine)
{
    game_state = GameState::MAIN_MENU;
    this->engine = engine;
    this->game_state = GameState::MAIN_MENU;

	textureManager = engine->GetTextureManager();
	modelManager = engine->GetModelManager();
	objectManager = engine->GetObjectManager();
	cameraManager = engine->GetCameraManager();

	threadController = engine->GetThreadController();
	input = engine->GetInputManager();



	width = engine->GetWindowWidth();
	height = engine->GetWindowHeight();

	ctx = engine->GetEngineContext();
}

SDL_AppResult Game::MainInit()
{
    RegisterGameComponents();   // ДО LoadScene: он резолвит компоненты по имени через реестр
    objectManager->CreateScene(kStartScene);
    Camera* camera = cameraManager->CreateCamera(width, height);
    cameraManager->SetActiveCamera(0);

    camera->SetView(
        glm::vec3(2.0f, 0.7f, 3.5f), // позиция камеры
        glm::vec3(0.43f, -0.4f, -0.8f), // точка взгляда
        glm::vec3(0.0f, 1.0f, 0.0f)  // вектор вверх
    );
    TextureAtlas* atlas = ctx->CreateTextureAtlas("albedo_atlas", TexturePresets::AlbedoAtlas(2048, 8, 5), DefaultSamplersNames::DEFAULT_SAMPLER);
	// Мипы обязательны: без них (а) POM-префильтр pomBias — no-op (нет грубых уровней), (б) нормаль
	// не сглаживается (мерцание, см. заметку про red-shift). FullMipLevels включает мип-цепочку.
	TextureAtlas* normal_atlas   = ctx->CreateTextureAtlas("normal_atlas",   TexturePresets::NormalAtlas(2048, 5, TexturePresets::FullMipLevels(2048)),   DefaultSamplersNames::DEFAULT_SAMPLER);
	TextureAtlas* orm_atlas      = ctx->CreateTextureAtlas("orm_atlas",      TexturePresets::ORMAtlas(2048, 5), DefaultSamplersNames::DEFAULT_SAMPLER);
	TextureAtlas* emissive_atlas = ctx->CreateTextureAtlas("emissive_atlas", TexturePresets::EmissiveAtlas(1024, 2), DefaultSamplersNames::DEFAULT_SAMPLER);


	// --- Шрифт: растеризуется в общий TextAtlas (CWD=src/game → путь fonts/…). ---
	ctx->CreateFont("default", "fonts/cuyabra-Regular.otf", 48.0f);

    {
        // Движковый набор compute-программ (DefaultShaderSet.h в движке).
        using namespace DefaultShaderProgramSet;
        SetBloomPrograms(ctx);
        SetAOPrograms(ctx);           // SSAO: глубина main'а → карта AO → вычитание из scene_hdr
        SetFogProgram(ctx);           // туман: линейная стена по дистанции, поверх глубины main'а
    }
    // Push/dispatch своих sp — ДО первого LoadScene: реестр ShaderManager вешает их на sp сам.
    GameShaderSet::RegisterShaderFuncs(ctx);

	// UI-материал (тип UI: bg/text цвета, albedo=default_albedo). Программу "UI" создаёт движок
	// (InitDefaultShaders) — к MainInit она уже есть.
	{
		Material* ui_mat = ctx->CreateMaterial("ui_mat",
			{ { TextureSlotRole::Albedo, { "default_albedo" } } }, { "UI" }, ResourceTag::CodeOwned);
		// Посадку/размер теперь считает Yoga (rect узла), поэтому text_height/anchor нейтральны
		// (1,0) — шейдер просто заливает узел текстом. Цвета: тёмный фон + золотой текст.
		ctx->SetMaterialParams(ui_mat, "UI", UIMaterialParams{ { 0.10f, 0.10f, 0.15f, 1.0f }, { 1.0f, 0.85f, 0.2f, 1.0f }, 1.0f, 0.0f });
	}

    ModelData* sphere = (*ctx->GetModelManager())["sphere"];

    debug_box_model = ctx->CreateModel<PosUVNormal>("debug_box", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
        auto P = [](float x, float y, float z) {
            PosUVNormal vert{}; vert.x = x; vert.y = y; vert.z = z; return vert;
        };
        v = {
            P(-1,-1,-1), P(1,-1,-1), P(1, 1,-1), P(-1, 1,-1),   // 0..3  z=-1
            P(-1,-1, 1), P(1,-1, 1), P(1, 1, 1), P(-1, 1, 1),   // 4..7  z=+1
        };
        i = {
            0,1, 1,2, 2,3, 3,0,   // рёбра грани z=-1
            4,5, 5,6, 6,7, 7,4,   // рёбра грани z=+1
            0,4, 1,5, 2,6, 3,7,   // рёбра вдоль z
        };
    });

    debug_sphere_model = ctx->CreateModel<PosUVNormal>("debug_sphere", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& idx) {
        const uint32_t stacks = 8, slices = 12;
        const float PI = 3.14159265358979323846f;
        for (uint32_t i = 0; i <= stacks; ++i) {
            float phi = PI * (float)i / (float)stacks;
            float cp = std::cos(phi), sp = std::sin(phi);
            for (uint32_t j = 0; j <= slices; ++j) {
                float theta = 2.0f * PI * (float)j / (float)slices;
                PosUVNormal vert{};
                vert.x = sp * std::cos(theta); vert.y = cp; vert.z = sp * std::sin(theta);
                v.push_back(vert);
            }
        }
        const uint32_t row = slices + 1;
        // параллели: ребро a -> a+1 вдоль каждого ряда
        for (uint32_t i = 0; i <= stacks; ++i)
            for (uint32_t j = 0; j < slices; ++j) {
                uint32_t a = i * row + j;
                idx.push_back(a); idx.push_back(a + 1);
            }
        // меридианы: ребро a -> a+row между рядами
        for (uint32_t i = 0; i < stacks; ++i)
            for (uint32_t j = 0; j <= slices; ++j) {
                uint32_t a = i * row + j;
                idx.push_back(a); idx.push_back(a + row);
            }
    });

    // --- Процедурные параллелепипеды "cube_0".."cube_(N-1)" для сцены из scene_gen.py ---
    // ВАЖНО: kCubeVariants должен совпадать с NUM_CUBE_MODELS в scripts/scene_gen.py —
    // питон-скрипт раздаёт сущностям имена именно из этого диапазона.
    const int kCubeVariants = 12;
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

    ctx->RegisterGenerator(kStartScene, [this] { CreateDebugColliders(); });
    ctx->LoadScene(kStartScene);   // папка сцены saved_scene/scene1 (scene.json + ресурсы внутри)

    debug_collider_material = ctx->GetMaterialManager()->GetMaterial("debug_collider");

    // --- UI: декларативное дерево через Yoga (flex-раскладка → энтити). Строим ПОСЛЕ LoadScene, в
    //     активной сцене. Engine::PrepareFunc зовёт ui->Emit каждый кадр (пересчёт по dirty). ---
    if (FontData* uifont = ctx->GetFontManager()->GetFont("default")) {
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

    {
        MaterialManager* mm = ctx->GetMaterialManager();
    }
	//ctx->ExecuteGenerators();   // CreateDebugColliders
    ChangeState(GameState::MAIN_MENU);
    return SDL_APP_CONTINUE;
}

// Наведение на UI. Дерево Yoga раскладывает узлы в NDC и кладёт рект прямо в Positions
// (центрированный юнит-квад разложен матрицей: диагональ = масштаб, 4-й столбец = центр, см. UI_Yoga::Emit),
// поэтому проверка попадания — это сравнение курсора с [w - x/2, w + x/2] x [d - b/2, d + b/2], без обратной
// математики и без обращения к раскладке.
//
// Курсор нормируем ОКНОМ, а не render-разрешением: рект узла уже в NDC (Emit поделил на своё),
// а картинка растягивается на окно — NDC у них общий, и расхождение render/window сюда не течёт.
//
// Состояния переписываются КАЖДЫЙ кадр, и это не расточительство: буфер состояний и так
// заливается целиком каждый кадр, а запись нуля стирает запись — не-наведённые узлы из буфера
// уходят сами. Зовётся с sim-потока, поэтому пишем через EngineContext напрямую, без команды
// (команда — это вход для UI-потока, у неё аллокация и кадр задержки).
void Game::UpdateUIHover()
{
    SceneData* scene = objectManager->GetActiveScene();
    if (!scene) return;
    const float ww = engine->GetWindowWidth(), wh = engine->GetWindowHeight();
    if (ww <= 0.0f || wh <= 0.0f) return;

    const float nx = 2.0f * input->MouseX() / ww - 1.0f;
    const float ny = 1.0f - 2.0f * input->MouseY() / wh;   // y вниз в окне → вверх в NDC

    MaterialManager* mm = ctx->GetMaterialManager();
    objectManager->ForEach<Positions, UIComponent, Renderable>(scene,
        [&](Entity e, SoAElement<Positions> pos, UIComponent&, SoAElement<Renderable> rend)
    {
        const std::vector<MaterialSlot>& parts = rend.container().materials[rend.i()];
        Positions& P = pos.container();
        const size_t i = pos.i();
        const float x0 = P.w[i] - 0.5f * P.x[i], x1 = P.w[i] + 0.5f * P.x[i];
        const float y0 = P.d[i] - 0.5f * P.b[i], y1 = P.d[i] + 0.5f * P.b[i];
        const bool hit = (nx >= x0 && nx <= x1 && ny >= y0 && ny <= y1);

        for (uint32_t k = 0; k < parts.size(); ++k) {
            // Узлы на невариативном материале (текст, фон панели) пропускаем: писать им состояние
            // значило бы затащить их в буфер состояний ради значения, которое шейдер всё равно
            // сожмёт клампом в дефолт.
            const Material* mat = mm->GetMaterial(parts[k].per_lod[0]);
            if (!mat) continue;
            auto tit = mat->textures.find(TextureSlotRole::Albedo);
            if (tit == mat->textures.end() || tit->second.size() < 2) continue;

            ctx->SetEntityTextureVariant(e, k, TextureSlotRole::Albedo, hit ? 1u : 0u);
        }
    });
}

SDL_AppResult Game::MainIterate()
{
    UpdateUIHover();
    input->DrainKeyEvents(key_events_scratch);
    for (const InputManager::KeyEvent& e : key_events_scratch) {
        if (!e.down) continue;
        switch (e.scancode) {
        case SDL_SCANCODE_ESCAPE:
            // Выход / меню
            break;
        default:
            break;
        }
    }

    Camera* camera = cameraManager->GetActiveCamera();
    // Снимаем один раз на тик: внутри тика ответ редактора не меняется (его кадр идёт на
    // рендер-потоке), а игра так не зависит от того, есть ли редактор в сборке вообще.
    const bool ui_mouse = UI_ImGui::WantCaptureMouse();

    float wheel = input->ConsumeWheelDelta();
    if (wheel != 0.0f && !ui_mouse) {
        camera->SpeedChange(wheel);   // щелчки колеса; шаг мультипликативный (см. Camera::SPEED_STEP)
    }
    mouse_x = input->MouseX();
    mouse_y = input->MouseY();
    bool rotate = input->IsMouseButtonDown(SDL_BUTTON_LEFT) && !ui_mouse;
    camera->RotateView(mouse_x, mouse_y, rotate);

    input->ExecuteCommands(ctx);

    this->MainMenu_Update();

    switch (game_state) {
    case GameState::MAIN_MENU:
        MainMenu_Iterate();
        break;
    }

    // Энтити с ColliderComponent берут явный радиус; остальные с Renderable — модельную сферу.

    return SDL_APP_CONTINUE;
}

void Game::CreateDebugColliders()
{
    SceneData* scene = objectManager->GetActiveScene();
    if (!scene) return;
    if (!debug_collider_material || !debug_box_model || !debug_sphere_model) return;

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
        ctx->CreateEntity(kStartScene,
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

void Game::SDL_AppQuit()
{
    ChangeState(GameState::MAIN_MENU);
    MainMenu_Quit();
}

void Game::ChangeState(GameState newState)
{
    bool skipQuit = ((game_state == GameState::MAIN_MENU && newState == GameState::SETTINGS) or (newState == GameState::MAIN_MENU && game_state == GameState::SETTINGS));
    bool skipInit = skipQuit;

    if (!skipQuit && newState != game_state) {
        switch (game_state) {
        case GameState::MAIN_MENU:
            MainMenu_Quit();
            break;
        default:
            break;
        }
    }

    game_state = newState;

    if (!skipInit) {
        switch (game_state) {
        case GameState::MAIN_MENU:
            MainMenu_Init();
            break;
        default:
            break;
        }
    }
}

void Game::MainMenu_Init()
{
}

void Game::MainMenu_Iterate()
{
}

void Game::MainMenu_Update()
{
    SimulateGravity();   // симуляция идёт всегда, до раннего выхода по WantCaptureMouse
    ReturnJets();

    if (UI_ImGui::WantCaptureMouse()) return;

    const float camSpeed = 0.05f;
    const float lightSpeed = 0.1f;

    glm::vec3 camMove(0.0f);     // x: лево/право, y: верх/низ, z: вперёд/назад
    glm::vec3 lightMove(0.0f);   // x: P.w, y: P.h, z: P.d
    bool camMoved = false;
    bool lightMoved = false;

    // Перебираем зажатые клавиши switch'ем, накапливая дельты.
    input->SnapshotHeldKeys(held_keys_scratch);
    for (SDL_Scancode sc : held_keys_scratch) {
        switch (sc) {
        case SDL_SCANCODE_LEFT:   camMove.x -= camSpeed; camMoved = true; break;
        case SDL_SCANCODE_RIGHT:  camMove.x += camSpeed; camMoved = true; break;
        case SDL_SCANCODE_UP:     camMove.z += camSpeed; camMoved = true; break;
        case SDL_SCANCODE_DOWN:   camMove.z -= camSpeed; camMoved = true; break;
        case SDL_SCANCODE_SPACE:  camMove.y += camSpeed; camMoved = true; break;
        case SDL_SCANCODE_LSHIFT: camMove.y -= camSpeed; camMoved = true; break;

        case SDL_SCANCODE_A: lightMove.x -= lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_D: lightMove.x += lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_W: lightMove.y += lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_S: lightMove.y -= lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_E: lightMove.z += lightSpeed; lightMoved = true; break;
        case SDL_SCANCODE_Q: lightMove.z -= lightSpeed; lightMoved = true; break;
        default: break;
        }
    }

    // Применяем накопленное по одному разу.
    if (camMoved) cameraManager->GetActiveCamera()->Move(camMove);

    if (lightMoved) {
        SceneData* scene = objectManager->GetActiveScene();
        objectManager->ForEach<Positions, SpotLightComponent>(scene,
            [lightMove](SoAElement<Positions> pos_el, SpotLightComponent&)
        {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
            P.w[i] += lightMove.x; P.h[i] += lightMove.y; P.d[i] += lightMove.z;
        });
        objectManager->ForEach<Positions, SphereLightComponent>(scene,
            [lightMove](SoAElement<Positions> pos_el, SphereLightComponent&)
        {
            Positions& P = pos_el.container();
            size_t i = pos_el.i();
            P.w[i] += lightMove.x; P.h[i] += lightMove.y; P.d[i] += lightMove.z;
        });
    }
}

// Гравитация центров, ЗАДАННЫХ СЦЕНОЙ: притягивает не безымянная константа, а сущность с
// GravityComponent — центр там, где её Transform, сила = её gm (см. BaseComponents.h). Центров
// может быть несколько, ускорения складываются; ни одного — кубы летят по инерции.
// gm ОБЯЗАН равняться GRAVITY_CONST * CENTRAL_MASS из scripts/scene_gen/scene_gen.py
// (сейчас 1 * 5000 = 5000): скорость круговой орбиты sqrt(GM/r) считает генератор — при другом gm
// орбиты станут эллиптичными/раскрутятся. Форму орбиты задаёт ТОЛЬКО gm.
// kSimDt — «скорость времени»: не влияет на форму орбиты, только на темп. У движка нет
// delta-time (камера/свет — тоже по кадрам), поэтому шаг фиксированный. При радиусах 50..350
// орбиты долгие — увеличивай kSimDt (или уменьшай радиусы диска в scene_gen.py), чтобы вращение
// было заметным; слишком большой шаг добавит прецессию/дрожание орбиты.
// Гравитация ОБЪЁМНАЯ: ускорение считается по полному радиусу |(x,y,z)| и меняет все три
// компоненты скорости. Раньше она была плоской (XZ-радиус, только vx/vz) — под секции с
// заметной высотой это не годится: генератор ставит кубу скорость круговой орбиты по ПОЛНОМУ
// радиусу, её плоскость наклонена, и без y-составляющей ускорения куб просто улетал бы по
// прямой вверх/вниз от своего кольца. Плата — диск с ненулевой высотой живёт своей жизнью:
// кубы качаются через y == 0, как и положено наклонным орбитам.
static constexpr float kSimDt    = 0.05f;
static constexpr float kGravSoft = 1e-3f;   // защита от деления на ~0 у самого центра
static constexpr float kGravSoft2 = kGravSoft * kGravSoft;   // зажим r2 значением, без ветки

// Дистанция от своего центра, дальше которой куб джета возвращается в центр. ОБЯЗАНА совпадать
// с JET_RANGE в scripts/scene_gen/scene_gen2.py: генератор раскладывает стартовые позиции по всей
// этой длине, и при другой дистанции часть кубов вернётся разом — струя пойдёт волной.
static constexpr float kJetReturnDistance = 2500.0f;
// Материал джета по уровням модели (materials.json сцены): модель, quad (LOD_Quad), точка (LOD_Splat).
static const char* const kJetMaterials[] = { "jet", "jet_lod", "jet_splat" };

void Game::SimulateGravity()
{
    PROF_SCOPE(Sim, "  simulate_gravity (Game)");
    SceneData* scene = objectManager->GetActiveScene();
    if (!scene) return;

    // Проход 1 — центры. Их единицы, и снимаем мы их ОДИН раз: иначе обход миллиона кубов
    // пришлось бы делать по разу на центр. Сам центр симуляция не двигает — он стоит там, куда
    // его поставила сцена (или редактор).
    gravity_sources.clear();
    objectManager->ForEach<Positions, GravityComponent>(scene,
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
    objectManager->ForEachArchetype<Positions, Velocities>(scene,
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

        // Состав компонентов у всех сущностей архетипа один, поэтому джет ли это — решает первая.
        const bool attracted = !objectManager->Has<JetComponent>(scene, ents[0]);

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
                vx[i] += dx * k * kSimDt;
                vy[i] += dy * k * kSimDt;
                vz[i] += dz * k * kSimDt;
            }
        }

        // Позиции (wdh) скоростями (xyz)
        VEC_HOT("integrate_positions");
        for (int i = 0; i < n; ++i) {
            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

void Game::ReturnJets()
{
    PROF_SCOPE(Sim, "  return_jets (Game)");
    SceneData* scene = objectManager->GetActiveScene();
    if (!scene || gravity_sources.empty()) return;

    // Центр и его объекты связаны ключом: объекты центра — джеты с center == его id. Отсюда и
    // форма обхода — [центры][их объекты]: внешний цикл по центрам, внутренний выбирает объекты
    // своего центра. Скорость не трогаем: вернувшийся куб снова летит тем же курсом.
    constexpr float kReturn2 = kJetReturnDistance * kJetReturnDistance;
    objectManager->ForEachArchetype<Positions, JetComponent>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<JetComponent>* jet_arr)
    {
        Positions& P = pos_arr->data;
        const std::vector<JetComponent>& J = jet_arr->data;
        const size_t n = J.size();
        for (const GravitySource& c : gravity_sources) {
            for (size_t i = 0; i < n; ++i) {
                if (J[i].center != c.id) continue;
                const float dx = P.w[i] - c.x, dy = P.d[i] - c.y, dz = P.h[i] - c.z;
                if (dx * dx + dy * dy + dz * dz <= kReturn2) continue;
                P.w[i] = c.x;
                P.d[i] = c.y;
                P.h[i] = c.z;
            }
        }
    });
}

void Game::MainMenu_Event(SDL_Event* event)
{
}


void Game::MainMenu_Quit()
{
}
