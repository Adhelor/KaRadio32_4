idf_component_register(SRCS "bt_speaker.c"
                            "bt_app_av.c"
                            "bt_app_core.c"
                    INCLUDE_DIRS "./include"
                    PRIV_REQUIRES common fifo mp3_decoder fdk-aac_decoder audio_renderer main
                    )