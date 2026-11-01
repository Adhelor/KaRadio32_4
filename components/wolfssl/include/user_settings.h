#ifndef USER_SETTINGS_H
#define USER_SETTINGS_H

#include <sdkconfig.h>

/* ============================================================
   Platform
   ============================================================ */
#define WOLFSSL_ESPID
#define WOLFSSL_ESP32

/* ============================================================
   Environment
   ============================================================ */
#define BENCH_EMBEDDED
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define FREERTOS

/* ============================================================
   TLS
   ============================================================ */
#define WOLFSSL_TLS13
#define HAVE_TLS_EXTENSIONS
#define HAVE_SESSION_TICKET

/* ============================================================
   Crypto primitives (minimal set)
   ============================================================ */
#define HAVE_AESGCM
#define HAVE_AEAD
#define HAVE_HKDF
#define WOLFSSL_SHA256

/* ============================================================
   ECC (recommended for TLS 1.3)
   ============================================================ */
#define HAVE_ECC
#define HAVE_SUPPORTED_CURVES
#define HAVE_CURVE25519
#define CURVE25519_SMALL

/* ============================================================
   RSA (only if required by some servers)
   ============================================================ */
#define WC_RSA_PSS
#define ESP32_USE_RSA_PRIMITIVE
#undef WOLFSSL_HW_METRICS

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* Conservative HW RSA thresholds (S3 is better than ESP32) */
    #undef  ESP_RSA_EXPT_XBITS
    #define ESP_RSA_EXPT_XBITS 32

    #undef  ESP_RSA_MULM_BITS
    #define ESP_RSA_MULM_BITS  16
#endif

#define USE_FAST_MATH
/* default 4096 limits RSA to 2048 bits; servers with RSA-4096 certs fail with BUFFER_ERROR */
#define FP_MAX_BITS 8192
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_LOW_MEMORY
#define RSA_LOW_MEM
#define WC_RSA_BLINDING
#define HAVE_FFDHE_2048
/* ============================================================
   Certificates
   ============================================================ */
#define USE_CERT_BUFFERS_1024
#define WOLFSSL_SYS_CA_CERTS   

/* ============================================================
   Time handling
   ============================================================ */
#define NO_ASN_TIME

/* ============================================================
   Debug 
   ============================================================ */
// #define DEBUG_WOLFSSL
// #define WOLFSSL_ESP32WROOM32_CRYPT_DEBUG

#endif /* USER_SETTINGS_H */
