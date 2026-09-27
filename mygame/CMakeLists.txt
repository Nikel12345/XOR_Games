add_executable(MyGame)

target_sources(MyGame PRIVATE
    src/main.cpp
    src/MyGame.cpp
    src/FractalShaderSet.cpp
    src/FractalUpdateSet.cpp
)

target_include_directories(MyGame PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(MyGame PRIVATE Engine Physics)

target_precompile_headers(MyGame PRIVATE [["PCH.h"]] [["Engine.h"]])

set_target_properties(MyGame PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

copy_runtime_dlls(MyGame)

# ============================================================
# INSTALL — запускается только cmake --install
# ============================================================

# Сам exe
install(TARGETS MyGame
    RUNTIME DESTINATION .       # MyGame.exe ляжет в корень dist/
)

# DLL-ки. SDL3.dll теперь собирается нами (external/SDL3), поэтому берём её у цели,
# а не по фиксированному пути; остальные — готовые бинарники.
install(FILES
    $<TARGET_FILE:SDL3::SDL3>
    "${XOR_ENGINE_DIR}/external/SDL3_image/lib/x64/SDL3_image.dll"
    DESTINATION .
)


# Ресурсы игры — раскомментируй и заведи каталоги, когда появятся свои ассеты:
# install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/models/"   DESTINATION models)
# install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/textures/" DESTINATION textures)
# install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sounds/"   DESTINATION sounds)
