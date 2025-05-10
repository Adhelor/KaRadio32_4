# Changes made
1. The S3 port would be pointless without SSL as most of the radio streams uses https protocol, so it was neccesary to update WolfSSL component at least to 5.6.6-stable-update2-esp32 (one of) the first with (more or less) full S3 support and still being compatible with ESP-IDF 4.4 (but this port can be compiled with IDF 5.2)

2. Partition table was rearanged as I've streched the OTA_0/OTA_1 partitions up to maksimum and still:
    KaRadio32_5.bin binary size 0x1e6800 bytes. Smallest app partition is 0x1eb000 bytes. 0x4800 bytes (1%) free.
    Warning: The smallest app partition is nearly full (1% free space left)!

    even with all heap tracking functions switched off in menuconfig... 

    There are partitions_X_MB.csv for 4 and 8 MB Flash on which you can check where exactly Flash hardware partition (and others partitions using Flash Tool).

3. The good information is that ESP32S3 chip is capable to decode AAC streams without any issues during tests on my N16R8 there was all the time more than 6Mb free
    space on heap, so there shouldn't be any issues with 4MB PSRAM units. However with base 520kb SRAM chips AAC decoding will not work.

4.  I've added another Output mode I2S_32BIT as my ES9038Q2M DAC do not work properly with a standard 16BIT I2S width (it just use some I2S configuration from MERUS). Webpage was also adjusted so it's possible to set the new output from there.
