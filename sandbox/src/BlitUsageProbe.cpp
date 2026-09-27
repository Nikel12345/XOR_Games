// ============================================================================
//  Sandbox: какие usage-флаги РЕАЛЬНО требуют blit / copy / GenerateMipmaps?
//
//  Зачем: проектируется автовывод usage-флагов из графа биндов (шейдеры/проходы).
//  Открытый вопрос — участвуют ли в этом выводе операции, которые идут МИМО
//  шейдерных биндов (blit, copy, mip-gen), и если да, то какие флаги им нужны.
//  Документация SDL про это МОЛЧИТ (докстринги SDL_BlitGPUTexture /
//  SDL_CopyGPUTextureToTexture / SDL_GenerateMipmapsForGPUTexture не содержат
//  формулировки "must have been created with...", в отличие от всех SDL_Bind*).
//
//  Метод: девайс с ВКЛЮЧЁННОЙ валидацией (debug=true). Для каждой комбинации
//  usage — создать src (белый 2x2) и dst (чёрный 2x2), выполнить операцию,
//  скачать dst и посмотреть на ПИКСЕЛИ. Три исхода:
//    OK      — операция прошла, dst побелел (флагов хватило);
//    NOP     — ошибки нет, но dst остался чёрным (тихо не сработало);
//    ERR     — SDL_GetError() непуст / валидация ругнулась.
//  Пиксели — единственный надёжный критерий: "нет ошибки" ещё не значит "сработало".
//
//  Форматы везде одинаковые (BGRA8_UNORM), варьируется ТОЛЬКО usage — чтобы
//  различие в исходе нельзя было списать на конвертацию формата.
// ============================================================================
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Валидация SDL_gpu — это SDL_assert_release, который по умолчанию АБОРТИТ процесс.
// Перехватываем: текст ассерта — ровно то требование по usage, которого нет в докстрингах,
// поэтому он нам нужен как результат, а не как способ умереть. IGNORE (не ALWAYS_IGNORE):
// хотим ловить срабатывание НА КАЖДОЙ комбинации, а не только на первой.
std::string g_assert_msg;

SDL_AssertState SDLCALL AssertHandler(const SDL_AssertData* data, void*)
{
    if (data && data->condition) g_assert_msg = data->condition;
    return SDL_ASSERTION_IGNORE;
}

// Собирает вердикт операции: приоритет у ассерта валидации, затем SDL_GetError, затем пиксели.
struct OpResult {
    std::string assert_msg;
    std::string error;
    int pixel = -1;
    bool failed() const { return !assert_msg.empty() || !error.empty(); }
};

constexpr uint32_t TEX_W = 2;
constexpr uint32_t TEX_H = 2;
constexpr uint32_t PIXEL_BYTES = 4;
constexpr uint32_t TEX_BYTES = TEX_W * TEX_H * PIXEL_BYTES;

struct UsageCase {
    const char* name;
    SDL_GPUTextureUsageFlags flags;
};

// Комбинации, которыми варьируем src и dst. Специально включены заведомо
// "неподходящие" (только COMPUTE_STORAGE_*) — user просил максимально
// несовпадающие usage, чтобы увидеть, гейтится ли blit вообще хоть чем-то.
const UsageCase USAGES[] = {
    { "SAMPLER",                 SDL_GPU_TEXTUREUSAGE_SAMPLER },
    { "COLOR_TARGET",            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET },
    { "SAMPLER|COLOR_TARGET",    SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET },
    { "COMPUTE_READ",            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ },
    { "COMPUTE_WRITE",           SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE },
};

SDL_GPUTexture* MakeTex(SDL_GPUDevice* dev, SDL_GPUTextureUsageFlags usage, uint32_t levels = 1)
{
    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    tci.usage = usage;
    tci.width = TEX_W;
    tci.height = TEX_H;
    tci.layer_count_or_depth = 1;
    tci.num_levels = levels;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(dev, &tci);
}

