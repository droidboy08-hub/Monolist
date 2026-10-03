<#
.SYNOPSIS
    Makes Monolist's app icons (packaging\icons\) from the owner's artwork.

.DESCRIPTION
    The icon is the owner's (2026-10-02): a black note on a soft white rounded
    square, packaging\icons\note.png at 1024 px. Settings > Appearance offers
    it and one more, the red note, which this draws from it: the same picture
    with the note in paper on the signal red (Theme.qml's red, a little lighter
    at the top and deeper at the foot, as the white is). For each icon it
    writes an .ico of the sizes Windows shows (16 to 256, as PNGs, which
    Windows Vista and later read) and the PNGs the app itself loads (icons\app\
    in the source tree, through Qt's resources). monolist.ico and monolist.png
    beside this script are the default's: the installer's and Explorer's.

    Rerun it after changing note.png, and commit what it writes.
#>
param(
    [string] $Packaging = $PSScriptRoot,
    [string] $AppIcons = (Join-Path $PSScriptRoot '..\icons\app')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class MonolistIcon {
    static byte[] Pixels(Bitmap b, out int stride) {
        var data = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        stride = data.Stride;
        var bytes = new byte[stride * b.Height];
        Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
        b.UnlockBits(data);
        return bytes;
    }
    static Bitmap FromPixels(byte[] bytes, int w, int h) {
        var b = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var data = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(bytes, 0, data.Scan0, bytes.Length);
        b.UnlockBits(data);
        return b;
    }
    static double Lerp(double a, double b, double t) { return a + (b - a) * t; }
    static double Clamp(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

    // The red note from the white one. Each pixel's grey says how much of it
    // is note and how much ground: the ground's grey at that height is read
    // off a column clear of the note, the note's off its stem, and the share
    // between them is the note's coverage (its antialiased edge included).
    // Coverage under a tenth is the note's soft shadow: kept as a shade of
    // the red rather than drawn as a faint pale halo.
    public static Bitmap Red(Bitmap white) {
        int w = white.Width, h = white.Height, stride;
        var src = Pixels(white, out stride);
        var ground = new double[h];
        int clearX = w * 150 / 1024;
        for (int y = 0; y < h; ++y)
            ground[y] = src[y * stride + clearX * 4 + 1];
        var dst = new byte[src.Length];
        for (int y = 0; y < h; ++y) {
            double v = (double)y / (h - 1);
            // The note's own grey: lighter at the flag, black from the middle down.
            double note = 64.0 * Clamp((0.586 - v) / (0.586 - 0.205));
            for (int x = 0; x < w; ++x) {
                int i = y * stride + x * 4;
                byte a = src[i + 3];
                if (a == 0) continue;
                double grey = src[i + 1];
                double g = Math.Max(ground[y], 1.0);
                double cover = Clamp((g - grey) / Math.Max(1.0, g - note));
                // The ground: the white's own fall from 255 to 236, as red.
                double fall = Clamp((255.0 - Math.Max(grey, g)) / 19.0);
                double r = Lerp(0xf0, 0xd4, fall), gg = Lerp(0x4a, 0x29, fall), bb = Lerp(0x2c, 0x0e, fall);
                double shade = 1.0 - 0.9 * Math.Min(cover, 0.1);
                r *= shade; gg *= shade; bb *= shade;
                // The note: paper, as bright as the ground's top at the flag.
                double k = Clamp((cover - 0.1) / 0.9);
                k = k * k * (3 - 2 * k);
                double paper = Lerp(255, 0xef, v);
                dst[i + 2] = (byte)Math.Round(Lerp(r, paper, k));
                dst[i + 1] = (byte)Math.Round(Lerp(gg, paper, k));
                dst[i + 0] = (byte)Math.Round(Lerp(bb, paper - 1, k));
                dst[i + 3] = a;
            }
        }
        return FromPixels(dst, w, h);
    }

    // Halving steps down to the size, then one last bicubic step: a straight
    // 1024 -> 16 skips most of the picture's pixels and comes out ragged.
    public static Bitmap Scale(Bitmap source, int size) {
        Bitmap current = source;
        while (current.Width / 2 >= size * 2) {
            current = Draw(current, current.Width / 2, current != source);
        }
        return Draw(current, size, current != source);
    }
    static Bitmap Draw(Bitmap from, int size, bool disposeFrom) {
        var to = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(to)) {
            g.CompositingMode = CompositingMode.SourceCopy;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.SmoothingMode = SmoothingMode.HighQuality;
            using (var wrap = new ImageAttributes()) {
                wrap.SetWrapMode(WrapMode.TileFlipXY);
                g.DrawImage(from, new Rectangle(0, 0, size, size), 0, 0, from.Width, from.Height, GraphicsUnit.Pixel, wrap);
            }
        }
        if (disposeFrom) from.Dispose();
        return to;
    }
}
"@

$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$appSizes = 16, 24, 32, 48, 64, 128, 256

function Write-Ico([System.Drawing.Bitmap] $art, [string] $path) {
    $frames = @()
    foreach ($size in $sizes) {
        $bitmap = [MonolistIcon]::Scale($art, $size)
        $stream = New-Object System.IO.MemoryStream
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames += , @{ Size = $size; Bytes = $stream.ToArray() }
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
    [System.IO.File]::WriteAllBytes($path, $out.ToArray())
}

$icons = Join-Path $Packaging 'icons'
New-Item -ItemType Directory -Force $AppIcons | Out-Null
$white = [System.Drawing.Bitmap]::FromFile((Resolve-Path (Join-Path $icons 'note.png')).ProviderPath)
$red = [MonolistIcon]::Red($white)
$red.Save((Join-Path $icons 'note-red.png'), [System.Drawing.Imaging.ImageFormat]::Png)

foreach ($icon in @(@{ Id = 'note'; Art = $white }, @{ Id = 'note-red'; Art = $red })) {
    Write-Ico $icon.Art (Join-Path $icons "$($icon.Id).ico")
    foreach ($size in $appSizes) {
        $bitmap = [MonolistIcon]::Scale($icon.Art, $size)
        $bitmap.Save((Join-Path $AppIcons "$($icon.Id)-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
    }
}

# The default, where the build, the installer and the docs look for it.
Copy-Item (Join-Path $icons 'note.ico') (Join-Path $Packaging 'monolist.ico') -Force
Copy-Item (Join-Path $AppIcons 'note-256.png') (Join-Path $Packaging 'monolist.png') -Force
$white.Dispose(); $red.Dispose()
"wrote note.ico, note-red.ico ($($sizes.Count) sizes each), monolist.ico, and $($appSizes.Count * 2) app PNGs"
