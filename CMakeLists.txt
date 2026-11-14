cmake_minimum_required(VERSION 3.16)
project(aow2_native LANGUAGES CXX)

# The earlier hand-written prototype is kept only in local copies of the project.
if(EXISTS "${CMAKE_SOURCE_DIR}/src/world.cpp")
    option(AOW2_PROTOTYPE "Also build the earlier hand-written prototype and its checks" ON)
endif()
option(AOW2_STATIC_DEPS "Link libpng, zlib and the C++ runtime statically (for handheld builds)" OFF)

if(AOW2_STATIC_DEPS)
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

find_package(SDL2 REQUIRED CONFIG)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(AOW2_JAR "${CMAKE_SOURCE_DIR}/ArtOfWar2__Nokia_5800.jar" CACHE FILEPATH "Original Nokia 5800 game JAR")
enable_testing()

# The port: runs the original game code from the JAR on a small built-in Java runtime.
add_executable(aow2 runtime/main.cpp runtime/jar.cpp runtime/vm.cpp runtime/natives_core.cpp runtime/natives_midp.cpp
                    runtime/midi.cpp)
target_compile_features(aow2 PRIVATE cxx_std_17)
# Debian's SDL2 config target name differs between versions.
if(TARGET SDL2::SDL2)
    target_link_libraries(aow2 PRIVATE SDL2::SDL2)
else()
    target_include_directories(aow2 PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(aow2 PRIVATE ${SDL2_LIBRARIES})
endif()
target_link_libraries(aow2 PRIVATE PNG::PNG ZLIB::ZLIB Threads::Threads)
if(AOW2_STATIC_DEPS)
    target_link_options(aow2 PRIVATE -static-libstdc++ -static-libgcc)
endif()
# Boots the original game headless to its main menu and checks that it keeps painting.
add_test(NAME runtime-boot
         COMMAND aow2 "${AOW2_JAR}" --headless 20 --speed 2 --saves "${CMAKE_BINARY_DIR}/test-saves"
                 --tap 8:20,340 --shot 19:${CMAKE_BINARY_DIR}/runtime-menu.png --min-frames 80)

if(AOW2_PROTOTYPE)
    find_package(LibArchive REQUIRED)
    # Game data loading and battle simulation, shared by the prototype and its tests.
    add_library(aow2-core STATIC src/assets.cpp src/world.cpp)
    target_compile_features(aow2-core PUBLIC cxx_std_17)
    target_include_directories(aow2-core PUBLIC src)
    target_link_libraries(aow2-core PUBLIC SDL2::SDL2 PNG::PNG LibArchive::LibArchive)

    add_executable(aow2-native src/main.cpp)
    target_link_libraries(aow2-native PRIVATE aow2-core)

    # The tests read the original game data. Run them with: ctest --test-dir build
    foreach(name movement economy combat campaign reconnaissance rebel-attack building-combat)
        add_executable(verify-${name} tests/verify-${name}.cpp)
        target_link_libraries(verify-${name} PRIVATE aow2-core)
        # The tests rely on assert, so keep it active in Release builds.
        target_compile_options(verify-${name} PRIVATE -UNDEBUG)
        add_test(NAME ${name} COMMAND verify-${name} "${AOW2_JAR}" "${CMAKE_BINARY_DIR}/${name}.bmp"
                 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}") # some tests also write build/*.bmp
    endforeach()
endif()