// Заливает mip 0 текстуры одним байтом (0xFF = белый, 0x00 = чёрный).
// Upload идёт через copy-pass — по гипотезе, copy никаких usage-флагов не требует;
// если это не так, зонд это тоже покажет (заливка src'а с "неподходящим" usage упадёт).
bool FillTexture(SDL_GPUDevice* dev, SDL_GPUTexture* tex, uint8_t value,
                 uint32_t w = TEX_W, uint32_t h = TEX_H, uint32_t mip = 0)
{
    const uint32_t bytes = w * h * PIXEL_BYTES;
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    if (!tb) return false;

    void* mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (!mapped) { SDL_ReleaseGPUTransferBuffer(dev, tb); return false; }
    SDL_memset(mapped, value, bytes);
    SDL_UnmapGPUTransferBuffer(dev, tb);

    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureTransferInfo srcinfo{};
    srcinfo.transfer_buffer = tb;
    srcinfo.pixels_per_row = w;
    srcinfo.rows_per_layer = h;
    SDL_GPUTextureRegion dstreg{};
    dstreg.texture = tex;
    dstreg.mip_level = mip;
    dstreg.w = w; dstreg.h = h; dstreg.d = 1;
    SDL_UploadToGPUTexture(cp, &srcinfo, &dstreg, false);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(dev, true, &f, 1);
    SDL_ReleaseGPUFence(dev, f);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return true;
}

// Скачивает первый пиксель мипа и возвращает его B-канал (все каналы равны — заливаем memset'ом).
// -1 = скачать не удалось.
int ReadPixel(SDL_GPUDevice* dev, SDL_GPUTexture* tex, uint32_t mip = 0,
              uint32_t w = TEX_W, uint32_t h = TEX_H)
{
    const uint32_t bytes = w * h * PIXEL_BYTES;
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    if (!tb) return -1;

    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTextureRegion srcreg{};
    srcreg.texture = tex;
    srcreg.mip_level = mip;
    srcreg.w = w; srcreg.h = h; srcreg.d = 1;
    SDL_GPUTextureTransferInfo dstinfo{};
    dstinfo.transfer_buffer = tb;
    dstinfo.pixels_per_row = w;
    dstinfo.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &srcreg, &dstinfo);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(dev, true, &f, 1);
    SDL_ReleaseGPUFence(dev, f);

    void* mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    int result = -1;
    if (mapped) {
        result = static_cast<int>(*static_cast<uint8_t*>(mapped));
        SDL_UnmapGPUTransferBuffer(dev, tb);
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return result;
}

// Печатает строку результата: что именно сказала валидация / что стало с пикселями.
void LogResult(const char* col1, const char* col2, const OpResult& r)
{
    std::string verdict;
    if (!r.assert_msg.empty())      verdict = "ОТКАЗ  :: " + r.assert_msg;
    else if (!r.error.empty())      verdict = "ERR    :: " + r.error;
    else if (r.pixel < 0)           verdict = "?      (не смог скачать dst)";
    else if (r.pixel >= 0xF0)       verdict = "OK     (dst побелел -> сработало)";
    else if (r.pixel <= 0x0F)       verdict = "NOP    (ошибки нет, но dst ЧЁРНЫЙ -> тихо не сработало)";
    else                            verdict = "?      (dst промежуточный)";

    if (col2) SDL_Log("%-16s %-16s %s", col1, col2, verdict.c_str());
    else      SDL_Log("%-33s %s", col1, verdict.c_str());
}

// Снимает исход только что засабмиченной операции (ассерт валидации + SDL_GetError).
OpResult TakeResult()
{
    OpResult r;
    r.assert_msg = g_assert_msg;
    const char* e = SDL_GetError();
    if (e && e[0] != '\0') r.error = e;
    return r;
}

// Сбрасывает состояние перед операцией, чтобы ассерт/ошибка не «протекли» из предыдущей.
void ResetProbeState()
{
    g_assert_msg.clear();
    SDL_ClearError();
}

