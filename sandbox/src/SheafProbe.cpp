// Зонд: проверка писателя и читателя Sheaf без GPU и окна.
//  1. Синтетика: колонки всех типов и флагов с данными, на которых писатель выбирает каждый из
//     четырёх способов записи (включая таблицу уникальных с номерами шириной 1, 2 и 4), —
//     запись, чтение, сравнение колонок значение за значением.
//  2. Обрезанный файл (каждый префикс) и лишний байт обязаны дать ошибку; файл с одним испорченным
//     байтом (каждая позиция) — ошибку или успех, но не падение.
//  3. Каждый переданный .sheaf (например, из saved_scene редактора): таблицы печатаются, перезапись
//     прочитанного обязана совпасть с файлом байт в байт; затем файл проходит через ECS —
//     ObjectManager::LoadScene и SaveScene, — и значения сверяются с исходными объект за объектом.
// Renderable здесь не зарегистрирован (его спека живёт в Engine вместе с менеджерами): его колонки
// загрузка отбрасывает, и сверяются только встроенные компоненты.
//  4. --damage: порча каждого байта и обрезка на пути движка — LoadScene не падает и при ошибке
//     оставляет сцену пустой (Renderable и компоненты игры зарегистрированы, как в --time).
//  5. --time: только замер загрузки — копия ObjectManager::LoadScene с таймерами между этапами;
//     Renderable и компоненты игры зарегистрированы копиями из Engine.cpp и GameComponents.cpp.
//     Файл .json грузится копией json-загрузки движка до перехода на Sheaf (коммит 39e40f1) —
//     для сравнения на той же сцене (json — от scene_gen2.py того же коммита).
// Запуск: Sandbox [<файл.sheaf> ...] или Sandbox --time <файл.sheaf> ....
#include "PCH.h"
#include "ObjectManager.h"
#include "ComponentSerializer.h"
#include "Sheaf.h"
#include "BaseComponents.h"
#include "MaterialManager.h"
#include "ModelManager.h"
#include "../../game/include/GameComponents.h"
#include "../../../external/yyjson/yyjson.h"
#include <bit>
#include <cfloat>
#include <cstring>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <set>
#include <map>
#include <random>
#include <sstream>

