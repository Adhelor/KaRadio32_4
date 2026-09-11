# Remove-UnusedUcgFonts.ps1
# Set paths
$ucgHeader = "d:\Arduino\Projects\KaRadio32_4\components\ucglib\csrc\ucg.h"
$fontDir = "d:\Arduino\Projects\KaRadio32_4\components\ucglib\csrc"
$unusedFontsFile = "unused_fonts.txt"

# Read unused font symbols
$unusedFonts = Get-Content $unusedFontsFile

# Remove declarations from ucg.h
foreach ($font in $unusedFonts) {
    (Get-Content $ucgHeader) | Where-Object {$_ -notmatch $font} | Set-Content $ucgHeader
}

# Remove definitions from all .c files in fontDir
foreach ($font in $unusedFonts) {
    Get-ChildItem -Path $fontDir -Filter *.c | ForEach-Object {
        (Get-Content $_.FullName) | Where-Object {$_ -notmatch $font} | Set-Content $_.FullName
    }
}

Write-Host "Unused fonts removed from ucg.h and all .c files in $fontDir."