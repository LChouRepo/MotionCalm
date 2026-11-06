<#
.SYNOPSIS
  MotionCalm 集成验证（DESIGN.md §13）：起 app → 起假游戏 → 模拟鼠标转动 → 截图 → 解析日志判定 PASS/FAIL。

.DESCRIPTION
  会占用屏幕和鼠标约 20 秒：fakegame 全屏、隐藏并锁住光标，inputsim 发送相对鼠标移动。运行期间不要碰鼠标键盘
  （任何按键都会让 fakegame 提前退出）。

  时间线（相对各进程启动）：
    app      : MotionCalm.exe --no-capture --verbose --force-game --exit-after 20   （不存在或 -SkipApp 时跳过）
    +0.5 s   : fakegame.exe --seconds 15 --mode lock
    +1.0 s   : 截图 test-output\01_idle.png
    inputsim : --profile "idle:1,turn:3@8000,idle:4"   （0–1 s 静止，1–4 s 向右转 8000 counts/s，4–8 s 静止）
      +3.5 s : 截图 02_turning.png（转动开始后 2.5 s）
      +5.0 s : 截图 03_faded.png（转动结束后 1 s）
  然后等所有进程结束，解析 out\<cfg>\MotionCalm*.log。

  ── 日志契约（app 代理请按此输出；--verbose 时生效）─────────────────────────────────────────────
  * 每行带本地时间戳，毫秒精度。取行内第一个匹配（$TimestampRegex）：
        (?:(?<date>\d{4}-\d{2}-\d{2})[ T])?(?<time>\d{1,2}:\d{2}:\d{2}(?:[.,]\d{1,6})?)
    没有日期时按测试当天处理。与 core-overlay 规格的行格式一致：
        2026-11-03 14:05:12.345 [debug] <消息>
  * 平滑后的强度 IntensityModel::value()（0..1），两种写法都认（每行先试第一种）：
      a) $IntensityRegex   \bintensity=([0-9.]+)          任意行；target_intensity= 之类不会被 \b 匹配
      b) $FrameValueRegex  \bframe\b.*?\bvalue=([0-9.]+)  core-overlay 规格"调试帧日志"的 frame 行：
         frame t=.. in_game=.. locked=.. mouse=.. moving=.. pad=.. target=0.000 value=0.000 exe=..
  * 样本按"保持到下一行"解释（日志可以只在值变化时写），所以静止期间也要定期写（规格：≥ 每 250 ms 一行）。
  * 可选：退出时的 info 行  exit reason=.. wall_s=.. cpu_s=.. cpu_pct=..  会被原样打印。
  ───────────────────────────────────────────────────────────────────────────────────────────────

  判定（需要 app 与日志）：
    1. 静止段（inputsim 第 1 段 idle）内强度恒为 0
    2. 转动段内最大强度 >= 0.8
    3. 转动结束后 0.6 s 时强度 <= 0.05，且之后到 +2.0 s 一直 <= 0.05（并打印首次 <= 0.05 的实际耗时）
    4. 游戏内阶段 app 的 CPU 时间 / 墙钟时间 < 2%（单核占比，Get-Process 采样）
  另外打印整体、转动中与游戏结束后空闲阶段的 CPU 占比。

.PARAMETER Config
  debug | release（默认 release），二进制取自 out\<Config>\。

.PARAMETER SkipApp
  不启动 MotionCalm.exe，只跑 fakegame + inputsim + 截图（用于检查测试工具本身）。

.PARAMETER Capture
  改跑截屏运动检测场景（capture-motion-detection tasks 5.2 / 5.3 / 5.6）：不发任何模拟鼠标，只靠画面运动触发。
    app      : MotionCalm.exe --config test-output\capture_verify.ini --verbose --force-game --exit-after 16
               （不带 --no-capture；每次先删掉这份 INI，让它按内嵌模板重新生成 = 默认配置：capture=1、hfov 90）
    +0.5 s   : fakegame.exe --seconds 12 --mode lock --texture noise --scroll <px/s> --scroll-start 2
               --scroll-seconds 4 --flash-at 8.5
               （noise = 非周期纹理；默认 64 px 网格是周期性的，会让位移估计锁到错误的整周期上）
               px/s = CaptureDegPerS × 主显示器宽 / 90，即默认 hfov 下约 CaptureDegPerS °/s
    +4 s     : 截图 test-output\04_capture_scroll.png（截屏检测开启时 overlay 对截屏隐藏，图里不应有遮罩，这是预期）
  判定（时刻取 fakegame 打印的 wall= 墙钟）：
    1. 日志有 "capture started:" 与 "overlay capture visibility: excluded from capture"，没有 "capture failed"
    2. 滚动前 1 s 内强度 <= 0.05
    3. 滚动的 4 s 内最大强度 > 0.3，且这段时间所有 frame 行 mouse=0.0（强度只来自截屏）
    4. 闪白前 0.1 s 到闪白后 1.5 s 内强度 <= 0.1
  另外打印滚动段 frame 行 capture= 的中位数（理论约 CaptureDegPerS）与 capture perf: 行。

.PARAMETER CaptureDegPerS
  -Capture 场景的滚动速度（°/s，默认 400；按默认阈值 90 / 540 °/s 对应目标强度约 0.77）。

.PARAMETER Forward
  改跑前进 / 倾斜检测场景（forward-and-roll-detection）：不发任何模拟鼠标，画面先放大（向前）再旋转（倾斜）。
    app      : MotionCalm.exe --config test-output\forward_verify.ini --verbose --force-game --exit-after 17
               （每次先删掉这份 INI = 默认配置：capture=1、hfov 90、forward_gain 1.0、detect_roll 1、
               前进阈值中档 20 / 150 °/s、倾斜阈值中档 15 / 90 °/s）
    +0.5 s   : fakegame.exe --seconds 13 --mode lock --texture noise --zoom <ForwardRate> --zoom-start 2
               --zoom-seconds 3 --roll <RollDegPerS> --roll-start 7 --roll-seconds 3
               （--zoom / --roll 让 fakegame 改画随缩放滚动的程序化场景；0–2 s 与 5–7 s、10 s 之后画面静止）
    +3.5 s   : 截图 test-output\05_forward_zoom.png；+8.5 s：06_forward_roll.png（截屏检测开启时 overlay 对截屏隐藏）
  判定（时刻取 fakegame 打印的 wall= 墙钟）：
    1. 日志有 "capture started:" 与 "overlay capture visibility: excluded from capture"，没有 "capture failed"
    2. 放大前 1 s 内强度 <= 0.05
    3. 放大段：最大强度 > 0.5，所有 frame 行 mouse=0.0；frame 行 zoom= 的中位数（开始 0.5 s 后）在
       ForwardRate × 45（默认 hfov 90 的一半）的 ±25% 内
    4. 放大结束 1 s 后到旋转开始：强度 <= 0.05
    5. 旋转段：最大强度 > 0.5；frame 行 roll= 的中位数（开始 0.5 s 后）在 RollDegPerS 的 ±25% 内
  另外打印放大 / 旋转段 capture=（平移，应保持较低）、zoom= / roll= 的串扰与 capture perf: 行。

