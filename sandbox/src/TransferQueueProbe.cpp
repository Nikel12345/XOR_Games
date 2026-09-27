// ============================================================================
//  Sandbox: полная череда ТРЁХ очередей на одном буфере.
//
//    фаза 1  КОПИРОВАЛЬНАЯ   заливает вершины треугольника
//    фаза 2  КОПИРОВАЛЬНАЯ   читает их обратно (сверка с исходником)
//    фаза 3a ВЫЧИСЛИТЕЛЬНАЯ  доворачивает вершины В САМОМ БУФЕРЕ, каждый кадр
//    фаза 3b ГРАФИЧЕСКАЯ     рисует ими треугольник
//    фаза 4  КОПИРОВАЛЬНАЯ   читает результат и судит, что это был поворот
//
//  Между фазами — блокирующий fence. Ни потоков, ни слотов: конвейер строго
//  последовательный, потому что проверяется маршрутизация, а не пейсинг.
//
//  ГЛАВНОЕ: после первой заливки CPU вершины НЕ ТРОГАЕТ. Вращение накапливается
//  в самой памяти буфера, то есть каждая семья видит записи предыдущей. Это и
//  есть то, ради чего буферы переведены в VK_SHARING_MODE_CONCURRENT: при
//  EXCLUSIVE содержимое на чужой семье было бы формально undefined, и fence бы
//  не помог — он даёт порядок и видимость, но не владение.
//
//  ЧТО ЗДЕСЬ ДВИЖКОВОЕ. Всё, кроме порядка вызовов: BufferData +
//  UpdateInstruction + ReadBackInstruction, TransferManager, ShaderManager
//  (vs/fs/cs + ShaderProgram + ComputeShaderProgram), PassManager (RenderPassStep
//  со свопчейном-атласом и ComputePrepass), PipeManager, BatchBuilder.
//  ОСТОРОЖНО С ПРЕПАССОМ. Вычислительная работа повешена на него потому, что
//  ExecutePrepassesSteps и ExecutePassesSteps — два независимых входа, и их легко
//  развести по разным очередям, не трогая PassManager. Но препасс НЕ задумывался
//  как шов под compute: это механизм PrepassDepended, сделанный под другую задачу
//  (сама она больше не используется), а каллинг переиспользовал его уже вторично.
//  То, что зонд через него прошёл, — не рекомендация; на нём могут висеть
//  допущения исходного назначения, и для боевой compute-фазы шов стоит выбирать
//  осознанно, а не по этому примеру.
//
//  Вершинный буфер — стрим пула (_VertexPosBuffer). usage ему объявляют сами
//  шейдеры: VERTEX от вершинника, COMPUTE_STORAGE_WRITE от вычислительной
//  программы; флаги сливаются объединением. Руками ничего не помечено. Заодно это
//  самый острый случай для масок барьеров: состояние по умолчанию такого буфера
//  переводится в VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, которой нет ни на
//  копировальной, ни на вычислительной очереди — без маски барьер уронил бы
//  устройство.
//
//  КРИТЕРИИ (все независимы):
//    1. в логе ТРИ разных 'first submit to queue family' — по одному на семью;
//    2. фаза 2: прочитанное совпадает с залитым;
//    3. фаза 4: вершины СДВИНУЛИСЬ и при этом СОХРАНИЛИ длину. Второе и отличает
//       поворот от мусора: неверный бинд или страйд длину не сохранит. Счётчик
//       кадров доказывал бы только то, что проход исполнился;
//    4. валидация Vulkan молчит — полностью, включая выход;
//    5. на экране вращается градиентный треугольник.
//
//  ВЫХОД — ПО ЗАКРЫТИЮ ОКНА (или Esc), а не по счётчику кадров: окно, исчезающее
//  само через полторы секунды, неотличимо от падения. Крышка по кадрам осталась
//  предохранителем для запуска без присмотра.
//
//  Запуск (рабочая директория — src/sandbox или корень репо, путь к шейдерам
//  подстрахован кандидатами):
//    SDL_LOGGING=*=info                        — иначе строк про очереди не видно
//    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
//    VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1
// ============================================================================
#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "TransferManager.h"
#include "BufferManager.h"
#include "BufferUpdateStruct.h"
#include "QueueManager.h"
#include "ShaderManager.h"
#include "ShaderData.h"
#include "ShaderTypes.h"
#include "PassManager.h"
#include "RenderCommandData.h"
#include "PipeManager.h"
#include "BatchBuilder.h"
#include "PositionStructure.h"
#include "TextureData.h"

