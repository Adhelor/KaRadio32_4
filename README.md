KaRadio32 S3 – Changelog
1. SSL / ESP32S3 support

The ESP32S3 port would not be practical without SSL support, as most radio streams currently use the HTTPS protocol. For this reason, the WolfSSL component was updated to version 5.6.6-stable-update2-esp32. This is one of the first versions that provides reasonably complete support for ESP32S3 while still remaining compatible with ESP-IDF 4.4.

This version of WolfSSL introduces a different memory management model, and for stable operation a SoC with PSRAM is required.

For compatibility, a ZIP archive with the older WolfSSL version is provided. To use it, replace the contents of components/wolfssl/ with the version from the archive (the correct user_settings.h is already included).

The project can also be built using ESP-IDF 5.2, although this requires minor adjustments.

2. Partition table and Flash usage

The partition table has been reorganized to maximize the size of the OTA partitions (OTA_0 and OTA_1). Despite this, the application size is very close to the partition limit:

KaRadio32_5.bin size: 0x1e6800 bytes
Smallest app partition: 0x1eb000 bytes
Free space: 0x4800 bytes (~1%)

When building with ESP-IDF 5.2, a warning about low free space is generated, even with all heap tracking features disabled in menuconfig.

For reference, partitions_X_MB.csv files are provided for both 4 MB and 8 MB Flash configurations. These can be used to verify the exact layout of the hardware partitions.

3. Flash layout and bootloader offset

The Flash layout differs between ESP32 and ESP32S3. In particular, the bootloader offset is not the same:

>ESP32 uses a bootloader offset of 0x1000
>ESP32S3 requires the bootloader to be placed at 0x0

This value is defined by the ROM bootloader and is not configurable in ESP-IDF.

For reference: 

<img width="691" height="676" alt="image" src="https://github.com/user-attachments/assets/167a4074-d484-457d-aa86-090726b6e70f" />

With mistake in bootloader offset for ESP32S3 target...

- I use idf.py flash -p (PORT) directly from VSCode and this handles all diferences in targets
- Flashtool is not working with my ESP32-WROOM-32UE-N8R2, so for hardware partition console esptool.py need to be used (flashtool detects SOC, can read but not write to flash)

 - flashing OTA_1 is not necessary (it will be flashed with first OTA update).

4. Audio decoding (AAC)

Testing shows that the ESP32S3 can decode AAC streams without issues. On an N16R8 module, there was consistently more than 6 MB of free heap available during operation.

This indicates that modules with 4 MB PSRAM should also handle AAC decoding reliably. However, devices with only 520 KB SRAM (without PSRAM) do not have sufficient memory to support AAC decoding.

5. I2S output mode

A new output mode, I2S_32BIT, has been added. This was required because the ES9038Q2M DAC does not operate correctly with the standard 16-bit I2S width and instead expects a configuration similar to that used in MERUS-based designs.

The web interface has been updated to allow selecting this output mode.

6. Volume control

Volume control is now based on a lookup table with 24 steps (0 = mute, 23 = reference level at 0 dB). The table is extended to allow per-station volume offsets in the range of -4 to +8.

In practice:

With offset 0, step 23 corresponds to an unchanged signal level (0 dB).
Negative offsets provide attenuation.
Positive offsets allow amplification. For example, with offset +4, the original loudness is reached at step 19, and higher steps introduce gain (up to approximately +3 dB).

For VS1053-based configurations, gain is not supported. In this case, positive offsets simply reduce the usable range by reaching the maximum volume earlier.

Additional CLI commands for mute and unmute have also been added.

7. Operating modes (WebRadio / Bluetooth / Sleep)

The project supports two primary operating modes: WebRadio and Bluetooth (A2DP sink), depending on the target hardware.

Bluetooth mode requires BT Classic with A2DP support, which is only available on the original ESP32. It is not available on ESP32S3, as that chip only supports BLE and does not implement BT Classic.

- A long press on the main encoder switches between WebRadio and Bluetooth modes (if supported by the target hardware).  
- The second encoder is used to control sleep (long press to enter sleep, press to wake).  
> Keep in mind that wake-up via encoder button requires the pin to be in the RTC GPIO range.
- Mode switching is also available via console commands IR or WebUI.  

When Bluetooth mode is active (ESP32 only):

- The web interface is not available.  
- Telnet is not available.  

This is due to resource and bandwidth constraints when running A2DP alongside Wi-Fi.

> As a result, switching from WebRadio to Bluetooth can be triggered from the WebUI/telnet command, but switching back requires the encoder, IR, or a direct console connection (UART/USB), as network-based control is not available in Bluetooth mode.

8. Encoder handling

Encoders are now handled using PCNT (hardware pulse counters) by default. This reduces CPU load and improves reliability.

An alternative legacy implementation based on interrupts (ISR) is still available via menuconfig.

Thresholds for fast rotation apply a multiplier (acceleration effect), while slow rotation is filtered to allow precise single-step adjustments.

9. LED and display handling

The status LED brightness can be adjusted in the range of 1–100%, although the response is not linear. ESP32S3 addressable LEDs are also supported.

If two encoders are present and a GPIO is assigned for display backlight control, the brightness can be adjusted by holding and rotating the volume encoder.

10. OTA update

The OTA mechanism has been rewritten, as this fork is not compatible with the original Karawin project due to differences in the partition table.

OTA updates are now performed through the web interface, which allows selecting and uploading a binary file.

For builds that include a display, a simple dynamic PIN verification mechanism has been added to authorize the update process.

11. Sleep mode

The sleep logic has been redesigned to better match momentary switch behavior.

Sleep can be triggered by a long press on the second encoder.
Wake-up is performed by any press on the same encoder.
The encoder button must be connected to an RTC-capable pin.

Sleep can also be triggered via IR remote or terminal command, but wake-up is available only through the encoder.

If the display does not support backlight control, it will remain powered during sleep.

Backlight control depends on the display hardware. In modules with an exposed LED pin, where the backlight ground is shared with the controller ground, brightness must be controlled on the high side. This requires a driver stage (e.g., an NPN transistor controlling a P-MOSFET). Such a simple circuit also allows the display to be fully turned off when the ESP enters sleep mode.