.PARAMETER ForwardRate
  -Forward 场景的放大速率 d(ln s)/dt（1/s，默认 3 → zoom ≈ 135 °/s，约为快速游动 / 冲刺；forward_gain 1.0、
  前进阈值中档 20 / 150 °/s → 目标强度约 0.96；估计偏低 25% 时仍约 0.68 > 0.5）。
  日常游动约 45–90 °/s（--zoom 1–2）对应 0.10–0.56。

.PARAMETER RollDegPerS
  -Forward 场景的旋转速度（°/s，默认 75 → 倾斜阈值中档 15 / 90 °/s 下目标强度约 0.90；偏低 25% 时约 0.57）。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Config debug -SkipApp
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Capture
  powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -Forward
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'release',
    [switch]$SkipApp,
    [switch]$Capture,
    [ValidateRange(100, 1000)]
    [double]$CaptureDegPerS = 400,
    [switch]$Forward,
    [ValidateRange(0.5, 15)]
    [double]$ForwardRate = 3,
    [ValidateRange(10, 1000)]
    [double]$RollDegPerS = 75
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "out\$Config"
$OutDir = Join-Path $Root 'test-output'
$AppExe = Join-Path $Bin 'MotionCalm.exe'
$FakeExe = Join-Path $Bin 'fakegame.exe'
$SimExe = Join-Path $Bin 'inputsim.exe'
$SimProfile = 'idle:1,turn:3@8000,idle:4'

# 正则（与上面的日志契约一致）
$IntensityRegex = '\bintensity=([0-9.]+)'
$FrameValueRegex = '\bframe\b.*?\bvalue=([0-9.]+)'
$ExitLineRegex = '\bexit reason=.*\bcpu_pct='
$TimestampRegex = '(?:(?<date>\d{4}-\d{2}-\d{2})[ T])?(?<time>\d{1,2}:\d{2}:\d{2}(?:[.,]\d{1,6})?)'
$SimSegRegex = '^seg=(?<n>\d+)\s+kind=(?<kind>\w+)\s.*\bt0=(?<t0>\S+)\s+t1=(?<t1>\S+)'

# ---------------------------------------------------------------- native helpers
if (-not ('MCVerifyNative' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class MCVerifyNative {
    [DllImport("user32.dll")] public static extern bool ClipCursor(IntPtr rect);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, uint rop);

    const uint SRCCOPY = 0x00CC0020;
    const uint CAPTUREBLT = 0x40000000;   // 包含分层 / 合成窗口（overlay）

    // 主显示器截图（等价于 Graphics.CopyFromScreen，但带 CAPTUREBLT）。主显示器左上角恒为 (0,0)。
    public static string CapturePrimary(string path) {
        int w = GetSystemMetrics(0), h = GetSystemMetrics(1);
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr dst = g.GetHdc();
                IntPtr src = GetDC(IntPtr.Zero);
                try { BitBlt(dst, 0, 0, w, h, src, 0, 0, SRCCOPY | CAPTUREBLT); }
                finally { ReleaseDC(IntPtr.Zero, src); g.ReleaseHdc(dst); }
            }
            bmp.Save(path, ImageFormat.Png);
        }
        return w + "x" + h;
    }
}
'@
}
# 截图要物理像素：在任何 DPI 相关调用之前声明 DPI aware。
[void][MCVerifyNative]::SetProcessDPIAware()

# ---------------------------------------------------------------- helpers
function Write-Banner([string[]]$Lines, [ConsoleColor]$Color) {
    $bar = '=' * 64
    Write-Host ''
    Write-Host $bar -ForegroundColor $Color
    foreach ($l in $Lines) { Write-Host ("  " + $l) -ForegroundColor $Color }
    Write-Host $bar -ForegroundColor $Color
    Write-Host ''
}

function Wait-Until([System.Diagnostics.Stopwatch]$Sw, [double]$Seconds) {
    $ms = [int](($Seconds - $Sw.Elapsed.TotalSeconds) * 1000)
    if ($ms -gt 0) { Start-Sleep -Milliseconds $ms }
}

function Save-Shot([string]$Name) {
    $path = Join-Path $OutDir $Name
    try {
        $size = [MCVerifyNative]::CapturePrimary($path)
        Write-Host ("[截图] {0}  ({1})  {2}" -f $Name, $size, (Get-Date).ToString('HH:mm:ss.fff'))
    } catch {
        Write-Host ("[截图] {0} 失败：{1}" -f $Name, $_.Exception.Message) -ForegroundColor Yellow
    }
}

# CPU 采样：返回 @{ Cpu = 秒; Wall = 秒（自进程启动）; At = DateTime }，进程已退出则返回 $null
function Get-CpuSample($Proc) {
    if ($null -eq $Proc) { return $null }
    try {
        $Proc.Refresh()
        if ($Proc.HasExited) { return $null }
        return @{
            Cpu  = $Proc.TotalProcessorTime.TotalSeconds
            Wall = ((Get-Date) - $Proc.StartTime).TotalSeconds
            At   = Get-Date
        }
    } catch {
        return $null
    }
}

function Format-CpuRatio($A, $B) {
    # 返回 [double] 占比（0..1），无法计算返回 $null
    if ($null -eq $A -or $null -eq $B) { return $null }
    $dw = $B.Wall - $A.Wall
    if ($dw -le 0.2) { return $null }
    return ($B.Cpu - $A.Cpu) / $dw
}

function Convert-LogTime([System.Text.RegularExpressions.Match]$M, [datetime]$DefaultDate) {
    $timeText = $M.Groups['time'].Value -replace ',', '.'
    $parts = $timeText.Split('.')
    $hms = [datetime]::ParseExact($parts[0], 'H:mm:ss', [Globalization.CultureInfo]::InvariantCulture)
    $date = $DefaultDate.Date
    if ($M.Groups['date'].Success) {
        $date = [datetime]::ParseExact($M.Groups['date'].Value, 'yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture)
    }
    $t = $date.Add($hms.TimeOfDay)
    if ($parts.Count -gt 1) {
        $frac = [double]('0.' + $parts[1])
        $t = $t.AddMilliseconds($frac * 1000.0)
    }
    return $t
}

function Parse-SimTime([string]$Text) {
    return [datetime]::ParseExact($Text, 'yyyy-MM-ddTHH:mm:ss.fff', [Globalization.CultureInfo]::InvariantCulture)
}

$Checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$Name, [bool]$Pass, [string]$Detail) {
    $Checks.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail })
}

