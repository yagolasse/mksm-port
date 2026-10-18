cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Mortal Kombat: Shaolin Monks (Xbox) - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(mksm_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# ── SDL2 (plan B9a1) ────────────────────────────────────────────
#
# MKSM_SDL_WINDOW=ON (default): the game window and its keys go through SDL2
# (toolkit XBOX_SDL_WINDOW, src/video/fb_present_sdl.c) instead of the GDI window.
# SDL2 is built from its source here and linked static (one exe, no SDL2.dll;
# zlib licence, see NOTICE). OFF = the GDI window, for A/B runs.
option(MKSM_SDL_WINDOW "Game window through SDL2 (static) instead of GDI" ON)
if(NOT WIN32)
    # POSIX (plan H.5c2): the toolkit always uses SDL2 and finds the system's (find_package).
    set(MKSM_SDL_WINDOW ON)
elseif(MKSM_SDL_WINDOW)
    include(FetchContent)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.32.10
        GIT_SHALLOW    ON)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL2)
    # The toolkit links SDL2::SDL2.
    if(NOT TARGET SDL2::SDL2)
        add_library(SDL2::SDL2 ALIAS SDL2-static)
    endif()
    set(XBOX_SDL_WINDOW ON)
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")
if(NOT WIN32)
    # Lifted CRT functions named like libc's (write, close, ...) would take over the toolkit's
    # libc calls on POSIX: rename them in the generated code (src/recomp_libc_names.h).
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS
        "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/recomp_libc_names.h")
endif()

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/settings.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)
if(WIN32)
    list(APPEND GAME_SOURCES src/prof.c src/setup.c src/xiso.c)
else()
    # POSIX (plan H.5c2a): the profiler, the first-launch setup and the disc extractor are
    # Win32 for now; src/posix_stubs.c stands in for them.
    list(APPEND GAME_SOURCES src/posix_stubs.c)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        xinput
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
        # CNG hashes for the default.xbe version check (src/xiso.c, B10b1).
        bcrypt
        # The first-launch setup's progress window (src/setup.c, B10b2).
        comctl32
    )
else()
    # dladdr names the faulting function in the crash report (src/main.c); -rdynamic puts the
    # generated sub_XXXXXXXX symbols in the dynamic table it reads.
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
    set_target_properties(${PROJECT_NAME} PROPERTIES ENABLE_EXPORTS ON)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    # Debug info in every config: the fault handler resolves host RIP to the
    # generated sub_XXXXXXXX symbol through dbghelp, which needs a PDB.
    # /Z7 keeps debug info in each .obj (no shared PDB server during parallel
    # compiles); /DEBUG:FULL makes the linker write a standalone .pdb.
    # /DEBUG turns off /OPT:REF and /OPT:ICF, so turn them back on for Release.
    target_compile_options(${PROJECT_NAME} PRIVATE /Z7)
    target_link_options(${PROJECT_NAME} PRIVATE
        /DEBUG:FULL
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
    )
endif()

# ── GPU renderer library (xemu PGRAPH, src/gpu) ─────────────────
#
# xemu's renderer is GNU C that cl rejects, so src/gpu is its own CMake project
# built with clang-cl + Ninja (MSVC-ABI objects) and imported here as a static
# library. cl-built code talks to it only through src/gpu/mksm_gpu.h; gpu_link is
# the cl/link.exe link check.
# The null renderer registers itself from a constructor in an object nothing
# references, so consumers must link the archive whole.
set(MKSM_CLANG_CL "C:/clang+llvm-18.1.8-x86_64-pc-windows-msvc/bin/clang-cl.exe" CACHE FILEPATH
    "clang-cl for the GPU renderer library (src/gpu); empty or missing skips it")
