add_executable(Game)

target_sources(Game PRIVATE
    src/main.cpp
    src/Game.cpp
    src/GravityScene.cpp
    src/GravitySystem.cpp
    src/GravitationalLensPassSet.cpp
    src/GravitationalLensDataModule.cpp
    src/GravityCenterDataModule.cpp
    src/GameUpdateSet.cpp
    src/SandboxScene.cpp
    src/GameShaderSet.cpp
    src/GameComponents.cpp
)

target_include_directories(Game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Game PRIVATE Engine Physics)

# Вердикт векторизации по каждому циклу (info C5001/C5002). Стоит постоянно: цена нулевая
# (только диагностика), а без него scripts/vec_check.py не может проверить метки VEC_HOT.
target_compile_options(Game PRIVATE /Qvec-report:2)

# PCH: std/SDL/glm + фасад движка. PCH_Engine.h тут не годится — он доклеивает imgui.h,
# а ImGui линкуется к Engine как PRIVATE, так что игре его include-пути не достаются.
# Заголовки названы, а не адресованы путём: резолвит их include-путь цели, поэтому переезд
# файла между ярусами ломается здесь так же, как обычное включение.
target_precompile_headers(Game PRIVATE [["PCH.h"]] [["Engine.h"]])

set_target_properties(Game PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

copy_runtime_dlls(Game)

# ============================================================
# INSTALL — запускается только cmake --install
# ============================================================

# Сам exe
install(TARGETS Game
    RUNTIME DESTINATION .       # Game.exe ляжет в корень dist/
)

# DLL-ки. SDL3.dll теперь собирается нами (external/SDL3), поэтому берём её у цели,
# а не по фиксированному пути; остальные — готовые бинарники.
install(FILES
    $<TARGET_FILE:SDL3::SDL3>
    "${XOR_ENGINE_DIR}/external/SDL3_image/lib/x64/SDL3_image.dll"
    DESTINATION .
)

# Ресурсы игры
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/models/"
    DESTINATION models
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/textures/"
    DESTINATION textures
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sounds/"
    DESTINATION sounds
)