# 打印 PASS / FAIL 汇总并以 0（全部通过）/ 1 退出。
function Exit-WithSummary([string]$Shots, [bool]$RanApp) {
    Write-Host ''
    $failed = 0
    foreach ($c in $Checks) {
        if ($c.Pass) {
            Write-Host ("[PASS] {0}  —  {1}" -f $c.Name, $c.Detail) -ForegroundColor Green
        } else {
            $failed++
            Write-Host ("[FAIL] {0}  —  {1}" -f $c.Name, $c.Detail) -ForegroundColor Red
        }
    }
    Write-Host ''
    Write-Host ("截图与输出在 {0}：{1}" -f $OutDir, $Shots)
    if (-not $RanApp) { Write-Host '（未运行 app：只验证了测试工具）' -ForegroundColor Yellow }
    if ($failed -eq 0) {
        Write-Host ("验证通过（{0} 项）" -f $Checks.Count) -ForegroundColor Green
        exit 0
    }
    Write-Host ("验证失败：{0} / {1} 项未通过" -f $failed, $Checks.Count) -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------- preflight
foreach ($exe in @($FakeExe, $SimExe)) {
    if (-not (Test-Path $exe)) {
        Write-Host "[错误] 找不到 $exe —— 先运行 build.cmd tools $Config" -ForegroundColor Red
        exit 2
    }
}
$RunApp = (-not $SkipApp) -and (Test-Path $AppExe)
if (-not $SkipApp -and -not $RunApp) {
    Write-Host "[提示] 找不到 $AppExe，跳过 app，只验证测试工具与截图。" -ForegroundColor Yellow
}
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$FakeOut = Join-Path $OutDir 'fakegame.out.txt'
$FakeErr = Join-Path $OutDir 'fakegame.err.txt'
$SimOut = Join-Path $OutDir 'inputsim.out.txt'
$SimErr = Join-Path $OutDir 'inputsim.err.txt'

# ================================================================ -Forward：前进 / 倾斜检测场景
if ($Forward) {
    if (-not (Test-Path $AppExe)) {
        Write-Host "[错误] 找不到 $AppExe —— 先运行 build.cmd all $Config" -ForegroundColor Red
        exit 2
    }
    $zoomExpect = $ForwardRate * 90.0 / 2.0   # 默认 hfov 90：|ln s|/dt × hfov/2
    $fwdIni = Join-Path $OutDir 'forward_verify.ini'
    Remove-Item -LiteralPath $fwdIni, ($fwdIni + '.tmp') -ErrorAction SilentlyContinue
    $inv = [Globalization.CultureInfo]::InvariantCulture
    $fakeArgs = @('--seconds', '13', '--mode', 'lock', '--texture', 'noise',
        '--zoom', $ForwardRate.ToString($inv), '--zoom-start', '2', '--zoom-seconds', '3',
        '--roll', $RollDegPerS.ToString($inv), '--roll-start', '7', '--roll-seconds', '3')

    Write-Banner -Color Yellow -Lines @(
        '!!! 前进 / 倾斜检测测试进行中，请勿移动鼠标、请勿按键 !!!',
        '',
        '接下来约 18 秒：假游戏会全屏、锁住并隐藏鼠标，画面在第 2–5 秒持续放大（向前），第 7–10 秒旋转（倾斜）。',
        '这一段不发送任何模拟鼠标：遮罩只能由画面运动触发。碰了鼠标会让判定失败，按键会让假游戏提前退出。',
        ('配置：{0}   放大 {1}/s（约 {2:N1}°/s 前进流速）   旋转 {3}°/s' -f $Config, $ForwardRate, $zoomExpect, $RollDegPerS)
    )
    for ($i = 3; $i -ge 1; $i--) {
        Write-Host ("  {0} 秒后开始……" -f $i) -ForegroundColor Yellow
        Start-Sleep -Seconds 1
    }

    $app = $null; $fake = $null; $appStart = $null; $runError = $null
    $testStart = Get-Date
    try {
        [void][MCVerifyNative]::AllowSetForegroundWindow(-1)
        $app = Start-Process -FilePath $AppExe -WorkingDirectory $Bin -PassThru `
            -ArgumentList @('--config', ('"{0}"' -f $fwdIni), '--verbose', '--force-game', '--exit-after', '17')
        $null = $app.Handle
        $appStart = Get-Date
        Write-Host ("[app] 已启动 pid={0}（截屏检测开启）" -f $app.Id)
        Start-Sleep -Milliseconds 500

        $fake = Start-Process -FilePath $FakeExe -WorkingDirectory $Bin -PassThru -NoNewWindow `
            -RedirectStandardOutput $FakeOut -RedirectStandardError $FakeErr -ArgumentList $fakeArgs
        $null = $fake.Handle
        Write-Host ("[fakegame] 已启动 pid={0}  {1}" -f $fake.Id, ($fakeArgs -join ' '))
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        Wait-Until $sw 3.5
        Save-Shot '05_forward_zoom.png'
        Wait-Until $sw 8.5
        Save-Shot '06_forward_roll.png'

        if (-not $fake.WaitForExit(20000)) { Write-Host '[fakegame] 超时未退出' -ForegroundColor Yellow }
        [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
        if (-not $app.WaitForExit(15000)) { Write-Host '[app] --exit-after 17 后仍未退出，将被强制结束' -ForegroundColor Yellow }
    } catch {
        $runError = $_.Exception.Message
        Write-Host ("[错误] 运行过程出错：{0}" -f $runError) -ForegroundColor Red
    } finally {
        [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
        foreach ($p in @($fake, $app)) {
            if ($null -ne $p) {
                try {
                    if (-not $p.HasExited) {
                        Write-Host ("[清理] 结束残留进程 pid={0}" -f $p.Id) -ForegroundColor Yellow
                        $p.Kill()
                        [void]$p.WaitForExit(3000)
                    }
                } catch { }
            }
        }
        [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
    }

    Write-Host ''
    Write-Host '---------------- 结果（前进 / 倾斜检测）----------------'
    if ($null -ne $runError) { Add-Check '流程正常执行' $false $runError }
    $fakeCode = $(try { $fake.ExitCode } catch { $null })
    Add-Check 'fakegame 正常退出' ($fakeCode -eq 0) ("exit={0}" -f $fakeCode)
    $events = @{}
    if (Test-Path $FakeOut) {
        foreach ($line in (Get-Content $FakeOut)) {
            Write-Host ("  [fakegame] " + $line)
            $m = [regex]::Match($line, '\[fakegame\] (?<ev>zoom start|zoom stop|roll start|roll stop) .*\bwall=(?<w>\S+)')
            if ($m.Success) { $events[$m.Groups['ev'].Value] = Parse-SimTime $m.Groups['w'].Value }
        }
    }

    # 日志：frame 行（强度、鼠标、平移 capture=、行尾的 zoom= / roll=）与截屏相关的 info / warn 行
    $frameRx = '\bframe t=\S+ in_game=(?<g>\d) locked=(?<l>\d) mouse=(?<m>[0-9.]+) moving=\d pad=[0-9.]+ capture=(?<c>[0-9.]+) target=(?<tg>[0-9.]+) value=(?<v>[0-9.]+) exe=(?<exe>\S*)'
    $tailRx = '\bzoom=(?<z>[0-9.]+) roll=(?<r>[0-9.]+)\s*$'
    $frames = New-Object System.Collections.Generic.List[object]
    $capLines = New-Object System.Collections.Generic.List[string]
    $runFrom = $appStart.AddSeconds(-1)
    $runTo = (Get-Date).AddSeconds(1)
    $logs = @(Get-ChildItem -Path $Bin -Filter 'MotionCalm*.log*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
    foreach ($log in $logs) {
        foreach ($line in (Get-Content -LiteralPath $log.FullName -Encoding UTF8)) {
            $isCap = $line -match '\bcapture (started|released|failed|access lost|grid rebuilt|perf:)|capture motion detection: thread started|overlay capture visibility|hide_from_capture=never|截屏检测'
            $mf = [regex]::Match($line, $frameRx)
            if (-not $isCap -and -not $mf.Success) { continue }
            $mt = [regex]::Match($line, $TimestampRegex)
            if (-not $mt.Success) { continue }
            try { $t = Convert-LogTime $mt $testStart } catch { continue }
            if ($t -lt $runFrom -or $t -gt $runTo) { continue }
            if ($isCap) { $capLines.Add($line) }
            if ($mf.Success) {
                $mz = [regex]::Match($line, $tailRx)
                $frames.Add([pscustomobject]@{
                    T = $t
                    Mouse = [double]::Parse($mf.Groups['m'].Value, $inv)
                    Cap = [double]::Parse($mf.Groups['c'].Value, $inv)
                    V = [double]::Parse($mf.Groups['v'].Value, $inv)
                    Zoom = $(if ($mz.Success) { [double]::Parse($mz.Groups['z'].Value, $inv) } else { [double]::NaN })
                    Roll = $(if ($mz.Success) { [double]::Parse($mz.Groups['r'].Value, $inv) } else { [double]::NaN })
                })
            }
        }
    }
    if ($logs.Count -gt 0) {
        Copy-Item -LiteralPath $logs[-1].FullName -Destination (Join-Path $OutDir 'MotionCalm.forward.log') -Force
    }
    $frames = @($frames | Sort-Object T)
    Write-Host ("  [app] exit={0}；本次运行的 frame 行 {1} 条" -f $(try { $app.ExitCode } catch { 'n/a' }), $frames.Count)
    foreach ($l in $capLines) { Write-Host ("  [app] " + ($l -replace '^\S+ \S+ ', '')) }

    # frame 行按"保持到下一行"解释
    function Get-FwdWindow([datetime]$A, [datetime]$B) {
        $held = $null
        $vals = New-Object System.Collections.Generic.List[object]
        foreach ($f in $frames) {
            if ($f.T -le $A) { $held = $f } elseif ($f.T -le $B) { $vals.Add($f) } else { break }
        }
        if ($null -ne $held) { $vals.Insert(0, $held) }
        return , $vals
    }
    function Get-Median($Values) {
        $v = @($Values | Where-Object { -not [double]::IsNaN($_) } | Sort-Object)
        if ($v.Count -eq 0) { return $null }
        return $v[[int][math]::Floor($v.Count / 2)]
    }

    $started = @($capLines | Where-Object { $_ -match '\bcapture started:' }).Count -gt 0
    $excluded = @($capLines | Where-Object { $_ -match 'overlay capture visibility: excluded from capture' }).Count -gt 0
    $capFailed = @($capLines | Where-Object { $_ -match '\bcapture failed\b' }).Count -gt 0
    Add-Check '截屏开始取帧（capture started）' $started $(if ($started) { '有' } else { '日志中没有 capture started: 行' })
    Add-Check 'overlay 对截屏隐藏（auto + 截屏检测开启）' $excluded $(if ($excluded) { '有 excluded from capture 行' } else { '没有找到' })
    Add-Check '没有进入"采集失败"' (-not $capFailed) $(if ($capFailed) { '日志有 capture failed' } else { '无' })
    $withTail = @($frames | Where-Object { -not [double]::IsNaN($_.Zoom) }).Count
    Add-Check 'frame 行末尾有 zoom= / roll=' ($frames.Count -gt 0 -and $withTail -eq $frames.Count) ("{0} / {1} 行" -f $withTail, $frames.Count)

    $z0 = $events['zoom start']; $z1 = $events['zoom stop']; $r0 = $events['roll start']; $r1 = $events['roll stop']
    if ($null -eq $z0 -or $null -eq $z1 -or $null -eq $r0 -or $null -eq $r1 -or $frames.Count -eq 0) {
        Add-Check '能对齐时间线' $false ('fakegame 事件 {0} 个、frame 行 {1} 条（需要 zoom start / zoom stop / roll start / roll stop 与 frame 行）' -f $events.Count, $frames.Count)
    } else {
        # 2. 放大前 1 s 静止
        $pre = Get-FwdWindow $z0.AddSeconds(-1.0) $z0
        if ($pre.Count -eq 0) {
            Add-Check '放大前静止时强度 <= 0.05' $false '放大前 1 s 没有样本'
        } else {
            $mx = ($pre | Measure-Object -Property V -Maximum).Maximum
            Add-Check '放大前静止时强度 <= 0.05' ($mx -le 0.05) ("{0} 个样本，最大 {1:N3}" -f $pre.Count, $mx)
        }
        # 3. 放大段
        $zw = Get-FwdWindow $z0 $z1
        if ($zw.Count -eq 0) {
            Add-Check '放大中最大强度 > 0.5' $false '放大段没有样本'
        } else {
            $mx = ($zw | Measure-Object -Property V -Maximum).Maximum
            $first = $zw | Where-Object { $_.T -gt $z0 -and $_.V -gt 0.5 } | Select-Object -First 1
            $rise = if ($first) { '{0:N0} ms 后超过 0.5' -f ($first.T - $z0).TotalMilliseconds } else { '未超过 0.5' }
            Add-Check '放大中最大强度 > 0.5' ($mx -gt 0.5) ("{0} 个样本，最大 {1:N3}，{2}" -f $zw.Count, $mx, $rise)
            $mmx = ($zw | Measure-Object -Property Mouse -Maximum).Maximum
            Add-Check '放大中 mouse=0.0（强度只来自画面）' ($mmx -eq 0) ("frame 行 mouse 最大 {0:N1}" -f $mmx)
            $mid = @($zw | Where-Object { $_.T -ge $z0.AddSeconds(0.5) })
            $zm = Get-Median ($mid | ForEach-Object { $_.Zoom })
            if ($null -eq $zm) {
                Add-Check ('放大中 zoom= 中位数 ≈ {0:N1}°/s（±25%）' -f $zoomExpect) $false '没有样本'
            } else {
                $ok = [math]::Abs($zm - $zoomExpect) -le 0.25 * $zoomExpect
                Add-Check ('放大中 zoom= 中位数 ≈ {0:N1}°/s（±25%）' -f $zoomExpect) $ok ("中位数 {0:N1}°/s（{1} 个样本，{2:+0.0;-0.0}%）" -f $zm, $mid.Count, (100.0 * ($zm / $zoomExpect - 1.0)))
            }
            $cm = Get-Median ($mid | ForEach-Object { $_.Cap })
            $rm = Get-Median ($mid | ForEach-Object { $_.Roll })
            Write-Host ("  [app] 放大段（开始 0.5 s 后）capture=（平移）中位数 {0:N1}°/s、最大 {1:N1}°/s；roll= 中位数 {2:N1}°/s（应接近 0）" -f `
                $cm, ($mid | Measure-Object -Property Cap -Maximum).Maximum, $rm)
        }
        # 4. 放大结束 1 s 后到旋转开始：回到静止
        $gap = Get-FwdWindow $z1.AddSeconds(1.0) $r0
        if ($gap.Count -gt 0) {
            $mx = ($gap | Measure-Object -Property V -Maximum).Maximum
            Add-Check '放大结束 1 s 后到旋转开始强度 <= 0.05' ($mx -le 0.05) ("{0} 个样本，最大 {1:N3}" -f $gap.Count, $mx)
        } else {
            Add-Check '放大结束 1 s 后到旋转开始强度 <= 0.05' $false '这段时间没有样本'
        }
        # 5. 旋转段
        $rw = Get-FwdWindow $r0 $r1
        if ($rw.Count -eq 0) {
            Add-Check '旋转中最大强度 > 0.5' $false '旋转段没有样本'
        } else {
            $mx = ($rw | Measure-Object -Property V -Maximum).Maximum
            Add-Check '旋转中最大强度 > 0.5' ($mx -gt 0.5) ("{0} 个样本，最大 {1:N3}" -f $rw.Count, $mx)
            $mmx = ($rw | Measure-Object -Property Mouse -Maximum).Maximum
            Add-Check '旋转中 mouse=0.0' ($mmx -eq 0) ("frame 行 mouse 最大 {0:N1}" -f $mmx)
            $mid = @($rw | Where-Object { $_.T -ge $r0.AddSeconds(0.5) })
            $rm = Get-Median ($mid | ForEach-Object { $_.Roll })
            if ($null -eq $rm) {
                Add-Check ('旋转中 roll= 中位数 ≈ {0:N0}°/s（±25%）' -f $RollDegPerS) $false '没有样本'
            } else {
                $ok = [math]::Abs($rm - $RollDegPerS) -le 0.25 * $RollDegPerS
                Add-Check ('旋转中 roll= 中位数 ≈ {0:N0}°/s（±25%）' -f $RollDegPerS) $ok ("中位数 {0:N1}°/s（{1} 个样本，{2:+0.0;-0.0}%）" -f $rm, $mid.Count, (100.0 * ($rm / $RollDegPerS - 1.0)))
            }
            $cm = Get-Median ($mid | ForEach-Object { $_.Cap })
            $zm = Get-Median ($mid | ForEach-Object { $_.Zoom })
            Write-Host ("  [app] 旋转段（开始 0.5 s 后）capture=（平移）中位数 {0:N1}°/s；zoom= 中位数 {1:N1}°/s（应接近 0）" -f $cm, $zm)
        }
    }
    Exit-WithSummary '05_forward_zoom.png、06_forward_roll.png（预期看不到遮罩）、MotionCalm.forward.log' $true
}

# ================================================================ -Capture：截屏运动检测场景
if ($Capture) {
    if (-not (Test-Path $AppExe)) {
        Write-Host "[错误] 找不到 $AppExe —— 先运行 build.cmd all $Config" -ForegroundColor Red
        exit 2
    }
    $screenW = [MCVerifyNative]::GetSystemMetrics(0)
    $scrollPx = [int][math]::Round($CaptureDegPerS * $screenW / 90.0)
    $capIni = Join-Path $OutDir 'capture_verify.ini'
    Remove-Item -LiteralPath $capIni, ($capIni + '.tmp') -ErrorAction SilentlyContinue
    $fakeArgs = @('--seconds', '12', '--mode', 'lock', '--texture', 'noise', '--scroll', "$scrollPx",
        '--scroll-start', '2', '--scroll-seconds', '4', '--flash-at', '8.5')

    Write-Banner -Color Yellow -Lines @(
        '!!! 截屏运动检测测试进行中，请勿移动鼠标、请勿按键 !!!',
        '',
        '接下来约 17 秒：假游戏会全屏、锁住并隐藏鼠标，画面在第 2–6 秒横向滚动，第 8.5 秒闪白一次。',
        '这一段不发送任何模拟鼠标：遮罩只能由画面运动触发。碰了鼠标会让判定失败，按键会让假游戏提前退出。',
        ('配置：{0}   滚动 {1} px/s（主显示器宽 {2} px，约 {3:N0}°/s）' -f $Config, $scrollPx, $screenW, $CaptureDegPerS)
    )
    for ($i = 3; $i -ge 1; $i--) {
        Write-Host ("  {0} 秒后开始……" -f $i) -ForegroundColor Yellow
        Start-Sleep -Seconds 1
    }

    $app = $null; $fake = $null; $appStart = $null; $runError = $null
    $testStart = Get-Date
    try {
        [void][MCVerifyNative]::AllowSetForegroundWindow(-1)
        $app = Start-Process -FilePath $AppExe -WorkingDirectory $Bin -PassThru `
            -ArgumentList @('--config', ('"{0}"' -f $capIni), '--verbose', '--force-game', '--exit-after', '16')
        $null = $app.Handle
        $appStart = Get-Date
        Write-Host ("[app] 已启动 pid={0}（截屏检测开启）" -f $app.Id)
        Start-Sleep -Milliseconds 500

        $fake = Start-Process -FilePath $FakeExe -WorkingDirectory $Bin -PassThru -NoNewWindow `
            -RedirectStandardOutput $FakeOut -RedirectStandardError $FakeErr -ArgumentList $fakeArgs
        $null = $fake.Handle
        Write-Host ("[fakegame] 已启动 pid={0}  {1}" -f $fake.Id, ($fakeArgs -join ' '))
        Start-Sleep -Milliseconds 4000
        Save-Shot '04_capture_scroll.png'

        if (-not $fake.WaitForExit(20000)) { Write-Host '[fakegame] 超时未退出' -ForegroundColor Yellow }
        [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
        if (-not $app.WaitForExit(15000)) { Write-Host '[app] --exit-after 16 后仍未退出，将被强制结束' -ForegroundColor Yellow }
    } catch {
        $runError = $_.Exception.Message
        Write-Host ("[错误] 运行过程出错：{0}" -f $runError) -ForegroundColor Red
    } finally {
        [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
        foreach ($p in @($fake, $app)) {
            if ($null -ne $p) {
                try {
                    if (-not $p.HasExited) {
                        Write-Host ("[清理] 结束残留进程 pid={0}" -f $p.Id) -ForegroundColor Yellow
                        $p.Kill()
                        [void]$p.WaitForExit(3000)
                    }
                } catch { }
            }
        }
        [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
    }

    Write-Host ''
    Write-Host '---------------- 结果（截屏运动检测）----------------'
    if ($null -ne $runError) { Add-Check '流程正常执行' $false $runError }
    $fakeCode = $(try { $fake.ExitCode } catch { $null })
    Add-Check 'fakegame 正常退出' ($fakeCode -eq 0) ("exit={0}" -f $fakeCode)
    $events = @{}
    if (Test-Path $FakeOut) {
        foreach ($line in (Get-Content $FakeOut)) {
            Write-Host ("  [fakegame] " + $line)
            $m = [regex]::Match($line, '\[fakegame\] (?<ev>scroll start|scroll stop|flash at) .*\bwall=(?<w>\S+)')
            if ($m.Success) { $events[$m.Groups['ev'].Value] = Parse-SimTime $m.Groups['w'].Value }
        }
    }

    # 日志：frame 行（强度、鼠标、截屏角速度）与截屏相关的 info / warn 行
    $frameRx = '\bframe t=\S+ in_game=(?<g>\d) locked=(?<l>\d) mouse=(?<m>[0-9.]+) moving=\d pad=[0-9.]+ capture=(?<c>[0-9.]+) target=(?<tg>[0-9.]+) value=(?<v>[0-9.]+) exe=(?<exe>\S*)'
    $frames = New-Object System.Collections.Generic.List[object]
    $capLines = New-Object System.Collections.Generic.List[string]
    $runFrom = $appStart.AddSeconds(-1)
    $runTo = (Get-Date).AddSeconds(1)
    $logs = @(Get-ChildItem -Path $Bin -Filter 'MotionCalm*.log*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
    $inv = [Globalization.CultureInfo]::InvariantCulture
    foreach ($log in $logs) {
        foreach ($line in (Get-Content -LiteralPath $log.FullName -Encoding UTF8)) {
            $isCap = $line -match '\bcapture (started|released|failed|access lost|grid rebuilt|perf:)|overlay capture visibility|hide_from_capture=never|截屏检测'
            $mf = [regex]::Match($line, $frameRx)
            if (-not $isCap -and -not $mf.Success) { continue }
            $mt = [regex]::Match($line, $TimestampRegex)
            if (-not $mt.Success) { continue }
            try { $t = Convert-LogTime $mt $testStart } catch { continue }
            if ($t -lt $runFrom -or $t -gt $runTo) { continue }
            if ($isCap) { $capLines.Add($line) }
            if ($mf.Success) {
                $frames.Add([pscustomobject]@{
                    T = $t
                    Mouse = [double]::Parse($mf.Groups['m'].Value, $inv)
                    Cap = [double]::Parse($mf.Groups['c'].Value, $inv)
                    V = [double]::Parse($mf.Groups['v'].Value, $inv)
                    Exe = $mf.Groups['exe'].Value
                })
            }
        }
    }
    if ($logs.Count -gt 0) {
        Copy-Item -LiteralPath $logs[-1].FullName -Destination (Join-Path $OutDir 'MotionCalm.capture.log') -Force
    }
    $frames = @($frames | Sort-Object T)
    Write-Host ("  [app] exit={0}；本次运行的 frame 行 {1} 条" -f $(try { $app.ExitCode } catch { 'n/a' }), $frames.Count)
    foreach ($l in $capLines) { Write-Host ("  [app] " + ($l -replace '^\S+ \S+ ', '')) }

    # frame 行按"保持到下一行"解释
    function Get-CapWindow([datetime]$A, [datetime]$B) {
        $held = $null
        $vals = New-Object System.Collections.Generic.List[object]
        foreach ($f in $frames) {
            if ($f.T -le $A) { $held = $f } elseif ($f.T -le $B) { $vals.Add($f) } else { break }
        }
        if ($null -ne $held) { $vals.Insert(0, $held) }
        return , $vals
    }

    $started = @($capLines | Where-Object { $_ -match '\bcapture started:' }).Count -gt 0
    $excluded = @($capLines | Where-Object { $_ -match 'overlay capture visibility: excluded from capture' }).Count -gt 0
    $capFailed = @($capLines | Where-Object { $_ -match '\bcapture failed\b' }).Count -gt 0
    Add-Check '截屏开始取帧（capture started）' $started $(if ($started) { '有' } else { '日志中没有 capture started: 行' })
    Add-Check 'overlay 对截屏隐藏（auto + 截屏检测开启）' $excluded $(if ($excluded) { '有 excluded from capture 行' } else { '没有找到' })
    Add-Check '没有进入"采集失败"' (-not $capFailed) $(if ($capFailed) { '日志有 capture failed' } else { '无' })

    $s0 = $events['scroll start']; $s1 = $events['scroll stop']; $fl = $events['flash at']
    if ($null -eq $s0 -or $null -eq $s1 -or $null -eq $fl -or $frames.Count -eq 0) {
        Add-Check '能对齐时间线' $false ('fakegame 事件 {0} 个、frame 行 {1} 条（需要 scroll start / scroll stop / flash at 与 frame 行）' -f $events.Count, $frames.Count)
    } else {
        # 2. 滚动前 1 s 静止
        $pre = Get-CapWindow $s0.AddSeconds(-1.0) $s0
        if ($pre.Count -eq 0) {
            Add-Check '滚动前静止时强度 <= 0.05' $false '滚动前 1 s 没有样本'
        } else {
            $mx = ($pre | Measure-Object -Property V -Maximum).Maximum
            Add-Check '滚动前静止时强度 <= 0.05' ($mx -le 0.05) ("{0} 个样本，最大 {1:N3}" -f $pre.Count, $mx)
        }
        # 3. 滚动段：强度 > 0.3，鼠标恒为 0
        $scr = Get-CapWindow $s0 $s1
        if ($scr.Count -eq 0) {
            Add-Check '滚动中最大强度 > 0.3' $false '滚动段没有样本'
        } else {
            $mx = ($scr | Measure-Object -Property V -Maximum).Maximum
            $first = $scr | Where-Object { $_.T -gt $s0 -and $_.V -gt 0.3 } | Select-Object -First 1
            $rise = if ($first) { '{0:N0} ms 后超过 0.3' -f ($first.T - $s0).TotalMilliseconds } else { '未超过 0.3' }
            Add-Check '滚动中最大强度 > 0.3' ($mx -gt 0.3) ("{0} 个样本，最大 {1:N3}，{2}" -f $scr.Count, $mx, $rise)
            $mmx = ($scr | Measure-Object -Property Mouse -Maximum).Maximum
            Add-Check '滚动中 mouse=0.0（强度只来自截屏）' ($mmx -eq 0) ("frame 行 mouse 最大 {0:N1}" -f $mmx)
            $mid = @($scr | Where-Object { $_.T -ge $s0.AddSeconds(0.5) } | ForEach-Object { $_.Cap } | Sort-Object)
            if ($mid.Count -gt 0) {
                Write-Host ("  [app] 滚动段（开始 0.5 s 后）frame 行 capture= 中位数 {0:N1}°/s（理论约 {1:N0}，{2} 个样本，最小 {3:N1} 最大 {4:N1}）" -f `
                    $mid[[int][math]::Floor($mid.Count / 2)], $CaptureDegPerS, $mid.Count, $mid[0], $mid[-1])
            }
        }
        # 4. 闪白不触发
        $fw = Get-CapWindow $fl.AddSeconds(-0.1) $fl.AddSeconds(1.5)
        if ($fw.Count -eq 0) {
            Add-Check '闪白不触发（强度 <= 0.1）' $false '闪白前后没有样本'
        } else {
            $mx = ($fw | Measure-Object -Property V -Maximum).Maximum
            $cmx = ($fw | Measure-Object -Property Cap -Maximum).Maximum
            Add-Check '闪白不触发（强度 <= 0.1）' ($mx -le 0.1) ("{0} 个样本，最大强度 {1:N3}，最大 capture {2:N1}°/s" -f $fw.Count, $mx, $cmx)
        }
    }
    Exit-WithSummary '04_capture_scroll.png（预期看不到遮罩）、MotionCalm.capture.log' $true
}

Write-Banner -Color Yellow -Lines @(
    '!!! 测试进行中，请勿移动鼠标、请勿按键 !!!',
    '',
    '接下来约 20 秒：假游戏会全屏并锁住 / 隐藏鼠标，模拟器会自动移动鼠标。',
    '任何按键都会让假游戏提前退出（测试会失败）。脚本结束时一定会释放鼠标。',
    ('配置：{0}   app：{1}' -f $Config, $(if ($RunApp) { '启动' } else { '跳过' }))
)
for ($i = 3; $i -ge 1; $i--) {
    Write-Host ("  {0} 秒后开始……" -f $i) -ForegroundColor Yellow
    Start-Sleep -Seconds 1
}

# ---------------------------------------------------------------- run
$app = $null; $fake = $null; $sim = $null
$appStart = $null; $simLaunch = $null; $fakeLaunch = $null
$cpuS1 = $null; $cpuS2 = $null; $cpuS3 = $null; $cpuTurnA = $null; $cpuTurnB = $null
$runError = $null
$testStart = Get-Date
try {
    # 让子进程可以抢前台（fakegame 自己也会用 AttachThreadInput / SwitchToThisWindow 兜底）
    [void][MCVerifyNative]::AllowSetForegroundWindow(-1)

    if ($RunApp) {
        $app = Start-Process -FilePath $AppExe -WorkingDirectory $Bin -PassThru `
            -ArgumentList @('--no-capture', '--verbose', '--force-game', '--exit-after', '20')
        $null = $app.Handle   # 缓存句柄，进程退出后仍能读 ExitCode
        $appStart = Get-Date
        Write-Host ("[app] 已启动 pid={0}" -f $app.Id)
        Start-Sleep -Milliseconds 500
    }

    $fake = Start-Process -FilePath $FakeExe -WorkingDirectory $Bin -PassThru -NoNewWindow `
        -RedirectStandardOutput $FakeOut -RedirectStandardError $FakeErr `
        -ArgumentList @('--seconds', '15', '--mode', 'lock')
    $null = $fake.Handle
    $fakeLaunch = Get-Date
    Write-Host ("[fakegame] 已启动 pid={0}" -f $fake.Id)

    Start-Sleep -Seconds 1
    Save-Shot '01_idle.png'
    $cpuS1 = Get-CpuSample $app

    $sim = Start-Process -FilePath $SimExe -WorkingDirectory $Bin -PassThru -NoNewWindow `
        -RedirectStandardOutput $SimOut -RedirectStandardError $SimErr `
        -ArgumentList @('--profile', $SimProfile)
    $null = $sim.Handle
    $simLaunch = Get-Date
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host ("[inputsim] 已启动 pid={0}  profile={1}" -f $sim.Id, $SimProfile)

    Wait-Until $sw 1.0;  $cpuTurnA = Get-CpuSample $app
    Wait-Until $sw 3.5;  Save-Shot '02_turning.png'
    Wait-Until $sw 4.0;  $cpuTurnB = Get-CpuSample $app
    Wait-Until $sw 5.0;  Save-Shot '03_faded.png'

    if (-not $sim.WaitForExit(15000)) { Write-Host '[inputsim] 超时未退出' -ForegroundColor Yellow }
    if (-not $fake.WaitForExit(20000)) { Write-Host '[fakegame] 超时未退出' -ForegroundColor Yellow }
    [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
    $cpuS2 = Get-CpuSample $app

    if ($app) {
        # app 在 --exit-after 20 后自己退出；退出前 1 s 再采一次 CPU
        $deadline = $appStart.AddSeconds(19)
        $waitMs = [int](($deadline - (Get-Date)).TotalMilliseconds)
        if ($waitMs -gt 0) { Start-Sleep -Milliseconds $waitMs }
        $cpuS3 = Get-CpuSample $app
        if (-not $app.WaitForExit(8000)) { Write-Host '[app] --exit-after 20 后仍未退出，将被强制结束' -ForegroundColor Yellow }
    }
} catch {
    $runError = $_.Exception.Message
    Write-Host ("[错误] 运行过程出错：{0}" -f $runError) -ForegroundColor Red
} finally {
    # 无论如何：释放光标限制，结束残留进程
    [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
    foreach ($p in @($sim, $fake, $app)) {
        if ($null -ne $p) {
            try {
                if (-not $p.HasExited) {
                    Write-Host ("[清理] 结束残留进程 pid={0}" -f $p.Id) -ForegroundColor Yellow
                    $p.Kill()
                    [void]$p.WaitForExit(3000)
                }
            } catch { }
        }
    }
    [void][MCVerifyNative]::ClipCursor([IntPtr]::Zero)
}

# ---------------------------------------------------------------- report: tools
Write-Host ''
Write-Host '---------------- 结果 ----------------'
function Get-ExitCode($P) { try { return $P.ExitCode } catch { return $null } }
if ($null -ne $runError) { Add-Check '流程正常执行' $false $runError }
$fakeCode = Get-ExitCode $fake
$simCode = Get-ExitCode $sim
Add-Check 'fakegame 正常退出' ($fakeCode -eq 0) ("exit={0}" -f $fakeCode)
Add-Check 'inputsim 正常退出' ($simCode -eq 0) ("exit={0}" -f $simCode)
if (Test-Path $FakeOut) { Get-Content $FakeOut | ForEach-Object { Write-Host ("  [fakegame] " + $_) } }

# inputsim 分段时间
$segs = @()
if (Test-Path $SimOut) {
    foreach ($line in (Get-Content $SimOut)) {
        $m = [regex]::Match($line, $SimSegRegex)
        if ($m.Success) {
            Write-Host ("  [inputsim] " + $line)
            $segs += [pscustomobject]@{
                N    = [int]$m.Groups['n'].Value
                Kind = $m.Groups['kind'].Value
                T0   = Parse-SimTime $m.Groups['t0'].Value
                T1   = Parse-SimTime $m.Groups['t1'].Value
            }
        } elseif ($line -like 'total*') {
            Write-Host ("  [inputsim] " + $line)
        }
    }
}
$turn = $segs | Where-Object { $_.Kind -eq 'turn' } | Select-Object -First 1
$idle = $segs | Where-Object { $_.N -eq 1 -and $_.Kind -eq 'idle' } | Select-Object -First 1
if ($null -eq $turn -and $null -ne $simLaunch) {
    Write-Host '  [提示] 未能解析 inputsim 输出，按启动时间估算分段' -ForegroundColor Yellow
    $idle = [pscustomobject]@{ N = 1; Kind = 'idle'; T0 = $simLaunch; T1 = $simLaunch.AddSeconds(1) }
    $turn = [pscustomobject]@{ N = 2; Kind = 'turn'; T0 = $simLaunch.AddSeconds(1); T1 = $simLaunch.AddSeconds(4) }
}

# ---------------------------------------------------------------- report: app log
if ($RunApp -and $null -eq $appStart) {
    Add-Check 'app 启动' $false '未能启动 MotionCalm.exe'
}
if ($RunApp -and $null -ne $appStart) {
    $appCode = Get-ExitCode $app
    Write-Host ("  [app] exit={0}" -f $appCode)

    # MotionCalm.log 与轮转出的 MotionCalm.log.1 都读，按时间过滤出本次运行
    $logs = @(Get-ChildItem -Path $Bin -Filter 'MotionCalm*.log*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
    $raw = New-Object System.Collections.Generic.List[object]
    $exitLines = New-Object System.Collections.Generic.List[string]
    $runFrom = $appStart.AddSeconds(-1)
    $runTo = (Get-Date).AddSeconds(1)
    foreach ($log in $logs) {
        foreach ($line in (Get-Content -LiteralPath $log.FullName -Encoding UTF8)) {
            $mi = [regex]::Match($line, $IntensityRegex)
            if (-not $mi.Success) { $mi = [regex]::Match($line, $FrameValueRegex) }
            $isExit = $line -match $ExitLineRegex
            if (-not $mi.Success -and -not $isExit) { continue }
            $mt = [regex]::Match($line, $TimestampRegex)
            if (-not $mt.Success) { continue }
            try { $t = Convert-LogTime $mt $testStart } catch { continue }
            if ($t -lt $runFrom -or $t -gt $runTo) { continue }
            if ($isExit) { $exitLines.Add($line) }
            if (-not $mi.Success) { continue }
            $v = 0.0
            if (-not [double]::TryParse($mi.Groups[1].Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) { continue }
            $raw.Add([pscustomobject]@{ T = $t; I = $v })
        }
    }
    $samples = @($raw | Sort-Object T)
    if ($logs.Count -gt 0) {
        Copy-Item -LiteralPath $logs[-1].FullName -Destination (Join-Path $OutDir 'MotionCalm.log') -Force
    }
    Write-Host ("  [app] 日志文件 {0} 个，本次运行的强度样本 {1} 条" -f $logs.Count, $samples.Count)
    foreach ($l in $exitLines) { Write-Host ("  [app] " + $l) }

    # 样本按"保持到下一行"解释：时刻 T 的强度 = T 之前（含）最后一条样本
    function Get-ValueAt([datetime]$T) {
        $v = $null
        foreach ($s in $samples) { if ($s.T -le $T) { $v = $s.I } else { break } }
        return $v
    }
    # (A, B] 内的样本值 + A 时刻的保持值
    function Get-WindowValues([datetime]$A, [datetime]$B) {
        $vals = @()
        $held = Get-ValueAt $A
        if ($null -ne $held) { $vals += $held }
        foreach ($s in $samples) { if ($s.T -gt $A -and $s.T -le $B) { $vals += $s.I } }
        return , $vals
    }

    if ($samples.Count -eq 0 -or $null -eq $turn) {
        Add-Check '日志中有强度样本' $false '没有可用样本（检查 --verbose 日志格式，见脚本开头的日志契约）'
    } else {
        # 1. 静止段强度恒为 0
        if ($null -ne $idle) {
            $vals = Get-WindowValues $idle.T0 $turn.T0.AddMilliseconds(-1)
            if ($vals.Count -eq 0) {
                Add-Check '静止时强度 == 0' $false '静止段没有样本'
            } else {
                $mx = ($vals | Measure-Object -Maximum).Maximum
                Add-Check '静止时强度 == 0' ($mx -eq 0) ("{0} 个样本，最大 {1}" -f $vals.Count, $mx)
            }
        }
        # 2. 转动中最大强度 >= 0.8
        $turnS = @($samples | Where-Object { $_.T -ge $turn.T0 -and $_.T -le $turn.T1 })
        if ($turnS.Count -eq 0) {
            Add-Check '转动中最大强度 >= 0.8' $false '转动段没有样本'
        } else {
            $mx = ($turnS | Measure-Object -Property I -Maximum).Maximum
            $first = $turnS | Where-Object { $_.I -ge 0.8 } | Select-Object -First 1
            $rise = if ($first) { '{0:N0} ms 后达到 0.8' -f ($first.T - $turn.T0).TotalMilliseconds } else { '未达到 0.8' }
            Add-Check '转动中最大强度 >= 0.8' ($mx -ge 0.8) ("{0} 个样本，最大 {1:N3}，{2}" -f $turnS.Count, $mx, $rise)
        }
        # 3. 转动结束后 0.6 s 时 <= 0.05，且到 +2.0 s 一直 <= 0.05
        $firstLow = $samples | Where-Object { $_.T -gt $turn.T1 -and $_.I -le 0.05 } | Select-Object -First 1
        $fadeText = if ($firstLow) { '首次 <= 0.05 在结束后 {0:N0} ms' -f ($firstLow.T - $turn.T1).TotalMilliseconds } else { '之后从未 <= 0.05' }
        $vals = Get-WindowValues $turn.T1.AddSeconds(0.6) $turn.T1.AddSeconds(2.0)
        if ($vals.Count -eq 0) {
            Add-Check '转动结束 0.6 s 内回到 <= 0.05' $false ("结束后 0.6–2.0 s 没有样本；{0}" -f $fadeText)
        } else {
            $mx = ($vals | Measure-Object -Maximum).Maximum
            Add-Check '转动结束 0.6 s 内回到 <= 0.05' ($mx -le 0.05) ("0.6–2.0 s 内最大 {0:N3}；{1}" -f $mx, $fadeText)
        }
    }

    # 4. CPU
    $rIn = Format-CpuRatio $cpuS1 $cpuS2
    $rTurn = Format-CpuRatio $cpuTurnA $cpuTurnB
    $rIdle = Format-CpuRatio $cpuS2 $cpuS3
    $last = if ($cpuS3) { $cpuS3 } elseif ($cpuS2) { $cpuS2 } else { $cpuS1 }
    $rAll = if ($last -and $last.Wall -gt 0) { $last.Cpu / $last.Wall } else { $null }
    function Pct($r) { if ($null -eq $r) { return 'n/a' } return ('{0:N2}%' -f ($r * 100)) }
    Write-Host ("  [app] CPU 时间 / 墙钟时间（单核占比）：整体 {0} | 游戏内 {1} | 转动中 {2} | 游戏结束后空闲 {3}" -f (Pct $rAll), (Pct $rIn), (Pct $rTurn), (Pct $rIdle))
    if ($null -ne $last) { Write-Host ("  [app] 累计 CPU {0:N3} s / 运行 {1:N1} s" -f $last.Cpu, $last.Wall) }
    if ($null -eq $rIn) {
        Add-Check '游戏内 CPU < 2%' $false '无法采样（app 提前退出？）'
    } else {
        Add-Check '游戏内 CPU < 2%' ($rIn -lt 0.02) (Pct $rIn)
    }
}

# ---------------------------------------------------------------- summary
Exit-WithSummary '01_idle.png 02_turning.png 03_faded.png' $RunApp
