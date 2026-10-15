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

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/prof.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

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
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

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
if(MKSM_CLANG_CL AND EXISTS "${MKSM_CLANG_CL}" AND MKSM_NINJA)
    include(ExternalProject)
    set(MKSM_GPU_BIN "${CMAKE_BINARY_DIR}/mksm_gpu")
    set(MKSM_GPU_LIB "${MKSM_GPU_BIN}/mksm_gpu.lib")
    # Always Release with /MD: only the game's Release build will link it.
    ExternalProject_Add(mksm_gpu_build
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu"
        BINARY_DIR "${MKSM_GPU_BIN}"
        CMAKE_GENERATOR Ninja
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_C_COMPILER=${MKSM_CLANG_CL}"
            "-DCMAKE_MAKE_PROGRAM=${MKSM_NINJA}"
        BUILD_ALWAYS ON
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${MKSM_GPU_LIB}"
    )
    add_library(mksm_gpu STATIC IMPORTED GLOBAL)
    set_target_properties(mksm_gpu PROPERTIES
        IMPORTED_LOCATION "${MKSM_GPU_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu"
        # The OpenGL renderer: WGL contexts on hidden windows (gloffscreen_wgl.c).
        INTERFACE_LINK_LIBRARIES "opengl32;gdi32;user32")
    add_dependencies(mksm_gpu mksm_gpu_build)

    add_executable(gpu_link src/gpu/gpu_link.c)
    target_link_libraries(gpu_link PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,mksm_gpu>")

    # The game: RECOMP_PB_GPU=1 hands the pushbuffer to it (src/main.c). The lib is always
    # Release /MD, so the Debug (/MDd) config builds without it.
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "$<$<NOT:$<CONFIG:Debug>>:$<LINK_LIBRARY:WHOLE_ARCHIVE,mksm_gpu>>")
    target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:MKSM_HAVE_GPU=1>")
else()
    message(STATUS "mksm_gpu skipped: set MKSM_CLANG_CL to clang-cl.exe and put ninja on PATH")
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
