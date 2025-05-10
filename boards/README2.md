# BOARDS Description

This directory contains as an template configuration of my ESP32S3 board with SSH1306 OLED and single encoder and I2S DAC. 
Just adjust the GPIO numbers to your needs and generate the hardware partition using one of the scripts and flash it to your board... 

More templates can be found on original KaRadio Repo, but keep in mind, that dome of them will not generate proper bin file, what is easy to fix:
1. No empty lines are allowed - so remove them before building your own *.bin file
2. There is mandatory "header" in the first line of csv:
                key,type,encoding,value
    and "section headers"
                label_space,namespace,,         //second line
                gpio_space,namespace,,          //before K_SPI definition
                option_space,namespace,,        //before O_XXX - option definisions
                custom_ir_space,namespace,,     //before K_XXX - IR keys definition

But its enough to copy interesting sections to the mentioned template. 

There is no difference at this point if your SOC have PSRAM or not, other than PSRAM is using some GPIOS which shouldn't be used for other purposes.

It's start to be more important when you have ESP32S3 Devkit board with OCTAL PSRAM (8MB) where pins 35,36,37 are in use by PSRAM
more informations about "better avoid to use those pins" on ESP32S3: https://github.com/atomic14/esp32-s3-pinouts.