namespace {

using sheaf::Column;
using sheaf::Type;

std::mt19937 rng(7);
constexpr uint32_t kStrings = 300;

uint32_t RandomValue(Type t)
{
    switch (t) {
    case Type::U8:   return rng() & 0xFF;
    case Type::Bool: return rng() & 1;
    case Type::Str:  return rng() % kStrings;
    case Type::F32: {
        const uint32_t special[] = { 0x80000000u, 0x7FC00001u, 0u, 0x3F800000u };
        return (rng() % 8 == 0) ? special[rng() % 4] : rng();
    }
    default:         return rng();
    }
}

// Строка колонки: список длин 0..4 или одно значение; у nullable часть ячеек пустая (значение 0).
struct Row {
    std::vector<uint32_t> values;
    std::vector<uint8_t>  present;
};

Row RandomRow(Type t, uint8_t flags, uint32_t max_len = 4)
{
    Row row;
    const uint32_t n = (flags & sheaf::List) ? rng() % (max_len + 1) : 1;
    for (uint32_t i = 0; i < n; ++i) {
        const bool p = !(flags & sheaf::Nullable) || rng() % 3 != 0;
        row.values.push_back(p ? RandomValue(t) : 0);
        row.present.push_back(p);
    }
    return row;
}

void Append(Column& c, const Row& row)
{
    if (c.flags & sheaf::List) c.lengths.push_back(static_cast<uint32_t>(row.values.size()));
    c.values.insert(c.values.end(), row.values.begin(), row.values.end());
    if (c.flags & sheaf::Nullable) c.present.insert(c.present.end(), row.present.begin(), row.present.end());
}

enum Pattern { Random, Same, MostlyDefault, FewUniques, Pool1000 };

Column MakeColumn(std::string name, Type t, uint8_t flags, uint32_t rows, Pattern pat)
{
    Column c{ .name = std::move(name), .type = t, .flags = flags };
    if (!(flags & sheaf::List)) c.def = RandomValue(t);
    const size_t pool_size = pat == Same ? 1 : pat == FewUniques ? 5 : pat == Pool1000 ? 1000 : 0;
    std::vector<Row> pool;
    for (size_t i = 0; i < pool_size; ++i) pool.push_back(RandomRow(t, flags));
    for (uint32_t r = 0; r < rows; ++r) {
        if (pat == MostlyDefault && !flags) Append(c, { { rng() % 10 ? c.def : RandomValue(t) }, { 1 } });
        else if (!pool.empty())             Append(c, pool[rng() % pool.size()]);
        else                                Append(c, RandomRow(t, flags));
    }
    return c;
}

bool SameColumn(const Column& a, const Column& b)
{
    return a.name == b.name && a.type == b.type && a.flags == b.flags && a.def == b.def
        && a.values == b.values && a.lengths == b.lengths && a.present == b.present;
}

std::vector<uint8_t> WriteTables(const std::vector<sheaf::Table>& tables)
{
    sheaf::Writer w;
    for (uint32_t i = 0; i < kStrings; ++i) w.Intern("s" + std::to_string(i));
    for (const sheaf::Table& t : tables) w.Add(t);
    return w.Finish();
}

int CheckRoundTrip(const std::vector<sheaf::Table>& tables, int enc_count[4])
{
    const std::vector<uint8_t> bytes = WriteTables(tables);
    auto file = sheaf::Read(bytes);
    if (!file) { SDL_Log("  чтение: %s", file.error().c_str()); return 1; }
    int bad = 0;
    if (file->tables.size() != tables.size()) { SDL_Log("  число таблиц"); return 1; }
    for (size_t t = 0; t < tables.size(); ++t) {
        const sheaf::Table& a = tables[t];
        const sheaf::Table& b = file->tables[t];
        if (a.rows != b.rows || a.components.size() != b.components.size()) { SDL_Log("  таблица %zu: форма", t); ++bad; continue; }
        for (size_t c = 0; c < a.components.size(); ++c) {
            if (a.components[c].name != b.components[c].name || a.components[c].fields.size() != b.components[c].fields.size()) {
                SDL_Log("  таблица %zu компонент %zu: форма", t, c); ++bad; continue;
            }
            for (size_t f = 0; f < a.components[c].fields.size(); ++f) {
                const Column& got = b.components[c].fields[f];
                ++enc_count[std::to_underlying(got.encoding)];
                if (!SameColumn(a.components[c].fields[f], got)) {
                    SDL_Log("  таблица %zu %s.%s (способ %d): данные разошлись", t, b.components[c].name.c_str(),
                            got.name.c_str(), std::to_underlying(got.encoding));
                    ++bad;
                }
            }
        }
    }
    return bad;
}

int SyntheticTest()
{
    const Type    types[] = { Type::F32, Type::U32, Type::I32, Type::U8, Type::Bool, Type::Str, Type::Ref };
    const uint8_t flagset[] = { 0, sheaf::List, sheaf::Nullable, sheaf::List | sheaf::Nullable };
    const Pattern pats[] = { Random, Same, MostlyDefault, FewUniques, Pool1000 };

    std::vector<sheaf::Table> tables;
    for (uint32_t rows : { 0u, 1u, 7u, 8u, 9u, 1000u, 3000u }) {
        sheaf::Table t{ .rows = rows };
        t.components.push_back({ .name = "Tag" });
        for (Type ty : types)
            for (uint8_t fl : flagset) {
                sheaf::Component comp{ .name = "T" + std::to_string(std::to_underlying(ty)) + "F" + std::to_string(fl) };
                for (Pattern p : pats)
                    comp.fields.push_back(MakeColumn("p" + std::to_string(p), ty, fl, rows, p));
                t.components.push_back(std::move(comp));
            }
        tables.push_back(std::move(t));
    }

    // Таблица уникальных с номером шириной 4: больше 65536 различных списков, и списки длинные,
    // чтобы повторы окупили 4 байта номера на строку.
    {
        constexpr uint32_t rows = 70000, uniq = 66000;
        Column c{ .name = "lists", .type = Type::U32, .flags = sheaf::List };
        std::vector<Row> pool;
        for (uint32_t i = 0; i < uniq; ++i) {
            Row row;
            for (uint32_t k = 0; k < 30; ++k) { row.values.push_back(rng()); row.present.push_back(1); }
            pool.push_back(std::move(row));
        }
        for (uint32_t r = 0; r < rows; ++r) Append(c, pool[r < uniq ? r : rng() % uniq]);
        tables.push_back({ .rows = rows, .components = { { .name = "Wide", .fields = { std::move(c) } } } });
    }

    int enc[4] = {};
    const int bad = CheckRoundTrip(tables, enc);
    auto file = sheaf::Read(WriteTables(tables));
    const Column& wide = file->tables.back().components[0].fields[0];
    SDL_Log("синтетика: расхождений %d; способы: подряд %d, одно на всех %d, отметки %d, уникальные %d; "
            "широкая таблица уникальных: способ %d, %zu Б",
            bad, enc[0], enc[1], enc[2], enc[3], std::to_underlying(wide.encoding), wide.encoded_bytes);
    return bad + (wide.encoding != sheaf::Encoding::Dict) + (enc[0] == 0 || enc[1] == 0 || enc[2] == 0 || enc[3] == 0);
}

int DamageTest()
{
    // Только случайные колонки: способ «одно на всех» раскрывается на rows значений, и испорченный
    // rows дал бы законную, но многогигабайтную аллокацию.
    sheaf::Table t{ .rows = 9 };
    sheaf::Component comp{ .name = "C" };
    comp.fields.push_back(MakeColumn("f", Type::F32, 0, 9, Random));
    comp.fields.push_back(MakeColumn("s", Type::Str, sheaf::List | sheaf::Nullable, 9, Random));
    comp.fields.push_back(MakeColumn("b", Type::U8, sheaf::Nullable, 9, Random));
    t.components.push_back(std::move(comp));
    const std::vector<uint8_t> bytes = WriteTables({ t });

    int bad = 0;
    for (size_t n = 0; n < bytes.size(); ++n)
        if (sheaf::Read(std::span(bytes.data(), n))) { SDL_Log("  префикс %zu Б прочитан без ошибки", n); ++bad; }
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    auto extra = sheaf::Read(longer);
    if (extra) ++bad;

    int ok = 0, failed = 0;
    std::vector<std::string> samples;
    for (size_t i = 0; i < bytes.size(); ++i)
        for (uint8_t v : { uint8_t{ 0xFF }, uint8_t{ 0x00 }, uint8_t{ 0x07 } }) {
            std::vector<uint8_t> broken = bytes;
            if (broken[i] == v) continue;
            broken[i] = v;
            auto r = sheaf::Read(broken);
            if (r) { ++ok; continue; }
            ++failed;
            if (samples.size() < 6 && std::find(samples.begin(), samples.end(), r.error()) == samples.end())
                samples.push_back(r.error());
        }
    SDL_Log("порча: префиксов %zu, прочитано без ошибки %d; лишний байт: %s; один испорченный байт: "
            "ошибка %d, прочитано %d",
            bytes.size(), bad, extra ? "прочитан!" : extra.error().c_str(), failed, ok);
    for (const std::string& s : samples) SDL_Log("    %s", s.c_str());
    return bad;
}

bool Rewrites(const sheaf::File& file, const std::vector<uint8_t>& bytes)
{
    sheaf::Writer w;
    for (const std::string& s : file.strings) w.Intern(s);
    for (const sheaf::Table& t : file.tables) w.Add(t);
    return w.Finish() == bytes;
}

std::vector<uint8_t> ReadBytes(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// Ключ таблицы после загрузки: только зарегистрированные компоненты, в порядке файла (он уже
// отсортирован по именам, как и у SaveScene).
std::string KnownKey(const sheaf::Table& t)
{
    std::string key;
    for (const sheaf::Component& c : t.components)
        if (ComponentSpecRegistry::Get().ByName(c.name)) key += (key.empty() ? "" : ",") + c.name;
    return key;
}

// Сцена через ECS: файл загружается в ObjectManager и сохраняется обратно. Таблицы оригинала с
// одинаковым набором известных компонентов после загрузки — один архетип: их строки идут подряд в
// порядке файла, и по этому порядку значения сверяются объект за объектом (ref — через ту же
// перенумерацию). Повторная загрузка сохранённого обязана дать тот же файл байт в байт.
int SceneTest(const char* path)
{
    const std::vector<uint8_t> bytes = ReadBytes(path);
    auto orig = sheaf::Read(bytes);
    if (!orig) { SDL_Log("%s: %s", path, orig.error().c_str()); return 1; }

    ObjectManager om;
    om.LoadScene("probe", bytes);
    const std::vector<uint8_t> saved = om.SaveScene(om.GetScene("probe"));
    auto again = sheaf::Read(saved);
    if (!again) { SDL_Log("%s: сохранённое не читается: %s", path, again.error().c_str()); return 1; }

    std::map<std::string, std::pair<const sheaf::Table*, uint32_t>> saved_by_key;   // таблица и её сквозная база
    uint32_t base = 0;
    for (const sheaf::Table& t : again->tables) {
        std::string key;
        for (const sheaf::Component& c : t.components) key += (key.empty() ? "" : ",") + c.name;
        saved_by_key[key] = { &t, base };
        base += t.rows;
    }

    std::vector<int64_t> new_row;           // сквозной номер в оригинале → в сохранённом, -1 — не загружен
    std::map<std::string, uint32_t> used;   // сколько строк ключа уже занято предыдущими таблицами
    std::vector<uint32_t> offset_of;        // по таблице оригинала: её первая строка в сохранённой таблице
    uint32_t loaded = 0, dropped_components = 0;
    for (const sheaf::Table& t : orig->tables) {
        const std::string key = KnownKey(t);
        const auto it = saved_by_key.find(key);
        const bool have = !key.empty() && it != saved_by_key.end();
        const uint32_t off = have ? used[key] : 0;
        offset_of.push_back(off);
        for (uint32_t r = 0; r < t.rows; ++r) new_row.push_back(have ? int64_t{ it->second.second } + off + r : -1);
        if (have) { used[key] += t.rows; loaded += t.rows; }
        for (const sheaf::Component& c : t.components) dropped_components += !ComponentSpecRegistry::Get().ByName(c.name);
    }

    uint32_t cells = 0, mismatches = 0, missing = 0;
    for (size_t ti = 0; ti < orig->tables.size(); ++ti) {
        const sheaf::Table& t = orig->tables[ti];
        const auto it = saved_by_key.find(KnownKey(t));
        if (it == saved_by_key.end()) continue;
        const sheaf::Table& s = *it->second.first;
        const uint32_t off = offset_of[ti];
        for (const sheaf::Component& c : t.components) {
            const auto sc = std::find_if(s.components.begin(), s.components.end(),
                                         [&](const sheaf::Component& x) { return x.name == c.name; });
            if (sc == s.components.end()) continue;
            for (const Column& f : c.fields) {
                const auto sf = std::find_if(sc->fields.begin(), sc->fields.end(),
                                             [&](const Column& x) { return x.name == f.name; });
                if (sf == sc->fields.end() || (f.flags & sheaf::List)) { ++missing; continue; }
                for (uint32_t r = 0; r < t.rows; ++r) {
                    ++cells;
                    const bool p0 = !(f.flags & sheaf::Nullable) || f.present[r];
                    const bool p1 = !(sf->flags & sheaf::Nullable) || sf->present[off + r];
                    uint32_t want = f.values[r];
                    if (f.type == Type::Ref) want = want < new_row.size() && new_row[want] >= 0 ? static_cast<uint32_t>(new_row[want]) : 0;
                    const bool same = p0 == p1 && (!p0 || want == sf->values[off + r]);
                    if (same) continue;
                    if (++mismatches <= 5)
                        SDL_Log("    расхождение: %s.%s строка %u: было %08X, стало %08X", c.name.c_str(), f.name.c_str(), r, want, sf->values[off + r]);
                }
            }
        }
    }

    ObjectManager om2;
    om2.LoadScene("probe", saved);
    const bool stable = om2.SaveScene(om2.GetScene("probe")) == saved;

    SDL_Log("%s: объектов %zu, загружено %u, незарегистрированных компонентов в таблицах %u; сверено "
            "ячеек %u, расхождений %u, полей без пары %u; повторная загрузка %s",
            path, new_row.size(), loaded, dropped_components, cells, mismatches, missing,
            stable ? "совпала байт в байт" : "РАЗОШЛАСЬ");
    return (mismatches == 0 && missing == 0 && stable) ? 0 : 1;
}

int SheafFileTest(const char* path)
{
    const std::vector<uint8_t> bytes = ReadBytes(path);
    auto file = sheaf::Read(bytes);
    if (!file) { SDL_Log("%s: %s", path, file.error().c_str()); return 1; }
    const char* enc_name[] = { "подряд", "одно", "отметки", "уникальные" };
    SDL_Log("%s: %zu Б, строк %zu, таблиц %zu", path, bytes.size(), file->strings.size(), file->tables.size());
    for (const sheaf::Table& t : file->tables) {
        std::string line;
        for (const sheaf::Component& c : t.components) {
            line += " " + c.name + "(";
            for (const Column& f : c.fields)
                line += f.name + ":" + enc_name[std::to_underlying(f.encoding)] + ":" + std::to_string(f.encoded_bytes) + " ";
            line += ")";
        }
        SDL_Log("  %u строк:%s", t.rows, line.c_str());
    }
    const bool same = Rewrites(*file, bytes);
    SDL_Log("  перезапись прочитанного %s", same ? "совпала байт в байт" : "РАЗОШЛАСЬ");
    return same ? 0 : 1;
}


// --- Режим --time: загрузка объектов сцены движком, по этапам ---
// Renderable регистрирует Engine вместе с менеджерами, компоненты игры — Game; ниже их копии, чтобы
// грузилось всё, что грузит игра. Менеджеры без GPU: загрузке Renderable нужны только их Intern.

auto MakeLoadRenderable(MaterialManager* mtm, ModelManager* mdm) {
return [mtm, mdm](Archetype& arch, const sheaf::Component* comp, size_t count, std::span<const std::string> strings)
{
    arch.ensure_component<Renderable>();
    arch.get_array<Renderable>()->reserve(arch.entities.size());
    std::vector<RenderableProxy> rows(count);
    auto column = [comp](const std::string& name, sheaf::Type type, uint8_t flags) -> const sheaf::Column* {
        if (!comp) return nullptr;
        for (const sheaf::Column& c : comp->fields) {
            if (c.name != name) continue;
            if (c.type == type && c.flags == flags) return &c;
            SDL_Log("LoadScene: Renderable.%s in file has unexpected type - defaults kept", name.c_str());
            return nullptr;
        }
        return nullptr;
    };
    std::vector<ModelId>    model_of(strings.size());
    std::vector<MaterialId> material_of(strings.size());
    std::vector<uint8_t>    model_done(strings.size()), material_done(strings.size());
    auto model = [&](uint32_t s) {
        if (!model_done[s]) { model_of[s] = mdm->InternModel(strings[s]); model_done[s] = 1; }
        return model_of[s];
    };
    auto material = [&](uint32_t s) {
        if (!material_done[s]) { material_of[s] = mtm->InternMaterial(strings[s]); material_done[s] = 1; }
        return material_of[s];
    };

    if (const sheaf::Column* c = column("visible", sheaf::Type::Bool, 0))
        for (size_t i = 0; i < count; ++i) rows[i].visible = c->values[i] != 0;
    if (const sheaf::Column* c = column("alpha", sheaf::Type::F32, 0))
        for (size_t i = 0; i < count; ++i) rows[i].alpha = std::bit_cast<float>(c->values[i]);
    if (const sheaf::Column* c = column("flags", sheaf::Type::U32, 0))
        for (size_t i = 0; i < count; ++i) rows[i].flags = c->values[i];
    if (const sheaf::Column* c = column("model", sheaf::Type::Str, 0))
        for (size_t i = 0; i < count; ++i) rows[i].model = model(c->values[i]);

    for (uint32_t L = 0; L < MAX_LOD; ++L) {
        const sheaf::Column* c = column("mat_lod" + std::to_string(L), sheaf::Type::Str, sheaf::List | sheaf::Nullable);
        if (!c) continue;
        size_t k = 0;
        for (size_t i = 0; i < count; ++i) {
            std::vector<MaterialSlot>& parts = rows[i].materials;
            const uint32_t n = c->lengths[i];
            if (parts.size() < n) parts.resize(n);
            for (uint32_t p = 0; p < n; ++p, ++k)
                if (c->present[k]) parts[p].per_lod[L] = material(c->values[k]);
        }
    }

    const sheaf::Column* st_part  = column("state_part",  sheaf::Type::U32, sheaf::List);
    const sheaf::Column* st_role  = column("state_role",  sheaf::Type::U32, sheaf::List);
    const sheaf::Column* st_value = column("state_value", sheaf::Type::U32, sheaf::List);
    if (st_part && st_role && st_value) {
        size_t kp = 0, kr = 0, kv = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t n = std::min({ st_part->lengths[i], st_role->lengths[i], st_value->lengths[i] });
            std::vector<MaterialSlot>& parts = rows[i].materials;
            for (uint32_t q = 0; q < n; ++q) {
                const uint32_t p = st_part->values[kp + q];
                if (p >= parts.size()) continue;
                parts[p].states.emplace_back(static_cast<TextureSlotRole>(safe_u32t_i(st_role->values[kr + q])),
                                             st_value->values[kv + q]);
            }
            kp += st_part->lengths[i];
            kr += st_role->lengths[i];
            kv += st_value->lengths[i];
        }
    }

    auto* a = arch.get_array<Renderable>();
    for (size_t i = 0; i < count; ++i) a->add(rows[i]);
};}

void RegisterGameSpecs(MaterialManager* mtm, ModelManager* mdm)
{
    using enum FieldKind;
    auto& reg = ComponentSpecRegistry::Get();
    reg.Register({ .name = "Renderable", .sig_type = typeid(Renderable),
        .add_default = AddDefaultSoA<Renderable, RenderableProxy>,
        .custom_load = MakeLoadRenderable(mtm, mdm) });
    reg.Register({ .name = "Mass", .sig_type = typeid(MassComponent),
        .add_default = AddDefaultAoS<MassComponent>,
        .fields = { FieldSpec::Num("mass", F32, AOS_NUM(MassComponent, mass), 0, FLT_MAX) } });
    reg.Register({ .name = "Gravity", .sig_type = typeid(GravityComponent),
        .add_default = AddDefaultAoS<GravityComponent>,
        .fields = { FieldSpec::Num("gm", F32, AOS_NUM(GravityComponent, gm), 0, FLT_MAX, 1.0f),
                    FieldSpec::Num("id", U32, AOS_NUM(GravityComponent, id), 0, 0, 1),
                    FieldSpec::Num("core_radius", F32, AOS_NUM(GravityComponent, core_radius), 0, FLT_MAX, 1.0f) } });
    reg.Register({ .name = "Jet", .sig_type = typeid(JetComponent),
        .add_default = AddDefaultAoS<JetComponent>,
        .fields = { FieldSpec::Num("center", U32, AOS_NUM(JetComponent, center), 0, 0, 1) } });
    reg.Register({ .name = "GravityWorld", .sig_type = typeid(GravityWorldComponent),
        .add_default = AddDefaultAoS<GravityWorldComponent>,
        .fields = { FieldSpec::Num("sim_dt", F32, AOS_NUM(GravityWorldComponent, sim_dt), 0, FLT_MAX, 0.001f),
                    FieldSpec::Num("jet_return_distance", F32, AOS_NUM(GravityWorldComponent, jet_return_distance), 0, FLT_MAX, 1.0f) } });
    reg.Register({ .name = "GravitationalLens", .sig_type = typeid(GravitationalLensComponent),
        .add_default = AddDefaultAoS<GravitationalLensComponent>,
        .fields = { FieldSpec::Num("schwarzschild_radius", F32, AOS_NUM(GravitationalLensComponent, schwarzschild_radius), 0, FLT_MAX, 0.1f),
                    FieldSpec::Num("inner_radius", F32, AOS_NUM(GravitationalLensComponent, inner_radius), 0, FLT_MAX, 0.1f) } });
}

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct LoadTimes {
    double parse = 0, specs = 0, entities = 0, rows = 0, data = 0, parents = 0;
    std::map<std::string, double> per_component;
};

// Копия SceneLoader и ObjectManager::LoadScene (ObjectManager.cpp) с таймерами вокруг колбэков:
// «разбор» — sheaf::Read за вычетом колбэков, то есть декодирование вместе с раскладкой полей прямо в
// колонки. Сцена берётся у CreateScene (scenes_data закрыт), entity_revision не двигается.
constexpr Entity kNoEntity = static_cast<Entity>(-1);

class TimedLoader final : public sheaf::TableVisitor {
public:
    TimedLoader(SceneData* scene, LoadTimes& tm) : scene_(scene), tm_(tm) {}

