// Зонд: round-trip scene.json через ObjectManager::Load/SaveScene, БЕЗ GPU и окна.
// Зачем: проверить раскладку файла (колонки компонентов пишутся в строчку — InlineScalarArrays)
// и то, что перезапись не теряет данные: load(A) → save → load(B) → save должно дать B == C.
// Запуск: рабочая директория src/game (или передай путь к scene.json аргументом).
#include "PCH.h"
#include "ObjectManager.h"
#include "ComponentSerializer.h"
#include <fstream>
#include <sstream>

static std::string ReadFileText(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "saved_scene/scene1/scene.json";
    const std::string text = ReadFileText(path);
    if (text.empty()) { SDL_Log("Нет '%s' (рабочая директория должна быть src/game).", path); return 1; }

    RegisterBuiltinComponentSpecs();
    ObjectManager om;

    SceneData* a = om.CreateScene("a");
    om.LoadScene("a", text);
    const std::string save1 = om.SaveScene(a);

    // Второй круг: то, что записали, должно читаться и записываться байт-в-байт так же.
    SceneData* b = om.CreateScene("b");
    om.LoadScene("b", save1);
    const std::string save2 = om.SaveScene(b);

    SDL_Log("Архетипов: %zu   исходник: %zu Б   после save: %zu Б   строк: %zu",
            a->archetypes.size(), text.size(), save1.size(),
            (size_t)std::count(save1.begin(), save1.end(), '\n') + 1);
    SDL_Log("Round-trip save1 == save2: %s", save1 == save2 ? "ДА" : "НЕТ");

    { std::ofstream f("__probe_scene.json",  std::ios::binary); f.write(save1.data(), (std::streamsize)save1.size()); }
    { std::ofstream f("__probe_scene2.json", std::ios::binary); f.write(save2.data(), (std::streamsize)save2.size()); }
    SDL_Log("Записан __probe_scene.json (глянь раскладку глазами).");
    return save1 == save2 ? 0 : 2;
}
