#define _ZERO_WITH_COMMA_1 0,
#define _second_arg(__ignored, val, ...) val
#define _is_enabled(x)                __is_enabled(x)
#define __is_enabled(val)             ___is_enabled(_ZERO_WITH_COMMA_##val)
#define ___is_enabled(junk_or_comma)  _second_arg(junk_or_comma 1, 0)
#define IS_ENABLED(config)            _is_enabled(config)
#define BL616CL 1
#undef CONFIG_ANTI_ROLLBACK
#define CONFIG_APP_ANTI_ROLLBACK_VER 0
#undef CONFIG_BACKTRACE
#undef CONFIG_BFLB_LOG
#undef CONFIG_BLE_USE_MAC2
#define CONFIG_BLUETOOTH 1
#define CONFIG_BL_WPA_SUPPLICANT 1
#define CONFIG_BTBLECONTROLLER_LIB "m2s1"
#define CONFIG_BTBLE_ENABLE 1
#define CONFIG_CHERRYUSB 1
#define CONFIG_CHERRYUSB_DEVICE 1
#define CONFIG_CHERRYUSB_DEVICE_CDC_ACM 1
#define CONFIG_CHERRYUSB_DEVICE_HID 1
#define CONFIG_CHERRYUSB_OSAL "freertos"
#define CONFIG_COMX "COM5"
#define CONFIG_CONSOLE_UART_BAUDRATE 2000000
#undef CONFIG_COREDUMP
#define CONFIG_DEBUG 1
#define CONFIG_DHCPD 1
#define CONFIG_FREERTOS 1
#undef CONFIG_GCC_COMPILE_LTO
#define CONFIG_GCC_OPTIMISE_LEVEL "-Os"
#define CONFIG_LOG_LEVEL 3
#define CONFIG_LWIP 1
#define CONFIG_MACSW_SELECT "default"
#define CONFIG_MBEDTLS 1
#define CONFIG_MBEDTLS_AES_USE_HW 1
#define CONFIG_MBEDTLS_BIGNUM_USE_HW 1
#define CONFIG_MBEDTLS_ECC_USE_HW 1
#define CONFIG_MBEDTLS_SHA1_USE_HW 1
#define CONFIG_MBEDTLS_SHA256_USE_HW 1
#define CONFIG_MBEDTLS_SHA512_USE_HW 1
#undef CONFIG_MULTIMEDIA_VIDEO
#undef CONFIG_NEWLIB
#undef CONFIG_NEWLIB_STANDARD
#undef CONFIG_PEC_V2
#define CONFIG_PING 1
#define CONFIG_POSIX 1
#undef CONFIG_PSRAM
#define CONFIG_RF 1
#define CONFIG_ROMAPI 1
#undef CONFIG_SCANF_FLOAT
#undef CONFIG_SCANF_FLOAT_EX
#define CONFIG_SHELL 1
#undef CONFIG_STRICT
#define CONFIG_TLSF 1
#define CONFIG_VSNPRINTF_FLOAT 1
#define CONFIG_VSNPRINTF_FLOAT_EX 1
#define CONFIG_VSNPRINTF_LONG_LONG 1
#undef CONFIG_VSNPRINTF_NANO
#define CONFIG_WIFI6 1
#define CONFIG_WL80211 1

#ifndef CONFIG_MACSW_SELECT_INCLUDE
#define CONFIG_MACSW_SELECT_INCLUDE "macsw_default_config.h"
#endif

/* The SDK runtime libraries are built with the FreeRTOS POSIX shim, but the
 * Arduino sketch build uses newlib/libstdc++ thread support.  Undefine the
 * SDK POSIX switch in the sketch compile context so the C++ gthreads headers
 * see the newlib pthread types (see sdk/libc/sys/types.h). */
#ifdef CONFIG_POSIX
#undef CONFIG_POSIX
#endif

#undef CONFIG_XTAL_POWER_TYPE_ACTIVE
