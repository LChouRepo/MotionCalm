<#
.SYNOPSIS
  MotionCalm 校准集成验证（in-game-calibration tasks 7.1–7.3）：起 app → 起假游戏 → 用 WM_HOTKEY 推进校准 →
  inputsim 发两次已知计数 → 截图 → 检查临时 INI 的 [calibration] fakegame.exe= 与日志，打印 PASS/FAIL。

.DESCRIPTION
  会占用屏幕和鼠标约 20 秒：fakegame 全屏、隐藏并锁住光标，inputsim 发送相对鼠标移动。运行期间不要碰鼠标键盘
  （任何按键都会让 fakegame 提前退出）。校准热键不用按：脚本用 FindWindow("MotionCalm.Host") + PostMessage(WM_HOTKEY)
  直接投递（id 10 = calibrate），不经过键盘，所以不会让 fakegame 退出，也不依赖 Ctrl+Alt+F7 是否被别的程序占用。

  配置写到临时文件 %TEMP%\mc_calib_verify\MotionCalm.ini（每次运行前删除，由内嵌模板重新生成），
  out\<cfg>\MotionCalm.ini 不受影响。日志仍写在 out\<cfg>\MotionCalm.log。

  时间线：
    app      : MotionCalm.exe --verbose --force-game --config <临时 INI> --exit-after 25
    +0.5 s   : fakegame.exe --seconds 20 --mode lock
    +1.5 s   : inputsim --profile "idle:2,turn:2@6552,idle:3,turn:2@6552,idle:2,turn:2@8000,idle:1"
               （两次转动各 13104 计数 → 每度 36.4 计数；最后一段 8000 counts/s 用来检查校准值已生效）
    相对 inputsim 启动：
      1.0 s  : 热键 #1  Idle → Run1             1.5 s 截图 calib_01_run1.png（"已转 0 计数"）
      3.0 s  :                                   截图 calib_02_turning.png（转动中，遮罩满强度时提示仍清晰）
      5.0 s  : 热键 #2  Run1 → Between（c1）     5.5 s 截图 calib_03_between.png（"第 1 次：13104 计数"）
      6.0 s  : 热键 #3  Between → Run2
     10.0 s  : 热键 #4  Run2 → Done → Idle（保存） 10.3 s 截图 calib_04_done.png（toast "校准完成：…"）
     11–13 s : 8000 counts/s 转动，frame 行 mouse ≈ 219.8°/s（校准前为 439.6）

  判定：
    1. 临时 INI 的 [calibration] 中 fakegame.exe= 的值在 36.4 ± 1%（36.036–36.764），且等于 inputsim 实际发送量算出的值（±0.001）
    2. 日志有 calib state= 的完整转移：Idle->Run1、Run1->Between、Between->Run2、Run2->Done->Idle
    3. 日志有 calib saved exe=fakegame.exe ... 与 toast "校准完成：每度 36.4 计数"
    4. 最后一段 8000 counts/s 转动中 frame 行 mouse 的中位数在 215–224°/s，target 的中位数在 0.50–0.60（理论 0.549）

.PARAMETER Config
  debug | release（默认 release），二进制取自 out\<Config>\。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\verify_calibration.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string]$Config = 'release'
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "out\$Config"
$OutDir = Join-Path $Root 'test-output'
$AppExe = Join-Path $Bin 'MotionCalm.exe'
$FakeExe = Join-Path $Bin 'fakegame.exe'
$SimExe = Join-Path $Bin 'inputsim.exe'
$CfgDir = Join-Path $env:TEMP 'mc_calib_verify'     # 短路径：Win32 API 不接受过长的路径
$CfgPath = Join-Path $CfgDir 'MotionCalm.ini'
$SimProfile = 'idle:2,turn:2@6552,idle:3,turn:2@6552,idle:2,turn:2@8000,idle:1'
$CalibHotkeyId = 10                                   # HotkeyManager::id_for(HotkeyAction::Calibrate) = 9 + 1
$WM_HOTKEY = 0x0312
$ExpectCpd = 36.4                                     # 13104 / 360
$TolCpd = 0.01                                        # ±1%

$TimestampRegex = '(?:(?<date>\d{4}-\d{2}-\d{2})[ T])?(?<time>\d{1,2}:\d{2}:\d{2}(?:[.,]\d{1,6})?)'
$SimSegRegex = '^seg=(?<n>\d+)\s+kind=(?<kind>\w+)\s.*\bsent_dx=(?<dx>-?\d+)\s.*\bt0=(?<t0>\S+)\s+t1=(?<t1>\S+)'
$FrameRegex = '\bframe t=\S+ in_game=(?<g>\d) locked=(?<l>\d) mouse=(?<m>[0-9.]+) .*\btarget=(?<tg>[0-9.]+) value=(?<v>[0-9.]+) exe=(?<exe>\S*)'