// ── Тест 1: BLIT. src(белый) → dst(чёрный), варьируем usage обоих. ──
void ProbeBlit(SDL_GPUDevice* dev)
{
    SDL_Log("");
    SDL_Log("=== BLIT: src(белый 2x2) -> dst(чёрный 2x2), формат у обоих BGRA8 ===");
    SDL_Log("%-16s %-16s %s", "src usage", "dst usage", "исход");
    SDL_Log("---------------------------------------------------------------------");

    for (const UsageCase& su : USAGES) {
        for (const UsageCase& du : USAGES) {
            SDL_GPUTexture* src = MakeTex(dev, su.flags);
            SDL_GPUTexture* dst = MakeTex(dev, du.flags);
            if (!src || !dst) {
                SDL_Log("%-16s %-16s СОЗДАНИЕ ТЕКСТУРЫ УПАЛО: %s", su.name, du.name, SDL_GetError());
                if (src) SDL_ReleaseGPUTexture(dev, src);
                if (dst) SDL_ReleaseGPUTexture(dev, dst);
                ResetProbeState();
                continue;
            }

            FillTexture(dev, src, 0xFF);   // белый
            FillTexture(dev, dst, 0x00);   // чёрный

            ResetProbeState();
            SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
            SDL_GPUBlitInfo bi{};
            bi.source.texture = src;
            bi.source.w = TEX_W; bi.source.h = TEX_H;
            bi.destination.texture = dst;
            bi.destination.w = TEX_W; bi.destination.h = TEX_H;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            bi.cycle = false;
            SDL_BlitGPUTexture(cb, &bi);
            SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
            if (f) { SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); }

            OpResult r = TakeResult();
            if (!r.failed()) r.pixel = ReadPixel(dev, dst);
            LogResult(su.name, du.name, r);

            ResetProbeState();
            SDL_ReleaseGPUTexture(dev, src);
            SDL_ReleaseGPUTexture(dev, dst);
        }
    }
}

// ── Тест 2: COPY. Тот же матричный прогон, но SDL_CopyGPUTextureToTexture. ──
//    Гипотеза (из кода движка: shadow_temp = только DEPTH_TARGET, а
//    shadow_depth_flat_array = только SAMPLER, и копия между ними работает) —
//    copy не гейтится usage вообще. Проверяем на самых несовместимых парах.
void ProbeCopy(SDL_GPUDevice* dev)
{
    SDL_Log("");
    SDL_Log("=== COPY (CopyGPUTextureToTexture): те же пары ===");
    SDL_Log("%-16s %-16s %s", "src usage", "dst usage", "исход");
    SDL_Log("---------------------------------------------------------------------");

    for (const UsageCase& su : USAGES) {
        for (const UsageCase& du : USAGES) {
            SDL_GPUTexture* src = MakeTex(dev, su.flags);
            SDL_GPUTexture* dst = MakeTex(dev, du.flags);
            if (!src || !dst) {
                if (src) SDL_ReleaseGPUTexture(dev, src);
                if (dst) SDL_ReleaseGPUTexture(dev, dst);
                ResetProbeState();
                continue;
            }

            FillTexture(dev, src, 0xFF);
            FillTexture(dev, dst, 0x00);

            ResetProbeState();
            SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
            SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
            SDL_GPUTextureLocation sloc{}; sloc.texture = src;
            SDL_GPUTextureLocation dloc{}; dloc.texture = dst;
            SDL_CopyGPUTextureToTexture(cp, &sloc, &dloc, TEX_W, TEX_H, 1, false);
            SDL_EndGPUCopyPass(cp);
            SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
            if (f) { SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); }

            OpResult r = TakeResult();
            if (!r.failed()) r.pixel = ReadPixel(dev, dst);
            LogResult(su.name, du.name, r);

            ResetProbeState();
            SDL_ReleaseGPUTexture(dev, src);
            SDL_ReleaseGPUTexture(dev, dst);
        }
    }
}

