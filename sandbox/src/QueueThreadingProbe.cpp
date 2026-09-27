// ============================================================================
//  Sandbox: очереди под НАСТОЯЩИМ конвейером потоков движка.
//
//  Одно­поточный зонд (TransferQueueProbe.cpp) доказал маршрутизацию: заливка на
//  копировальной, вращение на вычислительной, отрисовка на графической. Чего он
//  НЕ проверял — потокобезопасность. А форк тронул ровно то место, которое
//  многопоточностью и живёт: ключ пула команд стал (threadID, семья), то есть
//  разные потоки теперь заводят и ищут РАЗНЫЕ пулы в общей хэш-таблице.
//
//  Здесь работают ThreadController + SlotController как в движке: четыре потока
//  (sim / upload / render / fence) и BUFFERING_LEVEL слотов с обгоном и
//  frame skip. Никаких своих примитивов синхронизации — иначе проверялась бы
//  моя отсебятина, а не движок.
//
//  РАСКЛАДКА ПО ОЧЕРЕДЯМ:
//    sim-поток      → КОПИРОВАЛЬНАЯ    заливает пер-слотовый тик (угол за кадр)
//    compute-поток  → ВЫЧИСЛИТЕЛЬНАЯ   вращает вершины
//    render-поток   → ГРАФИЧЕСКАЯ      рисует
//
//  Пять стадий на трёх слотах, каждая на своей очереди и со своим фенсом. Каждый
//  поток блокируется ТОЛЬКО на своём фенсе: пока compute ждёт свой, render рисует
//  другой слот, а sim готовит третий. Ради этого стадия и заводилась — раньше
//  каллинг сидел в render-потоке и не мог взять следующий слот, пока текущий
//  кадр не дорисован.
//
//  ТИК ИДЁТ ЧЕРЕЗ БУФЕР, А НЕ ЧЕРЕЗ ПУШ. В одно­поточном зонде дельта лежала в
//  глобальной переменной, и пуш записывал её тем же потоком. Здесь она проходит
//  настоящий путь движка: sim пишет UpdateInstruction в СВОЙ слот → копировальная
//  очередь заливает → render читает. Так под проверку попадает и пер-слотовая
//  раскладка Dynamic-буфера, и передача между потоками.
//
//  ЧТО ИМЕННО МОЖЕТ СЛОМАТЬСЯ (ради чего зонд):
//    • гонка в VULKAN_INTERNAL_FetchCommandPool: sim и render одновременно ищут
//      и заводят пулы разных семей в одной хэш-таблице;
//    • неверный хэш ключа — пулы разных семей одного потока в одной корзине;
//    • сабмиты в разные очереди из разных потоков под одним submitLock.
//
//  КРИТЕРИИ:
//    1. в логе ТРИ 'first submit to queue family' с РАЗНЫМИ потоками — sim и
//       render должны отметиться каждый со своим threadID;
//    2. валидация Vulkan молчит (в т.ч. sync-валидация);
//    3. треугольник крутится, окно живёт сколько угодно;
//    4. счётчики UPS/FPS не деградируют и кадры не встают.
//
//  Запуск: рабочая директория — src/sandbox или корень репо.
//    SDL_LOGGING=*=info
//    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
//    VK_KHRONOS_VALIDATION_VALIDATE_SYNC=1
// ============================================================================
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdint>
#include <string>

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
#include "SlotController.h"
#include "ThreadController.h"
#include "PositionStructure.h"
#include "ModelManager.h"
#include "TextureData.h"

extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