# ---------------------------------------------------------------- native helpers
if (-not ('MCCalibVerifyNative' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class MCCalibVerifyNative {
    [DllImport("user32.dll")] public static extern bool ClipCursor(IntPtr rect);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool AllowSetForegroundWindow(int pid);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    // 标题传 IntPtr.Zero（NULL）：PowerShell 会把 $null 字符串参数变成 ""，只匹配空标题的窗口。
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, IntPtr title);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr hdc);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, uint rop);

    const uint SRCCOPY = 0x00CC0020;
    const uint CAPTUREBLT = 0x40000000;   // 包含分层 / 合成窗口（overlay）

    // 主显示器截图（带 CAPTUREBLT）；另存一张上部 45% 的裁剪图（校准提示在 22% 高度处），方便查看文字。
    public static string CapturePrimary(string path, string topPath) {
        int w = GetSystemMetrics(0), h = GetSystemMetrics(1);
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr dst = g.GetHdc();
                IntPtr src = GetDC(IntPtr.Zero);
                try { BitBlt(dst, 0, 0, w, h, src, 0, 0, SRCCOPY | CAPTUREBLT); }
                finally { ReleaseDC(IntPtr.Zero, src); g.ReleaseHdc(dst); }
            }
            bmp.Save(path, ImageFormat.Png);
            int cw = Math.Max(1, w * 3 / 4), ch = Math.Max(1, h * 45 / 100);
            using (Bitmap top = bmp.Clone(new Rectangle((w - cw) / 2, 0, cw, ch), bmp.PixelFormat)) {
                top.Save(topPath, ImageFormat.Png);
            }
        }
        return w + "x" + h;
    }
}
'@
}
[void][MCCalibVerifyNative]::SetProcessDPIAware()

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
    $path = Join-Path $OutDir ($Name + '.png')
    $top = Join-Path $OutDir ($Name + '_top.png')
    try {
        $size = [MCCalibVerifyNative]::CapturePrimary($path, $top)
        Write-Host ("[截图] {0}.png (+_top)  ({1})  {2}" -f $Name, $size, (Get-Date).ToString('HH:mm:ss.fff'))
    } catch {
        Write-Host ("[截图] {0} 失败：{1}" -f $Name, $_.Exception.Message) -ForegroundColor Yellow
    }
}

$script:HotkeyCount = 0
function Send-CalibrateHotkey([string]$What) {
    $script:HotkeyCount++
    $h = [MCCalibVerifyNative]::FindWindowW('MotionCalm.Host', [IntPtr]::Zero)
    if ($h -eq [IntPtr]::Zero) {
        Write-Host ("[热键 #{0}] 找不到 MotionCalm.Host 窗口（app 没在运行？）" -f $script:HotkeyCount) -ForegroundColor Yellow
        return
    }
    $ok = [MCCalibVerifyNative]::PostMessageW($h, [uint32]$WM_HOTKEY, [IntPtr]$CalibHotkeyId, [IntPtr]::Zero)
    Write-Host ("[热键 #{0}] calibrate（{1}） posted={2}  {3}" -f $script:HotkeyCount, $What, $ok, (Get-Date).ToString('HH:mm:ss.fff'))
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

function Parse-Double([string]$Text) {
    $v = 0.0
    if ([double]::TryParse($Text, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$v)) { return $v }
    return $null
}

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return $null }
    $s = @($Values | Sort-Object)
    $n = $s.Count
    if ($n % 2 -eq 1) { return $s[($n - 1) / 2] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2.0
}

$Checks = New-Object System.Collections.Generic.List[object]
function Add-Check([string]$Name, [bool]$Pass, [string]$Detail) {
    $Checks.Add([pscustomobject]@{ Name = $Name; Pass = $Pass; Detail = $Detail })
}

# ---------------------------------------------------------------- preflight
foreach ($exe in @($AppExe, $FakeExe, $SimExe)) {
    if (-not (Test-Path $exe)) {
        Write-Host "[错误] 找不到 $exe —— 先运行 build.cmd all $Config" -ForegroundColor Red
        exit 2
    }
}
if (Get-Process -Name MotionCalm -ErrorAction SilentlyContinue) {
    Write-Host '[错误] 已经有 MotionCalm.exe 在运行（单实例：新实例会直接退出）。请先从托盘退出它。' -ForegroundColor Red
    exit 2
}
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
if (-not (Test-Path $CfgDir)) { New-Item -ItemType Directory -Path $CfgDir | Out-Null }
foreach ($f in @($CfgPath, ($CfgPath + '.tmp'))) { if (Test-Path $f) { Remove-Item -LiteralPath $f -Force } }
$FakeOut = Join-Path $OutDir 'calib_fakegame.out.txt'
$FakeErr = Join-Path $OutDir 'calib_fakegame.err.txt'
$SimOut = Join-Path $OutDir 'calib_inputsim.out.txt'
$SimErr = Join-Path $OutDir 'calib_inputsim.err.txt'

