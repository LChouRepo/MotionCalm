@echo off
rem build_shaders.cmd - compiles the HLSL sources with fxc.exe into C headers (const BYTE arrays)
rem                     under %MC_OBJ%\shaders\ ; the code only calls CreateVertexShader / CreatePixelShader
rem                     on these arrays (no D3DCompile at run time):
rem   src\overlay\overlay.hlsl   -> overlay_vs.h   (kOverlayVS,   VSMain, vs_4_0)
rem                                 overlay_ps.h   (kOverlayPS,   PSMain, ps_4_0)
rem   src\capture\downscale.hlsl -> downscale_vs.h (kDownscaleVS, VSMain, vs_4_0)
rem                                 downscale_ps.h (kDownscalePS, PSMain, ps_4_0)
rem   build_shaders.cmd          uses %MC_CONFIG% (debug|release, default release)
rem Flags: /Ges (strict) /WX (any shader warning fails the build); release /O3, debug /Od /Zi.
rem Called by build_app.cmd, build_tests.cmd and build_tools.cmd before they compile; callers add
rem /I"%MC_OBJ%\shaders" themselves. %MC_OBJ%\shaders is not wiped by the other build scripts.
rem Exit code: 0 on success, 1 on any failure (fxc missing, compile error or warning).
rem Keep this file ASCII-only.
setlocal EnableExtensions DisableDelayedExpansion
call "%~dp0env.cmd"
if errorlevel 1 exit /b 1

rem fxc.exe ships with the Windows SDK, in the same bin directory as rc.exe (on PATH after vcvars64).
where fxc.exe >nul 2>nul
if errorlevel 1 goto :no_fxc

set "MC_SHOBJ=%MC_OBJ%\shaders"
if not exist "%MC_SHOBJ%\" mkdir "%MC_SHOBJ%"
if not exist "%MC_SHOBJ%\" goto :no_dir

set "MC_FXCOPT=/O3"
if "%MC_CONFIG%"=="debug" set "MC_FXCOPT=/Od /Zi"

call :fxc "%MC_ROOT%\src\overlay\overlay.hlsl" vs_4_0 VSMain kOverlayVS overlay_vs.h
if errorlevel 1 goto :failed
call :fxc "%MC_ROOT%\src\overlay\overlay.hlsl" ps_4_0 PSMain kOverlayPS overlay_ps.h
if errorlevel 1 goto :failed
call :fxc "%MC_ROOT%\src\capture\downscale.hlsl" vs_4_0 VSMain kDownscaleVS downscale_vs.h
if errorlevel 1 goto :failed
call :fxc "%MC_ROOT%\src\capture\downscale.hlsl" ps_4_0 PSMain kDownscalePS downscale_ps.h
if errorlevel 1 goto :failed
echo [shaders] OK  %MC_SHOBJ%
exit /b 0

:no_fxc
echo [shaders] ERROR: fxc.exe not found on PATH.
echo [shaders]        fxc.exe is part of the Windows SDK and lives next to rc.exe; install the
echo [shaders]        Windows 10/11 SDK with the Visual Studio Installer, "Desktop development with C++".
goto :failed

:no_dir
echo [shaders] ERROR: cannot create "%MC_SHOBJ%"
goto :failed

:failed
echo [shaders] BUILD FAILED
exit /b 1

rem ---------------------------------------------------------------- :fxc <hlsl> <profile> <entry> <array> <header>
rem Compiles one entry point into %MC_SHOBJ%\<header> as "const BYTE <array>[]". The old header is
rem deleted first, so a failed compile never leaves a stale one behind.
:fxc
set "MC_FXOUT=%MC_SHOBJ%\%~5"
if exist "%MC_FXOUT%" del /f /q "%MC_FXOUT%"
if exist "%MC_FXOUT%" (
  echo [shaders] cannot delete old %~5
  exit /b 1
)
echo [shaders] fxc  %~nx1 %~3 %~2 -^> %~5
fxc /nologo /WX /Ges %MC_FXCOPT% /T %~2 /E %~3 /Vn %~4 /Fh "%MC_FXOUT%" "%~1"
if errorlevel 1 (
  echo [shaders] fxc failed: %~nx1 %~3 %~2
  exit /b 1
)
if not exist "%MC_FXOUT%" (
  echo [shaders] fxc wrote no header: %~5
  exit /b 1
)
exit /b 0
