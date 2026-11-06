@echo off
rem build.cmd - MotionCalm build entry point.
rem   build.cmd [all|app|tools|tests] [debug|release]     (any order; defaults: all release)
rem Sets MC_CONFIG, calls scripts\env.cmd once, then scripts\build_<target>.cmd.
rem Exit code: 0 when every requested target succeeded (or app was skipped), 1 otherwise.
rem Keep this file ASCII-only (cmd parses batch files in the OEM code page).
setlocal EnableExtensions
set "MC_BUILD_DIR=%~dp0"
set "MC_TARGET="
set "MC_CONFIG="

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="all"     (set "MC_TARGET=all"     & shift & goto :parse)
if /i "%~1"=="app"     (set "MC_TARGET=app"     & shift & goto :parse)
if /i "%~1"=="tools"   (set "MC_TARGET=tools"   & shift & goto :parse)
if /i "%~1"=="tests"   (set "MC_TARGET=tests"   & shift & goto :parse)
if /i "%~1"=="debug"   (set "MC_CONFIG=debug"   & shift & goto :parse)
if /i "%~1"=="release" (set "MC_CONFIG=release" & shift & goto :parse)
if /i "%~1"=="help"    goto :usage
if /i "%~1"=="-h"      goto :usage
if /i "%~1"=="--help"  goto :usage
if "%~1"=="/?"         goto :usage
echo [build] unknown argument: %1
call :print_usage
exit /b 2

:usage
call :print_usage
exit /b 0

:print_usage
echo usage: build.cmd [all^|app^|tools^|tests] [debug^|release]
echo   all     app + tools + tests (default)
echo   app     MotionCalm.exe via scripts\build_app.cmd (skipped if not present yet)
echo   tools   fakegame.exe, inputsim.exe, capturetest.exe
echo   tests   tests.exe - compiled and run
echo   output: out\^<config^>\    objects: out\^<config^>\obj\
exit /b 0

:parsed
if not defined MC_TARGET set "MC_TARGET=all"
if not defined MC_CONFIG set "MC_CONFIG=release"

call "%MC_BUILD_DIR%scripts\env.cmd"
if errorlevel 1 (
  echo [build] environment setup failed
  exit /b 1
)
echo [build] target=%MC_TARGET% config=%MC_CONFIG% out=%MC_OUT%

set "MC_ANY_FAIL=0"
set "MC_SUM_app="
set "MC_SUM_tools="
set "MC_SUM_tests="
if /i "%MC_TARGET%"=="all" (
  call :run_target app
  call :run_target tools
  call :run_target tests
) else (
  call :run_target %MC_TARGET%
)

echo.
echo ================ build summary (%MC_CONFIG%) ================
if defined MC_SUM_app   echo   app   : %MC_SUM_app%
if defined MC_SUM_tools echo   tools : %MC_SUM_tools%
if defined MC_SUM_tests echo   tests : %MC_SUM_tests%
if "%MC_ANY_FAIL%"=="0" (
  echo [build] OK
  exit /b 0
)
echo [build] FAILED
exit /b 1

rem ---------------------------------------------------------------- :run_target <name>
:run_target
set "MC_T=%~1"
set "MC_S=%MC_BUILD_DIR%scripts\build_%MC_T%.cmd"
echo.
echo ---------------- [%MC_T%] ----------------
if exist "%MC_S%" goto :run_target_go
if /i "%MC_T%"=="app" (
  echo build_app.cmd not yet present
  set "MC_SUM_app=SKIPPED (scripts\build_app.cmd not yet present)"
  exit /b 0
)
echo [build] missing %MC_S%
set "MC_SUM_%MC_T%=FAILED (missing scripts\build_%MC_T%.cmd)"
set "MC_ANY_FAIL=1"
exit /b 0
:run_target_go
call "%MC_S%"
set "MC_RC=%ERRORLEVEL%"
if "%MC_RC%"=="0" (
  set "MC_SUM_%MC_T%=OK"
) else (
  set "MC_SUM_%MC_T%=FAILED (exit %MC_RC%)"
  set "MC_ANY_FAIL=1"
)
exit /b 0
