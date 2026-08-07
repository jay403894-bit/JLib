# ============================ JLib ============================
#
#   cmake -B build -G "Visual Studio 18 2026" -A x64
#   cmake --build build --config Development
#
# or generate a solution and open it, or use Ninja, or consume the whole thing from another
# project with add_subdirectory() / FetchContent.
#
# This exists ALONGSIDE the hand-written MSBuild projects (JLib.sln / JLib.slnx), not instead of
# them. Neither is generated from the other, and that is normally a maintenance trap - two source
# lists that drift. It is avoided here by having BOTH systems GLOB the same directories rather than
# enumerate files: Jolt.vcxproj already uses `Jolt\**\*.cpp`, and every target below globs the same
# way. Adding a .cpp is picked up by both with nothing to update. (CMake globbing is usually
# discouraged because new files need a re-configure; CONFIGURE_DEPENDS below makes CMake re-glob
# automatically, which is the trade that makes this honest.)
#
# WINDOWS ONLY, AND THAT IS A DECISION. The scheduler's context switch is hand-written x64 MASM and
# the renderer is DirectX 12. A CMake build does not make this portable and is not claiming to - it
# is here because `cmake -B build` is what C++ developers expect to be able to type, and because it
# lets other CMake projects consume JLib without hand-configuring anything.

cmake_minimum_required(VERSION 3.21)

project(JLib LANGUAGES CXX ASM_MASM)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR
        "JLib is Windows x64 + MSVC only, by construction: the scheduler's context switch is "
        "hand-written x64 MASM and the renderer is DirectX 12. This is a design decision, not a "
        "missing feature - see the README. A Linux port of the SCHEDULER ALONE is planned "
        "(ucontext instead of MASM); nothing else is portable as written.")
endif()

