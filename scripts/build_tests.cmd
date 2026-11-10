@echo off
rem build_tests.cmd - compiles tests\*.cpp + every src\**\*.cpp except src\app\main.cpp
rem                   into %MC_OUT%\tests.exe (console), then runs it.
rem   build_tests.cmd [tests.exe args...]    e.g.  build_tests.cmd intensity
rem Exit code: compile/link failure -> 1, otherwise the exit code of tests.exe
rem            (= number of failed tests).
rem Objects go to %MC_OBJ%\tests\<dir>\ (one cl /MP call per source directory, so files with
rem the same base name in different directories never collide). The tests obj dir is wiped
rem on every build so deleted sources do not linger.
rem The HLSL shaders are compiled first by build_shaders.cmd into %MC_OBJ%\shaders\ (extra /I).
rem Keep this file ASCII-only.
setlocal EnableExtensions
rem env.cmd (and vcvars64) run before delayed expansion is switched on.
call "%~dp0env.cmd"
if errorlevel 1 exit /b 1
rem HLSL -> %MC_OBJ%\shaders\*.h (included by the renderer, the GPU downscaler and tests\test_overlay.cpp).
call "%~dp0build_shaders.cmd"
if errorlevel 1 (
  echo [tests] BUILD FAILED
  exit /b 1
)
setlocal EnableDelayedExpansion

set "MC_TOBJ=%MC_OBJ%\tests"
set "MC_EXCLUDE=%MC_ROOT%\src\app\main.cpp"
set "MC_TINC=/I"%MC_ROOT%\src" /I"%MC_ROOT%\tests" /I"%MC_OBJ%\shaders""
if exist "%MC_TOBJ%\" rd /s /q "%MC_TOBJ%"
mkdir "%MC_TOBJ%"
if errorlevel 1 (
  echo [tests] cannot create %MC_TOBJ%
  exit /b 1
)

set "MC_NFILES=0"
call :compile_dir "%MC_ROOT%\tests" tests
if errorlevel 1 goto :failed
if exist "%MC_ROOT%\src\" (
  call :compile_dir "%MC_ROOT%\src" src
  if errorlevel 1 goto :failed
  for /d /r "%MC_ROOT%\src" %%D in (*) do (
    set "MC_REL=%%~fD"
    set "MC_REL=!MC_REL:%MC_ROOT%\=!"
    set "MC_REL=!MC_REL:\=_!"
    call :compile_dir "%%~fD" "!MC_REL!"
    if errorlevel 1 goto :failed
  )
) else (
  echo [tests] no src\ directory yet - building the test framework only
)
if "%MC_NFILES%"=="0" (
  echo [tests] no .cpp files found
  goto :failed
)

rem ---- link everything under %MC_TOBJ%
set "MC_LRSP=%MC_TOBJ%\_link.rsp"
if exist "%MC_LRSP%" del "%MC_LRSP%"
for /r "%MC_TOBJ%" %%O in (*.obj) do >>"%MC_LRSP%" echo "%%~fO"
set "MC_PDBOPT="
if "%MC_CONFIG%"=="debug" set "MC_PDBOPT=/PDB:"%MC_OUT%\tests.pdb""
echo [tests] link tests.exe (%MC_NFILES% sources)
link %MC_LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%MC_OUT%\tests.exe" %MC_PDBOPT% @"%MC_LRSP%" %MC_LIBS% kernel32.lib
if errorlevel 1 (
  echo [tests] link failed
  goto :failed
)

rem ---- run
echo [tests] running %MC_OUT%\tests.exe %*
"%MC_OUT%\tests.exe" %*
set "MC_TRC=%ERRORLEVEL%"
if "%MC_TRC%"=="0" (
  echo [tests] all tests passed
) else (
  echo [tests] tests.exe exit code %MC_TRC%
)
exit /b %MC_TRC%

:failed
echo [tests] BUILD FAILED
exit /b 1

rem ---------------------------------------------------------------- :compile_dir <dir> <objsubdir>
rem Compiles <dir>\*.cpp (non-recursive, minus %MC_EXCLUDE%) with one cl /MP call.
:compile_dir
set "MC_DIR=%~1"
set "MC_ODIR=%MC_TOBJ%\%~2"
set "MC_RSP=%MC_TOBJ%\_%~2.rsp"
set "MC_N=0"
for %%F in ("%MC_DIR%\*.cpp") do (
  if /i not "%%~fF"=="%MC_EXCLUDE%" (
    >>"%MC_RSP%" echo "%%~fF"
    set /a MC_N+=1
  )
)
if "!MC_N!"=="0" exit /b 0
set /a MC_NFILES+=MC_N
if not exist "%MC_ODIR%\" mkdir "%MC_ODIR%"
echo [tests] cl   %~2 (!MC_N! files)
cl %MC_CFLAGS% %MC_TINC% /MP /c /Fo"%MC_ODIR%\\" /Fd"%MC_TOBJ%\vc.pdb" @"%MC_RSP%"
if errorlevel 1 (
  echo [tests] compile failed in %MC_DIR%
  exit /b 1
)
exit /b 0
