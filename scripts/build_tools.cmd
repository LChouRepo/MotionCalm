@echo off
rem build_tools.cmd - builds the test tools into %MC_OUT%:
rem   tools\fakegame\main.cpp + src\capture\motion_estimator.cpp + similarity_estimator.cpp -> fakegame.exe
rem       (console; the estimators are only used by fakegame --selfcheck)
rem   tools\inputsim\main.cpp -> inputsim.exe   (console subsystem)
rem   tools\capturetest\main.cpp + src\capture\*.cpp + src\util\log.cpp -> capturetest.exe (console)
rem   tools\settingsdemo\main.cpp + src\**\*.cpp except src\app\ + src\res\app.rc -> settingsdemo.exe
rem       (console; the real settings window driven by a fake SettingsHost; + gdiplus.lib for PNG screenshots)
rem Uses %MC_CONFIG% (debug|release, default release). Can be run standalone or from build.cmd.
rem capturetest and settingsdemo compile src\capture\gpu_downscale.cpp (and settingsdemo also
rem src\overlay\renderer.cpp), which include the fxc headers: build_shaders.cmd runs first and those
rem two get /I"%MC_OBJ%\shaders". fakegame and inputsim do not need the shaders.
rem Keep this file ASCII-only.
setlocal EnableExtensions
call "%~dp0env.cmd"
if errorlevel 1 exit /b 1
call "%~dp0build_shaders.cmd"
if errorlevel 1 (
  echo [tools] FAILED
  exit /b 1
)
set "MC_SHINC=/I"%MC_OBJ%\shaders""

set "MC_TOOLS_FAIL=0"
call :build_one fakegame "%MC_ROOT%\src\capture\motion_estimator.cpp" "%MC_ROOT%\src\capture\similarity_estimator.cpp"
if errorlevel 1 set "MC_TOOLS_FAIL=1"
call :build_one inputsim
if errorlevel 1 set "MC_TOOLS_FAIL=1"
call :build_capturetest
if errorlevel 1 set "MC_TOOLS_FAIL=1"
call :build_settingsdemo
if errorlevel 1 set "MC_TOOLS_FAIL=1"
if "%MC_TOOLS_FAIL%"=="0" (
  echo [tools] OK
  exit /b 0
)
echo [tools] FAILED
exit /b 1

rem ---------------------------------------------------------------- :build_one <name> [extra .cpp ...]
rem Compiles tools\<name>\main.cpp plus any extra sources (include root src\) into one exe.
rem The obj dir is wiped so deleted sources do not linger.
:build_one
set "MC_NAME=%~1"
set "MC_SRC=%MC_ROOT%\tools\%MC_NAME%\main.cpp"
set "MC_TOBJ=%MC_OBJ%\tools\%MC_NAME%"
if not exist "%MC_SRC%" (
  echo [tools] missing %MC_SRC%
  exit /b 1
)
if exist "%MC_TOBJ%\" rd /s /q "%MC_TOBJ%"
mkdir "%MC_TOBJ%"
set "MC_CRSP=%MC_TOBJ%\_cl.rsp"
>"%MC_CRSP%" echo "%MC_SRC%"
:build_one_extra
shift
if "%~1"=="" goto :build_one_compile
>>"%MC_CRSP%" echo "%~1"
goto :build_one_extra
:build_one_compile
echo [tools] cl   %MC_NAME%
cl %MC_CFLAGS% /I"%MC_ROOT%\src" /MP /c /Fo"%MC_TOBJ%\\" /Fd"%MC_TOBJ%\vc.pdb" @"%MC_CRSP%"
if errorlevel 1 (
  echo [tools] compile failed: %MC_NAME%
  exit /b 1
)
set "MC_LRSP=%MC_TOBJ%\_link.rsp"
if exist "%MC_LRSP%" del "%MC_LRSP%"
for %%O in ("%MC_TOBJ%\*.obj") do >>"%MC_LRSP%" echo "%%~fO"
set "MC_PDBOPT="
if "%MC_CONFIG%"=="debug" set "MC_PDBOPT=/PDB:"%MC_OUT%\%MC_NAME%.pdb""
echo [tools] link %MC_NAME%.exe
link %MC_LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%MC_OUT%\%MC_NAME%.exe" %MC_PDBOPT% @"%MC_LRSP%" %MC_LIBS% kernel32.lib
if errorlevel 1 (
  echo [tools] link failed: %MC_NAME%
  exit /b 1
)
exit /b 0

