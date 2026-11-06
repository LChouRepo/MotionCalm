@echo off
rem build_app.cmd - compiles every src\**\*.cpp (including src\app\main.cpp) and src\res\app.rc
rem                 into %MC_OUT%\MotionCalm.exe (/SUBSYSTEM:WINDOWS).
rem   build_app.cmd             uses %MC_CONFIG% (debug|release, default release)
rem Exit code: 0 on success, 1 on any compile / rc / link failure.
rem Objects go to %MC_OBJ%\app\<dir>\ (one cl /MP call per source directory, so files with the
rem same base name in different directories never collide). The app obj dir is wiped on every
rem build so deleted sources do not linger.
rem The manifest is embedded by app.rc (RT_MANIFEST id 1), hence link /MANIFEST:NO.
rem Keep this file ASCII-only.
setlocal EnableExtensions
rem env.cmd (and vcvars64) run before delayed expansion is switched on.
call "%~dp0env.cmd"
if errorlevel 1 exit /b 1
setlocal EnableDelayedExpansion

set "MC_AOBJ=%MC_OBJ%\app"
set "MC_AINC=/I"%MC_ROOT%\src""
if not exist "%MC_ROOT%\src\app\main.cpp" (
  echo [app] missing %MC_ROOT%\src\app\main.cpp
  goto :failed
)
if exist "%MC_AOBJ%\" rd /s /q "%MC_AOBJ%"
mkdir "%MC_AOBJ%"
if errorlevel 1 (
  echo [app] cannot create %MC_AOBJ%
  exit /b 1
)

set "MC_NFILES=0"
call :compile_dir "%MC_ROOT%\src" src
if errorlevel 1 goto :failed
for /d /r "%MC_ROOT%\src" %%D in (*) do (
  set "MC_REL=%%~fD"
  set "MC_REL=!MC_REL:%MC_ROOT%\src\=!"
  set "MC_REL=!MC_REL:\=_!"
  call :compile_dir "%%~fD" "!MC_REL!"
  if errorlevel 1 goto :failed
)
if "%MC_NFILES%"=="0" (
  echo [app] no .cpp files found
  goto :failed
)

rem ---- resources (icon, manifest, default INI template, version info)
echo [app] rc   app.rc
rc /nologo /c65001 /I"%MC_ROOT%\src\res" /fo"%MC_AOBJ%\app.res" "%MC_ROOT%\src\res\app.rc"
if errorlevel 1 (
  echo [app] rc failed
  goto :failed
)

rem ---- link everything under %MC_AOBJ%
set "MC_LRSP=%MC_AOBJ%\_link.rsp"
if exist "%MC_LRSP%" del "%MC_LRSP%"
for /r "%MC_AOBJ%" %%O in (*.obj) do >>"%MC_LRSP%" echo "%%~fO"
set "MC_PDBOPT="
if "%MC_CONFIG%"=="debug" set "MC_PDBOPT=/PDB:"%MC_OUT%\MotionCalm.pdb""
echo [app] link MotionCalm.exe (%MC_NFILES% sources)
link %MC_LDFLAGS% /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:"%MC_OUT%\MotionCalm.exe" %MC_PDBOPT% @"%MC_LRSP%" "%MC_AOBJ%\app.res" %MC_LIBS% kernel32.lib
if errorlevel 1 (
  echo [app] link failed
  goto :failed
)
echo [app] OK  %MC_OUT%\MotionCalm.exe
exit /b 0

:failed
echo [app] BUILD FAILED
exit /b 1

rem ---------------------------------------------------------------- :compile_dir <dir> <objsubdir>
rem Compiles <dir>\*.cpp (non-recursive) with one cl /MP call.
:compile_dir
set "MC_DIR=%~1"
set "MC_ODIR=%MC_AOBJ%\%~2"
set "MC_RSP=%MC_AOBJ%\_%~2.rsp"
set "MC_N=0"
for %%F in ("%MC_DIR%\*.cpp") do (
  >>"%MC_RSP%" echo "%%~fF"
  set /a MC_N+=1
)
if "!MC_N!"=="0" exit /b 0
set /a MC_NFILES+=MC_N
if not exist "%MC_ODIR%\" mkdir "%MC_ODIR%"
echo [app] cl   %~2 (!MC_N! files)
cl %MC_CFLAGS% %MC_AINC% /MP /c /Fo"%MC_ODIR%\\" /Fd"%MC_AOBJ%\vc.pdb" @"%MC_RSP%"
if errorlevel 1 (
  echo [app] compile failed in %MC_DIR%
  exit /b 1
)
exit /b 0
