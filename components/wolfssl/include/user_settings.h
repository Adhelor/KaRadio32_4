/* user_settings.h
 *
 * Copyright (C) 2006-2023 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */
#include <sdkconfig.h>
#undef WOLFSSL_ESPIDF
#undef WOLFSSL_ESPWROOM32
#undef WOLFSSL_ESPWROOM32SE
#undef WOLFSSL_ESPWROOM32
#undef WOLFSSL_ESP8266

#define WOLFSSL_ESPIDF

/*
 * choose ONE of these Espressif chips to define:
 *
 * WOLFSSL_ESPWROOM32
 * WOLFSSL_ESPWROOM32SE
 * WOLFSSL_ESP8266
 */

#define WOLFSSL_ESP32

/* #define DEBUG_WOLFSSL_VERBOSE */

#define BENCH_EMBEDDED
#define USE_CERT_BUFFERS_1024
/* TLS 1.3                                 */
#define WOLFSSL_TLS13
#define HAVE_TLS_EXTENSIONS
#define WC_RSA_PSS
#define HAVE_HKDF
#define HAVE_AEAD
#define HAVE_SUPPORTED_CURVES

/* when you want to use SINGLE THREAD */
#define SINGLE_THREADED 
#define NO_FILESYSTEM

#define HAVE_AESGCM
#define WOLFSSL_RIPEMD
/* when you want to use SHA384 */
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define WOLFSSL_SHA3
// #define HAVE_ED25519 /* ED25519 requires SHA512 */
#define HAVE_ECC
#define HAVE_CURVE25519
#define CURVE25519_SMALL
// #define HAVE_ED25519
// #define WOLFSSL_MEMORY_LOG
#define OPENSSL_EXTRA
/* when you want to use pkcs7 */
/* #define HAVE_PKCS7 */

#define HAVE_PKCS7
#if defined(HAVE_PKCS7)
    #define HAVE_AES_KEYWRAP
    #define HAVE_X963_KDF
    #define WOLFSSL_AES_DIRECT
#endif

/* when you want to use aes counter mode */
/* #define WOLFSSL_AES_DIRECT */
/* #define WOLFSSL_AES_COUNTER */

/* esp32-wroom-32se specific definition */
#if defined(WOLFSSL_ESPWROOM32SE)
    #define WOLFSSL_ATECC508A
    #define HAVE_PK_CALLBACKS
    /* when you want to use a custom slot allocation for ATECC608A */
    /* unless your configuration is unusual, you can use default   */
    /* implementation.                                             */
    /* #define CUSTOM_SLOT_ALLOCATION                              */
#endif

/* rsa primitive specific definition */
#if defined(WOLFSSL_ESP32) || defined(WOLFSSL_ESPWROOM32SE)
    /* Define USE_FAST_MATH and SMALL_STACK                        */
    #define ESP32_USE_RSA_PRIMITIVE
    #if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)

        /* NOTE HW unreliable for small values! */
        /* threshold for performance adjustment for HW primitive use   */
        /* X bits of G^X mod P greater than                            */
        #undef  ESP_RSA_EXPT_XBITS
        #define ESP_RSA_EXPT_XBITS 32

        /* X and Y of X * Y mod P greater than                         */
        #undef  ESP_RSA_MULM_BITS
        #define ESP_RSA_MULM_BITS  16

    #endif
#endif

#define RSA_LOW_MEM
#define WOLFSSL_LOW_MEMORY
/* debug options */
#define DEBUG_WOLFSSL 
#define WOLFSSL_ESP32WROOM32_CRYPT_DEBUG
#define WOLFSSL_ATECC508A_DEBUG

/* date/time                               */
/* if it cannot adjust time in the device, */
/* enable macro below                      */
 #define NO_ASN_TIME 
/* #define XTIME time */

/* USE_FAST_MATH is default */
#define USE_FAST_MATH
#define WOLFSSL_SMALL_STACK
#define HAVE_VERSION_EXTENDED_INFO
/* #define HAVE_WC_INTROSPECTION */

#define  HAVE_SESSION_TICKET

/* #define HAVE_HASHDRBG */

#define WOLFSSL_KEY_GEN
#define WOLFSSL_CERT_REQ
#define WOLFSSL_CERT_GEN
#define WOLFSSL_CERT_EXT
#define WOLFSSL_SYS_CA_CERTS
#define FREERTOS

#define WOLFSSL_CERT_TEXT

#define WOLFSSL_ASN_TEMPLATE

/* when you want not to use HW acceleration */
/* #define NO_ESP32WROOM32_CRYPT */
/* #define NO_WOLFSSL_ESP32WROOM32_CRYPT_HASH*/
/* #define NO_WOLFSSL_ESP32WROOM32_CRYPT_AES */
/* #define NO_WOLFSSL_ESP32WROOM32_CRYPT_RSA_PRI */

#define WOLFSSL_HW_METRICS
/* adjust wait-timeout count if you see timeout in rsa hw acceleration */
// #define ESP_RSA_TIMEOUT_CNT    0x249F00
