The binnaries name convention: KaRadio(target)_Display_BTmode ie KaRadio32_A_BT means that it is esp32 build with All supported displays and BT mode
KaRadio32s3_A is a build for ESP32S3 (no classic BT so A2DP sink not possible for this target),
_N stays for No display, options _C for color LCD (Ucg) _M for monochrome OLED (U8g)
as bootloaders are target specific there is esp32 and esp32s3 versions
There ara also 2 partition tables versions 4 and 8MB, 
it's possible to fit BT with ALL screens support to 4MB flash... in this case most (internal ESP) logging functions must be switched off (this is the default configuration)
Binaries are more a proof of concept - there is huge difference between be able to compile a project and having a stable and working project...
