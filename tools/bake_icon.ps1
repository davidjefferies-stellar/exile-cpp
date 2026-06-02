param(
    [string]$Source = "$PSScriptRoot\..\icon.png",
    [string]$OutFile = "$PSScriptRoot\..\src\rendering\embedded_icon.h"
)

# Bake icon.png into src/rendering/embedded_icon.h as a raw RGBA8 byte
# array, so main.cpp can hand it to sokol_app's d.icon without pulling
# in a PNG decoder. Run after replacing icon.png.

Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Image]::FromFile((Resolve-Path $Source).Path)
$bmp = New-Object System.Drawing.Bitmap $src.Width, $src.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::Transparent)
$g.DrawImage($src, 0, 0, $src.Width, $src.Height)
$g.Dispose()
$rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
$data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] ($data.Stride * $data.Height)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$bmp.UnlockBits($data)

$W = $bmp.Width
$H = $bmp.Height
$rgba = New-Object byte[] ($W * $H * 4)
for ($y = 0; $y -lt $H; $y++) {
    for ($x = 0; $x -lt $W; $x++) {
        $srcOff = $y * $data.Stride + $x * 4
        $dstOff = ($y * $W + $x) * 4
        # Format32bppArgb is BGRA in memory; emit RGBA for sapp_icon_desc.
        $rgba[$dstOff + 0] = $bytes[$srcOff + 2]
        $rgba[$dstOff + 1] = $bytes[$srcOff + 1]
        $rgba[$dstOff + 2] = $bytes[$srcOff + 0]
        $rgba[$dstOff + 3] = $bytes[$srcOff + 3]
    }
}
$src.Dispose()
$bmp.Dispose()

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('// Generated from icon.png by tools/bake_icon.ps1.')
[void]$sb.AppendLine("// ${W}x${H} RGBA8 - embedded so the build does not need a PNG decoder.")
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('#include <cstdint>')
[void]$sb.AppendLine('')
[void]$sb.AppendLine("static constexpr int kAppIconWidth  = $W;")
[void]$sb.AppendLine("static constexpr int kAppIconHeight = $H;")
[void]$sb.AppendLine('static constexpr uint8_t kAppIconRGBA[] = {')
$line = ''
for ($i = 0; $i -lt $rgba.Length; $i++) {
    $line += ('0x{0:x2},' -f $rgba[$i])
    if ((($i + 1) % 16) -eq 0) {
        [void]$sb.AppendLine('    ' + $line)
        $line = ''
    }
}
if ($line -ne '') { [void]$sb.AppendLine('    ' + $line) }
[void]$sb.AppendLine('};')
[System.IO.File]::WriteAllText($OutFile, $sb.ToString(), [System.Text.Encoding]::UTF8)
"wrote $OutFile ($($rgba.Length) bytes)"
