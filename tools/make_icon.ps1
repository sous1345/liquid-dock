# Generates assetspp.ico (16-256 px) and app.png: a pink-to-blue rounded square with one thick glass dock.
# The SFF gallery mark (assets\sff-mark.png) sits above the dock.
# With -Large <px> it also writes assetspp-<px>.png (for the promo video).
param([int]$Large = 0)
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'assets'
$script:mark = [System.Drawing.Image]::FromFile((Join-Path $out 'sff-mark.png'))
New-Item -ItemType Directory -Force $out | Out-Null

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure(); return $p
}

function Brush([float]$x0, [float]$y0, [float]$x1, [float]$y1, $stops) {
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new($x0, $y0)), ([System.Drawing.PointF]::new($x1, $y1)),
        ([System.Drawing.Color]::Black), ([System.Drawing.Color]::White)
    $cb = New-Object System.Drawing.Drawing2D.ColorBlend $stops.Count
    $cb.Colors = [System.Drawing.Color[]]@($stops | ForEach-Object { [System.Drawing.Color]::FromArgb([Convert]::ToInt32($_[1], 16)) })
    $cb.Positions = [single[]]@($stops | ForEach-Object { $_[0] })
    $br.InterpolationColors = $cb
    return $br
}

function Render([int]$s) {
    $b = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $k = $s / 256.0
    # background: a rounded square in the wallpaper's flower colours, pink to violet to blue
    $bg = RoundRect (8*$k) (8*$k) (240*$k) (240*$k) (58*$k)
    $g.FillPath((Brush 0 0 $s $s @(@(0, 'FFFF7EB3'), @(0.55, 'FFA77BFF'), @(1, 'FF4F7DFF'))), $bg)
    # one thick glass dock: a soft shadow, the glass body, its rim and a top highlight
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(64, 42, 27, 94))), (RoundRect (38*$k) (156*$k) (180*$k) (62*$k) (31*$k)))
    $dock = RoundRect (38*$k) (148*$k) (180*$k) (62*$k) (31*$k)
    $g.FillPath((Brush 0 (148*$k) 0 (210*$k) @(@(0, 'B3FFFFFF'), @(0.5, '38FFFFFF'), @(1, '73FFFFFF'))), $dock)
    $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230, 255, 255, 255)), ([float][Math]::Max(1, 3*$k))), $dock)
    if ($s -ge 24) { $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(191, 255, 255, 255))), (RoundRect (56*$k) (154*$k) (144*$k) (12*$k) (6*$k))) }
    # the SFF gallery mark above it, with a soft shadow
    $mw = 104 * $k * $script:mark.Width / $script:mark.Height; $mh = 104 * $k
    $mr = [System.Drawing.RectangleF]::new([float](128*$k - $mw/2), [float](34*$k), [float]$mw, [float]$mh)
    $cm = New-Object System.Drawing.Imaging.ColorMatrix
    $cm.Matrix00 = 0; $cm.Matrix11 = 0; $cm.Matrix22 = 0; $cm.Matrix33 = 0.3
    $cm.Matrix40 = 42/255; $cm.Matrix41 = 27/255; $cm.Matrix42 = 94/255
    $ia = New-Object System.Drawing.Imaging.ImageAttributes; $ia.SetColorMatrix($cm)
    $sr = [System.Drawing.Rectangle]::new([int]$mr.X, [int]($mr.Y + 5*$k), [int]$mw, [int]$mh)
    $g.DrawImage($script:mark, $sr, 0, 0, $script:mark.Width, $script:mark.Height, [System.Drawing.GraphicsUnit]::Pixel, $ia)
    $g.DrawImage($script:mark, $mr)
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
if ($Large -gt 0) { $big = Render $Large; $big.Save((Join-Path $out "app-$Large.png")); $big.Dispose() }
Get-Item (Join-Path $out 'app.ico') | Select-Object Name, Length