rem ---------------------------------------------------------------- :build_capturetest
rem capturetest links the real capture pipeline: tools\capturetest\main.cpp + src\capture\*.cpp
rem + src\util\log.cpp (include root src\). The obj dir is wiped so deleted sources do not linger.
:build_capturetest
set "MC_NAME=capturetest"
set "MC_SRC=%MC_ROOT%\tools\capturetest\main.cpp"
set "MC_TOBJ=%MC_OBJ%\tools\capturetest"
if not exist "%MC_SRC%" (
  echo [tools] missing %MC_SRC%
  exit /b 1
)
if exist "%MC_TOBJ%\" rd /s /q "%MC_TOBJ%"
mkdir "%MC_TOBJ%"
set "MC_CRSP=%MC_TOBJ%\_cl.rsp"
>"%MC_CRSP%" echo "%MC_SRC%"
for %%F in ("%MC_ROOT%\src\capture\*.cpp") do >>"%MC_CRSP%" echo "%%~fF"
>>"%MC_CRSP%" echo "%MC_ROOT%\src\util\log.cpp"
echo [tools] cl   %MC_NAME%
cl %MC_CFLAGS% /I"%MC_ROOT%\src" %MC_SHINC% /MP /c /Fo"%MC_TOBJ%\\" /Fd"%MC_TOBJ%\vc.pdb" @"%MC_CRSP%"
if errorlevel 1 (
  echo [tools] compile failed: %MC_NAME%
  exit /b 1
)
set "MC_LRSP=%MC_TOBJ%\_link.rsp"
if exist "%MC_LRSP%" del "%MC_LRSP%"
for %%O in ("%MC_TOBJ%\*.obj") do >>"%MC_LRSP%" echo "%%~fO"
set "MC_PDBOPT="
if "%MC_CONFIG%"=="debug" set "MC_PDBOPT=/PDB:"%MC_OUT%\%MC_NAME%.pdb""
echo [tools] link %MC_NAME%.exe
link %MC_LDFLAGS% /SUBSYSTEM:CONSOLE /OUT:"%MC_OUT%\%MC_NAME%.exe" %MC_PDBOPT% @"%MC_LRSP%" %MC_LIBS% kernel32.lib
if errorlevel 1 (
  echo [tools] link failed: %MC_NAME%
  exit /b 1
)
exit /b 0

rem ---------------------------------------------------------------- :build_settingsdemo
rem settingsdemo opens the real settings window (src\ui) without the app: tools\settingsdemo\main.cpp
rem + every src\**\*.cpp except src\app\ (one cl /MP call per source directory, objects in per-directory
rem subfolders so equal base names never collide) + src\res\app.rc (dialog templates, manifest, INI template).
:build_settingsdemo
setlocal EnableDelayedExpansion
set "MC_NAME=settingsdemo"
set "MC_SRC=%MC_ROOT%\tools\settingsdemo\main.cpp"
set "MC_TOBJ=%MC_OBJ%\tools\settingsdemo"
if not exist "%MC_SRC%" (
  echo [tools] missing %MC_SRC%
  exit /b 1
)
if exist "%MC_TOBJ%\" rd /s /q "%MC_TOBJ%"
mkdir "%MC_TOBJ%"
call :sd_compile_dir "%MC_ROOT%\tools\settingsdemo" tool
if errorlevel 1 exit /b 1
call :sd_compile_dir "%MC_ROOT%\src" src
if errorlevel 1 exit /b 1
for /d /r "%MC_ROOT%\src" %%D in (*) do (
  set "MC_REL=%%~fD"
  set "MC_REL=!MC_REL:%MC_ROOT%\src\=!"
  set "MC_SKIP=0"
  if /i "!MC_REL!"=="app" set "MC_SKIP=1"
  if /i "!MC_REL:~0,4!"=="app\" set "MC_SKIP=1"
  if "!MC_SKIP!"=="0" (
    set "MC_REL=!MC_REL:\=_!"
    call :sd_compile_dir "%%~fD" "!MC_REL!"
    if errorlevel 1 exit /b 1
  )
)
echo [tools] rc   %MC_NAME% (app.rc)
rc /nologo /c65001 /I"%MC_ROOT%\src\res" /fo"%MC_TOBJ%\app.res" "%MC_ROOT%\src\res\app.rc"
if errorlevel 1 (
  echo [tools] rc failed: %MC_NAME%
  exit /b 1
)
set "MC_LRSP=%MC_TOBJ%\_link.rsp"
if exist "%MC_LRSP%" del "%MC_LRSP%"
for /r "%MC_TOBJ%" %%O in (*.obj) do >>"%MC_LRSP%" echo "%%~fO"
set "MC_PDBOPT="
if "%MC_CONFIG%"=="debug" set "MC_PDBOPT=/PDB:"%MC_OUT%\%MC_NAME%.pdb""
echo [tools] link %MC_NAME%.exe
link %MC_LDFLAGS% /SUBSYSTEM:CONSOLE /MANIFEST:NO /OUT:"%MC_OUT%\%MC_NAME%.exe" %MC_PDBOPT% @"%MC_LRSP%" "%MC_TOBJ%\app.res" %MC_LIBS% gdiplus.lib kernel32.lib
if errorlevel 1 (
  echo [tools] link failed: %MC_NAME%
  exit /b 1
)
exit /b 0

rem ---------------------------------------------------------------- :sd_compile_dir <dir> <objsubdir>
rem Compiles <dir>\*.cpp (non-recursive) with one cl /MP call (needs delayed expansion, see above).
:sd_compile_dir
set "MC_DIR=%~1"
set "MC_ODIR=%MC_TOBJ%\%~2"
set "MC_RSP=%MC_TOBJ%\_%~2.rsp"
set "MC_N=0"
for %%F in ("%MC_DIR%\*.cpp") do (
  >>"%MC_RSP%" echo "%%~fF"
  set /a MC_N+=1
)
if "!MC_N!"=="0" exit /b 0
if not exist "%MC_ODIR%\" mkdir "%MC_ODIR%"
echo [tools] cl   %MC_NAME%: %~2 (!MC_N! files)
cl %MC_CFLAGS% /I"%MC_ROOT%\src" %MC_SHINC% /MP /c /Fo"%MC_ODIR%\\" /Fd"%MC_TOBJ%\vc.pdb" @"%MC_RSP%"
if errorlevel 1 (
  echo [tools] compile failed: %MC_NAME% in %MC_DIR%
  exit /b 1
)
exit /b 0
