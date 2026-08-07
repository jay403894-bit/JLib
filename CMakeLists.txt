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

# MASM only exists on Windows; the Linux context switch is GAS, which CMake handles as plain ASM.
if(WIN32)
    project(JLib LANGUAGES CXX ASM_MASM)
else()
    project(JLib LANGUAGES CXX ASM)
endif()

# The scheduler alone is portable (Windows x64 + Linux x86-64); everything above it -- the DX12
# renderer and the libraries that depend on it -- is Windows-only by construction. On a non-Windows
# host this file therefore builds JUST the scheduler and its bench, rather than failing outright.
set(JLIB_SCHEDULER_ONLY OFF)
if(NOT WIN32)
    set(JLIB_SCHEDULER_ONLY ON)
    message(STATUS "JLib: non-Windows host -- building JLib::Scheduler only (the renderer is DX12).")
endif()

if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR
        "On Windows, JLib requires MSVC: the context switch is MASM and the renderer is DirectX 12.")
endif()
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "JLib::Scheduler supports Windows x64 and Linux x86-64. The context switch is hand-written "
        "assembly per ABI, so a new platform needs a new ContextSwitch, not a compiler flag.")
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
# Set explicitly so subdirectory targets land here too. Without it, anything added via
# add_subdirectory (the scheduler) writes its .lib into a per-subdirectory folder instead, which
# breaks the "one output directory, delete it to clean" property the MSBuild side advertises.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

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
    # Platform sources live in src/win32 and src/posix. Exactly ONE directory is added, which is
    # the reason the split is by DIRECTORY and not by filename suffix: the glob above would happily
    # pick up both Topology_win32.cpp and Topology_posix.cpp and collide on duplicate symbols.
    if(WIN32)
        file(GLOB _plat CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${A_DIR}/src/win32/*.cpp")
    else()
        file(GLOB _plat CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${A_DIR}/src/posix/*.cpp")
    endif()
    list(APPEND _srcs ${_plat} ${A_EXTRA_SRC})
    add_library(${NAME} STATIC ${_srcs})
    add_library(JLib::${NAME} ALIAS ${NAME})
    # BUILD_INTERFACE / INSTALL_INTERFACE, not a bare path. A bare absolute source path would work
    # fine when building in place and then be baked into the exported package, so an installed JLib
    # would point every consumer back at the source tree it was built from - which breaks the moment
    # that tree moves, and silently uses stale headers until then.
    target_include_directories(${NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${A_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    foreach(_inc IN LISTS A_EXTRA_INC)
        target_include_directories(${NAME} PUBLIC $<BUILD_INTERFACE:${_inc}>)
    endforeach()
    if(A_EXTRA_INC)
        target_include_directories(${NAME} PUBLIC $<INSTALL_INTERFACE:include>)
    endif()
    target_compile_features(${NAME} PUBLIC cxx_std_${A_STD})
    # Windows-only: both exist purely to tame <windows.h>. Defining them on Linux is meaningless
    # and collides with the headers that already #define NOMINMAX themselves, producing a
    # "macro redefined" warning on every translation unit.
    if(WIN32)
        target_compile_definitions(${NAME} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(A_DEPS)
        target_link_libraries(${NAME} PUBLIC ${A_DEPS})
    endif()
    set_target_properties(${NAME} PROPERTIES FOLDER "JLib")
endfunction()

# ---- header-only -----------------------------------------------------------------------------
add_library(Assets INTERFACE)
add_library(JLib::Assets ALIAS Assets)
target_include_directories(Assets INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Assets/include> $<INSTALL_INTERFACE:include>)

add_library(Geometry INTERFACE)
add_library(JLib::Geometry ALIAS Geometry)
target_include_directories(Geometry INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Geometry/include> $<INSTALL_INTERFACE:include>)

# ---- Scheduler: the .asm is the fiber context switch, not optional ---------------------------
# The scheduler defines itself, in Scheduler/CMakeLists.txt -- one file that is ALSO the top-level
# build of the standalone JLib-Scheduler repository. It is published in both places, and a Linux
# user wanting only the scheduler clones that repo rather than this one, so its build rules cannot
# live up here. It also brings SchedulerBench with it.
add_subdirectory(Scheduler)

# ---- Everything above the scheduler is Windows-only AS WRITTEN -------------------------------
# Not all of it is inherently unportable -- Scene is plain C++ and PlatformerPhysics2D needs only
# the scheduler -- but Renderer is DirectX 12, Input is Raw Input/XInput, and Time, Sound and
# Physics3D all reach for <Windows.h> today. Rather than ship a half-ported subset that implies
# more than it delivers, a non-Windows build produces exactly what actually works: the scheduler.
if(NOT JLIB_SCHEDULER_ONLY)

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
target_include_directories(Jolt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Jolt> $<INSTALL_INTERFACE:include>)
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
# DirectXTex is a prebuilt lib with no local source, so it becomes an IMPORTED target rather than a
# raw path. A raw path cannot be exported: CMake refuses to bake a build-machine absolute path into
# an installed package, and rightly so. The installed JLibConfig.cmake re-creates this target
# pointing at the copy that ships alongside the libraries.
add_library(JLib_DirectXTex STATIC IMPORTED GLOBAL)
set_target_properties(JLib_DirectXTex PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/DirectXTex/lib/release/DirectXTex.lib")
target_link_libraries(Renderer PUBLIC
    $<BUILD_INTERFACE:JLib_DirectXTex>
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

endif()  # NOT JLIB_SCHEDULER_ONLY

# SchedulerBench is defined by Scheduler/CMakeLists.txt, alongside the library it measures, so the
# standalone repo gets it too. Only its IDE placement is set here.
set_target_properties(Scheduler      PROPERTIES FOLDER "JLib")
set_target_properties(SchedulerBench PROPERTIES FOLDER "Tools")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT SchedulerBench)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

# =============================================================================================
# INSTALL
#
#   cmake --install build --config Release --prefix C:/libs/JLib
#
# You do NOT need this to use JLib. Building in place and pointing at the tree (ProjectReference,
# UseJLib.props, or add_subdirectory) is the normal path and involves no copying at all. Install
# exists for the two cases where a copy is actually wanted: handing someone a prebuilt drop, and
# `find_package(JLib)` from an unrelated CMake project.
#
# Libraries go to lib/<Config>/ rather than using a debug postfix, because Debug, Development and
# Release all produce a file called Scheduler.lib and one of them would otherwise overwrite the
# others. It also matches the layout the C:\libs convention already used.
# =============================================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(JLIB_SCHEDULER_ONLY)
    set(JLIB_TARGETS Scheduler Assets Geometry)
else()
    set(JLIB_TARGETS Scheduler Renderer Time Scene Input Sound PlatformerPhysics2D Physics3D Jolt
                     Assets Geometry)
endif()

install(TARGETS ${JLIB_TARGETS}
        EXPORT JLibTargets
        ARCHIVE DESTINATION "lib/$<CONFIG>"
        INCLUDES DESTINATION include)

# Public headers. Each library's include/ is flattened into one include/ directory, which is what
# consumers already expect (#include <TaskScheduler.h>, not <Scheduler/TaskScheduler.h>).
if(JLIB_SCHEDULER_ONLY)
    set(_hdr_libs Scheduler Assets Geometry)
else()
    set(_hdr_libs Scheduler Renderer Time Scene Input Sound PlatformerPhysics2D Physics3D
                  Assets Geometry)
endif()
foreach(_lib IN LISTS _hdr_libs)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${_lib}/include/" DESTINATION include)
endforeach()

if(NOT JLIB_SCHEDULER_ONLY)
    # Jolt keeps its directory structure: its headers are included as <Jolt/Jolt.h>.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Jolt/Jolt" DESTINATION include)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/DirectX-Headers/include/" DESTINATION include)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/DirectXTex/include/" DESTINATION include)
    # The prebuilt DirectXTex, which must travel with a redistributed binary.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/DirectXTex/lib/release/DirectXTex.lib"
            DESTINATION lib)
endif()

# Licence files always ship, on every platform.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
              "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
        DESTINATION .)

# Compiled shaders. Without these the renderer links but has nothing to load at runtime.
if(JLIB_DXC)
    install(DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/" DESTINATION bin
            FILES_MATCHING PATTERN "*.cso")
endif()

install(EXPORT JLibTargets
        FILE JLibTargets.cmake
        NAMESPACE JLib::
        DESTINATION lib/cmake/JLib)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/JLibConfigVersion.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/JLibConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/JLibConfig.cmake"
    INSTALL_DESTINATION lib/cmake/JLib)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/JLibConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/JLibConfigVersion.cmake"
        DESTINATION lib/cmake/JLib)
