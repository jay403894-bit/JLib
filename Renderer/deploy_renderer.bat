@echo off
REM Builds Renderer.lib (Debug + Release) AND deploys BOTH libs + ALL headers to C:\libs\Renderer
REM in ONE pass. The header and the lib MUST move together: RendererCore/ResourceManager headers
REM define classes the app allocates BY VALUE, so adding/reordering a member is an ABI break --
REM a lib/header desync makes the app read members at the wrong offset (garbage crash; e.g. the
REM 2026-07-22 SET_SCISSOR_RECTS_INVALID_RECT crash was a stale deployed RendererCore.h after the
REM lib was rebuilt with the new depth-buffer members). Same lesson as Threads' deploy_lib.bat:
REM never "just copy the lib" -- headers ALWAYS ship too, both configs. After running this, do a
REM CLEAN rebuild of the consuming app (particlesdemo) so its TUs recompile against the new header.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "C:\Users\jay40\source\repos\DirectX12\DirectX12"

echo Building Renderer.lib (Debug)...
msbuild DirectX12.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:m
if errorlevel 1 ( echo BUILD_FAILED_DEBUG & exit /b 1 )

echo Building Renderer.lib (Release)...
msbuild DirectX12.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m
if errorlevel 1 ( echo BUILD_FAILED_RELEASE & exit /b 1 )

REM COPY FROM x64\... (PROJECT-level), NOT ..\x64\... (solution-level). This script builds the .vcxproj
REM DIRECTLY, so $(SolutionDir) collapses to $(ProjectDir) and msbuild writes output to THIS project's
REM x64\<config>\ -- i.e. C:\...\DirectX12\DirectX12\x64\<config>\. Visual Studio (building the .sln) writes
REM one level up (C:\...\DirectX12\x64\<config>\). Using ..\x64 here deployed VS's STALE output instead of
REM the fresh build we just made -- on 2026-07-28 that silently shipped a stale Debug lib+shader for hours
REM (Debug frozen while only Release updated). Keep these paths project-relative so the script is self-consistent.
echo Deploying libs...
if not exist "C:\libs\Renderer\lib\debug"   mkdir "C:\libs\Renderer\lib\debug"
if not exist "C:\libs\Renderer\lib\release" mkdir "C:\libs\Renderer\lib\release"
copy /Y "x64\Debug\Renderer.lib"   "C:\libs\Renderer\lib\debug\Renderer.lib"   >nul
if errorlevel 1 ( echo COPY_FAILED_DEBUG_LIB & exit /b 1 )
copy /Y "x64\Release\Renderer.lib" "C:\libs\Renderer\lib\release\Renderer.lib" >nul
if errorlevel 1 ( echo COPY_FAILED_RELEASE_LIB & exit /b 1 )

echo Deploying headers (ALWAYS -- both configs share these)...
if not exist "C:\libs\Renderer\include" mkdir "C:\libs\Renderer\include"
xcopy /Y /Q "include\*.h" "C:\libs\Renderer\include\" >nul
if errorlevel 1 ( echo COPY_FAILED_HEADERS & exit /b 1 )

REM xcopy PRESERVES each source header's mtime. When Renderer3D.h etc. change ABI, that "backdated"
REM deployed header can be OLDER than the consuming app's existing .obj files, so MSBuild's incremental
REM up-to-date check SKIPS recompiling them -> stale .obj vs new lib layout -> ABI-mismatch crash (e.g.
REM crash on SetRenderer3D). Touch every deployed header to NOW so any older app .obj is rebuilt.
forfiles /p "C:\libs\Renderer\include" /m *.h /c "cmd /c copy /b @path+,, @path >nul" >nul 2>&1

echo Deploying compiled shaders (.cso -- FxCompile output; runtime loads these by name)...
if not exist "C:\libs\Renderer\shaders" mkdir "C:\libs\Renderer\shaders"
xcopy /Y /Q "x64\Debug\shaders\*.cso" "C:\libs\Renderer\shaders\" >nul
if errorlevel 1 ( echo COPY_FAILED_SHADERS & exit /b 1 )

REM The runtime loads shaders\<name>.cso RELATIVE TO THE EXE, so the .cso must also sit next to the
REM consuming app's exe -- NOT just in C:\libs\Renderer\shaders. A stale exe-folder copy vs new renderer
REM code is an invisible mismatch: on 2026-07-27 a 3-day-old pre-PBR Basic3D_VS.cso next to the exe
REM collapsed the instanced static geometry (looked like the skybox was "covering" it) and cost hours.
REM So deploy the freshly-built .cso straight into the live app's exe folders here, both configs. Bytecode
REM is config-agnostic, but copy config-matched anyway. LIVE APP IS 3DTest (particlesdemo is defunct -- its
REM .vcxproj went unloadable, project was recreated as 3DTest). If Game01 starts consuming 3D shaders, add it too.
echo Deploying .cso next to the 3DTest exe (both configs)...
if not exist "C:\Users\jay40\source\repos\3DTest\x64\Debug\shaders"   mkdir "C:\Users\jay40\source\repos\3DTest\x64\Debug\shaders"
if not exist "C:\Users\jay40\source\repos\3DTest\x64\Release\shaders" mkdir "C:\Users\jay40\source\repos\3DTest\x64\Release\shaders"
xcopy /Y /Q "x64\Debug\shaders\*.cso"   "C:\Users\jay40\source\repos\3DTest\x64\Debug\shaders\"   >nul
if errorlevel 1 ( echo COPY_FAILED_DEMO_DEBUG_SHADERS & exit /b 1 )
xcopy /Y /Q "x64\Release\shaders\*.cso" "C:\Users\jay40\source\repos\3DTest\x64\Release\shaders\" >nul
if errorlevel 1 ( echo COPY_FAILED_DEMO_RELEASE_SHADERS & exit /b 1 )

REM Game01 too, as of 2026-08-03: Renderer2D's 2D primitives (DrawCircle/DrawLine/...) load
REM shaders\CirclePS.cso for the SDF circle path. Without it the renderer still runs -- it falls
REM back to N-gon meshes -- but circles visibly facet when the editor zooms in. Any app using the
REM primitives needs the .cso beside its exe, so 2D apps belong in this list now, not just 3D ones.
echo Deploying .cso next to the Game01 exe (both configs)...
if not exist "C:\Users\jay40\source\repos\Game01\x64\Debug\shaders"   mkdir "C:\Users\jay40\source\repos\Game01\x64\Debug\shaders"
if not exist "C:\Users\jay40\source\repos\Game01\x64\Release\shaders" mkdir "C:\Users\jay40\source\repos\Game01\x64\Release\shaders"
xcopy /Y /Q "x64\Debug\shaders\*.cso"   "C:\Users\jay40\source\repos\Game01\x64\Debug\shaders\"   >nul
if errorlevel 1 ( echo COPY_FAILED_GAME01_DEBUG_SHADERS & exit /b 1 )
xcopy /Y /Q "x64\Release\shaders\*.cso" "C:\Users\jay40\source\repos\Game01\x64\Release\shaders\" >nul
if errorlevel 1 ( echo COPY_FAILED_GAME01_RELEASE_SHADERS & exit /b 1 )

echo DEPLOY_RENDERER_OK (debug + release libs + headers + shaders + exe-folder shaders)
endlocal