    std::vector<Entity>   created;
    std::vector<Entity>   by_row;
    std::set<std::string> unknown;
    std::vector<std::pair<size_t, size_t>> with_refs;
    double                callbacks = 0;

    void Begin(const sheaf::Table& t, std::span<const std::string>, std::span<sheaf::Destination> dests) override
    {
        auto t0 = Clock::now();
        const size_t base = by_row.size();
        by_row.resize(base + t.rows, kNoEntity);
        specs_.clear();
        arch_ = nullptr;
        std::set<std::type_index> sig;
        size_t first = 0;
        for (const sheaf::Component& c : t.components) {
            if (const ComponentSpec* h = ComponentSpecRegistry::Get().ByName(c.name)) {
                specs_.push_back({ h, &c, first });
                sig.insert(h->sig_type);
            } else {
                unknown.insert(c.name);
            }
            first += c.fields.size();
        }
        if (t.rows == 0 || specs_.empty()) { Spend(tm_.specs, t0); return; }
        arch_ = &scene_->archetypes[sig];
        Spend(tm_.specs, t0);

        t0 = Clock::now();
        arch_->reserve(arch_->entities.size() + t.rows);
        ReserveRows(created, created.size() + t.rows);
        scene_->entity_to_archetype.reserve(scene_->entity_to_archetype.size() + t.rows);
        scene_->entity_to_index.reserve(scene_->entity_to_index.size() + t.rows);
        for (size_t i = 0; i < t.rows; ++i) {
            const Entity e = scene_->next_entity_id++;
            arch_->entities.push_back(e);
            scene_->entity_to_archetype[e] = arch_;
            scene_->entity_to_index[e] = arch_->entities.size() - 1;
            by_row[base + i] = e;
            created.push_back(e);
        }
        const bool refs = std::ranges::any_of(specs_, [](const Spec& s) {
            return std::ranges::any_of(s.header->fields, [](const sheaf::Column& c) { return c.type == sheaf::Type::Ref; });
        });
        if (refs) with_refs.emplace_back(created.size() - t.rows, t.rows);
        Spend(tm_.entities, t0);

        for (const Spec& s : specs_) {
            t0 = Clock::now();
            s.spec->BeginLoad(*arch_, *s.header, t.rows, dests.subspan(s.first_field, s.header->fields.size()));
            tm_.per_component[s.spec->name] += Spend(tm_.rows, t0);
        }
    }