// ── Тест 3: GENERATE MIPMAPS. Нужен ли COLOR_TARGET? ──
//    4x4, 3 мипа. mip0 = белый, mip1 = ПРЕДзалит чёрным. После GenerateMipmaps
//    mip1 должен побелеть (усреднение белого = белый). Остался чёрным → мип-ген
//    тихо не сработал. Так отличаем "работает" от "молча ничего не делает".
void ProbeMipmaps(SDL_GPUDevice* dev)
{
    SDL_Log("");
    SDL_Log("=== GENERATE MIPMAPS: 4x4, 3 мипа; mip0=белый, mip1 предзалит ЧЁРНЫМ ===");
    SDL_Log("%-33s %s", "usage", "исход (смотрим mip1)");
    SDL_Log("---------------------------------------------------------------------");

    const UsageCase MIP_USAGES[] = {
        { "SAMPLER",              SDL_GPU_TEXTUREUSAGE_SAMPLER },
        { "COLOR_TARGET",         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET },
        { "SAMPLER|COLOR_TARGET", SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET },
    };

    for (const UsageCase& u : MIP_USAGES) {
        SDL_GPUTextureCreateInfo tci{};
        tci.type = SDL_GPU_TEXTURETYPE_2D;
        tci.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        tci.usage = u.flags;
        tci.width = 4; tci.height = 4;
        tci.layer_count_or_depth = 1;
        tci.num_levels = 3;
        tci.sample_count = SDL_GPU_SAMPLECOUNT_1;

        ResetProbeState();
        SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &tci);
        if (!tex) {
            SDL_Log("%-33s СОЗДАНИЕ УПАЛО :: %s", u.name, SDL_GetError());
            ResetProbeState();
            continue;
        }

        FillTexture(dev, tex, 0xFF, 4, 4, 0);   // mip0 = белый
        FillTexture(dev, tex, 0x00, 2, 2, 1);   // mip1 = чёрный (предзаливка)

        ResetProbeState();
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GenerateMipmapsForGPUTexture(cb, tex);
        SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        if (f) { SDL_WaitForGPUFences(dev, true, &f, 1); SDL_ReleaseGPUFence(dev, f); }

        OpResult r = TakeResult();
        if (!r.failed()) r.pixel = ReadPixel(dev, tex, 1, 2, 2);
        LogResult(u.name, nullptr, r);

        ResetProbeState();
        SDL_ReleaseGPUTexture(dev, tex);
    }
}

} // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    // ДО создания девайса: иначе первый же отказ валидации убьёт процесс (SDL_assert_release).
    SDL_SetAssertionHandler(AssertHandler, nullptr);
    SDL_Window* win = SDL_CreateWindow("blit usage probe", 320, 240, 0);

    // debug=true — включает валидационный слой: нарушения usage должны стать видимыми
    // (VUID в лог), а не молча ломаться.
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!dev) { SDL_Log("CreateGPUDevice: %s", SDL_GetError()); return 1; }
    SDL_ClaimWindowForGPUDevice(dev, win);

    SDL_Log("Драйвер: %s", SDL_GetGPUDeviceDriver(dev));
    SDL_Log("Вопрос: гейтятся ли blit / copy / mip-gen по usage-флагам?");
    SDL_Log("Критерий — ПИКСЕЛИ dst, а не отсутствие ошибки (тихий no-op тоже провал).");

    ProbeBlit(dev);
    ProbeCopy(dev);
    ProbeMipmaps(dev);

    SDL_Log("");
    SDL_Log("=== Как читать ===");
    SDL_Log("blit:   если строки с src без SAMPLER или dst без COLOR_TARGET дают ERR/NOP,");
    SDL_Log("        значит blit ТРЕБУЕТ эти флаги -> роль 'участник блита' обязана");
    SDL_Log("        попадать в автовывод (или тегироваться вручную).");
    SDL_Log("copy:   ожидаем OK во ВСЕХ строках -> copy можно игнорировать в автовыводе.");
    SDL_Log("mip:    если SAMPLER без COLOR_TARGET даёт ERR/NOP -> num_levels>1 обязан");
    SDL_Log("        добавлять COLOR_TARGET автоматически.");

    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
