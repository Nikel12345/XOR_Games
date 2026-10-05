#include "PCH.h"
#include "BaseComponents.h"
#include "MyGame.h"
// Engine.h теперь только forward-декларации — полные типы тянет этот TU.
#include "EngineContext.h"
#include "ObjectManager.h"
#include "CameraManager.h"
#include "TextureManager.h"
#include "ModelManager.h"
#include "InputManager.h"
#include "ThreadController.h"
#include "LightDataModule.h"
#include "FractalShaderSet.h"
#include "BufferManager.h"
#include "FractalUpdateSet.h"
#include "MaterialManager.h"
#include "ModelData.h"
#include "PositionStructure.h"
#include "TexturesPresets.h"
#include "UI_ImGui.h"

MyGame::MyGame(Engine* engine)
{
    this->engine = engine;

    textureManager = engine->GetTextureManager();
    modelManager   = engine->GetModelManager();
    objectManager  = engine->GetObjectManager();
    cameraManager  = engine->GetCameraManager();

    threadController = engine->GetThreadController();
    input            = engine->GetInputManager();

    width  = engine->GetWindowWidth();
    height = engine->GetWindowHeight();

    ctx = engine->GetEngineContext();
}

SDL_AppResult MyGame::MainInit()
{
    Camera* camera = cameraManager->CreateCamera(width, height);
    cameraManager->SetActiveCamera(0);
    camera->SetView(
        glm::vec3(2.2f, 1.4f, 3.2f), // позиция камеры
        // Взгляд на центр губки Менгера (ноль мира, полуразмер 1); вращение — ЛКМ,
        // приближение к поверхности раскрывает уровни детализации адаптивно.
        glm::vec3(-0.533f, -0.339f, -0.775f),
        glm::vec3(0.0f, 1.0f, 0.0f)  // вектор вверх
    );

    // Сцена = выбор фрактала. Всё сценозависимое — буфер кадра (CreateBufferData) и его
    // апдейтер (FractalUpdateSet) — под if'ом с её именем (пуши шейдеров уже нет: она
    // находит свой sp по имени сама, см. RegisterShaderFuncs). Буфер — ДО LoadScene: sp из манифеста сцены ссылается на него по имени,
    // резолв идёт по уже существующим. Рендер-ресурсы (vs/fs/sp/материал) — в манифестах.
    const std::string scene_name = "scene_fractal";
    BufferManager* bm = ctx->GetBufferManager();
    if (scene_name == "scene_fractal") {
        // Без usage: GRAPHICS_STORAGE_READ выведется из sp манифеста, который назовёт этот буфер.
        bm->CreateBufferData(FractalUpdateSet::MENGER_FRAME_BUFFER,
            FractalUpdateSet::MENGER_FRAME_BYTES, BufferDataType::Dynamic);
        FractalUpdateSet::SetMengerFrameUpdater(ctx);
        // Атлас под альбедо якорённого куба — кодовая инфраструктура (как в Game.cpp), ДО
        // LoadScene: текстура iron из textures.json садится в него при загрузке. Роли
        // Normal/ORM/Emissive берут движковые default_* из FallbackAtlas — свой атлас не нужен.
        ctx->CreateTextureAtlas("albedo_atlas",
            TexturePresets::AlbedoAtlas(2048, 1, TexturePresets::FullMipLevels(2048)),
            DefaultSamplersNames::DEFAULT_SAMPLER);
    }
    else if (scene_name == "scene_mandelbrot") {
        bm->CreateBufferData(FractalUpdateSet::MANDELBROT_ORBIT_BUFFER,
            FractalUpdateSet::MANDELBROT_ORBIT_BYTES, BufferDataType::Dynamic);
        FractalUpdateSet::SetMandelbrotOrbitUpdater(ctx);
    }

    // Push-константы sp — ИНСТРУКЦИИ ПО ИМЕНИ в реестре ShaderManager (как resize-инструкции
    // атласов): регистрируем ДО первого LoadScene, а вешает их на sp сам движок — и на создании
    // программы, и общим проходом в конце каждой загрузки. Имя сцены сюда не идёт: функции
    // фракталов названы по своим sp и находят их сами.
    FractalShaderSet::RegisterShaderFuncs(ctx);

    objectManager->CreateScene(scene_name);
    ctx->LoadScene(scene_name);   // папка сцены saved_scene/<имя> (scene.sheaf + ресурсы)

    // ── Якорённые кубы (этапы 3-4). Модель — процедурный куб полу-размера 1, CodeOwned;
    // материал/sp — из манифестов сцены (iron_block/AnchorObject: штатный main-суржейс — свет
    // в сцене нулевой, куб живёт на полу AMBIENT_LIGHT). Модель и материал захватываются в
    // члены: клавиша N в MainIterate спавнит из них новые кубы. Все кубы — кодовые энтити с
    // GeneratedComponent (SaveScene не пишет) + FractalAnchorComponent (позиция во фрактале;
    // движок её не трогает, правит только гизмо через съём правки); их Positions переписывает
    // каждый тик общий ForEach-контур в MainIterate — стартовый ноль живёт меньше тика. ──
    fractal_scene = (scene_name == "scene_fractal");
    if (fractal_scene) {
        // Туман губки насыщается на 2500σ ≤ 7500 юнитов якоря — far-плоскость должна быть
        // ДАЛЬШЕ, иначе якорённые объекты режутся клипом на 5000 раньше, чем растворяются
        // туманом (поп у горизонта). Сама губка far не знает — она рэймарч на z=w.
        camera->SetPlanes(0.01f, 8000.0f);

        // Куб — движковый примитив (Engine.cpp, рядом с quad/sphere), берём по имени. Раньше здесь
        // жила его копия-генератор; вынесена в движок, чтобы канон развёртки (v-down) был один.
        // Тест-куб этапа 3 — теперь просто первый якорённый объект: корневой размер (σ=1,
        // глубина 0), вплотную справа от губки. Дальше им занимается общий контур этапа 4.
        FractalUpdateSet::FractalPos root_pos;
        root_pos.local = glm::dvec3(3.0, 0.0, 0.0);
        ctx->CreateEntity(scene_name,
            RenderableProxy::Single(ctx->GetModelManager()->InternModel(kAnchorModel),
                                    { ctx->GetMaterialManager()->InternMaterial(kAnchorMaterial) }),
            PositionProxy16{ 0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,1 },
            GeneratedComponent{},
            FractalUpdateSet::FractalAnchorComponent{ root_pos });
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult MyGame::MainIterate()
{
    // Дискретные события: B — дебаг-закладка позиции во фрактале (этап 2 переноса объектов:
    // апдейтер губки раз в секунду логирует смещение до неё — живая проверка разности адресов;
    // в сцене Мандельброта апдейтер губки не зарегистрирован — вызов безвреден). Остальной
    // ввод — непрерывный, на удержании ниже.
    input->DrainKeyEvents(key_events_scratch);

    Camera* camera = cameraManager->GetActiveCamera();
    // Снимаем один раз на тик: внутри тика ответ редактора не меняется (его кадр идёт на
    // рендер-потоке), а игра так не зависит от того, есть ли редактор в сборке вообще.
    const bool ui_mouse    = UI_ImGui::WantCaptureMouse();
    const bool ui_keyboard = UI_ImGui::WantCaptureKeyboard();

    float wheel = input->ConsumeWheelDelta();
    if (wheel != 0.0f && !ui_mouse) {
        camera->SpeedChange(wheel);   // щелчки колеса; шаг мультипликативный (см. Camera::SPEED_STEP)
    }
    mouse_x = input->MouseX();
    mouse_y = input->MouseY();
    bool rotate = input->IsMouseButtonDown(SDL_BUTTON_LEFT) && !ui_mouse;
    camera->RotateView(mouse_x, mouse_y, rotate);

    input->ExecuteCommands(ctx);

    // Полёт: стрелки — горизонталь/вперёд, Space/LShift — вертикаль. Камера — аккумулятор
    // сдвига в ПОСТОЯННЫХ игровых юнитах: мировой масштаб (АВТО: следует за расстоянием до
    // поверхности) применяет апдейтер FractalUpdateSet при внесении дельты. «Зум» = лететь к грани:
    // подлёт сам замедляется и раскрывает детализацию, врезаться нельзя.
    // Колесо (SpeedChange камеры) — множитель скорости поверх; удержание I/K — ручной сдвиг
    // окна масштаба относительно авто (~1 уровень в секунду, I — мельче, K — крупнее).
    if (!ui_mouse) {
        const float camSpeed = 1.0f;           // игровые юниты за тик (полное отклонение)
        const float zoomRate = 1.0f / 60.0f;   // уровней за тик удержания (при 60 UPS = 1 ур/с)
        glm::vec3 camMove(0.0f);   // x: лево/право, y: верх/низ, z: вперёд/назад
        bool camMoved = false;

        input->SnapshotHeldKeys(held_keys_scratch);
        for (SDL_Scancode sc : held_keys_scratch) {
            switch (sc) {
            case SDL_SCANCODE_LEFT:   camMove.x -= camSpeed; camMoved = true; break;
            case SDL_SCANCODE_RIGHT:  camMove.x += camSpeed; camMoved = true; break;
            case SDL_SCANCODE_UP:     camMove.z += camSpeed; camMoved = true; break;
            case SDL_SCANCODE_DOWN:   camMove.z -= camSpeed; camMoved = true; break;
            case SDL_SCANCODE_SPACE:  camMove.y += camSpeed; camMoved = true; break;
            case SDL_SCANCODE_LSHIFT: camMove.y -= camSpeed; camMoved = true; break;

            case SDL_SCANCODE_I: FractalUpdateSet::FractalScaleStep(+zoomRate); break;  // глубже
            case SDL_SCANCODE_K: FractalUpdateSet::FractalScaleStep(-zoomRate); break;  // наружу
            default: break;
            }
        }
        if (camMoved) camera->Move(camMove);
    }

    // ── Губка: тик позиции/масштаба (снимает дельту камеры, обнуляет аккумулятор) — СТРОГО
    // после внесения ввода и ДО prepare (контракт MengerTick). Затем спавн по N и матрицы
    // всех якорённых объектов от свежей позиции — их подхватит StoreTransforms этого тика. ──
    if (fractal_scene) {
        FractalUpdateSet::MengerTick(cameraManager);

        // Дискретные клавиши якорей. N — «поставить куб здесь»: якорь = текущая ячейка
        // камеры, центр — перед камерой на 1.5σ, размер σ/2 (авто-масштаб держит стенку на
        // ~2σ: куб виден целиком и почти не тонет в ней); дальше позицию меняют гизмо/G.
        // G — взять выбранный куб / отпустить (этап 8b, HUD-вариант): на время переноски куб
        // ВЫКЛЮЧЕН из мира и рисуется в фиксированной части экрана всегда одинаково (HUD-поза
        // в ForEach-контуре ниже); выбор ЗАБИРАЕТСЯ у UI — гизмо гаснет само. Отпуск кладёт
        // куб в мир заново спавн-позой — личная логика масштаба доводит размер под окружение.
        if (!ui_keyboard) {
            for (const InputManager::KeyEvent& e : key_events_scratch) {
                if (!e.down) continue;
                if (e.scancode == SDL_SCANCODE_N) {
                    const FractalUpdateSet::FractalPos& cam = FractalUpdateSet::MengerCameraPos();
                    FractalUpdateSet::FractalPos p = cam;
                    p.local += glm::dvec3(camera->GetForward()) * (1.5 * cam.sigma);
                    p.sigma  = 0.5 * cam.sigma;
                    ctx->CreateEntity("scene_fractal",
                        RenderableProxy::Single(ctx->GetModelManager()->InternModel(kAnchorModel),
                                                { ctx->GetMaterialManager()->InternMaterial(kAnchorMaterial) }),
                        PositionProxy16{ 0,0,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,1 },
                        GeneratedComponent{},
                        FractalUpdateSet::FractalAnchorComponent{ std::move(p) });
                    SDL_Log("Anchored cube spawned: depth=%d sigma=%.4f",
                        (int)cam.addr.size(), 0.5 * cam.sigma);
                }
                else if (e.scancode == SDL_SCANCODE_G) {
                    SceneData* scene = objectManager->GetActiveScene();
                    if (!carrying) {
                        const std::vector<uint32_t> sel = UI_ImGui::GetSelectedEntities();
                        if (!sel.empty()) {
                            if (objectManager->Has<FractalUpdateSet::FractalAnchorComponent>(scene, sel.front())) {
                                // Взятие «в руку»: мировая поза куба с этого момента не имеет
                                // смысла (не читается) — контур рисует его HUD-позой. Свои
                                // ориентация и пропорции (rot/model_scale) сохраняются.
                                carried  = sel.front();
                                carrying = true;
                                SDL_Log("Carry: grabbed entity %u", carried);
                            }
                            else UI_ImGui::SetSelectedEntities(sel);   // не якорёный (скайбокс) — вернуть выбор
                        }
                    }
                    else {
                        // Отпуск: куб появляется в мире заново СПАВН-позой (1.5σ перед
                        // камерой, стартовый масштаб σ/2) — дальше его личная логика сама
                        // пересчитает масштаб под окружение. Удалённый энтити — просто бросок.
                        if (objectManager->Has<FractalUpdateSet::FractalAnchorComponent>(scene, carried)) {
                            FractalUpdateSet::FractalAnchorComponent& a =
                                objectManager->GetComponent<FractalUpdateSet::FractalAnchorComponent>(scene, carried);
                            const FractalUpdateSet::FractalPos& cam = FractalUpdateSet::MengerCameraPos();
                            a.pos = cam;
                            a.pos.local += glm::dvec3(camera->GetForward()) * (1.5 * cam.sigma);
                            a.pos.sigma  = 0.5 * cam.sigma;
                        }
                        carrying = false;
                        UI_ImGui::SetSelectedEntities({ carried });   // выбор обратно — гизмо оживает
                        SDL_Log("Carry: dropped entity %u at depth=%d", carried,
                            (int)FractalUpdateSet::MengerCameraPos().addr.size());
                    }
                }
            }
        }

        // Матрицы ВСЕХ якорённых объектов: разность адресов с камерой → float-матрица
        // (масштаб+сдвиг, без поворота) прямо в Positions. Отсев (этап 5) — в double, ДО
        // float-каста: (а) целиком за туманом — дальше насыщения (MengerFogFar) не видно
        // ничего, горизонт общий с губкой; (б) субпиксель — угловой размер меньше ~полпикселя
        // (K_PX шейдера 8e-4); (в) крупнее ~12 уровней камеры — float-матрица начинает
        // дрожать (ulp ∝ scale), а губка и сама прячет структуру крупнее K=8 предков туманом:
        // горизонты согласованы. Вырожденный ноль → инстанс отсекается куллингом, без
        // inf/NaN в буфере. Свежеспавненный куб попадает в этот же проход.
        const double fog_far = FractalUpdateSet::MengerFogFar();
        const glm::mat3 view_rot(camera->GetView());   // для HUD-позы несомого (оси взгляда)
        objectManager->ForEach<FractalUpdateSet::FractalAnchorComponent, Renderable, Positions>(
            objectManager->GetActiveScene(),
            [this, fog_far, view_rot](Entity e, FractalUpdateSet::FractalAnchorComponent& a,
                                      SoAElement<Renderable> rend, SoAElement<Positions> el)
        {
            Positions& P = el.container();
            const size_t i = el.i();

            // Несомый куб НЕ существует в мире: рисуется HUD-позой в фиксированной части
            // экрана, всегда одинаково (константы в осях взгляда; экранные координаты и есть
            // юниты якоря — камера растра в нуле кадра). Мировая поза не читается вовсе;
            // ориентация фиксирована к экрану (¾-ракурс), пропорции model_scale сохраняются
            // нормированными на максимум. written_fs=0 → съём правки по HUD-матрице не
            // сработает никогда (в т.ч. первым тиком после отпуска).
            if (carrying && e == carried) {
                const glm::vec3 hud_pos(0.55f, -0.38f, -1.6f);   // право-низ-перед, оси взгляда
                const glm::quat hud_rot(glm::vec3(glm::radians(24.0f), glm::radians(-36.0f), 0.0f));
                const double    mmax = std::max(a.model_scale.x,
                                       std::max(a.model_scale.y, a.model_scale.z));
                const glm::vec3 cs = glm::vec3(a.model_scale / mmax) * 0.16f;   // экранный полу-размер

                const glm::mat3 W = glm::transpose(view_rot);    // оси взгляда → мир/якорь
                const glm::mat3 R = W * glm::mat3_cast(hud_rot);
                const glm::mat3 m(R[0] * cs.x, R[1] * cs.y, R[2] * cs.z);
                const glm::vec3 t = W * hud_pos;
                P.x[i] = m[0].x; P.y[i] = m[1].x; P.z[i] = m[2].x; P.w[i] = t.x;
                P.a[i] = m[0].y; P.b[i] = m[1].y; P.c[i] = m[2].y; P.d[i] = t.y;
                P.e[i] = m[0].z; P.f[i] = m[1].z; P.g[i] = m[2].z; P.h[i] = t.z;
                P.i[i] = 0.0f;   P.j[i] = 0.0f;   P.k[i] = 0.0f;   P.l[i] = 1.0f;
                a.written_fs = 0.0f;
                return;
            }

            // (7/8) Съём правки гизмо: SetTransformCmd исполнен в ExecuteCommands выше по
            // тику, и Positions мог разойтись с кэшем прошлой записи контура. Совпадение
            // бит-в-бит → пропуск (никакого дрейфа декомпозиции в покое). Правка:
            //   трансляция — ДЕЛЬТОЙ (она камерно-относительна): юниты якоря камеры прошлого
            //     тика → юниты якоря объекта через written_fs (σ_объ/written_fs = 3^{Δd});
            //   линейная часть — АБСОЛЮТОМ (ориентация и масштаб от кадра не зависят):
            //     нормы столбцов/written_fs → model_scale, нормированные столбцы → rot.
            if (a.written_fs > 0.0f) {
                const glm::mat3 m_now(
                    glm::vec3(P.x[i], P.a[i], P.e[i]),    // столбцы линейной части
                    glm::vec3(P.y[i], P.b[i], P.f[i]),    // (Positions хранит строки)
                    glm::vec3(P.z[i], P.c[i], P.g[i]));
                const glm::vec3 t_now(P.w[i], P.d[i], P.h[i]);
                if (m_now != a.written_m || t_now != a.written_t) {
                    const glm::dvec3 dt = glm::dvec3(t_now) - glm::dvec3(a.written_t);
                    a.pos.local += dt * (a.pos.sigma / (double)a.written_fs);

                    const double n0 = glm::length(glm::dvec3(m_now[0]));
                    const double n1 = glm::length(glm::dvec3(m_now[1]));
                    const double n2 = glm::length(glm::dvec3(m_now[2]));
                    if (n0 > 1e-30 && n1 > 1e-30 && n2 > 1e-30) {   // вырожденную правку не берём
                        a.model_scale = glm::dvec3(n0, n1, n2) / (double)a.written_fs;
                        a.rot = glm::normalize(glm::quat_cast(glm::mat3(
                            m_now[0] / (float)n0, m_now[1] / (float)n1, m_now[2] / (float)n2)));
                    }
                }
            }

            // Радиус тела — из bound-сфер модели (union сабмеш-сфер вокруг origin, юниты
            // модели): контур универсален для ЛЮБОЙ якорёной модели, не только куба (у куба
            // получается прежний √3). Пер-осевое растяжение гизмо в отсев входит максимумом
            // (консервативно), а в правило размера — НЕ входит (см. MengerObjectScaleTick).
            // Модель у энтити — имя, поэтому ищем её в словаре: якорей десятки, поиск на тик копеечный.
            const ModelData* model = modelManager->FindModel(rend.container().model[rend.i()]);
            double r_model = 0.0;
            if (model)
                for (const SubMeshData& sm : model->submeshes)
                    r_model = std::max(r_model,
                        (double)glm::length(glm::vec3(sm.sphere)) + (double)sm.sphere.w);
            if (r_model <= 0.0) r_model = 1.0;   // пустая/вырожденная модель — нейтральный радиус
            const double ms_max = std::max(a.model_scale.x,
                                  std::max(a.model_scale.y, a.model_scale.z));

            // Личная логика масштаба — ЖИВАЯ для ВСЕХ якорей (не только несомого): размер —
            // свойство места, поэтому и перенос гизмо в другую полость перескейливает куб.
            // С гизмо-масштабом не дерётся (тот живёт в model_scale, логика правит только σ);
            // в сошедшемся состоянии σ-шаг = exp(≈0) — дрейфа нет. СТРОГО ПОСЛЕ съёма правки:
            // съём интерпретирует дельты в терминах σ прошлой записи. Заодно нормализует
            // локаль/адрес после гизмо-переносов (объект всегда каноничен).
            FractalUpdateSet::MengerObjectScaleTick(a.pos, r_model);

            const FractalUpdateSet::FractalOffset off =
                FractalUpdateSet::DiffFractalPos(a.pos, FractalUpdateSet::MengerCameraPos());
            const double dist   = glm::length(off.offset);
            const double radius = off.scale * r_model * ms_max;
            const bool visible = (dist - radius) < fog_far
                && radius > dist * 4e-4
                && off.scale < 6.0e5;

            // Матрица = T·R·S: столбцы = оси ориентации × полный пер-осевой масштаб.
            glm::mat3 m(0.0f);
            glm::vec3 t(0.0f);
            if (visible) {
                const glm::mat3 R  = glm::mat3_cast(a.rot);
                const glm::vec3 cs = glm::vec3(off.scale * a.model_scale);
                m = glm::mat3(R[0] * cs.x, R[1] * cs.y, R[2] * cs.z);
                t = glm::vec3(off.offset);
            }
            P.x[i] = m[0].x; P.y[i] = m[1].x; P.z[i] = m[2].x; P.w[i] = t.x;
            P.a[i] = m[0].y; P.b[i] = m[1].y; P.c[i] = m[2].y; P.d[i] = t.y;
            P.e[i] = m[0].z; P.f[i] = m[1].z; P.g[i] = m[2].z; P.h[i] = t.z;
            P.i[i] = 0.0f;   P.j[i] = 0.0f;   P.k[i] = 0.0f;   P.l[i] = 1.0f;

            // Кэш записанного — база съёма правки на следующем тике.
            a.written_m  = m;
            a.written_t  = t;
            a.written_fs = visible ? (float)off.scale : 0.0f;
        });
    }

    // B — дебаг-закладка позиции (после MengerTick: состояние тика уже свежее).
    if (!ui_keyboard) {
        for (const InputManager::KeyEvent& e : key_events_scratch) {
            if (e.down && e.scancode == SDL_SCANCODE_B)
                FractalUpdateSet::MengerBookmarkHere();
        }
    }

    return SDL_APP_CONTINUE;
}