namespace {

constexpr const char* TICK_BUFFER = "probe_tick_buffer";
constexpr uint32_t VERTEX_COUNT = 3;
constexpr uint32_t VERTEX_BYTES = VERTEX_COUNT * sizeof(PosOnly);

constexpr PosOnly TRIANGLE[VERTEX_COUNT] = {
    {  0.0f, -0.6f, 0.0f },
    {  0.6f,  0.6f, 0.0f },
    { -0.6f,  0.6f, 0.0f },
};

struct RotateParams { uint32_t vertex_count = VERTEX_COUNT; uint32_t pad[3]{}; };
struct DummyDispatchData {};
struct ViewParams { float aspect = 1.0f; float pad[3]{}; };

// Счётчики — атомарные: их трогают разные потоки конвейера.
std::atomic<uint32_t> g_prepares{ 0 }, g_uploads{ 0 }, g_computes{ 0 }, g_renders{ 0 }, g_fences{ 0 };
std::atomic<bool>     g_error{ false };
bool g_moved = false, g_len_ok = false;   // читаются после останова потоков

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
        return SDL_CreateWindow("queue probe: threaded pipeline", 640, 480, 0);
    };
    SDL_Window* win = make_window();
    if (!win) { SDL_Log("SDL_CreateWindow: %s", SDL_GetError()); return 1; }

    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    if (!dev) { SDL_Log("SDL_CreateGPUDevice: %s", SDL_GetError()); return 1; }
    SDL_ClaimWindowForGPUDevice(dev, win);
    // Баг «первого окна» SDL 3.4.14 — см. src/game/src/main.cpp.
    if (SDL_GetGPUSwapchainTextureFormat(dev, win) == SDL_GPU_TEXTUREFORMAT_INVALID) {
        SDL_ReleaseWindowFromGPUDevice(dev, win);
        SDL_DestroyWindow(win);
        win = make_window();
        if (!win || !SDL_ClaimWindowForGPUDevice(dev, win)) {
            SDL_Log("Повторный claim не удался: %s", SDL_GetError()); return 1;
        }
    }
    const SDL_GPUTextureFormat swap_fmt = SDL_GetGPUSwapchainTextureFormat(dev, win);

    const std::string vs_path = FindShader("triangle.vert.hlsl");
    const std::string fs_path = FindShader("triangle.frag.hlsl");
    const std::string cs_path = FindShader("rotate_verts_tick.comp.hlsl");
    if (vs_path.empty() || fs_path.empty() || cs_path.empty()) {
        SDL_Log("Не найдены шейдеры (жду CWD = src/sandbox или корень репо)."); return 1;
    }

    {
        QueueManager    qm(dev);
        TransferManager trm(dev);
        BufferManager   bm(dev, &trm);
        ShaderManager   sm(dev);
        // Буферы геометрии живут в пуле, а не заводятся BufferManager'ом — зонду хватает
        // POSITION-стрима, поэтому заводим свою минимальную раскладку.
        ModelManager    mm;
        GeometryPool*   pool = mm.CreateGeometryPool(&bm, "Probe", 12,
            { { { { ShaderBase::POSITION, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3 } }, 12, 0 } });
        const BufferDataName VPOS = pool->Streams()[0].buffer_name;
        PassManager     pm;
        PipeManager     pipes(dev, win);
        BatchBuilder    bb;
        SlotController  slots;

        // Пер-слотовый тик: Dynamic значит по копии на слот, и BufferManager сам подставляет
        // нужную при бинде. Это и делает конвейер честным — sim пишет свой слот, пока render
        // читает чужой.
        bm.CreateBufferData(TICK_BUFFER, sizeof(float), BufferDataType::Dynamic);

        SDL_GPUGraphicsPipeline** pipeline_slot = new SDL_GPUGraphicsPipeline*(nullptr);

        RenderPassTexturesInfo rptd;
        rptd.CreateColorTextureInfo(SDL_GPU_LOADOP_CLEAR, SDL_GPU_STOREOP_STORE,
                                    SDL_FColor{ 0.06f, 0.06f, 0.09f, 1.0f }, swap_fmt);
        rptd.SetColorTexture(pm.GetSwapchainAtlas(), 0);

        pm.CreateRenderPass("TRIANGLE_PASS",
            [&bm, VPOS, pipeline_slot](SDL_GPUCommandBuffer* cb, PassManager* p, RenderPassStep& rp)
        {
            rp.renderPassTexsData.ResolveTargets();
            if (rp.renderPassTexsData.color_targets.empty() || !rp.renderPassTexsData.color_targets[0].info.texture) return;

            SDL_GPUColorTargetInfo color_infos[MAX_COLOR_TARGETS];
            const Uint32 color_count = rp.renderPassTexsData.CollectColorTargetInfos(color_infos, MAX_COLOR_TARGETS);
            SDL_GPURenderPass* sdl_rp = SDL_BeginGPURenderPass(cb, color_infos, color_count, nullptr);

            BufferData* vb = bm.GetBufferData(VPOS);
            if (*pipeline_slot && vb && vb->Static.buffer) {
                SDL_BindGPUGraphicsPipeline(sdl_rp, *pipeline_slot);
                const TextureAtlas* sw = p->GetSwapchainAtlas();
                ViewParams vp{};
                vp.aspect = (sw && sw->height) ? (float)sw->width / (float)sw->height : 1.0f;
                SDL_PushGPUVertexUniformData(cb, 0, &vp, sizeof(vp));

                SDL_GPUBufferBinding vbind{};
                vbind.buffer = vb->Static.buffer;
                SDL_BindGPUVertexBuffers(sdl_rp, 0, &vbind, 1);
                SDL_DrawGPUPrimitives(sdl_rp, VERTEX_COUNT, 1, 0, 0);
            }
            SDL_EndGPURenderPass(sdl_rp);
        },
            std::move(rptd), 10);

        // Вращение — препассом (см. оговорку про препасс в TransferQueueProbe.cpp: он взят
        // потому, что ExecutePrepassesSteps отдельный вход, а не потому, что предназначен
        // для compute). ВРЕМЕННО исполняется на графическом cb: compute-стадии у
        // ThreadController нет.
        ComputePassStep* rot_pass = pm.CreateComputePrepass("ROTATE_PREPASS",
            [&bm](SDL_GPUCommandBuffer* cb, PassManager* p, ComputePassStep& cp, uint8_t frame)
        {
            RotateParams      push{};
            DummyDispatchData dd{};
            p->ComputePassStandardBody(cb, &cp, &bm, &push, &dd, frame);
        },
            0);

        sm.CreateVertexShader("triangle_vs", vs_path.c_str(), pool, { ShaderBase::POSITION }, &bm);
        sm.CreateFragmentShader("triangle_fs", fs_path.c_str());

        ShaderProgramDescription spd;
        spd.cull_mode = SDL_GPU_CULLMODE_NONE;
        spd.depth_test = false;
        spd.depth_write = false;
        ShaderProgram* sp = sm.CreateShaderProgram("sp_triangle", spd,
            "TRIANGLE_PASS",
            "triangle_vs", {}, "triangle_fs", {}, {}, &bm);
        if (!sp) { SDL_Log("CreateShaderProgram не вернул программу."); return 1; }

        sm.CreateComputeShader("rotate_tick_cs", cs_path.c_str());
        ComputeShaderProgram* csp = sm.CreateComputeShaderProgram("csp_rotate_tick", "rotate_tick_cs",
            { VPOS },          // rw  u0 space1
            { TICK_BUFFER },   // ro  t0 space0
            {}, {}, {}, "ROTATE_PREPASS", &bm, /*tm=*/nullptr);   // ссылки по имени; атласов у зонда нет
        if (!csp) { SDL_Log("CreateComputeShaderProgram не вернул программу."); return 1; }

        sm.CreateComputePushInstruction<RotateParams>("csp_rotate_tick",
            [](const PushConstantBinder& binder, RotateParams data) {
            data.vertex_count = VERTEX_COUNT;
            binder.Push(data);
        });
        sm.CreateDispatchInstruction<DummyDispatchData>("csp_rotate_tick",
            [](DispatchSizeBinder& binder, DummyDispatchData) { binder.Dispatch(VERTEX_COUNT, 1, 1); });

        // Тик слота. Лямбда исполняется на sim-потоке в СВОЁМ слоте — общего состояния между
        // слотами нет, поэтому синхронизировать нечего.
        bm.CreateUpdateInstruction(TICK_BUFFER,
            [](SDL_GPUCopyPass*, BufferManager* bmm, UploadTask& task)
        {
            float* dst = static_cast<float*>(bmm->AcquireTransferWritePtr(&task, sizeof(float)));
            if (dst) *dst = 0.015f;   // радиан за кадр
        },
            []() -> uint32_t { return sizeof(float); });

        bm.BakePending();
        BufferData* vb = bm.GetBufferData(VPOS);
        if (!vb || !vb->Static.buffer) { SDL_Log("BakePending не создал вершинный буфер."); return 1; }

        pipes.CreateGraphicsPiplenes(sm.GetShaderPrograms(), &sm, &pm);
        *pipeline_slot = pipes.GetGraphicPipeline(sp).get();
        if (!*pipeline_slot) { SDL_Log("Графический пайплайн не собрался."); return 1; }
        pipes.CreateComputePipelines(sm.GetComputeShaderPrograms(), &sm);
        pm.FillRenderPasses();
        bb.BuildComputeBatches(&pm, &pipes, &sm, &bm, /*tm=*/nullptr);

        // ── Стартовая заливка вершин: ОДИН раз, до старта потоков. Дальше их правит только
        //    GPU (вращение), CPU к ним не возвращается. ──
        {
            UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
            UploadCopyPass cp = cb.BeginBufferCopyPass();
            SDL_GPUTransferBufferCreateInfo tci{};
            tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            tci.size  = VERTEX_BYTES;
            SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tci);
            void* map = SDL_MapGPUTransferBuffer(dev, tb, false);
            SDL_memcpy(map, TRIANGLE, VERTEX_BYTES);
            SDL_UnmapGPUTransferBuffer(dev, tb);

            SDL_GPUTransferBufferLocation src{ tb, 0 };
            SDL_GPUBufferRegion dst{ vb->Static.buffer, 0, VERTEX_BYTES };
            cp.UploadToBuffer(&src, &dst, false);
            cp.End();

            SDL_GPUFence* f = cb.SubmitAndAcquireFence();
            SDL_WaitForGPUFences(dev, true, &f, 1);
            SDL_ReleaseGPUFence(dev, f);
            SDL_ReleaseGPUTransferBuffer(dev, tb);
            SDL_Log("Стартовая заливка вершин выполнена (upload queue).");
        }

        // ══ КОНВЕЙЕР ДВИЖКА ══
        // Пер-слотовые transfer-буферы заливки: upload-стадия после своего fence помечает запись,
        // а возвращает её в пул ближайшая аренда на sim — как в движке (пулом владеет один поток).
        TransferBufferData* pending_upload_tbs[BUFFERING_LEVEL] = {};

        auto game_iter_cb = [] {};

        // sim-поток → КОПИРОВАЛЬНАЯ очередь
        auto prepare_cb = [&](uint8_t slot) {
            UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
            if (!cb) { g_error = true; return; }

            UploadCopyPass cp = cb.BeginBufferCopyPass();
            TransferBufferData* tbd = bm.ExecuteUpdateInstructions(cp.Raw());
            bm.ExecuteUploadTasks(cp.Raw(), slot);
            cp.End();

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            if (!fence) { g_error = true; return; }

            pending_upload_tbs[slot] = tbd;
            slots.GetSlotsData()[slot].upload.submit_time = std::chrono::steady_clock::now();
            slots.PushUploadFence(slot, fence);           // fence ДО флага — как в движке
            slots.SetSlotState(slot, SlotState::UPLOADING);
            ++g_prepares;
        };

        // upload-поток: ждёт upload-fence и промоутит слот в PREPARED
        auto upload_cb = [&](uint8_t slot) {
            StageFences& uf = slots.GetSlotsData()[slot].upload;
            if (uf.Empty()) return;
            SDL_WaitForGPUFences(dev, true, uf.items, uf.count);
            for (uint8_t i = 0; i < uf.count; ++i)
                SDL_ReleaseGPUFence(dev, uf.items[i]);
            uf.Clear();

            trm.MarkReturnable(pending_upload_tbs[slot]);   // возврат сделает аренда на sim
            pending_upload_tbs[slot] = nullptr;

            slots.SetSlotState(slot, SlotState::PREPARED);
            ++g_uploads;
        };

        // render-поток → ГРАФИЧЕСКАЯ очередь
        // compute-поток → ВЫЧИСЛИТЕЛЬНАЯ очередь. Слот берёт сам (WaitComputableSlot), сам
        // пишет команды и сам ждёт свой фенс — блокируется при этом ТОЛЬКО он: render в это
        // время рисует другой слот, sim готовит третий. В этом и весь смысл стадии.
        auto compute_cb = [&](uint8_t slot) {
            slots.SetSlotState(slot, SlotState::COMPUTING);

            ComputeCommandBuffer ccb = qm.GetComputeQueue().AcquireCommandBuffer();
            if (!ccb) { g_error = true; return; }

            pm.ExecutePrepassesSteps(ccb.Raw(), slot);

            SDL_GPUFence* fence = ccb.SubmitAndAcquireFence();
            if (!fence) { g_error = true; return; }
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);

            slots.SetSlotState(slot, SlotState::COMPUTED);
            ++g_computes;
        };

        auto render_cb = [&](uint8_t slot) -> bool {
            RenderCommandBuffer cb = qm.GetRenderQueue().AcquireCommandBuffer();
            if (!cb) { g_error = true; return false; }

            SDL_GPUTexture* tex = nullptr; Uint32 w = 0, h = 0;
            if (!cb.AcquireSwapchainTexture(win, &tex, &w, &h) || !tex) {
                cb.Cancel();     // текстуры нет — буфер вернуть в пул, иначе утечёт
                return false;    // кадром не считается, ThreadController повторит
            }

            pm.SetSwapchain(tex, w, h);
            pm.ExecutePassesSteps(cb.Raw(), slot);   // только отрисовка: вращение уехало
                                                     // на свою стадию и свою очередь

            SDL_GPUFence* fence = cb.SubmitAndAcquireFence();
            if (!fence) { g_error = true; return false; }
            slots.GetSlotsData()[slot].render.submit_time = std::chrono::steady_clock::now();
            slots.SetRenderFence(slot, fence);
            ++g_renders;
            return true;
        };

        // fence-поток
        auto fence_cb = [&](uint8_t slot) {
            StageFences& rf = slots.GetSlotsData()[slot].render;
            if (rf.Empty()) return;
            SDL_GPUFence* fence = rf.items[0];
            SDL_WaitForGPUFences(dev, true, &fence, 1);
            SDL_ReleaseGPUFence(dev, fence);
            rf.Clear();
            slots.NotifyRenderFenceDone();
            slots.SetSlotState(slot, SlotState::RENDERED);
            ++g_fences;
        };

        SDL_Log("--- старт конвейера: sim→upload→render→fence, слотов %d ---", BUFFERING_LEVEL);
        // ThreadController живёт во ВЛОЖЕННОЙ области: StopThreads в его API нет, останов и
        // join делает только деструктор — а финальная сверка обязана идти при СТОЯЩИХ потоках,
        // иначе она прочитает буфер, который кто-то ещё крутит.
        {
        ThreadController threads(&slots);
        threads.SetGameIterationCallback(game_iter_cb);
        threads.SetPrepareCallback(prepare_cb);
        threads.SetUploadCallback(upload_cb);
        threads.SetComputeCallback(compute_cb);
        threads.SetRenderCallback(render_cb);
        threads.SetFenceCallback(fence_cb);
        threads.StartThreads();

        bool running = true;
        uint32_t ticks = 0;
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT ||
                    ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
            }
            SDL_Delay(16);
            if (++ticks % 125 == 0) {   // ~каждые 2 секунды
                SDL_Log("prepare=%u upload=%u compute=%u render=%u fence=%u%s",
                        g_prepares.load(), g_uploads.load(), g_computes.load(),
                        g_renders.load(), g_fences.load(),
                        g_error.load() ? "  [БЫЛА ОШИБКА]" : "");
            }
        }

        }   // ← потоки остановлены и приджойнены деструктором
        SDL_Log("--- останов: потоки приджойнены ---");

        // Останов застаёт конвейер В ПОЛЁТЕ: слот мог быть засабмичен, но его стадия не успела
        // дождаться фенса — тот остаётся живым и утекает (валидация ловит его как
        // VUID-vkDestroyDevice-device-05137 на VkFence). Дочищаем сами, потоки уже стоят.
        for (uint8_t i = 0; i < BUFFERING_LEVEL; ++i) {
            for (StageFences* sf : { &slots.GetSlotsData()[i].upload, &slots.GetSlotsData()[i].render }) {
                if (sf->Empty()) continue;
                SDL_WaitForGPUFences(dev, true, sf->items, sf->count);
                for (uint8_t k = 0; k < sf->count; ++k)
                    SDL_ReleaseGPUFence(dev, sf->items[k]);
                sf->Clear();
            }
        }
        SDL_WaitForGPUIdle(dev);

        // Счётчики стадий доказывают, что конвейер КРУТИЛСЯ, но не что вращение доехало:
        // при неверном бинде пер-слотового тика шейдер прочитал бы 0 и треугольник стоял бы.
        // Читаем вершины обратно копировальной очередью и сверяем.
        {
            SDL_GPUTransferBufferCreateInfo dci{};
            dci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
            dci.size  = VERTEX_BYTES;
            SDL_GPUTransferBuffer* dtb = SDL_CreateGPUTransferBuffer(dev, &dci);

            UploadCommandBuffer cb = qm.GetUploadQueue().AcquireCommandBuffer();
            UploadCopyPass cp = cb.BeginBufferCopyPass();
            SDL_GPUBufferRegion src{ vb->Static.buffer, 0, VERTEX_BYTES };
            SDL_GPUTransferBufferLocation dst{ dtb, 0 };
            cp.DownloadFromBuffer(&src, &dst);
            cp.End();
            SDL_GPUFence* f = cb.SubmitAndAcquireFence();
            SDL_WaitForGPUFences(dev, true, &f, 1);
            SDL_ReleaseGPUFence(dev, f);

            const PosOnly* got = (const PosOnly*)SDL_MapGPUTransferBuffer(dev, dtb, false);
            if (got) {
                const float r0 = SDL_sqrtf(TRIANGLE[0].x * TRIANGLE[0].x + TRIANGLE[0].y * TRIANGLE[0].y);
                const float r1 = SDL_sqrtf(got[0].x * got[0].x + got[0].y * got[0].y);
                g_moved  = SDL_fabsf(got[0].x - TRIANGLE[0].x) > 1e-4f ||
                           SDL_fabsf(got[0].y - TRIANGLE[0].y) > 1e-4f;
                g_len_ok = SDL_fabsf(r1 - r0) < 1e-3f;
                SDL_Log("Вершина 0: было (%.3f, %.3f), стало (%.3f, %.3f)",
                        TRIANGLE[0].x, TRIANGLE[0].y, got[0].x, got[0].y);
                SDL_UnmapGPUTransferBuffer(dev, dtb);
            }
            SDL_ReleaseGPUTransferBuffer(dev, dtb);
        }
        SDL_Log("Вершины сдвинулись: %s;  длина сохранена: %s",
                g_moved ? "ДА" : "НЕТ", g_len_ok ? "ДА" : "НЕТ");

        // Шейдеры сносим явно, пока ShaderManager жив: иначе их GPU-модули доживают до
        // vkDestroyDevice и валидация ругается (см. TransferQueueProbe.cpp).
        sm.DeleteShaderProgram("sp_triangle");
        sm.DeleteVertexShader("triangle_vs");
        sm.DeleteFragmentShader("triangle_fs");
        // Вычислительный шейдер НЕ сносим: снять его можно только вместе с программой, а
        // публичного удаления ComputeShaderProgram у ShaderManager нет — попытка даёт
        // «delete refused». Утечки при этом нет: его модуль освобождается штатно (валидация
        // на выходе молчит), в отличие от именованных vs/fs.
        delete pipeline_slot;
    }

    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();

    const bool ok = !g_error.load() && g_renders.load() > 0 && g_fences.load() > 0
                    && g_computes.load() > 0 && g_moved && g_len_ok;
    SDL_Log("ВЕРДИКТ: %s (prepare=%u upload=%u compute=%u render=%u fence=%u)",
            ok ? "пятистадийный конвейер отработал, три очереди выдержали многопоточность" : "ПРОВАЛ",
            g_prepares.load(), g_uploads.load(), g_computes.load(),
            g_renders.load(), g_fences.load());
    return ok ? 0 : 1;
}