# ---------------------------------------------------------------------------------------------
# Configurations. Development is the third one: optimized, with symbols AND assertions live.
#
# It deliberately does NOT define NDEBUG. CMake's own RelWithDebInfo does, which would compile out
# every assert - the exact thing this configuration exists to keep. So it is defined explicitly
# rather than aliased onto RelWithDebInfo. See BUILD.md.
# ---------------------------------------------------------------------------------------------
get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_multi)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Development;Release" CACHE STRING "" FORCE)
elseif(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Development" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEVELOPMENT        "/O2 /Ob2 /Zi /DJLIB_DEVELOPMENT" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT "/DEBUG /INCREMENTAL:NO"          CACHE STRING "")
set(CMAKE_STATIC_LINKER_FLAGS_DEVELOPMENT ""                             CACHE STRING "")
set(CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT "/DEBUG"                       CACHE STRING "")
mark_as_advanced(CMAKE_CXX_FLAGS_DEVELOPMENT CMAKE_EXE_LINKER_FLAGS_DEVELOPMENT
                 CMAKE_STATIC_LINKER_FLAGS_DEVELOPMENT CMAKE_SHARED_LINKER_FLAGS_DEVELOPMENT)

# /MDd for Debug, /MD for everything else. This MUST match across every linked library or you get
# an ODR violation that surfaces as heap corruption rather than a clean error - the same trap
# JLib.Common.props exists to prevent on the MSBuild side.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------------------------
# jlib_library(<name> DIR <dir> [STD <17|20>] [DEPS <targets...>] [EXTRA_INC <dirs...>])
#
# Globs <dir>/src/*.cpp, exposes <dir>/include, and creates a JLib::<name> alias so consumers
# always name targets the same way whether they add_subdirectory or (later) find_package.
# ---------------------------------------------------------------------------------------------
function(jlib_library NAME)
    cmake_parse_arguments(A "" "DIR;STD" "DEPS;EXTRA_INC;EXTRA_SRC" ${ARGN})
    if(NOT A_STD)
        set(A_STD 17)
    endif()
    file(GLOB _srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${A_DIR}/src/*.cpp")
    list(APPEND _srcs ${A_EXTRA_SRC})
    add_library(${NAME} STATIC ${_srcs})
    add_library(JLib::${NAME} ALIAS ${NAME})
    target_include_directories(${NAME} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${A_DIR}/include" ${A_EXTRA_INC})
    target_compile_features(${NAME} PUBLIC cxx_std_${A_STD})
    target_compile_definitions(${NAME} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    if(A_DEPS)
        target_link_libraries(${NAME} PUBLIC ${A_DEPS})
    endif()
    set_target_properties(${NAME} PROPERTIES FOLDER "JLib")
endfunction()

# ---- header-only -----------------------------------------------------------------------------
add_library(Assets INTERFACE)
add_library(JLib::Assets ALIAS Assets)
target_include_directories(Assets INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Assets/include")

add_library(Geometry INTERFACE)
add_library(JLib::Geometry ALIAS Geometry)
target_include_directories(Geometry INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Geometry/include")

# ---- Scheduler: the .asm is the fiber context switch, not optional ---------------------------
jlib_library(Scheduler DIR Scheduler STD 17
             EXTRA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Scheduler/src/ContextSwitch.asm")

jlib_library(Time  DIR Time  STD 17)
jlib_library(Scene DIR Scene STD 17)
jlib_library(Input DIR Input STD 17)
jlib_library(Sound DIR Sound STD 17 DEPS Scheduler Assets)
jlib_library(PlatformerPhysics2D DIR PlatformerPhysics2D STD 20 DEPS Scheduler)

# ---- Jolt: vendored, built from source -------------------------------------------------------
# The define set is PUBLIC, so every consumer inherits it automatically. That is strictly stronger
# than the MSBuild side, where Jolt.props has to be imported by hand: here a consumer CANNOT
# compile against different JPH_* definitions than the library was built with, because the
# definitions travel with the target. These defines change Jolt's STRUCTURE LAYOUTS; a mismatch is
# an ODR violation whose good outcome is an LNK2001 vtable error.
file(GLOB_RECURSE JOLT_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Jolt/Jolt/*.cpp")
add_library(Jolt STATIC ${JOLT_SRC})
add_library(JLib::Jolt ALIAS Jolt)
target_include_directories(Jolt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Jolt")
target_compile_features(Jolt PUBLIC cxx_std_20)
target_compile_definitions(Jolt PUBLIC
    NOMINMAX JPH_FLOATING_POINT_EXCEPTIONS_ENABLED JPH_USE_DX12 JPH_USE_CPU_COMPUTE
    JPH_DEBUG_RENDERER JPH_PROFILE_ENABLED JPH_OBJECT_STREAM
    JPH_USE_AVX2 JPH_USE_AVX JPH_USE_SSE4_1 JPH_USE_SSE4_2
    JPH_USE_LZCNT JPH_USE_TZCNT JPH_USE_F16C JPH_USE_FMADD)
# The defines above only DECLARE the instruction set; /arch is what makes the emitted code match.
target_compile_options(Jolt PUBLIC /arch:AVX2)
set_target_properties(Jolt PROPERTIES FOLDER "ThirdParty")

jlib_library(Physics3D DIR Physics3D STD 20 DEPS Scheduler Jolt)

# ---- Renderer --------------------------------------------------------------------------------
file(GLOB IMGUI_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/include/imgui/*.cpp")
jlib_library(Renderer DIR Renderer STD 20
             DEPS Scheduler Time Assets Geometry
             EXTRA_SRC ${IMGUI_SRC}
             EXTRA_INC "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/DirectX-Headers/include"
                       "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/DirectXTex/include")
target_link_libraries(Renderer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/DirectXTex/lib/release/DirectXTex.lib"
    d3d12 dxgi dxguid d3dcompiler)

# ---- Shaders ---------------------------------------------------------------------------------
# Shader Model 6.1, so this is DXC, not FXC - fxc.exe only reaches SM 5.1. MSBuild's FxCompile
# task switches to dxc automatically at 6.x, which is easy to miss when porting the build.
find_program(JLIB_DXC dxc
    HINTS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.0.26100.0/x64"
          "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/x64"
    PATH_SUFFIXES bin)

set(JLIB_SHADERS
    Basic3D_PS:ps  Basic3D_VS:vs  CirclePS:ps  EquirectToCube_PS:ps  GridOutlinePS:ps
    IrradianceConv_PS:ps  Particle3D_PS:ps  Particle3D_VS:vs  ParticlePS:ps  ParticleVS:vs
    PixelShader:ps  PrefilterEnv_PS:ps  RoundRectPS:ps  ShadowDepth_VS:vs  Skinned3D_VS:vs
    Skybox_PS:ps  Skybox_VS:vs  SSAO_PS:ps  SSAO_VS:vs  VertexShader:vs
    BuildDispatchArgs:cs  CompactParticles:cs  ResetAliveCounter:cs  SpawnBurst3D:cs
    SpawnParticles:cs  UpdateParticles:cs  UpdateParticles3D:cs  WaveParticles:cs)

if(JLIB_DXC)
    set(_shader_outputs "")
    foreach(_entry IN LISTS JLIB_SHADERS)
        string(REPLACE ":" ";" _parts "${_entry}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _stage)
        if(_stage STREQUAL "cs")
            set(_ep main)
        elseif(_stage STREQUAL "vs")
            set(_ep VSMain)
        else()
            set(_ep PSMain)
        endif()
        set(_out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_name}.cso")
        add_custom_command(OUTPUT "${_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMAND "${JLIB_DXC}" -T ${_stage}_6_1 -E ${_ep}
                    "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/src/${_name}.hlsl" -Fo "${_out}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Renderer/src/${_name}.hlsl"
            COMMENT "dxc ${_name}.hlsl (${_stage}_6_1)"
            VERBATIM)
        list(APPEND _shader_outputs "${_out}")
    endforeach()
    add_custom_target(Shaders ALL DEPENDS ${_shader_outputs})
    set_target_properties(Shaders PROPERTIES FOLDER "JLib")
    add_dependencies(Renderer Shaders)
else()
    message(WARNING
        "dxc not found: shaders will NOT be compiled and the renderer will have nothing to load at "
        "runtime. The libraries still build. Install the Windows 10/11 SDK, or set JLIB_DXC.")
endif()

# ---- Bench -----------------------------------------------------------------------------------
add_executable(SchedulerBench "${CMAKE_CURRENT_SOURCE_DIR}/Scheduler/bench/bench.cpp")
target_link_libraries(SchedulerBench PRIVATE Scheduler)
# C++20 even though the Scheduler it measures is C++17: the crossover sweep accumulates into a
# std::atomic<double>, and atomic<floating-point>::fetch_add is C++20. Without this the target
# inherits cxx_std_17 through Scheduler and fails to compile - which says nothing about the
# library's own C++17 compatibility claim, only about the benchmark harness.
target_compile_features(SchedulerBench PRIVATE cxx_std_20)
set_target_properties(SchedulerBench PROPERTIES FOLDER "Tools")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT SchedulerBench)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()
