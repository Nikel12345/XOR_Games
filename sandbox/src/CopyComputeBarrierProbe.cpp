// ============================================================================
//  Sandbox: барьерит ли SDL_GPU автоматически copy → compute → copy БЕЗ fence?
//
//  Вопрос: движковый prepare страхуется fence'ами между фазами (upload / compute /
//  readback). Нужны ли они для КОРРЕКТНОСТИ данных, или SDL сам расставляет барьеры
//  между проходами (как он делает между compute-пассами, см. culling_clear)?
//
//  Текущий сценарий — ГОНКА ТРЁХ ПОТОКОВ, каждый со СВОИМ cb, ни одного fence:
//    [T1] copy pass:    заливка 300 МБ "единиц" (39 321 600 элементов по 8 байт;
//                       элемент = uint64 как uint2, см. шейдер) в СВЕЖИЙ буфер;
//    [T2] compute pass: инкремент КАЖДОГО элемента на 1. Диспатч 2D (строки по
//                       ROW_ELEMS): 1D превысил бы Vulkan-минимум лимита групп (65535);
//    [T3] copy pass:    readback всех 300 МБ в download transfer-буфер.
//  CPU-часть каждого потока — заполнение transfer-буфера, запись cb и submit —
//  СЕРИАЛИЗОВАНА мьютексом с номером хода в порядке вызова T1→T2→T3: CPU-гонка
//  «кто первее засабмитит» к тесту барьеров проходов не относится (без мьютекса
//  порядок сабмитов разыгрывался планировщиком: T1 ~20 мс заполнял 300 МБ единиц и
//  сабмитился ПОСЛЕДНИМ → очередь compute→readback→запись → ридбэк привозил единицы
//  (0+1 на занулённой свежей VRAM), стабильно во всех запусках).
//  Join всех → SDL_WaitForGPUIdle → печать. Времена сабмитов печатаются для контроля.
//
//  Суть: упорядочивают ли автобарьеры SDL работы трёх cb, ЗАПИСАННЫХ РАЗНЫМИ ПОТОКАМИ,
//  когда порядок сабмитов правильный. GPU-перекрытие остаётся реальным: DMA 300 МБ ещё
//  идёт, когда сабмитятся compute и readback.
//
//  Критерий — СОДЕРЖИМОЕ: все 2 → между-cb синхронизация работает и при многопоточной
//  записи. Единицы = readback без compute; нули/мусор = чтение до заливки; смесь =
//  частичное перекрытие. Ранее: разрезы 1/2/2/3 cb одним потоком — везде все 2;
//  «1 → +1 → заливка 3 → чтение» одним cb — все 3 (порядок записи).
//
//  Абстракции — движковые (BufferData + UpdateInstruction + ReadBackInstruction,
//  ComputePassStep + ComputePassStandardBody, ComputeShaderProgram + push/dispatch-
//  биндеры, бейк usage-флагов из деклараций), конвейер исполнения — СВОЙ, не
//  движковый prep (там fence'ы, которые как раз и проверяем на необходимость).
//
//  Если в конструкторе BufferManager включены дефолтные буферы движка, BakePending
//  шумит ошибками про их usage (в песочнице его никто не декларирует) — к зонду
//  отношения не имеет.
//
//  Рабочая директория — src/game (как у остальных зондов); пути шейдера ниже
//  подстрахованы кандидатами.
// ============================================================================
#include <SDL3/SDL.h>
#include <cinttypes>
#include <cstdint>
#include <vector>

#include "config.h"
#include "TransferManager.h"
#include "BufferManager.h"
#include "BufferUpdateStruct.h"
#include "ShaderManager.h"
#include "ShaderData.h"
#include "PassManager.h"
#include "RenderCommandData.h"
#include "PipeManager.h"
#include "BatchBuilder.h"

