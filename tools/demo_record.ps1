# Records a ~15 s demo of the dock on the primary monitor (left edge) and renders GIF + MP4.
# Takes over the mouse while running. Usage: powershell -File tools\demo_record.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\LiquidDock.exe'
$rec = Join-Path $root 'build\data\rec'
$out = Join-Path $root 'demo'
$ff = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ff) { $ff = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe | Select-Object -First 1 -ExpandProperty FullName }

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class M {
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[M]::SetProcessDPIAware() | Out-Null

$script:cx = 1500.0; $script:cy = 1100.0
function Move-To([double]$x, [double]$y, [double]$sec) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $x0 = $script:cx; $y0 = $script:cy
  while ($true) {
    $t = [Math]::Min(1.0, $sw.Elapsed.TotalSeconds / [Math]::Max($sec, 0.001))
    $e = $t * $t * (3 - 2 * $t)           # smoothstep
    [M]::SetCursorPos([int]($x0 + ($x - $x0) * $e), [int]($y0 + ($y - $y0) * $e)) | Out-Null
    if ($t -ge 1) { break }
    [Threading.Thread]::Sleep(6)
  }
  $script:cx = $x; $script:cy = $y
}
function Wait([double]$sec) { [Threading.Thread]::Sleep([int]($sec * 1000)) }

# dock layout on the primary monitor (from the dock log): icons centred at x~61
$iconX = 60; $appsTop = 380; $appsBottom = 1560; $kakaoY = 1388; $sensY = 1764

[M]::SetCursorPos(1500, 1100) | Out-Null
& $exe --rec-start
Wait 1.0
Move-To 0 1000 0.7                 # hit the edge -> dock slides in
Wait 0.35
Move-To $iconX 1000 0.25
Move-To $iconX $appsTop 1.3        # magnification wave up...
Move-To $iconX $appsBottom 2.6     # ...and down
Move-To $iconX 1150 0.9
Move-To $iconX $sensY 0.8          # system monitor panel
Wait 2.4
Move-To 1500 1100 0.8              # leave -> dock hides
Wait 1.0
& $exe --debug-bounce              # KakaoTalk "message": icon peeks and bounces
Wait 2.6
Move-To 0 $kakaoY 0.8              # open the dock again: orange attention dot + tooltip
Move-To $iconX $kakaoY 0.3
Wait 1.6
Move-To 1500 1100 0.8
Wait 1.0
& $exe --rec-stop
Wait 1.5

$info = (Get-Content "$rec\info.txt").Trim().Split(' ')
$size = "$($info[0])x$($info[1])"
New-Item -ItemType Directory -Force $out | Out-Null
& $ff -loglevel error -y -f rawvideo -pix_fmt bgra -s $size -r $info[2] -i "$rec\frames.raw" `
  -c:v libx264 -preset slow -crf 18 -pix_fmt yuv420p -movflags +faststart "$out\liquid-dock.mp4"
& $ff -loglevel error -y -f rawvideo -pix_fmt bgra -s $size -r $info[2] -i "$rec\frames.raw" `
  -vf "fps=24,scale=600:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=256:stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a:diff_mode=rectangle" `
  "$out\liquid-dock.gif"
Remove-Item "$rec\frames.raw"
Get-ChildItem $out | Select-Object Name, @{n='MB';e={[Math]::Round($_.Length/1MB,2)}}