    void End(const sheaf::Table& t, std::span<const std::string> strings) override
    {
        if (!arch_) return;
        for (const Spec& s : specs_) {
            const auto t0 = Clock::now();
            s.spec->FinishLoad(*arch_, *s.header, t.rows, strings);
            tm_.per_component[s.spec->name] += Spend(tm_.data, t0);
        }
    }

private:
    struct Spec {
        const ComponentSpec*    spec;
        const sheaf::Component* header;
        size_t                  first_field;
    };

    double Spend(double& stage, Clock::time_point t0)
    {
        const double ms = MsSince(t0);
        stage += ms;
        callbacks += ms;
        return ms;
    }

    SceneData*        scene_;
    LoadTimes&        tm_;
    Archetype*        arch_ = nullptr;
    std::vector<Spec> specs_;
};

std::vector<Entity> LoadSceneTimed(ObjectManager& om, const SceneName& scene_name, std::span<const uint8_t> bytes, LoadTimes& tm)
{
    SceneData* scene = om.CreateScene(scene_name);
    TimedLoader loader(scene, tm);
    auto t0 = Clock::now();
    const auto read = sheaf::Read(bytes, loader);
    tm.parse = MsSince(t0) - loader.callbacks;
    if (!read) { SDL_Log("LoadScene: scene.sheaf: %s", read.error().c_str()); scene->clear(); return {}; }

    t0 = Clock::now();
    for (const auto& [first, count] : loader.with_refs)
    for (size_t i = first; i < first + count; ++i) {
        const Entity e = loader.created[i];
        if (!om.Has<ParentComponent>(scene, e)) continue;
        ParentComponent& pc = om.GetComponent<ParentComponent>(scene, e);
        const Entity parent = pc.parent < loader.by_row.size() ? loader.by_row[pc.parent] : kNoEntity;
        if (parent == kNoEntity || parent == e) { pc.parent = e; continue; }
        pc.parent = parent;
        scene->children[parent].push_back(e);
    }
    tm.parents = MsSince(t0);

    for (const std::string& n : loader.unknown)
        SDL_Log("LoadScene: component '%s' is not registered - its columns dropped", n.c_str());
    return std::move(loader.created);
}

// --- Для сравнения: json-загрузка движка до перехода на Sheaf (коммит 39e40f1) ---
// ScenePool, ComponentSpec::Load, загрузчик Renderable (Engine.cpp) и ObjectManager::LoadScene —
// копии оттуда; у спеки берутся только add_default и fields, её custom_load теперь читает Sheaf.
namespace json_old {

bool TryGetNum(yyjson_val* v, double& out)
{
    if (yyjson_is_real(v)) { out = yyjson_get_real(v); return true; }
    if (yyjson_is_sint(v)) { out = (double)yyjson_get_sint(v); return true; }
    if (yyjson_is_uint(v)) { out = (double)yyjson_get_uint(v); return true; }
    if (yyjson_is_bool(v)) { out = yyjson_get_bool(v) ? 1.0 : 0.0; return true; }
    return false;
}

constexpr const char* FieldPoolName(FieldKind k) { return k == FieldKind::AssetModel ? "models" : nullptr; }

class ScenePool {
public:
    struct List {
        std::vector<std::string>                  names;
        std::unordered_map<std::string, uint32_t> index;
    };