find_program(MKSM_NINJA ninja)
if(WIN32 AND MKSM_CLANG_CL AND EXISTS "${MKSM_CLANG_CL}" AND MKSM_NINJA)
    include(ExternalProject)
    set(MKSM_GPU_BIN "${CMAKE_BINARY_DIR}/mksm_gpu")
    set(MKSM_GPU_LIB "${MKSM_GPU_BIN}/mksm_gpu.lib")
    # GL contexts: through SDL2 with the SDL window (plan B9a2; the lib compiles against SDL's
    # headers, the game links the static SDL), WGL with the GDI window.
    if(MKSM_SDL_WINDOW)
        set(MKSM_GPU_GL_ARGS
            "-DMKSM_GL_SDL_INCLUDE=${sdl2_BINARY_DIR}/include/SDL2|${sdl2_BINARY_DIR}/include-config-release/SDL2")
        set(MKSM_GPU_GL_LIBS "SDL2::SDL2;opengl32")
    else()
        set(MKSM_GPU_GL_ARGS "-DMKSM_GL_SDL_INCLUDE=")
        set(MKSM_GPU_GL_LIBS "opengl32;gdi32;user32")
    endif()
    # Always Release with /MD: only the game's Release build will link it.
    ExternalProject_Add(mksm_gpu_build
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu"
        BINARY_DIR "${MKSM_GPU_BIN}"
        CMAKE_GENERATOR Ninja
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_C_COMPILER=${MKSM_CLANG_CL}"
            "-DCMAKE_MAKE_PROGRAM=${MKSM_NINJA}"
            ${MKSM_GPU_GL_ARGS}
        LIST_SEPARATOR |
        BUILD_ALWAYS ON
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${MKSM_GPU_LIB}"
    )
    add_library(mksm_gpu STATIC IMPORTED GLOBAL)
    set_target_properties(mksm_gpu PROPERTIES
        IMPORTED_LOCATION "${MKSM_GPU_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu"
        # The OpenGL renderer's contexts: SDL (gloffscreen_sdl.c) or WGL (gloffscreen_wgl.c).
        INTERFACE_LINK_LIBRARIES "${MKSM_GPU_GL_LIBS}")
    add_dependencies(mksm_gpu mksm_gpu_build)
    if(MKSM_SDL_WINDOW)
        # SDL's generated headers (SDL_config.h per config) exist once its Release build ran.
        add_dependencies(mksm_gpu_build SDL2-static)
    endif()

    add_executable(gpu_link src/gpu/gpu_link.c)
    target_link_libraries(gpu_link PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,mksm_gpu>")

    # The game: RECOMP_PB_GPU=1 hands the pushbuffer to it (src/main.c). The lib is always
    # Release /MD, so the Debug (/MDd) config builds without it.
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:$<LINK_LIBRARY:WHOLE_ARCHIVE,mksm_gpu>>")
    target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:MKSM_HAVE_GPU=1>")
    if(NOT MKSM_SDL_WINDOW)
        target_compile_definitions(${PROJECT_NAME} PRIVATE MKSM_GL_WGL=1)
    endif()
else()
    message(STATUS "mksm_gpu skipped: set MKSM_CLANG_CL to clang-cl.exe and put ninja on PATH")
endif()

# ── Settings menu (plan 4.6, B7a1) ──────────────────────────────
#
# Dear ImGui (external/imgui, vendored unmodified, MIT) with its SDL2 + OpenGL3 backends,
# drawn by mksm_gpu's present thread (src/ui/menu.cpp). Needs the SDL window and the GPU
# present, so it is linked where mksm_gpu is (non-Debug configs).
if(MKSM_SDL_WINDOW AND TARGET mksm_gpu)
    enable_language(CXX)
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
    add_library(mksm_menu STATIC
        src/ui/menu.cpp
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(mksm_menu PRIVATE
        ${IMGUI_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu
        ${XBOXRECOMP_DIR}/src)
    target_link_libraries(mksm_menu PRIVATE SDL2::SDL2)
    target_compile_definitions(mksm_menu PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mksm_menu PRIVATE /Z7)
    target_link_libraries(${PROJECT_NAME} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:mksm_menu>")
    target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:MKSM_HAVE_MENU=1>")
endif()

# ── Tests ───────────────────────────────────────────────────────
#
# tools: unit tests for the Python tools (tests/), no game data needed.
# smoke: boot smoke test, runs the exe headless against the extracted game
# folder and checks the log (tools/smoke_test.py). Needs game data, which is
# never in the repo: without default.xbe in MKSM_GAME_DIR it is skipped.
#   ctest --test-dir build -C Release --output-on-failure [-R tools|smoke]
enable_testing()
# gpu_link: xemu's PGRAPH + null renderer come up from a cl/link.exe-built exe;
# gpu_link_gl: the same with the OpenGL renderer (needs a GL 4.0 core driver).
if(TARGET gpu_link)
    add_test(NAME gpu_link COMMAND gpu_link)
    add_test(NAME gpu_link_gl COMMAND gpu_link gl)
endif()
# settings: the settings file's parse/write and the monitor -> scale/aspect defaults (B7b1).
add_executable(settings_test tests/settings_test.c src/settings.c)
target_include_directories(settings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME settings COMMAND settings_test)
# xiso: the XDVDFS reader/extractor and the CNG hashes on synthetic images (B10b1);
# `xiso_test --extract <image> <dir>` extracts a real image (dev use, the B10b1 gate).
if(WIN32)
    add_executable(xiso_test tests/xiso_test.c src/xiso.c)
    target_include_directories(xiso_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(xiso_test PRIVATE bcrypt shell32)
    add_test(NAME xiso COMMAND xiso_test)
endif()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(MKSM_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../game_files" CACHE PATH
        "Extracted game folder (holding default.xbe) for the smoke test")
    add_test(NAME smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/smoke_test.py
            --game-dir ${MKSM_GAME_DIR} --exe $<TARGET_FILE:${PROJECT_NAME}>)
    set_tests_properties(smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    # Unit tests for tools/*.py on synthetic inputs (no game data, ~1 s).
    add_test(NAME tools
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
