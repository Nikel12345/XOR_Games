cmake_minimum_required(VERSION 3.20)
project(XOR_Games)

set(XOR_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "")

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/MP>")

add_subdirectory("${XOR_ENGINE_DIR}" xor_engine)

add_subdirectory(game)
add_subdirectory(mygame)
add_subdirectory(sandbox)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Game)