    List* Find(const std::string& list_name)
    {
        auto it = lists_.find(list_name);
        return it != lists_.end() ? &it->second : nullptr;
    }

    const char* Cell(const List* list, yyjson_val* v)
    {
        if (const char* s = yyjson_get_str(v)) return s;
        if (yyjson_is_uint(v)) {
            const uint64_t i = yyjson_get_uint(v);
            if (list && i < list->names.size()) return list->names[(size_t)i].c_str();
        }
        ++misses_;
        return nullptr;
    }

    void Read(yyjson_val* root)
    {
        size_t k, m; yyjson_val *key, *val;
        yyjson_obj_foreach(root, k, m, key, val) {
            if (!yyjson_is_arr(val)) continue;
            const char* list_name = yyjson_get_str(key);
            if (!list_name) continue;
            List& list = lists_[list_name];
            size_t i, n; yyjson_val* s;
            yyjson_arr_foreach(val, i, n, s) {
                const char* str = yyjson_get_str(s);
                if (!str) { ++misses_; str = ""; }
                list.names.emplace_back(str);
            }
        }
    }

private:
    std::map<std::string, List> lists_;
    uint32_t misses_ = 0;
};

void SpecLoad(const ComponentSpec& spec, Archetype& arch, yyjson_val* comp, size_t count, ScenePool* pool)
{
    for (size_t i = 0; i < count; ++i) spec.add_default(arch);
    if (!comp) return;
    const size_t base = arch.entities.size() - count;
    for (const FieldSpec& f : spec.fields) {
        if (!f.set_num && !f.set_str) continue;
        yyjson_val* col = yyjson_obj_get(comp, f.key);
        if (!col) continue;
        size_t idx, max; yyjson_val* v;
        if (f.set_str) {
            const char* list_name = FieldPoolName(f.kind);
            ScenePool::List* list = (pool && list_name) ? pool->Find(list_name) : nullptr;
            yyjson_arr_foreach(col, idx, max, v) {
                if (idx >= count) break;
                if (const char* s = pool ? pool->Cell(list, v) : yyjson_get_str(v))
                    f.set_str(arch, base + idx, s);
            }
        }
        else {
            yyjson_arr_foreach(col, idx, max, v) {
                if (idx >= count) break;
                double d;
                if (!TryGetNum(v, d)) continue;
                if (f.clamp_on_load) d = d < f.lo ? f.lo : (d > f.hi ? f.hi : d);
                f.set_num(arch, base + idx, d);
            }
        }
    }
}

void LoadRenderable(MaterialManager* mtm, ModelManager* mdm, Archetype& arch, yyjson_val* comp, size_t count, ScenePool* pool)
{
    arch.ensure_component<Renderable>();
    std::vector<RenderableProxy> rows(count);
    ScenePool::List* mat_list = pool ? pool->Find("materials") : nullptr;
    ScenePool::List* mdl_list = pool ? pool->Find("models") : nullptr;
    auto name_of = [pool](ScenePool::List* list, yyjson_val* v) -> const char* {
        return pool ? pool->Cell(list, v) : yyjson_get_str(v);
    };
    auto rows_of = [&](const char* key, auto&& fn) {
        yyjson_val* col = comp ? yyjson_obj_get(comp, key) : nullptr;
        if (!col) return;
        size_t idx, max; yyjson_val* v;
        yyjson_arr_foreach(col, idx, max, v) { if (idx >= count) break; fn(rows[idx], v); }
    };

    rows_of("visible", [](RenderableProxy& p, yyjson_val* v) { p.visible = yyjson_get_bool(v); });
    rows_of("alpha",   [](RenderableProxy& p, yyjson_val* v) { p.alpha = static_cast<float>(yyjson_get_num(v)); });
    rows_of("flags",   [](RenderableProxy& p, yyjson_val* v) { p.flags = safe_u32(yyjson_get_uint(v)); });
    rows_of("model", [&](RenderableProxy& p, yyjson_val* v) {
        if (const char* s = name_of(mdl_list, v)) p.model = mdm->InternModel(s);
    });
    rows_of("materials", [&](RenderableProxy& p, yyjson_val* row) {
        size_t k, km; yyjson_val* lv;
        yyjson_arr_foreach(row, k, km, lv) {
            MaterialSlot& part = p.materials.emplace_back();
            size_t L, lm; yyjson_val* v;
            yyjson_arr_foreach(lv, L, lm, v) {
                if (L >= MAX_LOD) break;
                if (yyjson_is_null(v)) continue;
                if (const char* s = name_of(mat_list, v)) part.per_lod[L] = mtm->InternMaterial(s);
            }
        }
    });
    rows_of("states", [](RenderableProxy& p, yyjson_val* row) {
        size_t j, jm; yyjson_val* pairs;
        yyjson_arr_foreach(row, j, jm, pairs) {
            if (j >= p.materials.size()) break;
            auto& st = p.materials[j].states;
            const size_t n = yyjson_arr_size(pairs) & ~size_t(1);
            std::vector<int64_t> flat; flat.reserve(n);
            size_t k, km; yyjson_val* v;
            yyjson_arr_foreach(pairs, k, km, v) { if (flat.size() >= n) break; flat.push_back(yyjson_get_sint(v)); }
            for (size_t q = 0; q + 1 < flat.size(); q += 2)
                st.emplace_back(static_cast<TextureSlotRole>(flat[q]),
                                static_cast<uint32_t>(flat[q + 1] < 0 ? 0 : flat[q + 1]));
        }
    });

    auto* a = arch.get_array<Renderable>();
    for (size_t i = 0; i < count; ++i) a->add(rows[i]);
}

std::vector<Entity> LoadSceneTimed(ObjectManager& om, MaterialManager* mtm, ModelManager* mdm, const SceneName& scene_name,
                                   const std::string& text, LoadTimes& tm)
{
    std::vector<Entity> created;
    SceneData* scene = om.CreateScene(scene_name);
    auto& reg = ComponentSpecRegistry::Get();

    std::unordered_map<uint32_t, Entity> old_to_new;

    auto t0 = Clock::now();
    yyjson_doc* doc = yyjson_read(text.data(), text.size(), 0);
    if (!doc) { SDL_Log("LoadScene: scene.json parse failed"); return created; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    ScenePool pool;
    pool.Read(root);
    tm.parse = MsSince(t0);

    t0 = Clock::now();
    size_t est = 0;
    {
        size_t ak, am; yyjson_val *an, *ab;
        yyjson_obj_foreach(root, ak, am, an, ab) est += (size_t)yyjson_get_uint(yyjson_obj_get(ab, "count"));
    }
    created.reserve(est);
    old_to_new.reserve(est * 2);
    scene->entity_to_archetype.reserve(scene->entity_to_archetype.size() + est);
    scene->entity_to_index.reserve(scene->entity_to_index.size() + est);
    tm.entities += MsSince(t0);

    {
        size_t ak, am; yyjson_val *aname, *block;
        yyjson_obj_foreach(root, ak, am, aname, block) {
            if (!yyjson_is_obj(block)) continue;
            t0 = Clock::now();
            std::vector<const ComponentSpec*> hs;
            std::set<std::type_index> sig;
            {
                size_t ck, cm; yyjson_val *cname, *cval;
                yyjson_obj_foreach(block, ck, cm, cname, cval) {
                    const char* nm = yyjson_get_str(cname);
                    if (!nm || !std::strcmp(nm, "count") || !std::strcmp(nm, "entities")) continue;
                    const ComponentSpec* h = reg.ByName(nm);
                    if (h) { hs.push_back(h); sig.insert(h->sig_type); }
                }
            }
            yyjson_val* ents  = yyjson_obj_get(block, "entities");
            yyjson_val* cnt_v = yyjson_obj_get(block, "count");
            size_t count = cnt_v ? (size_t)yyjson_get_uint(cnt_v) : (ents ? yyjson_arr_size(ents) : 0);
            if (count == 0 || hs.empty()) { tm.specs += MsSince(t0); continue; }
            Archetype& arch = scene->archetypes[sig];
            tm.specs += MsSince(t0);

            t0 = Clock::now();
            std::vector<uint32_t> ids(count, 0);
            if (ents) { size_t i, m; yyjson_val* v; yyjson_arr_foreach(ents, i, m, v) { if (i >= count) break; ids[i] = (uint32_t)yyjson_get_uint(v); } }
            for (size_t i = 0; i < count; ++i) {
                Entity e = scene->next_entity_id++;
                arch.entities.push_back(e);
                scene->entity_to_archetype[e] = &arch;
                scene->entity_to_index[e] = arch.entities.size() - 1;
                old_to_new[ids[i]] = e;
                created.push_back(e);
            }
            tm.entities += MsSince(t0);

            for (const ComponentSpec* h : hs) {
                t0 = Clock::now();
                yyjson_val* comp = yyjson_obj_get(block, h->name.c_str());
                if (h->name == "Renderable") LoadRenderable(mtm, mdm, arch, comp, count, &pool);
                else                         SpecLoad(*h, arch, comp, count, &pool);
                const double ms = MsSince(t0);
                tm.data += ms;
                tm.per_component[h->name] += ms;
            }
        }
    }

    t0 = Clock::now();
    for (Entity e : created) {
        if (!om.Has<ParentComponent>(scene, e)) continue;
        ParentComponent& pc = om.GetComponent<ParentComponent>(scene, e);
        auto it = old_to_new.find(pc.parent);
        if (it == old_to_new.end()) { pc.parent = e; continue; }
        pc.parent = it->second;
        scene->children[pc.parent].push_back(e);
    }
    tm.parents = MsSince(t0);

    yyjson_doc_free(doc);
    return created;
}

} // namespace json_old

// Файл читается, как его читал Engine::LoadScene: одним read в буфер.
void TimeTest(const char* path, MaterialManager* mtm, ModelManager* mdm)
{
    const bool json = std::string_view(path).ends_with(".json");
    auto t0 = Clock::now();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { SDL_Log("Нет '%s'.", path); return; }
    std::string text(static_cast<size_t>(in.tellg()), '\0');
    in.seekg(0);
    in.read(text.data(), safe_size_ss(text.size()));
    const double disk = MsSince(t0);

    ObjectManager om;
    LoadTimes tm;
    t0 = Clock::now();
    const size_t n = json
        ? json_old::LoadSceneTimed(om, mtm, mdm, "time", text, tm).size()
        : LoadSceneTimed(om, "time", std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), tm).size();
    const double load = MsSince(t0);

