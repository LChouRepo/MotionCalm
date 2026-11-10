@echo off
rem env.cmd - shared build environment: MSVC x64 (vcvars64, once per cmd session),
rem           common compiler / linker flags and output dirs (DESIGN.md section 13).
rem
rem Input : MC_CONFIG  = debug | release   (default release)
rem         MC_VCVARS  = optional override: full path of vcvars64.bat (skips the search)
rem         MC_VS_PATH = optional fallback: Visual Studio install folder (the one that
rem                      contains VC\Auxiliary\Build), used when vswhere finds nothing
rem Visual Studio is found with vswhere.exe: the newest install (any edition, including
rem Build Tools) that has the x64 C++ tools.
rem Output: MC_ROOT    repo root (absolute, no trailing backslash)
rem         MC_CONFIG  normalized to lower case
rem         MC_CFLAGS  cl flags (common + per-config)
rem         MC_LDFLAGS link flags (per-config; no /SUBSYSTEM - each script adds its own)
rem         MC_LIBS    import libraries
rem         MC_OUT     %MC_ROOT%\out\%MC_CONFIG%       (final .exe files)
rem         MC_OBJ     %MC_OUT%\obj                    (all .obj/.pdb/.rsp - never in the source tree)
rem
rem No setlocal on purpose: the variables are exported to the caller.
rem Keep this file ASCII-only (cmd parses batch files in the OEM code page).

if not defined MC_CONFIG set "MC_CONFIG=release"
if /i "%MC_CONFIG%"=="debug" goto :cfg_debug
if /i "%MC_CONFIG%"=="release" goto :cfg_release
echo [env] ERROR: MC_CONFIG must be debug or release, got "%MC_CONFIG%"
exit /b 1
:cfg_debug
set "MC_CONFIG=debug"
goto :cfg_done
:cfg_release
set "MC_CONFIG=release"
:cfg_done

for %%I in ("%~dp0..") do set "MC_ROOT=%%~fI"

rem ---------------------------------------------------------------- MSVC env
if defined MC_VCVARS_DONE goto :flags
rem Already inside an x64 developer prompt? Then do not call vcvars again.
if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :find_vcvars
where cl.exe >nul 2>nul
if errorlevel 1 goto :find_vcvars
set "MC_VCVARS_DONE=1"
goto :flags

:find_vcvars
rem 0) explicit override
if defined MC_VCVARS goto :check_vcvars
rem 1) vswhere: newest Visual Studio with the x64 C++ tools
set "MC_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MC_VSWHERE%" goto :try_vs_path
rem "call" keeps the quoted path intact: the ")" of "(x86)" must stay inside quotes for the for parser.
for /f "usebackq delims=" %%P in (`call "%MC_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "MC_VCVARS=%%P\VC\Auxiliary\Build\vcvars64.bat"
if defined MC_VCVARS if exist "%MC_VCVARS%" goto :call_vcvars
set "MC_VCVARS="
:try_vs_path
rem 2) MC_VS_PATH: Visual Studio install folder set by the user
if not defined MC_VS_PATH goto :no_vcvars
set "MC_VCVARS=%MC_VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
:check_vcvars
if exist "%MC_VCVARS%" goto :call_vcvars
:no_vcvars
echo [env] ERROR: vcvars64.bat not found (tried MC_VCVARS, vswhere and MC_VS_PATH).
echo [env]        Install Visual Studio 2022 or later with "Desktop development with C++",
echo [env]        or set MC_VS_PATH to your Visual Studio install folder,
echo [env]        or set MC_VCVARS to the full path of vcvars64.bat.
exit /b 1

:call_vcvars
rem vcvars looks for vswhere.exe on PATH and complains on stderr when it is not there.
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%MC_VCVARS%" >nul
rem vcvars' own errorlevel is unreliable; check that the tools are really there.
where cl.exe >nul 2>nul
if errorlevel 1 goto :vcvars_failed
where link.exe >nul 2>nul
if errorlevel 1 goto :vcvars_failed
set "MC_VCVARS_DONE=1"
goto :flags
:vcvars_failed
echo [env] ERROR: "%MC_VCVARS%" ran but cl.exe / link.exe are not on PATH.
exit /b 1

rem ---------------------------------------------------------------- flags
:flags
set "MC_CFLAGS=/nologo /std:c++20 /EHsc /W4 /WX- /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /permissive- /Zc:__cplusplus"
if "%MC_CONFIG%"=="debug" goto :flags_debug
rem /MT, /MTd: static C runtime, so the .exe files need no VC++ redistributable (vcruntime140.dll,
rem msvcp140.dll, ...). Every target is one self-contained .exe (no project DLLs), so no CRT boundary.
set "MC_CFLAGS=%MC_CFLAGS% /O2 /GL /DNDEBUG /MT"
set "MC_LDFLAGS=/NOLOGO /LTCG /OPT:REF /OPT:ICF"
goto :flags_done
:flags_debug
rem /FS: several cl invocations (and /MP workers) may share one .pdb.
set "MC_CFLAGS=%MC_CFLAGS% /Od /Zi /FS /MTd"
set "MC_LDFLAGS=/NOLOGO /DEBUG"
:flags_done

rem DESIGN.md section 13 list, plus winmm (timeBeginPeriod), uuid/dxguid (IID_* symbols).
rem No d3dcompiler.lib: shaders are compiled at build time by scripts\build_shaders.cmd (fxc).
set "MC_LIBS=user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib comctl32.lib dwmapi.lib d3d11.lib dxgi.lib dcomp.lib d2d1.lib dwrite.lib xinput9_1_0.lib shcore.lib winmm.lib uuid.lib dxguid.lib"

rem ---------------------------------------------------------------- dirs
set "MC_OUT=%MC_ROOT%\out\%MC_CONFIG%"
set "MC_OBJ=%MC_OUT%\obj"
if not exist "%MC_OUT%\" mkdir "%MC_OUT%"
if not exist "%MC_OBJ%\" mkdir "%MC_OBJ%"
if not exist "%MC_OBJ%\" (
  echo [env] ERROR: cannot create "%MC_OBJ%"
  exit /b 1
)
exit /b 0
