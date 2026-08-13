#define _ZERO_WITH_COMMA_1 0,
#define _second_arg(__ignored, val, ...) val
#define _is_enabled(x)                __is_enabled(x)
#define __is_enabled(val)             ___is_enabled(_ZERO_WITH_COMMA_##val)
#define ___is_enabled(junk_or_comma)  _second_arg(junk_or_comma 1, 0)
#define IS_ENABLED(config)            _is_enabled(config)
#define BL616CL 1
#undef CONFIG_BACKTRACE
#undef CONFIG_BFLB_LOG
#undef CONFIG_BLE_USE_MAC2
#define CONFIG_BLUETOOTH 1
#define CONFIG_BTBLECONTROLLER_LIB "m2s1"
#define CONFIG_BTBLE_ENABLE 1
#define CONFIG_CHERRYUSB 1
#define CONFIG_CHERRYUSB_DEVICE 1
#define CONFIG_CHERRYUSB_DEVICE_CDC_ACM 1
#define CONFIG_CHERRYUSB_DEVICE_HID 1
#define CONFIG_CHERRYUSB_OSAL "freertos"
#define CONFIG_CONSOLE_UART_BAUDRATE 2000000
#undef CONFIG_COREDUMP
#define CONFIG_DEBUG 1
#define CONFIG_FREERTOS 1
#undef CONFIG_GCC_COMPILE_LTO
#define CONFIG_LOG_LEVEL 3
#undef CONFIG_MULTIMEDIA_VIDEO
#undef CONFIG_NEWLIB
#undef CONFIG_NEWLIB_STANDARD
#undef CONFIG_PEC_V2
#undef CONFIG_PSRAM
#define CONFIG_RF 1
#define CONFIG_ROMAPI 1
#undef CONFIG_SCANF_FLOAT
#undef CONFIG_SCANF_FLOAT_EX
#define CONFIG_SHELL 1
#define CONFIG_TLSF 1
#undef CONFIG_VSNPRINTF_FLOAT
#undef CONFIG_VSNPRINTF_FLOAT_EX
#define CONFIG_VSNPRINTF_LONG_LONG 1
#undef CONFIG_VSNPRINTF_NANO