Write-Banner -Color Yellow -Lines @(
    '!!! 校准测试进行中，请勿移动鼠标、请勿按键 !!!',
    '',
    '接下来约 20 秒：假游戏会全屏并锁住 / 隐藏鼠标，模拟器会自动转两圈。',
    '屏幕中上方会出现"校准 1/2 …"提示，最后出现"校准完成"。',
    '任何按键都会让假游戏提前退出（测试会失败）。脚本结束时一定会释放鼠标。',
    ('配置：{0}   临时 INI：{1}' -f $Config, $CfgPath)
)
for ($i = 3; $i -ge 1; $i--) {
    Write-Host ("  {0} 秒后开始……" -f $i) -ForegroundColor Yellow
    Start-Sleep -Seconds 1
}

# ---------------------------------------------------------------- run
$app = $null; $fake = $null; $sim = $null
$appStart = $null
$runError = $null
$testStart = Get-Date
try {
    [void][MCCalibVerifyNative]::AllowSetForegroundWindow(-1)

    $app = Start-Process -FilePath $AppExe -WorkingDirectory $Bin -PassThru `
        -ArgumentList @('--verbose', '--force-game', '--config', ('"' + $CfgPath + '"'), '--exit-after', '25')
    $null = $app.Handle
    $appStart = Get-Date
    Write-Host ("[app] 已启动 pid={0}" -f $app.Id)
    Start-Sleep -Milliseconds 500

    $fake = Start-Process -FilePath $FakeExe -WorkingDirectory $Bin -PassThru -NoNewWindow `
        -RedirectStandardOutput $FakeOut -RedirectStandardError $FakeErr `
        -ArgumentList @('--seconds', '20', '--mode', 'lock')
    $null = $fake.Handle
    Write-Host ("[fakegame] 已启动 pid={0}" -f $fake.Id)
    Start-Sleep -Milliseconds 1000

    $sim = Start-Process -FilePath $SimExe -WorkingDirectory $Bin -PassThru -NoNewWindow `
        -RedirectStandardOutput $SimOut -RedirectStandardError $SimErr `
        -ArgumentList @('--profile', $SimProfile)
    $null = $sim.Handle
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Write-Host ("[inputsim] 已启动 pid={0}  profile={1}" -f $sim.Id, $SimProfile)

    Wait-Until $sw 1.0;  Send-CalibrateHotkey 'Idle -> Run1'
    Wait-Until $sw 1.5;  Save-Shot 'calib_01_run1'
    Wait-Until $sw 3.0;  Save-Shot 'calib_02_turning'
    Wait-Until $sw 5.0;  Send-CalibrateHotkey 'Run1 -> Between'
    Wait-Until $sw 5.5;  Save-Shot 'calib_03_between'
    Wait-Until $sw 6.0;  Send-CalibrateHotkey 'Between -> Run2'
    Wait-Until $sw 10.0; Send-CalibrateHotkey 'Run2 -> 结算'
    Wait-Until $sw 10.3; Save-Shot 'calib_04_done'

    if (-not $sim.WaitForExit(15000)) { Write-Host '[inputsim] 超时未退出' -ForegroundColor Yellow }
    if (-not $fake.WaitForExit(20000)) { Write-Host '[fakegame] 超时未退出' -ForegroundColor Yellow }
    [void][MCCalibVerifyNative]::ClipCursor([IntPtr]::Zero)
    Write-Host '[app] 等待 --exit-after 25 自行退出……'
    if (-not $app.WaitForExit(20000)) { Write-Host '[app] 仍未退出，将被强制结束' -ForegroundColor Yellow }
} catch {
    $runError = $_.Exception.Message
    Write-Host ("[错误] 运行过程出错：{0}" -f $runError) -ForegroundColor Red
} finally {
    # 无论如何：释放光标限制，结束残留进程
    [void][MCCalibVerifyNative]::ClipCursor([IntPtr]::Zero)
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
    [void][MCCalibVerifyNative]::ClipCursor([IntPtr]::Zero)
}

# ---------------------------------------------------------------- report: tools
Write-Host ''
Write-Host '---------------- 结果 ----------------'
function Get-ExitCode($P) { try { return $P.ExitCode } catch { return $null } }
if ($null -ne $runError) { Add-Check '流程正常执行' $false $runError }
Add-Check 'fakegame 正常退出' ((Get-ExitCode $fake) -eq 0) ("exit={0}" -f (Get-ExitCode $fake))
Add-Check 'inputsim 正常退出' ((Get-ExitCode $sim) -eq 0) ("exit={0}" -f (Get-ExitCode $sim))
Write-Host ("  [app] exit={0}" -f (Get-ExitCode $app))

$segs = @()
if (Test-Path $SimOut) {
    foreach ($line in (Get-Content $SimOut)) {
        $m = [regex]::Match($line, $SimSegRegex)
        if ($m.Success) {
            Write-Host ("  [inputsim] " + $line)
            $segs += [pscustomobject]@{
                N    = [int]$m.Groups['n'].Value
                Kind = $m.Groups['kind'].Value
                Dx   = [long]$m.Groups['dx'].Value
                T0   = Parse-SimTime $m.Groups['t0'].Value
                T1   = Parse-SimTime $m.Groups['t1'].Value
            }
        } elseif ($line -like 'total*') {
            Write-Host ("  [inputsim] " + $line)
        }
    }
}
$turns = @($segs | Where-Object { $_.Kind -eq 'turn' } | Sort-Object N)
$simCpd = $null
if ($turns.Count -ge 2) {
    $simCpd = (([math]::Abs($turns[0].Dx) + [math]::Abs($turns[1].Dx)) / 2.0) / 360.0
    Write-Host ("  [inputsim] 两次校准转动实际发送 {0} / {1} 计数 → 期望每度 {2:N3}" -f $turns[0].Dx, $turns[1].Dx, $simCpd)
}

# ---------------------------------------------------------------- 1. 临时 INI
$iniCpd = $null
if (Test-Path $CfgPath) {
    Copy-Item -LiteralPath $CfgPath -Destination (Join-Path $OutDir 'calib_MotionCalm.ini') -Force
    $section = ''
    foreach ($line in (Get-Content -LiteralPath $CfgPath -Encoding UTF8)) {
        $t = $line.Trim()
        if ($t.StartsWith('[')) { $section = ($t -replace '^\[([^\]]*)\].*$', '$1').Trim().ToLowerInvariant(); continue }
        if ($section -eq 'calibration' -and $t -match '^fakegame\.exe\s*=\s*([0-9.]+)') {
            Write-Host ("  [ini] [calibration] " + $t)
            $iniCpd = Parse-Double $Matches[1]
        }
    }
}
if ($null -eq $iniCpd) {
    Add-Check 'INI [calibration] fakegame.exe ≈ 36.4' $false ("{0} 中没有 [calibration] fakegame.exe= 条目" -f $CfgPath)
} else {
    $lo = $ExpectCpd * (1 - $TolCpd); $hi = $ExpectCpd * (1 + $TolCpd)
    Add-Check 'INI [calibration] fakegame.exe ≈ 36.4' (($iniCpd -ge $lo) -and ($iniCpd -le $hi)) ("{0:N3}（允许 {1:N3}–{2:N3}）" -f $iniCpd, $lo, $hi)
    if ($null -ne $simCpd) {
        Add-Check 'INI 值 = inputsim 实际发送量 / 360' ([math]::Abs($iniCpd - $simCpd) -le 0.0011) ("INI {0:N3} vs 发送量 {1:N3}" -f $iniCpd, $simCpd)
    }
}

# ---------------------------------------------------------------- 2–4. 日志
$logs = @(Get-ChildItem -Path $Bin -Filter 'MotionCalm*.log*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime)
$runFrom = if ($appStart) { $appStart.AddSeconds(-1) } else { $testStart }
$runTo = (Get-Date).AddSeconds(1)
$lines = New-Object System.Collections.Generic.List[object]
foreach ($log in $logs) {
    foreach ($line in (Get-Content -LiteralPath $log.FullName -Encoding UTF8)) {
        $mt = [regex]::Match($line, $TimestampRegex)
        if (-not $mt.Success) { continue }
        try { $t = Convert-LogTime $mt $testStart } catch { continue }
        if ($t -lt $runFrom -or $t -gt $runTo) { continue }
        $lines.Add([pscustomobject]@{ T = $t; Text = $line })
    }
}
$lines = @($lines | Sort-Object T)
$runLog = Join-Path $OutDir 'calib_MotionCalm.log'
$lines | ForEach-Object { $_.Text } | Set-Content -LiteralPath $runLog -Encoding UTF8
Write-Host ("  [app] 本次运行的日志 {0} 行（已存到 {1}）" -f $lines.Count, $runLog)

foreach ($l in $lines) {
    if ($l.Text -match '\] (calib (state|saved|cancelled)|toast: |hotkeys registered|config: \[calibration\])') { Write-Host ("  [log] " + $l.Text) }
}

# 2. 状态转移
$transitions = @($lines | Where-Object { $_.Text -match '\bcalib state=(\S+)' } | ForEach-Object { [regex]::Match($_.Text, '\bcalib state=(\S+)').Groups[1].Value })
$wantTransitions = @('Idle->Run1', 'Run1->Between', 'Between->Run2', 'Run2->Done->Idle')
$transText = $transitions -join ', '
Add-Check '状态转移 Idle→Run1→Between→Run2→Done→Idle' ((($transitions -join '|') -eq ($wantTransitions -join '|'))) $(if ($transText) { $transText } else { '日志中没有 calib state= 行' })

# 3. 保存行与 toast
$saved = $lines | Where-Object { $_.Text -match '\bcalib saved exe=fakegame\.exe\b' } | Select-Object -First 1
Add-Check '日志 calib saved exe=fakegame.exe' ($null -ne $saved) $(if ($saved) { ($saved.Text -replace '^.*\bcalib saved ', 'calib saved ') } else { '没有找到' })
# 界面语言跟随系统（或 [ui] language），三种语言的文案都算通过
$wantToasts = @('校准完成：每度 36.4 计数', '校準完成：每度 36.4 計數', 'Calibration done: 36.4 counts per degree')
$wantToast = $wantToasts -join ' / '
$toast = $lines | Where-Object { $t = $_.Text; @($wantToasts | Where-Object { $t -like ('*toast: ' + $_ + '*') }).Count -gt 0 } | Select-Object -First 1
$anyToast = @($lines | Where-Object { $_.Text -match '\btoast: ' } | ForEach-Object { $_.Text -replace '^.*\btoast: ', '' }) -join ' | '
Add-Check ('toast "' + $wantToast + '"') ($null -ne $toast) $(if ($toast) { '已出现' } else { "实际 toast：$anyToast" })

# 4. 校准值立即生效：最后一段 8000 counts/s 的 frame 行
if ($turns.Count -ge 3) {
    $t3 = $turns[2]
    $from = $t3.T0.AddMilliseconds(300); $to = $t3.T1.AddMilliseconds(-200)
    $mouse = @(); $target = @()
    foreach ($l in $lines) {
        if ($l.T -lt $from -or $l.T -gt $to) { continue }
        $m = [regex]::Match($l.Text, $FrameRegex)
        if (-not $m.Success -or $m.Groups['exe'].Value -ne 'fakegame.exe') { continue }
        $mouse += (Parse-Double $m.Groups['m'].Value)
        $target += (Parse-Double $m.Groups['tg'].Value)
    }
    if ($mouse.Count -eq 0) {
        Add-Check '8000 counts/s → mouse ≈ 219.8°/s' $false '转动段内没有 exe=fakegame.exe 的 frame 行'
    } else {
        $mm = Get-Median $mouse
        $mt = Get-Median $target
        Add-Check '8000 counts/s → mouse ≈ 219.8°/s' (($mm -ge 215) -and ($mm -le 224)) ("{0} 个 frame 样本，中位数 {1:N1}°/s（允许 215–224；未校准时约 439.6）" -f $mouse.Count, $mm)
        Add-Check '8000 counts/s → target ≈ 0.549' (($mt -ge 0.50) -and ($mt -le 0.60)) ("中位数 {0:N3}（允许 0.50–0.60；未校准时为 1.0）" -f $mt)
    }
} else {
    Add-Check '8000 counts/s → mouse ≈ 219.8°/s' $false '未能解析 inputsim 的第 3 段转动'
}

# ---------------------------------------------------------------- summary
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
Write-Host ("截图在 {0}：calib_01_run1 / calib_02_turning / calib_03_between / calib_04_done（各有 _top 裁剪图）" -f $OutDir)
Write-Host ("临时 INI 副本：{0}" -f (Join-Path $OutDir 'calib_MotionCalm.ini'))
if ($failed -eq 0) {
    Write-Host ("PASS：校准验证通过（{0} 项）" -f $Checks.Count) -ForegroundColor Green
    exit 0
}
Write-Host ("FAIL：{0} / {1} 项未通过" -f $failed, $Checks.Count) -ForegroundColor Red
exit 1
