add_executable(Sandbox)

# Площадки для запуска подсистем движка по отдельности.
# Каждый сценарий — свой .cpp; main решает, что именно запустить.
set(SANDBOX_SRC
    src/DxcIncludeProbe.cpp
    # src/CullingWaveProbe.cpp          # ЗОНД: culling_scatter с wave-свёрткой атомиков против поштучного атомика (см. шапку файла)
    # src/ShadercrossBuildProbe.cpp   # ЗОНД: shadercross из исходников компилирует и рефлексирует, DXC берётся из каталога exe (см. шапку файла)
    # src/VulkanVersionProbe.cpp      # ЗОНД: отсечение устройств по объявленной версии Vulkan (см. шапку файла)
    # src/IndirectEmptyCmdProbe.cpp   # ЗОНД: цена пустых команд (num_instances = 0) в indirect-мультидроу (см. шапку файла)
    # src/FrustumVecProbe.cpp         # ЗОНД: фрустум-отсев 1М объектов на CPU через ECS - форма, которая векторизуется
    # src/TransposeVecProbe.cpp       # ЗОНД: нужны ли интринсики в TransformDataModule (транспонирование SoA->AoS и умножение)
    # src/GravityVecProbe.cpp           # ЗОНД: векторизуется ли гравитационный шаг через ForEach (без интринсиков)
    # src/QueueThreadingProbe.cpp       # зонд: очереди под конвейером потоков движка (ThreadController+SlotController) — потокобезопасность  (см. историю)
    # src/ShaderVramProbe.cpp         # зонд: цена резидентного шейдера — модуль/пайплайн, VRAM vs RAM (см. шапку файла)
    # src/TransferQueueProbe.cpp      # зонд: три очереди последовательно, без потоков (см. шапку файла)
    # src/CopyComputeBarrierProbe.cpp # зонд: порядок нескольких операций над одним буфером в одном cb, без fence (см. шапку файла)
    # src/ClaimWindowProbe.cpp        # зонд: claim окна GPU-девайсом — им найден баг «первого окна» в SDL 3.4 (см. шапку файла)
    # src/main.cpp              # зонд: TDM store под контеншеном памяти
    # src/SheafProbe.cpp       # зонд: писатель и читатель Sheaf — синтетика всех типов и способов, порча файла, готовые .sheaf и их путь через ECS (см. шапку файла)
    # src/BlitUsageProbe.cpp    # зонд: какие usage-флаги требуют blit/copy/mip-gen (см. шапку файла)
    # src/CameraScene.cpp
    # src/BufferScene.cpp
)

target_sources(Sandbox PRIVATE ${SANDBOX_SRC})
source_group("Source" FILES ${SANDBOX_SRC})

# Свои заголовки песочницы. Публичный API движка и его PUBLIC-зависимости
# (glm, SDL3_image, а после правки в engine — и SDL3) приходят транзитивно от Engine.
target_include_directories(Sandbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Sandbox PRIVATE Engine)

# ЗОНД: вердикт автовекторизации по каждому циклу (info C5001/C5002 + код причины).
target_compile_options(Sandbox PRIVATE /Qvec-report:2)

# PCH: только std/SDL/glm — Sandbox дергает подсистемы точечно, Engine.h ему не нужен.
target_precompile_headers(Sandbox PRIVATE [["PCH.h"]])

set_target_properties(Sandbox PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../game"
)

# DLL рядом с exe, иначе Sandbox.exe соберётся, но не запустится.
copy_runtime_dlls(Sandbox)