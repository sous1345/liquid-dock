# Generates assets\app.ico (16-256 px): a blue squircle with a frosted glass dock and three app tiles.
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'assets'
New-Item -ItemType Directory -Force $out | Out-Null

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure(); return $p
}

function Render([int]$s) {
    $b = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $k = $s / 256.0
    # background squircle
    $bg = RoundRect (8*$k) (8*$k) (240*$k) (240*$k) (56*$k)
    $lg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 0)), ([System.Drawing.PointF]::new($s, $s)),
        ([System.Drawing.Color]::FromArgb(255, 72, 160, 255)), ([System.Drawing.Color]::FromArgb(255, 112, 70, 230))
    $g.FillPath($lg, $bg)
    # soft top highlight
    $hl = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, 8*$k)), ([System.Drawing.PointF]::new(0, 130*$k)),
        ([System.Drawing.Color]::FromArgb(70, 255, 255, 255)), ([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
    $g.SetClip($bg)
    $g.FillRectangle($hl, 0, [float](8*$k), [float]$s, [float](121*$k))
    $g.ResetClip()
    # glass dock
    $dock = RoundRect (34*$k) (150*$k) (188*$k) (64*$k) (32*$k)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(95, 255, 255, 255))), $dock)
    if ($s -ge 32) { $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 255, 255, 255)), ([float][Math]::Max(1, 3*$k))), $dock) }
    # three app tiles sitting in the dock, the middle one magnified
    $tiles = @(@(54, 164, 36, 'FFFFB020'), @(100, 140, 56, 'FF34D399'), @(166, 164, 36, 'FFFF5C7A'))
    foreach ($t in $tiles) {
        $c = [System.Drawing.Color]::FromArgb([Convert]::ToInt32($t[3], 16))
        $tp = RoundRect ($t[0]*$k) ($t[1]*$k) ($t[2]*$k) ($t[2]*$k) ($t[2]*0.26*$k)
        $g.FillPath((New-Object System.Drawing.SolidBrush $c), $tp)
    }
    $g.Dispose()
    return $b
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($s in $sizes) {
    $bmp = Render $s
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $bmp.Save((Join-Path $out 'app.png')) }
    $bmp.Dispose()
    , $ms.ToArray()
}
# ICO container with PNG entries
$fs = [IO.File]::Create((Join-Path $out 'app.ico'))
$w = New-Object IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$len); $w.Write([UInt32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close()
Get-Item (Join-Path $out 'app.ico') | Select-Object Name, Length