    std::string comps;
    for (const auto& [name, ms] : tm.per_component)
        if (ms >= 1.0) comps += "\n      " + name + ": " + std::to_string(static_cast<int>(ms)) + " мс";
    SDL_Log("%s: %.1f МБ, объектов %zu\n"
            "  чтение с диска:                 %8.1f мс\n"
            "  разбор файла:                   %8.1f мс\n"
            "  создание энтити:                %8.1f мс\n"
            "  строки компонентов (BeginLoad): %8.1f мс\n"
            "  обработка данных (Load):        %8.1f мс%s\n"
            "  поиск спек и архетипов:         %8.1f мс\n"
            "  родители (проход 2):            %8.1f мс\n"
            "  итого:                          %8.1f мс",
            path, text.size() / (1024.0 * 1024.0), n, disk, tm.parse, tm.entities, tm.rows, tm.data, comps.c_str(),
            tm.specs, tm.parents, disk + load);
}


// Порча файла на пути движка: каждый префикс и каждый байт с инвертированными битами.
// ObjectManager::LoadScene не падает, а при ошибке оставляет сцену пустой.
int EngineDamageTest(const char* path)
{
    const std::vector<uint8_t> bytes = ReadBytes(path);
    int bad = 0, failed = 0, loaded = 0;
    auto check = [&](std::span<const uint8_t> b) {
        ObjectManager om;
        const auto r = om.LoadScene("damage", b);
        if (r) { ++loaded; return; }
        ++failed;
        const SceneData* s = om.GetScene("damage");
        if (!s->archetypes.empty() || !s->entity_to_archetype.empty()) ++bad;
    };
    for (size_t n = 0; n < bytes.size(); ++n) check(std::span(bytes.data(), n));
    std::vector<uint8_t> broken = bytes;
    for (size_t i = 0; i < bytes.size(); ++i) {
        broken[i] ^= 0xFF;
        check(broken);
        broken[i] = bytes[i];
    }
    SDL_Log("%s: загрузок %zu, ошибка %d (сцена осталась непустой: %d), загружено %d",
            path, 2 * bytes.size(), failed, bad, loaded);
    return bad;
}


// Ссылки Parent переживают сохранение и загрузку: корень, четыре его ребёнка и внук. У загруженных
// детей родитель — загруженный объект, и они лежат в его children.
int ParentTest()
{
    auto& reg = ComponentSpecRegistry::Get();
    const std::vector<const ComponentSpec*> plain = { reg.ByName("Transform") };
    const std::vector<const ComponentSpec*> child = { reg.ByName("Transform"), reg.ByName("Parent") };
    ObjectManager om;
    SceneData* scene = om.CreateScene("p");
    const Entity root = om.CreateEntityFromSpecs(scene, plain);
    std::vector<Entity> kids;
    for (int i = 0; i < 5; ++i) kids.push_back(om.CreateEntityFromSpecs(scene, child));
    for (int i = 0; i < 4; ++i) om.GetComponent<ParentComponent>(scene, kids[i]).parent = root;
    om.GetComponent<ParentComponent>(scene, kids[4]).parent = kids[0];

    ObjectManager loaded_om;
    const auto loaded = loaded_om.LoadScene("p", om.SaveScene(scene));
    if (!loaded) { SDL_Log("родители: %s", loaded.error().c_str()); return 1; }
    SceneData* s = loaded_om.GetScene("p");
    Entity root2 = 0;
    std::vector<Entity> with_parent;
    for (Entity e : *loaded) (loaded_om.Has<ParentComponent>(s, e) ? with_parent.push_back(e) : void(root2 = e));
    int to_root = 0, to_kid = 0, in_children = 0;
    for (Entity e : with_parent) {
        const Entity p = loaded_om.GetComponent<ParentComponent>(s, e).parent;
        if (p == root2) ++to_root;
        else if (p != e && loaded_om.Has<ParentComponent>(s, p)) ++to_kid;
        const auto it = s->children.find(p);
        if (it != s->children.end() && std::ranges::count(it->second, e) == 1) ++in_children;
    }
    const bool ok = with_parent.size() == 5 && to_root == 4 && to_kid == 1 && in_children == 5;
    SDL_Log("родители: детей %zu, к корню %d, к ребёнку %d, в children %d — %s",
            with_parent.size(), to_root, to_kid, in_children, ok ? "сошлось" : "РАЗОШЛОСЬ");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    RegisterBuiltinComponentSpecs();
    if (argc > 2 && std::string_view(argv[1]) == "--damage") {
        RegisterBuiltinComponentSpecs();
        MaterialManager mtm;
        ModelManager    mdm;
        RegisterGameSpecs(&mtm, &mdm);
        int bad = 0;
        for (int i = 2; i < argc; ++i) bad += EngineDamageTest(argv[i]);
        SDL_Log(bad ? "ПРОВАЛ" : "OK");
        return bad ? 1 : 0;
    }
    if (argc > 2 && std::string_view(argv[1]) == "--time") {
        RegisterBuiltinComponentSpecs();
        MaterialManager mtm;
        ModelManager    mdm;
        RegisterGameSpecs(&mtm, &mdm);
        for (int i = 2; i < argc; ++i) TimeTest(argv[i], &mtm, &mdm);
        return 0;
    }
    int bad = SyntheticTest() + DamageTest() + ParentTest();
    for (int i = 1; i < argc; ++i) bad += SheafFileTest(argv[i]) + SceneTest(argv[i]);
    SDL_Log(bad ? "ПРОВАЛ" : "OK");
    return bad ? 1 : 0;
}
