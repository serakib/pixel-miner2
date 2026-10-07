cmake_minimum_required(VERSION 3.16)
project(pixel_miner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(pixel_miner src/main.cpp)

set_target_properties(pixel_miner PROPERTIES SUFFIX ".html")

target_link_options(pixel_miner PRIVATE
    "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
    "-sWASM=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sASSERTIONS=1"
    "-sNO_EXIT_RUNTIME=1"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
    "-sEXPORTED_FUNCTIONS=['_main','_move_player','_toggle_pause','_restart_game']"
    "-sENVIRONMENT=web"
    "-O3"
)
