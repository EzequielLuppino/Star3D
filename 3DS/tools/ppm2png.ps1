# Convert binary P6 PPM files to PNG (scaled up) for viewing: ppm2png.ps1 -Out dir -Scale 8 a.ppm b.ppm
param([string]$Out = ".", [int]$Scale = 1, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)
Add-Type -AssemblyName System.Drawing
foreach ($f in $Files) {
    $b = [IO.File]::ReadAllBytes($f)
    $pos = 0; $fields = @()
    while ($fields.Count -lt 4) {
        while ([char]$b[$pos] -match '\s') { $pos++ }
        $s = $pos; while (-not ([char]$b[$pos] -match '\s')) { $pos++ }
        $fields += [Text.Encoding]::ASCII.GetString($b, $s, $pos - $s)
    }
    $pos++
    $w = [int]$fields[1]; $h = [int]$fields[2]
    $bmp = New-Object Drawing.Bitmap ($w * $Scale), ($h * $Scale)
    for ($y = 0; $y -lt $h; $y++) { for ($x = 0; $x -lt $w; $x++) {
        $i = $pos + ($y * $w + $x) * 3
        $c = [Drawing.Color]::FromArgb($b[$i], $b[$i + 1], $b[$i + 2])
        for ($dy = 0; $dy -lt $Scale; $dy++) { for ($dx = 0; $dx -lt $Scale; $dx++) { $bmp.SetPixel($x * $Scale + $dx, $y * $Scale + $dy, $c) } }
    } }
    $bmp.Save((Join-Path $Out ([IO.Path]::GetFileNameWithoutExtension($f) + ".png")))
}