// Дискретная карта на переключаемой графике. Тот же экспорт, что в src/game/src/main.cpp:
// без него драйвер отдаёт зонду встройку, и мерить/проверять пришлось бы не на той карте,
// на которой работает игра.
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

namespace {

constexpr uint32_t VERTEX_COUNT = 3;
constexpr uint32_t VERTEX_BYTES = VERTEX_COUNT * sizeof(PosOnly);   // float3, страйд 12

// Треугольник сразу в clip-пространстве: ни камеры, ни трансформа (см. шапку шейдера).
constexpr PosOnly TRIANGLE[VERTEX_COUNT] = {
    {  0.0f, -0.6f, 0.0f },
    {  0.6f,  0.6f, 0.0f },
    { -0.6f,  0.6f, 0.0f },
};

// Последнее прочитанное содержимое буфера. Читатель только копирует сюда, а СУДИТ вызывающий:
// фаза 2 сверяет с исходником, фаза 4 — проверяет, что это именно поворот.
PosOnly  g_read[VERTEX_COUNT]{};
bool     g_read_ok = false;
bool     g_readback_ok = false;
uint32_t g_frames_drawn = 0;
uint32_t g_frames_rotated = 0;
bool     g_exit_ok = false;

// Пуш вычислительной фазы: дельта поворота за кадр. Тик живёт на CPU, сам поворот
// накапливается В БУФЕРЕ (см. шапку rotate_verts.comp.hlsl).
struct RotateParams {
    float    delta_angle = 0.0f;
    uint32_t vertex_count = VERTEX_COUNT;
};
struct DummyDispatchData {};

// Юниформа вершинника: поправка на форму окна (см. шапку triangle.vert.hlsl).
// Дополнена до 16 байт — cbuffer в HLSL кладёт скаляр в регистр такого размера.
struct ViewParams { float aspect = 1.0f; float pad[3]{}; };

// Дельта текущего кадра. Пуш-лямбда обязана брать данные ТОЛЬКО через биндер, но у зонда нет
// ни слотов, ни слепков, поэтому тик просто лежит здесь и обновляется в цикле кадров.
float g_delta_angle = 0.0f;

// Шейдеры ищем от нескольких корней: рабочая директория у зондов исторически разная.
// Возврат ПО ЗНАЧЕНИЮ намеренно: со статическим буфером второй вызов затирал результат первого.
std::string FindShader(const char* leaf)
{
    const char* roots[] = {
        "../sandbox/shaders_code/", "shaders_code/",
        "src/sandbox/shaders_code/", "../../src/sandbox/shaders_code/",
    };
    for (const char* r : roots) {
        std::string p = std::string(r) + leaf;
        if (SDL_GetPathInfo(p.c_str(), nullptr)) return p;
    }
    return {};
}

}   // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }

    auto make_window = [] {
        return SDL_CreateWindow("queue probe: transfer -> compute -> graphics", 640, 480, 0);
    };
    SDL_Window* win = make_window();
    if (!win) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); return 1; }

    // debug=true обязателен: под ним SDL включает валидацию и печатает, в какую семью ушёл
    // первый сабмит. Без этого зонд ничего не докажет.
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    if (!dev) { SDL_Log("SDL_CreateGPUDevice: %s", SDL_GetError()); return 1; }
    SDL_ClaimWindowForGPUDevice(dev, win);

    // БАГ SDL 3.4.14 «первого окна»: claim возвращает true, но окно не регистрируется, и
    // свопчейн потом отвечает «Must claim window before…». Проверяем ФАКТ (формат свопчейна),
    // как в src/game/src/main.cpp, и один раз пересоздаём окно.
    if (SDL_GetGPUSwapchainTextureFormat(dev, win) == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_Log("Claim не сработал (баг первого окна SDL 3.4) — пересоздаю окно");
        SDL_ReleaseWindowFromGPUDevice(dev, win);
        SDL_DestroyWindow(win);
        win = make_window();
        if (!win || !SDL_ClaimWindowForGPUDevice(dev, win)) {
            SDL_Log("Повторный claim не удался: %s", SDL_GetError());
            return 1;
        }
    }
    const SDL_GPUTextureFormat swap_fmt = SDL_GetGPUSwapchainTextureFormat(dev, win);
    SDL_Log("Драйвер: %s", SDL_GetGPUDeviceDriver(dev));

    const std::string vs_path = FindShader("triangle.vert.hlsl");
    const std::string fs_path = FindShader("triangle.frag.hlsl");
    const std::string cs_path = FindShader("rotate_verts.comp.hlsl");
    if (vs_path.empty() || fs_path.empty() || cs_path.empty()) {
        SDL_Log("Не найдены шейдеры треугольника (жду CWD = src/sandbox или корень репо).");
        return 1;
    }

    {   // менеджеры — во вложенной области: их деструкторы освобождают GPU-ресурсы и обязаны
        // отработать ДО SDL_DestroyGPUDevice, иначе валидация ругается на живые объекты.
        QueueManager    qm(dev);
        TransferManager trm(dev);
        BufferManager   bm(dev, &trm);
        ShaderManager   sm(dev);
        PassManager     pm;
        PipeManager     pipes(dev, win);
        BatchBuilder    bb;

        // Вершинный буфер заводить НЕ НУЖНО: стримы пула (_VertexPosBuffer и соседей) создаёт
        // сам конструктор BufferManager — Static, ~2.2 МБ, RESIZE_AND_COPY. usage ему объявит
        // вершинник ниже, GPU-буфер создаст BakePending.

        // ── Проход: цель — свопчейн-атлас PassManager'а (движок отдаёт его как обычный атлас) ──
        SDL_GPUGraphicsPipeline** pipeline_slot = new SDL_GPUGraphicsPipeline*(nullptr);
        RenderPassTexturesInfo rptd;
        rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE,
                                    SDL_FColor{ 0.06f, 0.06f, 0.09f, 1.0f }, swap_fmt);
        rptd.SetColorTexture(pm.GetSwapchainAtlas(), 0);

        RenderPassStep* tri_pass = pm.CreateRenderPass(
            "TRIANGLE_PASS",
            [&bm, pipeline_slot](SDL_GPUCommandBuffer* cb, PassManager* p, RenderPassStep& rp)
        {
            // Резолв таргетов — на исполнении: текстура свопчейна меняется каждый кадр.
            // В движке это делает RenderPassStandardBody; здесь тело своё, потому что
            // штатное рисует ИНСТАНСНЫМ ИНДИРЕКТОМ по слепку батчей, а у зонда ни моделей,
            // ни инстансов, ни каллинга нет — и заводить их ради треугольника незачем.
            rp.renderPassTexsData.ResolveTargets();
            if (rp.renderPassTexsData.color_targets.empty() || !rp.renderPassTexsData.color_targets[0].info.texture) return;   // кадра нет

            SDL_GPUColorTargetInfo color_infos[MAX_COLOR_TARGETS];
            const Uint32 color_count = rp.renderPassTexsData.CollectColorTargetInfos(color_infos, MAX_COLOR_TARGETS);
            SDL_GPURenderPass* sdl_rp = SDL_BeginGPURenderPass(cb, color_infos, color_count, nullptr);

            BufferData* vb = bm.GetBufferData(GeometryStreams::VERTEX_POS_BUFFER);
            if (*pipeline_slot && vb && vb->Static.buffer) {
                SDL_BindGPUGraphicsPipeline(sdl_rp, *pipeline_slot);
                // Поправка на форму окна — см. шапку triangle.vert.hlsl. Берётся у свопчейн-
                // атласа, а не запоминается при старте: он переживает смену размера окна.
                const TextureAtlas* sw = p->GetSwapchainAtlas();
                ViewParams vp{};
                vp.aspect = (sw && sw->height) ? (float)sw->width / (float)sw->height : 1.0f;
                SDL_PushGPUVertexUniformData(cb, 0, &vp, sizeof(vp));
                SDL_GPUBufferBinding vbind{};
                vbind.buffer = vb->Static.buffer;
                vbind.offset = 0;
                SDL_BindGPUVertexBuffers(sdl_rp, 0, &vbind, 1);
                SDL_DrawGPUPrimitives(sdl_rp, VERTEX_COUNT, 1, 0, 0);
                ++g_frames_drawn;
            }

            SDL_EndGPURenderPass(sdl_rp);
        },
            std::move(rptd), 0);

        // ── Вычислительный ПРЕПАСС: штатный шов движка под работу, которая идёт в ОТДЕЛЬНОМ
        //    командном буфере до основных проходов (в игре там каллинг). Нам это и нужно:
        //    препассы исполняет ExecutePrepassesSteps, обычные проходы — ExecutePassesSteps,
        //    значит их легко развести по разным очередям, не трогая PassManager. ──
        ComputePassStep* rot_pass = pm.CreateComputePrepass(
            "ROTATE_PREPASS",
            [&bm](SDL_GPUCommandBuffer* cb, PassManager* p, ComputePassStep& cp, uint8_t frame)
        {
            RotateParams     push{};
            DummyDispatchData dd{};
            push.delta_angle = g_delta_angle;   // тик кадра
            p->ComputePassStandardBody(cb, &cp, &bm, &push, &dd, frame);
            ++g_frames_rotated;
        },
            0);

        // ── Шейдеры и программа. Вершинник перечисляет ПОТРЕБЛЯЕМЫЕ стримы: это и есть
        //    декларация usage=VERTEX для _VertexPosBuffer (см. BufferData.h) ──
        sm.CreateVertexShader("triangle_vs", vs_path.c_str(),
                              { GeometryStreams::VERTEX_POS_BUFFER }, &bm);
        sm.CreateFragmentShader("triangle_fs", fs_path.c_str());

        ShaderProgramDescription spd;
        spd.cull_mode   = SDL_GPU_CULLMODE_NONE;
        spd.depth_test  = false;   // depth-таргета у прохода нет
        spd.depth_write = false;
        ShaderProgram* sp = sm.CreateShaderProgram(
            "sp_triangle", spd, tri_pass,
            "triangle_vs", {},    // vs-буферов нет: позиция приходит вершинным стримом, не storage
            "triangle_fs", {},    // fs-буферов нет
            {},                   // слотов текстур нет
            &bm);
        if (!sp) { SDL_Log("CreateShaderProgram не вернул программу."); return 1; }

        // Вычислительная программа. Объявление буфера как rw даёт ему COMPUTE_STORAGE_WRITE —
        // ЛОЖИТСЯ ПОВЕРХ VERTEX от вершинника, потому что usage-флаги сливаются объединением
        // (см. BufferData.h). Один и тот же буфер законно и вершинный, и storage.
        sm.CreateComputeShader("rotate_verts_cs", cs_path.c_str());
        ComputeShaderProgram* csp = sm.CreateComputeShaderProgram(
            "csp_rotate_verts", "rotate_verts_cs",
            { GeometryStreams::VERTEX_POS_BUFFER },   // rw (u0, space1), ссылка по имени
            {}, {}, {}, {},
            "ROTATE_PREPASS", &bm, /*tm=*/nullptr);
        if (!csp) { SDL_Log("CreateComputeShaderProgram не вернул программу."); return 1; }

        // push-инструкция: лямбда получает заготовку из тела прохода и досылает её шейдеру.
        // Здесь она только проставляет счётчик и пушит — дельту уже положило тело прохода.
        sm.CreateComputePushInstruction<RotateParams>("csp_rotate_tick",
            [](const PushConstantBinder& binder, RotateParams data) {
            data.vertex_count = VERTEX_COUNT;
            binder.Push(data);   // слот 0 = b0, space2 (см. шейдер)
        });
        sm.CreateDispatchInstruction<DummyDispatchData>("csp_rotate_tick",
            [](DispatchSizeBinder& binder, DummyDispatchData) {
            binder.Dispatch(VERTEX_COUNT, 1, 1);   // счёт ЭЛЕМЕНТОВ, деление на numthreads — внутри
        });

        // Бейк создаёт GPU-буфер по объявленному usage; затем пайплайн и порядок проходов.
        bm.BakePending();
        BufferData* vb = bm.GetBufferData(GeometryStreams::VERTEX_POS_BUFFER);
        if (!vb || !vb->Static.buffer) { SDL_Log("BakePending не создал вершинный буфер."); return 1; }

        pipes.CreateGraphicsPiplenes(sm.GetShaderPrograms(), &sm, &pm);
        *pipeline_slot = pipes.GetGraphicPipeline(sp).get();
        if (!*pipeline_slot) { SDL_Log("Пайплайн не собрался — рисовать нечем."); return 1; }
        pipes.CreateComputePipelines(sm.GetComputeShaderPrograms(), &sm);
        // Порядок обязателен: FillRenderPasses сливает ordered-списки, по которым идёт
        // BuildComputeBatches. Наоборот — батчи собрались бы по пустому списку.
        pm.FillRenderPasses();
        bb.BuildComputeBatches(&pm, &pipes, &sm, &bm, /*tm=*/nullptr);

        // ── Инструкции жизненного цикла буфера (как DefaultUpdateSet) ──
        bm.CreateUpdateInstruction(GeometryStreams::VERTEX_POS_BUFFER,
            [](SDL_GPUCopyPass*, BufferManager* bmm, UploadTask& task)
        {
            void* dst = bmm->AcquireTransferWritePtr(&task, VERTEX_BYTES);
            if (!dst) { SDL_Log("Не получил transfer-память под вершины."); return; }
            SDL_memcpy(dst, TRIANGLE, VERTEX_BYTES);
        },
            []() -> uint32_t { return VERTEX_BYTES; });

        bm.CreateReadBackInstruction(GeometryStreams::VERTEX_POS_BUFFER,
            [](BufferManager* bmm, ReadBackTask& task)
        {
            auto span = bmm->ReadFromTransferBuffer(&task, VERTEX_BYTES);
            if (span.size() != VERTEX_BYTES) { SDL_Log("Readback: %u байт вместо %u.",
                                                      (uint32_t)span.size(), VERTEX_BYTES); return; }
            SDL_memcpy(g_read, span.data(), VERTEX_BYTES);
            g_read_ok = true;
        },
            []() -> uint32_t { return VERTEX_BYTES; });

        // ══ ФАЗА 1 — ЗАЛИВКА НА КОПИРОВАЛЬНОЙ ОЧЕРЕДИ ══
        SDL_Log("--- фаза 1: заливка вершин (upload queue) ---");
        {
            UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
            if (!cb) { SDL_Log("Acquire (upload): %s", SDL_GetError()); return 1; }

            UploadCopyPass cp = cb.BeginBufferCopyPass();
            TransferBufferData* tbd = bm.ExecuteUpdateInstructions(cp.Raw());
            bm.ExecuteUploadTasks(cp.Raw(), 0);   // слотов нет — индекс 0
            cp.End();

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            if (!fence) { SDL_Log("Submit (upload): %s", SDL_GetError()); return 1; }
            // БЛОКИРУЮЩЕЕ ожидание: рендер стартует строго после того, как заливка ЗАКОНЧИЛАСЬ.
            // Это же и половина «пусть увидит» в барьере — на копировальной очереди её назвать
            // нечем (стадии вершинной выборки там нет), и закрывает её именно fence.
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            trm.ReleaseTB(tbd);   // только ПОСЛЕ fence: до него GPU ещё читает transfer-буфер
        }

        // ══ ФАЗА 2 — ЧТЕНИЕ ОБРАТНО, ТОЖЕ НА КОПИРОВАЛЬНОЙ ══
        // Скачивание буфера копировальной очередью. Зовётся дважды: до вращения (сверка с
        // исходником) и после (проверка, что вычислительная фаза записала ИМЕННО поворот).
        auto read_back_vertices = [&] {
            g_read_ok = false;
            UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
            UploadCopyPass cp = cb.BeginBufferCopyPass();
            TransferBufferData* tbd = bm.ExecuteReadBackInstructionsSize();
            bm.ExecuteDownloadTasks(cp.Raw(), 0);
            cp.End();

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            bm.ExecuteReadBackInstructionsReader();   // читатель — ПОСЛЕ fence
            trm.ReleaseTB(tbd);
        };

        SDL_Log("--- фаза 2: чтение вершин обратно (upload queue) ---");
        read_back_vertices();
        g_readback_ok = g_read_ok && SDL_memcmp(g_read, TRIANGLE, VERTEX_BYTES) == 0;

        // ══ ФАЗА 3 — ОТРИСОВКА НА ГРАФИЧЕСКОЙ ОЧЕРЕДИ ══
        // Вершины уже на месте (fence фазы 1 отработал), поэтому графическая очередь читает
        // буфер, ЗАПОЛНЕННЫЙ ЧУЖОЙ СЕМЬЁЙ. Ровно ради этого буферы и переведены в CONCURRENT.
        // Крутимся, ПОКА ОКНО НЕ ЗАКРОЮТ, а не фиксированное число кадров: зонд визуальный, и
        // самопроизвольно исчезающее через полторы секунды окно неотличимо от падения.
        // Предохранитель на случай запуска без присмотра — жёсткая крышка по кадрам.
        SDL_Log("--- фаза 3: вращение (compute queue) + отрисовка (render queue). Закрой окно, чтобы выйти. ---");
        const int FRAME_CAP = 60 * 60 * 5;   // ~5 минут при vsync
        bool quit = false;
        for (int i = 0; i < FRAME_CAP && !quit; ++i) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT ||
                    ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) quit = true;
            }
            if (quit) break;

            // ── ФАЗА 3a: ВРАЩЕНИЕ НА ВЫЧИСЛИТЕЛЬНОЙ ОЧЕРЕДИ ──
            // Отдельный командный буфер и блокирующий fence — та же схема, что у заливки.
            // CPU вершины больше не трогает: он даёт только дельту, а сам поворот копится
            // в самом буфере. Значит буфер к этому моменту прошёл через ВСЕ ТРИ семьи.
            g_delta_angle = 0.015f;   // радиан за кадр; тик кадра, дальше уедет в push_func
            {
                ComputeCommandBuffer ccb = qm.GetComputeQueue().AcquireCommandBuffer();
                if (!ccb) { SDL_Log("Acquire (compute): %s", SDL_GetError()); break; }

                pm.ExecutePrepassesSteps(ccb.Raw(), 0);

                SDL_GPUFence* cf = ccb.SubmitAndAcquireFence();
                if (!cf) { SDL_Log("Submit (compute): %s", SDL_GetError()); break; }
                // Блокирующее ожидание: половину «пусть увидит» на вычислительной очереди
                // назвать нечем (стадии вершинной выборки там нет — её срезает маска), и
                // закрывает её именно fence.
                SDL_WaitForGPUFences(dev, true, &cf, 1);
                SDL_ReleaseGPUFence(dev, cf);
            }

            // ── ФАЗА 3b: ОТРИСОВКА НА ГРАФИЧЕСКОЙ ──
            RenderCommandBuffer cb = qm.GetRenderQueue().AcquireCommandBuffer();
            if (!cb) { SDL_Log("Acquire (render): %s", SDL_GetError()); break; }

            SDL_GPUTexture* tex = nullptr;
            Uint32 w = 0, h = 0;
            if (!cb.AcquireSwapchainTexture(win, &tex, &w, &h) || !tex) {
                cb.Cancel();   // кадра нет — буфер вернуть в пул, иначе утечёт
                continue;
            }

            pm.SetSwapchain(tex, w, h);
            pm.ExecutePassesSteps(cb.Raw(), 0);

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            if (!fence) { SDL_Log("Submit (render): %s", SDL_GetError()); break; }
            SDL_WaitForGPUFences(dev, true, &fence, 1);   // без слотов — ждём каждый кадр
            SDL_ReleaseGPUFence(dev, fence);
        }

        // ══ ФАЗА 4 — ЧТО ИМЕННО НАПИСАЛА ВЫЧИСЛИТЕЛЬНАЯ ФАЗА ══
        // Счётчик кадров доказывает лишь то, что проход ИСПОЛНИЛСЯ. Судим по содержимому:
        // поворот обязан (а) сдвинуть вершины и (б) СОХРАНИТЬ длину каждой в плоскости XY.
        // Мусор от неверного бинда или страйда завалит второе условие.
        SDL_Log("--- фаза 4: проверка результата вращения (upload queue) ---");
        read_back_vertices();
        bool rotated_moved = false, length_kept = g_read_ok;
        if (g_read_ok) {
            for (uint32_t i = 0; i < VERTEX_COUNT; ++i) {
                const float r0 = SDL_sqrtf(TRIANGLE[i].x * TRIANGLE[i].x + TRIANGLE[i].y * TRIANGLE[i].y);
                const float r1 = SDL_sqrtf(g_read[i].x * g_read[i].x + g_read[i].y * g_read[i].y);
                if (SDL_fabsf(r1 - r0) > 1e-3f) length_kept = false;
                if (SDL_fabsf(g_read[i].x - TRIANGLE[i].x) > 1e-4f ||
                    SDL_fabsf(g_read[i].y - TRIANGLE[i].y) > 1e-4f) rotated_moved = true;
            }
            SDL_Log("Вершина 0: было (%.3f, %.3f), стало (%.3f, %.3f)",
                    TRIANGLE[0].x, TRIANGLE[0].y, g_read[0].x, g_read[0].y);
        }
        SDL_Log("Вершины сдвинулись: %s;  длина сохранена: %s",
                rotated_moved ? "ДА" : "НЕТ", length_kept ? "ДА" : "НЕТ");

        // Именованные шейдеры держат GPU-модули в реестрах ShaderManager, а те разрушаются ПОСЛЕ
        // гашения его токена — делитер тогда сознательно становится no-op (чтобы не звать релиз
        // по мёртвому устройству), и модули доживают до vkDestroyDevice. Сносим их явно, пока
        // менеджер жив: иначе валидация на выходе сыпет VUID-vkDestroyDevice-device-05137, и её
        // молчание перестаёт быть годным критерием.
        sm.DeleteShaderProgram("sp_triangle");
        sm.DeleteVertexShader("triangle_vs");
        sm.DeleteFragmentShader("triangle_fs");

        // ── Вердикт ──
        SDL_Log("Вершины после ридбэка: %s", g_readback_ok ? "СОВПАЛИ" : "НЕ СОВПАЛИ");
        SDL_Log("Кадров: вращений %u, отрисовок %u", g_frames_rotated, g_frames_drawn);
        const bool all_ok = g_readback_ok && g_frames_drawn > 0 && g_frames_rotated > 0
                            && rotated_moved && length_kept;
        if (all_ok)
            SDL_Log("ВЕРДИКТ: буфер прошёл через ВСЕ ТРИ семьи — залит копировальной, "
                    "повёрнут вычислительной, прочитан графической. Вращение на экране и есть "
                    "доказательство: CPU вершины после первой заливки не трогал.");
        else
            SDL_Log("ВЕРДИКТ: ПРОВАЛ. Смотри строки 'GPU queue families' / "
                    "'GPU first submit to queue family N' и вывод валидации.");
        SDL_Log("В логе должно быть ТРИ разных 'first submit to queue family' — "
                "копировальная (фаза 1), вычислительная (3a) и графическая (3b).");

        g_exit_ok = all_ok;
        delete pipeline_slot;
    }   // ← менеджеры разрушены здесь, устройство ещё живо

    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return g_exit_ok ? 0 : 1;
}
