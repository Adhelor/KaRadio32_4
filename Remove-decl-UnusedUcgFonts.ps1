# Remove-UnusedUcgFonts.ps1
# This script deletes font declarations from ucg.h for all fonts listed in unused_fonts.txt

$ucgHeader = "d:\Arduino\Projects\KaRadio32_4\components\ucglib\csrc\ucg.h"
$unusedFontsFile = "unused_fonts.txt"

# Read unused font symbols
$unusedFonts = Get-Content $unusedFontsFile

# Read all lines from ucg.h
$ucgLines = Get-Content $ucgHeader

# Remove lines declaring unused fonts
foreach ($font in $unusedFonts) {
    $pattern = "extern const ucg_fntpgm_uint8_t $font"
    $ucgLines = $ucgLines | Where-Object {$_ -notmatch [regex]::Escape($pattern)}
}

# Write the filtered lines back to ucg.h
$ucgLines | Set-Content $ucgHeader

Write-Host "Declarations for unused fonts removed from $ucgHeader."