<!--- This file has the version text in `idf_published_versions.txt` automatically replaced. Edit with caution. --->

This is wolfSSL version 5.7.6, published as an Espressif Managed Component.

Included in this library are post-release changes in [PR #8418](https://github.com/wolfSSL/wolfssl/pull/8418).

When using wolfSSL as a ESP-IDF Managed Component, be sure to keep the component Manager updated:

```
pip install -U idf-component-manager
```

The Component Manager version is specific to each ESP-IDF version installed. Each must be updated separately.

When creating your own application that uses wolfSSL as a managed component, ensure that the wolfSSL component directory is not listed in `EXTRA_COMPONENT_DIRS` variable in `CMakeLists.txt`.
See the [IDF Component Manager Documentation](https://docs.espressif.com/projects/idf-component-manager/en/latest/guides/packaging_components.html#adding-dependency-on-the-component-for-examples) for details.

Additional information including Getting Started can be found on GitHub:

[github.com/wolfSSL/wolfssl/tree/master/IDE/Espressif](https://github.com/wolfSSL/wolfssl/tree/master/IDE/Espressif)

For questions about this library, please send a message to support@wolfssl.com

See the [wolfSSL Manual](https://www.wolfssl.com/documentation/manuals/wolfssl/index.html).

The [wolfSSL embedded TLS library](https://www.wolfssl.com/products/wolfssl/) is a lightweight, portable,
C-language-based SSL/TLS library targeted at IoT, embedded, and RTOS environments primarily because of its size,
speed, and feature set. It works seamlessly in desktop, enterprise, and cloud environments as well.
wolfSSL supports industry standards up to the current [TLS 1.3](https://www.wolfssl.com/tls13) and DTLS 1.3,
is up to 20 times smaller than OpenSSL, offers a simple API, an OpenSSL compatibility layer,
OCSP and CRL support, is backed by the robust [wolfCrypt cryptography library](https://github.com/wolfssl/wolfssl/tree/master/wolfcrypt),
and much more.

The CMVP has issued FIPS 140-2 Certificates #3389 and #2425 for the wolfCrypt Module developed by wolfSSL Inc.
For more information, see our [FIPS FAQ](https://www.wolfssl.com/license/fips/) or contact fips@wolfssl.com.

# Getting Started

Check out the Examples on the right pane of the [wolfssl component page](https://components.espressif.com/components/wolfssl/wolfssl/).

Typically you need only 4 lines to run an example from scratch in the EDP-IDF environment:

```bash
. ~/esp/esp-idf/export.sh
idf.py create-project-from-example "wolfssl/wolfssl^5.7.6:wolfssl_benchmark"
cd wolfssl_benchmark
idf.py -b 115200 flash monitor
```

or for VisualGDB:

```bash
. /mnt/c/SysGCC/esp32/esp-idf/v5.1/export.sh
idf.py create-project-from-example "wolfssl/wolfssl^5.7.6:wolfssl_benchmark"
cd wolfssl_benchmark
idf.py -b 115200 flash monitor
```


### Espressif Component Notes

Here are some ESP Registry-specific details of the wolfssl component.

#### Component Name

The naming convention of the build-system name of a dependency installed by the component manager
is always `namespace__component`. The namespace for wolfSSL is `wolfssl`. The build-system name
is thus `wolfssl__wolfssl`. We'll soon be publishing `wolfssl__wolfssh`, `wolfssl__wolfmqtt` and more.

A project `cmakelists.txt` doesn't need to mention it at all when using wolfSSL as a managed component.


#### Component Manager

To check which version of the [Component Manager](https://docs.espressif.com/projects/idf-component-manager/en/latest/getting_started/index.html#checking-the-idf-component-manager-version)
is currently available, use the command:

```
python -m idf_component_manager -h
```

The Component Manager should have been installed during the [installation of the ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/#installation).
If your version of ESP-IDF doesn't come with the IDF Component Manager,
you can [install it](https://docs.espressif.com/projects/idf-component-manager/en/latest/guides/updating_component_manager.html#installing-and-updating-the-idf-component-manager):

```
python -m pip install --upgrade idf-component-manager
```

For further details on the Espressif Component Manager, see the [idf-component-manager repo](https://github.com/espressif/idf-component-manager/).

## Staging Preview Versions

There are experimental, generally unsupported preview versions of components available at https://components-staging.espressif.com/

The staging versions are developer-specific and components are renamed with a "my" prefix. (e.g. "mywolfssl")

There are some [gojimmypi] versions available here in the "gojimmypi" namespace:

https://components-staging.espressif.com/components?q=namespace:gojimmypi

When using staging components, be sure to define the `IDF_COMPONENT_REGISTRY_URL` environment variable:

```
export IDF_COMPONENT_REGISTRY_URL="https://components-staging.espressif.com"
```

#### Contact

Have a specific request or questions? We'd love to hear from you! Please contact us at
[support@wolfssl.com](mailto:support@wolfssl.com?subject=Espressif%20Component%20Question) or
[open an issue on GitHub](https://github.com/wolfSSL/wolfssl/issues/new/choose).

# Licensing and Support

wolfSSL (formerly known as CyaSSL) and wolfCrypt are either licensed for use
under the GPLv2 (or at your option any later version) or a standard commercial
license. For our users who cannot use wolfSSL under GPLv2
(or any later version), a commercial license to wolfSSL and wolfCrypt is
available.

See the [LICENSE.txt](./LICENSE.txt), visit [wolfssl.com/license](https://www.wolfssl.com/license/),
contact us at [licensing@wolfssl.com](mailto:licensing@wolfssl.com?subject=Espressif%20Component%20License%20Question)
or call +1 425 245 8247

View Commercial Support Options: [wolfssl.com/products/support-and-maintenance](https://www.wolfssl.com/products/support-and-maintenance/)

# Release README
# wolfSSL Embedded SSL/TLS Library

The [wolfSSL embedded SSL library](https://www.wolfssl.com/products/wolfssl/)
(formerly CyaSSL) is a lightweight SSL/TLS library written in ANSI C and
targeted for embedded, RTOS, and resource-constrained environments - primarily
because of its small size, speed, and feature set.  It is commonly used in
standard operating environments as well because of its royalty-free pricing
and excellent cross platform support. wolfSSL supports industry standards up
to the current [TLS 1.3](https://www.wolfssl.com/tls13) and DTLS 1.3, is up to
20 times smaller than OpenSSL, and offers progressive ciphers such as ChaCha20,
Curve25519, Blake2b and Post-Quantum TLS 1.3 groups. User benchmarking and
feedback reports dramatically better performance when using wolfSSL over
OpenSSL.

wolfSSL is powered by the wolfCrypt cryptography library. Two versions of
wolfCrypt have been FIPS 140-2 validated (Certificate #2425 and
certificate #3389). FIPS 140-3 validation is in progress. For additional
information, visit the [wolfCrypt FIPS FAQ](https://www.wolfssl.com/license/fips/)
or contact fips@wolfssl.com.

## Why Choose wolfSSL?

There are many reasons to choose wolfSSL as your embedded, desktop, mobile, or
enterprise SSL/TLS solution. Some of the top reasons include size (typical
footprint sizes range from 20-100 kB), support for the newest standards
(SSL 3.0, TLS 1.0, TLS 1.1, TLS 1.2, TLS 1.3, DTLS 1.0, DTLS 1.2, and DTLS 1.3),
current and progressive cipher support (including stream ciphers), multi-platform,
royalty free, and an OpenSSL compatibility API to ease porting into existing
applications which have previously used the OpenSSL package. For a complete
feature list, see [Chapter 4](https://www.wolfssl.com/docs/wolfssl-manual/ch4/)
of the wolfSSL manual.

## Notes, Please Read

### Note 1
wolfSSL as of 3.6.6 no longer enables SSLv3 by default.  wolfSSL also no longer
supports static key cipher suites with PSK, RSA, or ECDH. This means if you
plan to use TLS cipher suites you must enable DH (DH is on by default), or
enable ECC (ECC is on by default), or you must enable static key cipher suites
with one or more of the following defines:

```
WOLFSSL_STATIC_DH
WOLFSSL_STATIC_RSA
WOLFSSL_STATIC_PSK
```
Though static key cipher suites are deprecated and will be removed from future
versions of TLS.  They also lower your security by removing PFS.

When compiling `ssl.c`, wolfSSL will now issue a compiler error if no cipher
suites are available. You can remove this error by defining
`WOLFSSL_ALLOW_NO_SUITES` in the event that you desire that, i.e., you're
not using TLS cipher suites.

### Note 2
wolfSSL takes a different approach to certificate verification than OpenSSL
does. The default policy for the client is to verify the server, this means
that if you don't load CAs to verify the server you'll get a connect error,
no signer error to confirm failure (-188).

If you want to mimic OpenSSL behavior of having `SSL_connect` succeed even if
verifying the server fails and reducing security you can do this by calling:

```c
wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);
```

before calling `wolfSSL_new();`. Though it's not recommended.

### Note 3
The enum values SHA, SHA256, SHA384, SHA512 are no longer available when
wolfSSL is built with `--enable-opensslextra` (`OPENSSL_EXTRA`) or with the
macro `NO_OLD_SHA_NAMES`. These names get mapped to the OpenSSL API for a
single call hash function. Instead the name `WC_SHA`, `WC_SHA256`, `WC_SHA384` and
`WC_SHA512` should be used for the enum name.


# wolfSSL Release 5.7.6 (Dec 31, 2024)

Release 5.7.6 has been developed according to wolfSSL's development and QA
process (see link below) and successfully passed the quality criteria.
https://www.wolfssl.com/about/wolfssl-software-development-process-quality-assurance

NOTE:
 * --enable-heapmath is deprecated.
 * In this release, the default cipher suite preference is updated to prioritize
 TLS_AES_256_GCM_SHA384 over TLS_AES_128_GCM_SHA256 when enabled.
 * This release adds a sanity check for including wolfssl/options.h or
 user_settings.h.


PR stands for Pull Request, and PR <NUMBER> references a GitHub pull request
 number where the code change was added.


## Vulnerabilities
* [Med] An OCSP (non stapling) issue was introduced in wolfSSL version 5.7.4
 when performing OCSP requests for intermediate certificates in a certificate
 chain. This affects only TLS 1.3 connections on the server side. It would not
 impact other TLS protocol versions or connections that are not using the
 traditional OCSP implementation. (Fix in pull request 8115)


## New Feature Additions
* Add support for RP2350 and improve RP2040 support, both with RNG optimizations
 (PR 8153)
* Add support for STM32MP135F, including STM32CubeIDE support and HAL support
 for SHA2/SHA3/AES/RNG/ECC optimizations. (PR 8223, 8231, 8241)
* Implement Renesas TSIP RSA Public Enc/Private support (PR 8122)
* Add support for Fedora/RedHat system-wide crypto-policies (PR 8205)
* Curve25519 generic keyparsing API added with  wc_Curve25519KeyToDer and
 wc_Curve25519KeyDecode (PR 8129)
* CRL improvements and update callback, added the functions
 wolfSSL_CertManagerGetCRLInfo and wolfSSL_CertManagerSetCRLUpdate_Cb (PR 8006)
* For DTLS, add server-side stateless and CID quality-of-life API. (PR 8224)


## Enhancements and Optimizations
* Add a CMake dependency check for pthreads when required. (PR 8162)
* Update OS_Seed declarations for legacy compilers and FIPS modules (boundary
 not affected). (PR 8170)
* Enable WOLFSSL_ALWAYS_KEEP_SNI by default when using --enable-jni. (PR 8283)
* Change the default cipher suite preference, prioritizing
 TLS_AES_256_GCM_SHA384 over TLS_AES_128_GCM_SHA256. (PR 7771)
* Add SRTP-KDF (FIPS module v6.0.0) to checkout script for release bundling
 (PR 8215)
* Make library build when no hardware crypto available for Aarch64 (PR 8293)
* Update assembly code to avoid `uint*_t` types for better compatibility with
 older C standards. (PR 8133)
* Add initial documentation for writing ASN template code to decode BER/DER.
 (PR 8120)
* Perform full reduction in sc_muladd for EdDSA with Curve448 (PR 8276)
* Allow SHA-3 hardware cryptography instructions to be explicitly not used in
 MacOS builds (PR 8282)
* Make Kyber and ML-KEM available individually and together. (PR 8143)
* Update configuration options to include Kyber/ML-KEM and fix defines used in
 wolfSSL_get_curve_name. (PR 8183)
* Make GetShortInt available with WOLFSSL_ASN_EXTRA (PR 8149)
* Improved test coverage and minor improvements of X509 (PR 8176)
* Add sanity checks for configuration methods, ensuring the inclusion of
 wolfssl/options.h or user_settings.h. (PR 8262)
* Enable support for building without TLS (NO_TLS). Provides reduced code size
 option for non-TLS users who want features like the certificate manager or
 compatibility layer. (PR 8273)
* Exposed get_verify functions with OPENSSL_EXTRA. (PR 8258)
* ML-DSA/Dilithium: obtain security level from DER when decoding (PR 8177)
* Implementation for using PKCS11 to retrieve certificate for SSL CTX (PR 8267)
* Add support for the RFC822 Mailbox attribute (PR 8280)
* Initialize variables and adjust types resolve warnings with Visual Studio in
 Windows builds. (PR 8181)
* Refactors and expansion of opensslcoexist build (PR 8132, 8216, 8230)
* Add DTLS 1.3 interoperability, libspdm and DTLS CID interoperability tests
 (PR 8261, 8255, 8245)
* Remove trailing error exit code in wolfSSL install setup script (PR 8189)
* Update Arduino files for wolfssl 5.7.4 (PR 8219)
* Improve Espressif SHA HW/SW mutex messages (PR 8225)
* Apply post-5.7.4 release updates for Espressif Managed Component examples
 (PR 8251)
* Expansion of c89 conformance (PR 8164)
* Added configure option for additional sanity checks with --enable-faultharden
 (PR 8289)
* Aarch64 ASM additions to check CPU features before hardware crypto instruction
 use (PR 8314)


## Fixes
* Fix a memory issue when using the compatibility layer with
 WOLFSSL_GENERAL_NAME and handling registered ID types. (PR 8155)
* Fix a build issue with signature fault hardening when using public key
 callbacks (HAVE_PK_CALLBACKS). (PR 8287)
* Fix for handling heap hint pointer properly when managing multiple WOLFSSL_CTX
 objects and free’ing one of them (PR 8180)
* Fix potential memory leak in error case with Aria. (PR 8268)
* Fix Set_Verify flag behaviour on Ada wrapper. (PR 8256)
* Fix a compilation error with the NO_WOLFSSL_DIR flag. (PR 8294)
* Resolve a corner case for Poly1305 assembly code on Aarch64. (PR 8275)
* Fix incorrect version setting in CSRs. (PR 8136)
* Correct debugging output for cryptodev. (PR 8202)
* Fix for benchmark application use with /dev/crypto GMAC auth error due to size
 of AAD (PR 8210)
* Add missing checks for the initialization of sp_int/mp_int with DSA to free
 memory properly in error cases. (PR 8209)
* Fix return value of wolfSSL_CTX_set_tlsext_use_srtp (8252)
* Check Root CA by Renesas TSIP before adding it to ca-table (PR 8101)
* Prevent adding a certificate to the CA cache for Renesas builds if it does not
 set CA:TRUE in basic constraints. (PR 8060)
* Fix attribute certificate holder entityName parsing. (PR 8166)
* Resolve build issues for configurations without any wolfSSL/openssl
 compatibility layer headers. (PR 8182)
* Fix for building SP RSA small and RSA public only (PR 8235)
* Fix for Renesas RX TSIP RSA Sign/Verify with wolfCrypt only (PR 8206)
* Fix to ensure all files have settings.h included (like wc_lms.c) and guards
 for building all `*.c` files (PR 8257 and PR 8140)
* Fix x86 target build issues in Visual Studio for non-Windows operating
 systems. (PR 8098)
* Fix wolfSSL_X509_STORE_get0_objects to handle no CA (PR 8226)
* Properly handle reference counting when adding to the X509 store. (PR 8233)
* Fix for various typos and improper size used with FreeRTOS_bind in the Renesas
 example. Thanks to Hongbo for the report on example issues. (PR 7537)
* Fix for potential heap use after free with wolfSSL_PEM_read_bio_PrivateKey.
 Thanks to Peter for the issue reported. (PR 8139)


For additional vulnerability information visit the vulnerability page at:
https://www.wolfssl.com/docs/security-vulnerabilities/

See INSTALL file for build instructions.
More info can be found on-line at: https://wolfssl.com/wolfSSL/Docs.html

# Resources

[wolfSSL Website](https://www.wolfssl.com/)

[wolfSSL Wiki](https://github.com/wolfSSL/wolfssl/wiki)

[FIPS 140-2/140-3 FAQ](https://wolfssl.com/license/fips)

[wolfSSL Documentation](https://wolfssl.com/wolfSSL/Docs.html)

[wolfSSL Manual](https://wolfssl.com/wolfSSL/Docs-wolfssl-manual-toc.html)

[wolfSSL API Reference](https://wolfssl.com/wolfSSL/Docs-wolfssl-manual-17-wolfssl-api-reference.html)

[wolfCrypt API Reference](https://wolfssl.com/wolfSSL/Docs-wolfssl-manual-18-wolfcrypt-api-reference.html)

[TLS 1.3](https://www.wolfssl.com/docs/tls13/)

[wolfSSL Vulnerabilities](https://www.wolfssl.com/docs/security-vulnerabilities/)

[Additional wolfSSL Examples](https://github.com/wolfssl/wolfssl-examples)

# Directory structure

```
<wolfssl_root>
├── certs   [Certificates used in tests and examples]
├── cmake   [Cmake build utilities]
├── debian  [Debian packaging files]
├── doc     [Documentation for wolfSSL (Doxygen)]
├── Docker  [Prebuilt Docker environments]
├── examples    [wolfSSL examples]
│   ├── asn1    [ASN.1 printing example]
│   ├── async   [Asynchronous Cryptography example]
│   ├── benchmark   [TLS benchmark example]
│   ├── client  [Client example]
│   ├── configs [Example build configurations]
│   ├── echoclient  [Echoclient example]
│   ├── echoserver  [Echoserver example]
│   ├── pem [Example for convert between PEM and DER]
│   ├── sctp    [Servers and clients that demonstrate wolfSSL's DTLS-SCTP support]
│   └── server  [Server example]
├── IDE     [Contains example projects for various development environments]
├── linuxkm [Linux Kernel Module implementation]
├── m4      [Autotools utilities]
├── mcapi   [wolfSSL MPLAB X Project Files]
├── mplabx  [wolfSSL MPLAB X Project Files]
├── mqx     [wolfSSL Freescale CodeWarrior Project Files]
├── rpm     [RPM packaging metadata]
├── RTOS
│   └── nuttx   [Port of wolfSSL for NuttX]
├── scripts [Testing scripts]
├── src     [wolfSSL source code]
├── sslSniffer  [wolfSSL sniffer can be used to passively sniff SSL traffic]
├── support [Contains the pkg-config file]
├── tests   [Unit and configuration testing]
├── testsuite   [Test application that orchestrates tests]
├── tirtos  [Port of wolfSSL for TI RTOS]
├── wolfcrypt   [The wolfCrypt component]
│   ├── benchmark   [Cryptography benchmarking application]
│   ├── src         [wolfCrypt source code]
│   │   └── port    [Supported hardware acceleration ports]
│   └── test        [Cryptography testing application]
├── wolfssl [Header files]
│   ├── openssl [Compatibility layer headers]
│   └── wolfcrypt   [Header files]
├── wrapper [wolfSSL language wrappers]
└── zephyr  [Port of wolfSSL for Zephyr RTOS]
```
