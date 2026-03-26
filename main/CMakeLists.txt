set(COMPONENT_SRCS 		
	"addon.c"
	"addoncommon.c"
	"app_main.c"
	"cencode.c"
	"ClickButtons.c"
	"ClickEncoder.c"
	"ClickJoystick.c"
	"custom.c"
	"eeprom.c"
	"gpio.c"
	"interface.c"
	"libsha1.c"
	"ntp.c"
	"ota.c"
	"servers.c"
	"timezone.c"
	"webclient.c"
	"webserver.c"
	"websocket.c"
    )
# Conditionally add display source files based on Kconfig options
if(CONFIG_DISPLAY_TYPE_MONO OR CONFIG_DISPLAY_TYPE_ALL)
    list(APPEND COMPONENT_SRCS "addonu8g2.c")
endif()
if(CONFIG_DISPLAY_TYPE_COLOR OR CONFIG_DISPLAY_TYPE_ALL)
    list(APPEND COMPONENT_SRCS "addonucg.c")
endif()
if (CONFIG_USE_IRNEC)
	list(APPEND COMPONENT_SRCS "irnec.c")
endif()
if (CONFIG_USE_TELNET)
	list(APPEND COMPONENT_SRCS "telnet.c"
	)
endif()
if (CONFIG_USE_VS1053)
	list(APPEND COMPONENT_SRCS "vs1053.c"
	"vs1053b-patches.c"
	)
endif()
set(COMPONENT_ADD_INCLUDEDIRS 
	"./include/"
	"../webpage/"
	"../build/"
	
	PRIV_REQUIRES wolfssl bt_speaker
)

register_component()
component_compile_options(-Wno-error=unused-label -Wno-error=return-type -Wno-error=missing-braces 
-Wno-error=pointer-sign -Wno-error=parentheses -Wno-error=maybe-uninitialized -DWOLFSSL_USER_SETTINGS -DWOLFSSL_STATIC_RSA )

