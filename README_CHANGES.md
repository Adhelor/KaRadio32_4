# Changes made
1. The S3 port would be pointless without SSL as most of the radio streams uses https protocol, so it was neccesary to update WolfSSL component at least to 5.6.6-stable-update2-esp32 (one of) the first with (more or less) full S3 support and still being compatible with ESP-IDF 4.4 (but this port can be compiled with IDF 5.2)

2. Partition table was rearanged - I've streched the OTA_0/OTA_1 partitions up to maksimum and still when trying to port this to IDF 5.2:
    KaRadio32_5.bin binary size 0x1e6800 bytes. Smallest app partition is 0x1eb000 bytes. 0x4800 bytes (1%) free.
    Warning: The smallest app partition is nearly full (1% free space left)!

    even with all heap tracking functions switched off in menuconfig... 

    There are 2 files partitions_X_MB.csv for 4 and 8 MB Flash on which you can check where exactly Flash hardware partition (and others partitions using Flash Tool) should be depending on wchich partition scheme Tou     pick.

3. The good information is that ESP32S3 chip is capable to decode AAC streams without any issues during tests on my N16R8 there was all the time more than 6Mb free
    space on heap, so there shouldn't be any issues with 4MB PSRAM units. However with base 520kb SRAM chips AAC decoding will not work.

4.  I've added another Output mode I2S_32BIT as my ES9038Q2M DAC do not work properly with a standard 16BIT I2S width (it just use some I2S configuration from MERUS). Webpage was also adjusted so it's possible to set     the new output from there.
5.  Use idf.py set-target if ESP32S3 is not the option you are intrested in.

6. Use idf.py menuconfig to adjust the configuration to your needs (ie PSRAM), sdkconfig is prepared for ESP32S3N16R8 so with octal PSRAM choose quad if you have 4MB of PSRAM or switch it off completly if it's absent SPI Flash is setted to QIO mode CPU freq at 240MHz...
   I've tried to switch from mbedTLS to wolfSSL, but it's not working with IDF 4.4 (after change it will even not let you start menuconfig again without deleting sdkconfig file or editing it). Menuconfig is quite     
   handy tool - when it will show up pres "/" and you can write PSRAM and all PSRAM related settings will show up...

7. After changes you need just build the project idf.py build and after it's finished idf.py flash -p [PORT] it will flash everithing except hardware partition which need to be flashed by Flash Tool.

8. I don't have Merus, or VS1053 so this part is not tested.
