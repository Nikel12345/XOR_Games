#include "PCH.h"
#include "Engine.h"
#include "Game.h"

extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

int main() {
    EngineConfig cfg;
    cfg.title = "GPU-triangle (basic)";
    cfg.present_mode = SDL_GPU_PRESENTMODE_MAILBOX;

    Engine engine(cfg);
    if (!engine.IsValid()) return 1;

    Game game(&engine);
    game.MainInit();

    engine.SetGameIterate([&game] { game.MainIterate(); });
    return engine.Run();
}
