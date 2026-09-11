# Find-UnusedUcgFonts.ps1
# Set the path to your ucg.h file and project root
$ucgHeader = "d:\Arduino\Projects\KaRadio32_4\components\ucglib\csrc\ucg.h"
$projectRoot = "d:\Arduino\Projects\KaRadio32_4"

# 1. Extract all font symbols from ucg.h
$allFonts = Select-String -Path $ucgHeader -Pattern 'extern const ucg_fntpgm_uint8_t (ucg_font_[a-zA-Z0-9_]+)' | 
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique

# 2. For each font, search for usage in the project (excluding the declaration itself)
$unusedFonts = @()
foreach ($font in $allFonts) {
    $found = Get-ChildItem -Path $projectRoot -Recurse -Include *.c,*.h,*.cpp | 
        Select-String -Pattern $font | 
        Where-Object { $_.Path -ne $ucgHeader }
    if (-not $found) {
        $unusedFonts += $font
    }
}

# 3. Output the unused fonts and save to file
if ($unusedFonts.Count -eq 0) {
    Write-Host "All fonts are used somewhere in the project."
} else {
    Write-Host "Unused fonts (safe to remove):"
    $unusedFonts | ForEach-Object { Write-Host $_ }
    $unusedFonts | Set-Content -Path "unused_fonts.txt"
    Write-Host "Unused font list saved to unused_fonts.txt"
}