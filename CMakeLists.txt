cmake_minimum_required(VERSION 3.9)
project(my-rcdrivers-project)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

# This is what you cloned previously.
# After this line an rcdrivers target is available for linking
add_subdirectory(raspberry-sbus)

# Use pkg-config to find SDL2 and SDL2_ttf. This is more reliable than
# find_package(SDL2_ttf ...) on Debian/Raspberry Pi OS, where the packaged
# SDL2_ttf CMake config exposes an imported target (SDL2_ttf::SDL2_ttf)
# rather than the plain "SDL2_ttf" name that was linked before.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)

# --- Original target (no display) ---
add_executable(main RpiJoy.cpp)
target_link_libraries(main PUBLIC rcdrivers)

# --- New target with SDL2 channel display ---
add_executable(main_sdl RpiJoySDL.cpp)
target_include_directories(main_sdl PUBLIC ${SDL2_INCLUDE_DIRS} ${SDL2_TTF_INCLUDE_DIRS})
target_link_directories(main_sdl PUBLIC ${SDL2_LIBRARY_DIRS} ${SDL2_TTF_LIBRARY_DIRS})
target_link_libraries(main_sdl PUBLIC rcdrivers ${SDL2_LIBRARIES} ${SDL2_TTF_LIBRARIES})
target_compile_options(main_sdl PUBLIC ${SDL2_CFLAGS_OTHER} ${SDL2_TTF_CFLAGS_OTHER})