namespace {

// Каноничное имя буфера: реестр BufferManager ключуется указателем (BufferDataName =
// const char*), поэтому везде используется ОДИН этот литерал.
inline constexpr const char* PROBE_BUFFER = "SandboxBarrierProbeBuffer";

constexpr uint32_t ELEM_BYTES = 8;                                   // uint64 (uint2 в шейдере)
constexpr uint32_t NUM_ELEMENTS = 39'321'600;                        // 300 МБ / 8 байт
constexpr uint32_t BUF_BYTES = NUM_ELEMENTS * ELEM_BYTES;            // ровно 300 МБ

// «Строка» 2D-диспатча: 4096 групп по 256 потоков. Обе размерности диспатча обязаны
// влезать в гарантированный Vulkan-минимум лимита групп (65535 на измерение).
constexpr uint32_t ROW_ELEMS = 256u * 4096u;
constexpr uint32_t NUM_ROWS = (NUM_ELEMENTS + ROW_ELEMS - 1) / ROW_ELEMS;
static_assert(ROW_ELEMS / 256 <= 65535 && NUM_ROWS <= 65535, "dispatch: групп больше Vulkan-минимума 65535");

// Раскладка = cbuffer IncParams в increment_u64.comp.hlsl.
struct alignas(16) IncParams { Uint32 total_elements = 0; Uint32 row_elems = 0; };
struct DummyDispatchData {};

// Метка текущего прогона для вердикта reader'а (reader регистрируется один раз,
// прогонов может быть несколько) и ожидаемое значение всех элементов в текущем
// чтении (2 — заливка единиц + инкремент).
const char* g_run_label = "?";
uint64_t g_expected = 2;

const char* FindShaderPath()
{
    static const char* candidates[] = {
        "../sandbox/shaders_code/increment_u64.comp.hlsl",   // CWD = src/game (канон зондов)
        "src/sandbox/shaders_code/increment_u64.comp.hlsl",  // CWD = корень репозитория
        "shaders_code/increment_u64.comp.hlsl",              // CWD = src/sandbox
    };
    for (const char* p : candidates) {
        size_t n = 0;
        if (void* data = SDL_LoadFile(p, &n)) { SDL_free(data); return p; }
    }
    return nullptr;
}

} // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("copy-compute barrier probe", 320, 240, SDL_WINDOW_HIDDEN);

    // debug=true: если стык проходов требует ручной синхронизации, пусть об этом скажет
    // валидация, а не только испорченные данные.
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!dev) { SDL_Log("CreateGPUDevice: %s", SDL_GetError()); return 1; }
    SDL_Log("Драйвер: %s", SDL_GetGPUDeviceDriver(dev));

    const char* shader_path = FindShaderPath();
    if (!shader_path) { SDL_Log("Не найден increment_u64.comp.hlsl (жду CWD = src/game)."); return 1; }

    TransferManager* trm = new TransferManager(dev);
    BufferManager*   bm  = new BufferManager(dev, trm);
    ShaderManager*   sm  = new ShaderManager(dev, "");
    PassManager*     pm  = new PassManager();
    PipeManager*     pipes = new PipeManager(dev, win);
    BatchBuilder*    bb  = new BatchBuilder();

    // ── Setup движковыми путями ──
    // Буфер: только размеры (Static, 300 МБ). usage наполнит декларация compute-программы,
    // GPU-буфер создаст BakePending.
    bm->CreateBufferData(PROBE_BUFFER, BUF_BYTES, BufferDataType::Static, ResizeBehaviour::RESIZE_ONLY);

    // Compute-проход: лямбда — стандартное тело, как BLOOM_PASS/CULLING_PASS в DefaultRenderPassSet.
    ComputePassStep* inc_pass = pm->CreateComputePass(
        "INCREMENT_PASS",
        [bm](SDL_GPUCommandBuffer* cb, PassManager* pm, ComputePassStep& cp, uint8_t pass_frame)
    {
        IncParams push{};
        DummyDispatchData dd{};
        pm->ComputePassStandardBody(cb, &cp, bm, &push, &dd, pass_frame);
    },
        PassAnchor::ChainStart());

    // CSD (компиляция HLSL→SPIR-V через кэш) + программа (rw-декларация даёт буферу
    // COMPUTE_STORAGE_WRITE — ровно то, что требует RW-бинд BeginGPUComputePass).
    sm->CreateComputeShader("increment_u64_cs", shader_path);
    ComputeShaderProgram* csp = sm->CreateComputeShaderProgram("csp_increment_u64", "increment_u64_cs",
        { bm->GetBufferData(PROBE_BUFFER) },   // rw (u0, space1)
        {}, {}, {}, {},
        inc_pass);
    sm->CreateComputePushInstruction<IncParams>("csp_increment_u64",
        [](const PushConstantBinder& binder, IncParams data) {
        data.total_elements = NUM_ELEMENTS;
        data.row_elems = ROW_ELEMS;
        binder.Push(data);
    });
    sm->CreateDispatchInstruction<DummyDispatchData>("csp_increment_u64",
        [](DispatchSizeBinder& binder, DummyDispatchData) {
        // 2D: x — внутри строки (threadcount_x=256 → 4096 групп), y — строки (по одной группе).
        binder.element_count = { ROW_ELEMS, NUM_ROWS, 1 };
    });

    // Бейк (создаёт GPU-буфер по декларациям) → пайплайн → батчи прохода.
    // FillRenderPasses обязателен ДО BuildComputeBatches: тот идёт по ordered-спискам.
    bm->BakePending();
    pipes->CreateComputePipelines(sm->GetComputeShaderPrograms(), sm);
    pm->FillRenderPasses();
    bb->BuildComputeBatches(pm, pipes, sm);

    // ── Инструкции жизненного цикла буфера (как DefaultUpdateSet) ──
    // Upload: 300 МБ "единиц" напрямую в mapped transfer-буфер таска. Заполнение —
    // заметная CPU-работа (~десятки мс), она входит в тело потока T1 ДО его сабмита.
    bm->CreateUpdateInstruction(PROBE_BUFFER,
        [](SDL_GPUCopyPass* cp, BufferManager* bmm, UploadTask& task)
    {
        uint64_t* dst = static_cast<uint64_t*>(bmm->AcquireTransferWritePtr(&task, BUF_BYTES));
        if (!dst) { SDL_Log("Не удалось получить transfer-память под единицы."); return; }
        for (uint32_t i = 0; i < NUM_ELEMENTS; ++i) dst[i] = 1ull;
    },
        []() -> uint32_t { return BUF_BYTES; });

    // Readback: вердикт по СОДЕРЖИМОМУ (гистограмма значений), не по отсутствию ошибок.
    bm->CreateReadBackInstruction(PROBE_BUFFER,
        [](BufferManager* bmm, ReadBackTask& task)
    {
        auto span = bmm->ReadFromTransferBuffer(&task, BUF_BYTES);
        if (span.size() != BUF_BYTES) { SDL_Log("Readback: получил %u байт вместо %u.", (uint32_t)span.size(), BUF_BYTES); return; }
        const uint64_t* v = reinterpret_cast<const uint64_t*>(span.data());

        uint64_t counts[4] = {};   // нули/единицы/двойки/тройки
        uint64_t nother = 0;
        int64_t first_bad = -1;
        for (uint32_t i = 0; i < NUM_ELEMENTS; ++i) {
            if (v[i] < 4) ++counts[v[i]]; else ++nother;
            if (v[i] != g_expected && first_bad < 0) first_bad = i;
        }
        SDL_Log("Гистограмма из %u элементов:  троек=%" PRIu64 "  двоек=%" PRIu64 "  единиц=%" PRIu64 "  нулей=%" PRIu64 "  прочего=%" PRIu64,
                NUM_ELEMENTS, counts[3], counts[2], counts[1], counts[0], nother);

        const uint64_t n_expected = (g_expected < 4) ? counts[g_expected] : 0;
        if (n_expected == NUM_ELEMENTS)
            SDL_Log("ВЕРДИКТ [%s]: все %" PRIu64 " — операции легли в ожидаемом порядке.", g_run_label, g_expected);
        else
            SDL_Log("ВЕРДИКТ [%s]: ОЖИДАЛИСЬ %" PRIu64 ", первый неожиданный — индекс %" PRId64 ". "
                    "Трактовка: единицы = readback без compute; нули/мусор = чтение до заливки (свежая VRAM); смесь = частичное перекрытие.",
                    g_run_label, g_expected, first_bad);
    },
        []() -> uint32_t { return BUF_BYTES; });

    // ── 3 потока, у каждого СВОЙ cb, без fence. CPU-часть (запись в transfer-буфер,
    //    запись cb, submit) — под мьютексом с НОМЕРОМ ХОДА: T1 → T2 → T3, как в вызове.
    //    Голый мьютекс порядок бы не дал (кто первым схватит), поэтому + cv и turn.
    //    SDL-контракт соблюдён: каждый cb acquire'ится и используется СВОИМ потоком.
    bm->logic_index = 0;
    const uint8_t slot = 0;

    g_run_label = "3 потока / 3 cb + мьютекс порядка: T1 запись 1, T2 compute +1, T3 readback";
    g_expected = 2;
    SDL_Log("");
    SDL_Log("=== Прогон [%s] ===", g_run_label);

    TransferBufferData* up_tbd = nullptr;
    TransferBufferData* down_tbd = nullptr;
    // Тайм-штампы сабмитов (нс от старта гонки) — печать после join, чтобы не месить лог.
    const uint64_t race_t0 = SDL_GetTicksNS();
    uint64_t t1_submit = 0, t2_submit = 0, t3_submit = 0;

    std::mutex order_mtx;
    std::condition_variable order_cv;
    int turn = 0;
    // Взять мьютекс СВОЕГО хода: лок держится на всю CPU-часть потока (запись tb + submit).
    auto WaitTurn = [&](int my) {
        std::unique_lock<std::mutex> lk(order_mtx);
        order_cv.wait(lk, [&] { return turn == my; });
        return lk;
    };
    auto PassTurn = [&](std::unique_lock<std::mutex>& lk) {
        ++turn;
        lk.unlock();
        order_cv.notify_all();
    };

    std::thread t_upload([&]() {
        auto lk = WaitTurn(0);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        up_tbd = bm->ExecuteUpdateInstructions(cp);   // тут же заполнение 300 МБ единиц
        bm->ExecuteUploadTasks(cp, slot);
        SDL_EndGPUCopyPass(cp);
        if (!SDL_SubmitGPUCommandBuffer(cb)) SDL_Log("T1 submit упал: %s", SDL_GetError());
        t1_submit = SDL_GetTicksNS() - race_t0;
        PassTurn(lk);
    });
    std::thread t_compute([&]() {
        auto lk = WaitTurn(1);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        inc_pass->compute_function(cb, pm, *inc_pass, slot);
        if (!SDL_SubmitGPUCommandBuffer(cb)) SDL_Log("T2 submit упал: %s", SDL_GetError());
        t2_submit = SDL_GetTicksNS() - race_t0;
        PassTurn(lk);
    });
    std::thread t_read([&]() {
        auto lk = WaitTurn(2);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        down_tbd = bm->ExecuteReadBackInstructionsSize();
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
        bm->ExecuteDownloadTasks(cp, slot);
        SDL_EndGPUCopyPass(cp);
        if (!SDL_SubmitGPUCommandBuffer(cb)) SDL_Log("T3 submit упал: %s", SDL_GetError());
        t3_submit = SDL_GetTicksNS() - race_t0;
        PassTurn(lk);
    });

    t_upload.join();
    t_compute.join();
    t_read.join();

    SDL_Log("Сабмиты завершились (мс от старта гонки):  T1 запись=%.3f  T2 compute=%.3f  T3 readback=%.3f",
            t1_submit / 1e6, t2_submit / 1e6, t3_submit / 1e6);

    // Не барьер между GPU-работами — только право CPU прочитать download-память.
    SDL_WaitForGPUIdle(dev);

    bm->ExecuteReadBackInstructionsReader();

    // GPU idle → контракт ReleaseTB соблюдён для обеих аренд.
    trm->ReleaseTB(up_tbd);
    trm->ReleaseTB(down_tbd);

    delete bb; delete pipes; delete pm; delete sm; delete bm; delete trm;
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
