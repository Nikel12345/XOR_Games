#include "PCH.h"
#include "Game.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "EngineContext.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "CameraManager.h"
#include "TextureManager.h"
#include "MaterialManager.h"
#include "InputManager.h"
#include "LightDataModule.h"
#include "UI_ImGui.h"
#include "TexturesPresets.h"
#include "GameShaderSet.h"
#include "MaterialParams.h"
#include "PositionStructure.h"
#include "UI_DataModule.h"
#include "Engine.h"
// Свой тип params регистрируется отсюда же одной записью, движок для этого не правится:
//   ParamsSpecRegistry::Materials().Register(MakeParamsSpec<MyParams>("MyType", {...}));
// см. ParamsSpec.h
#include "GameComponents.h"   // игровые компоненты: объявление + своя регистрация в реестре
#include "GravityScene.h"
#include "SandboxScene.h"

// Стартовая сцена: имя ОДНО и то же для ECS и для файлов — папка сцены зовётся так же
// (saved_scene/scene1, см. kScenesRoot). Литерал в одном месте: разъедься имя сцены с именем
// папки — LoadScene молча грузил бы пустоту, а генератор навесился бы на чужую сцену.
static const char* const kGravityScene = "scene1M";
static const char* const kSandboxScene = "scene2";
static const char* const kStartScene   = kSandboxScene;

Game::Game(Engine* engine)
{
    this->engine = engine;

	objectManager = engine->GetObjectManager();
	cameraManager = engine->GetCameraManager();
	input = engine->GetInputManager();

	width = engine->GetWindowWidth();
	height = engine->GetWindowHeight();

	ctx = engine->GetEngineContext();
}

SDL_AppResult Game::MainInit()
{
    RegisterGameComponents();   // ДО LoadScene: он резолвит компоненты по имени через реестр
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

    objectManager->CreateScene(kGravityScene);
    ctx->RegisterSceneGenerator(kGravityScene, [this] { GravityScene::CreateModels(ctx); });
    ctx->RegisterSceneDestructor(kGravityScene, [this] { GravityScene::DeleteModels(ctx); });

    objectManager->CreateScene(kSandboxScene);
    ctx->RegisterSceneGenerator(kSandboxScene, [this] { SandboxScene::CreateModels(ctx); });
    ctx->RegisterSceneDestructor(kSandboxScene, [this] { SandboxScene::DeleteModels(ctx); });
    ctx->RegisterSceneGenerator(kSandboxScene, [this] { SandboxScene::CreateDebugColliders(ctx, kSandboxScene); });
    ctx->RegisterSceneGenerator(kSandboxScene, [this] { SandboxScene::BuildUI(ctx); });

    ctx->LoadScene(kStartScene);

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

    gravity.Tick(objectManager);
    UpdateKeyboardControls();

    return SDL_APP_CONTINUE;
}

void Game::UpdateKeyboardControls()
{
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
