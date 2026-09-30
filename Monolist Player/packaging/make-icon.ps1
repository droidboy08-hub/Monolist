<#
.SYNOPSIS
    Draws Monolist's icon (packaging\monolist.ico and monolist.png) from the
    wordmark's own type and colours.

.DESCRIPTION
    The mark is the wordmark cut to its first letter and its point: "M." in
    Archivo ExtraBold, paper on ink, the point in signal red, square corners,
    as DESIGN.md has everything else. Drawn at each size an icon is shown at,
    rather than scaled from one picture, so the small ones stay sharp; the .ico
    holds them as PNGs (what Windows Vista and later read). Run it again after
    changing it, and commit what it writes.
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$here  = $PSScriptRoot
$font  = Join-Path $here '..\fonts\Archivo-ExtraBold.ttf'
$ink   = [System.Drawing.Color]::FromArgb(255, 0x20, 0x1e, 0x1d)
$paper = [System.Drawing.Color]::FromArgb(255, 0xf3, 0xf2, 0xf2)
$red   = [System.Drawing.Color]::FromArgb(255, 0xec, 0x30, 0x13)

$fonts = New-Object System.Drawing.Text.PrivateFontCollection
$fonts.AddFontFile((Resolve-Path $font).ProviderPath)
$family = $fonts.Families[0]

function Draw-Mark([int] $size) {
    $bitmap = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear($ink)

    # The letter, as large as the tile allows beside its point.
    $emSize = $size * 0.74
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $format = [System.Drawing.StringFormat]::GenericTypographic
    $path.AddString('M', $family, [int][System.Drawing.FontStyle]::Regular, $emSize, (New-Object System.Drawing.PointF(0, 0)), $format)
    $bounds = $path.GetBounds()
    $dot = [Math]::Max(2.0, [Math]::Round($size * 0.14))
    $gap = [Math]::Max(1.0, [Math]::Round($size * 0.035))
    $width = $bounds.Width + $gap + $dot
    $left = ($size - $width) / 2
    $top = ($size - $bounds.Height) / 2
    $matrix = New-Object System.Drawing.Drawing2D.Matrix
    $matrix.Translate([float]($left - $bounds.X), [float]($top - $bounds.Y))
    $path.Transform($matrix)
    $g.FillPath((New-Object System.Drawing.SolidBrush($paper)), $path)

    # The point: a square on the letter's baseline, in the one red.
    $dotX = [Math]::Round($left + $bounds.Width + $gap)
    $dotY = [Math]::Round($top + $bounds.Height - $dot)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush($red)), [float]$dotX, [float]$dotY, [float]$dot, [float]$dot)
    $g.Dispose()
    return $bitmap
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$frames = @()
foreach ($size in $sizes) {
    $bitmap = Draw-Mark $size
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $frames += , @{ Size = $size; Bytes = $stream.ToArray() }
    if ($size -eq 256) { $bitmap.Save((Join-Path $here 'monolist.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bitmap.Dispose()
}

# ICONDIR, then one ICONDIRENTRY per frame, then the PNGs.
$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($out)
$writer.Write([UInt16]0); $writer.Write([UInt16]1); $writer.Write([UInt16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($frame in $frames) {
    $side = if ($frame.Size -ge 256) { 0 } else { $frame.Size }
    $writer.Write([Byte]$side); $writer.Write([Byte]$side)
    $writer.Write([Byte]0); $writer.Write([Byte]0)
    $writer.Write([UInt16]1); $writer.Write([UInt16]32)
    $writer.Write([UInt32]$frame.Bytes.Length); $writer.Write([UInt32]$offset)
    $offset += $frame.Bytes.Length
}
foreach ($frame in $frames) { $writer.Write($frame.Bytes) }
$writer.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $here 'monolist.ico'), $out.ToArray())
"wrote monolist.ico ($($frames.Count) sizes) and monolist.png"
