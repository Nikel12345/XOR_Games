// Зонд: проверка писателя и читателя Sheaf без GPU и окна.
//  1. Синтетика: колонки всех типов и флагов с данными, на которых писатель выбирает каждый из
//     четырёх способов записи (включая таблицу уникальных с номерами шириной 1, 2 и 4), —
//     запись, чтение, сравнение колонок значение за значением.
//  2. Обрезанный файл (каждый префикс) и лишний байт обязаны дать ошибку; файл с одним испорченным
//     байтом (каждая позиция) — ошибку или успех, но не падение.
//  3. Если передан json сцены: та же сцена в scene.json и scene.sheaf из одной загрузки (их сверка
//     значение за значением — compare.py), и scene.sheaf, прочитанный и записанный заново, обязан
//     совпасть с собой байт в байт.
//  4. Если переданы готовые .sheaf (например, из saved_scene редактора): каждый читается, его таблицы
//     печатаются, и перезапись прочитанного обязана совпасть с файлом байт в байт.
// Renderable здесь не зарегистрирован (его спека живёт в Engine вместе с менеджерами), и колонки
// Renderable из входного json загрузка отбрасывает.
// Запуск: Sandbox [<вход.json> <каталог для вывода>] [<файл.sheaf> ...].
#include "PCH.h"
#include "ObjectManager.h"
#include "ComponentSerializer.h"
#include "Sheaf.h"
#include <fstream>
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

int SceneTest(const char* in_path, const std::string& dir)
{
    std::ifstream in(in_path, std::ios::binary);
    if (!in) { SDL_Log("Нет '%s'.", in_path); return 1; }
    std::ostringstream ss; ss << in.rdbuf();

    RegisterBuiltinComponentSpecs();
    ObjectManager om;
    SceneData* scene = om.CreateScene("probe");
    om.LoadScene("probe", ss.str());

    const std::string json = om.SaveScene(scene);
    const std::vector<uint8_t> bytes = om.SaveSceneSheaf(scene);
    { std::ofstream f(dir + "/scene.json",  std::ios::binary); f.write(json.data(), safe_size_ss(json.size())); }
    { std::ofstream f(dir + "/scene.sheaf", std::ios::binary);
      f.write(reinterpret_cast<const char*>(bytes.data()), safe_size_ss(bytes.size())); }

    auto file = sheaf::Read(bytes);
    if (!file) { SDL_Log("scene.sheaf: %s", file.error().c_str()); return 1; }
    const bool same = Rewrites(*file, bytes);
    SDL_Log("сцена: scene.json %zu Б, scene.sheaf %zu Б, таблиц %zu; перезапись прочитанного %s",
            json.size(), bytes.size(), file->tables.size(), same ? "совпала байт в байт" : "РАЗОШЛАСЬ");
    return same ? 0 : 1;
}

int SheafFileTest(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
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

} // namespace

int main(int argc, char** argv)
{
    int bad = SyntheticTest() + DamageTest();
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]).ends_with(".sheaf")) bad += SheafFileTest(argv[i]);
        else if (i + 1 < argc) { bad += SceneTest(argv[i], argv[i + 1]); ++i; }
    }
    SDL_Log(bad ? "ПРОВАЛ" : "OK");
    return bad ? 1 : 0;
}
