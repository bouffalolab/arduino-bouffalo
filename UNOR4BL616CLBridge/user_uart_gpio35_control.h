#ifndef UNOR4_BL616CL_USER_UART_GPIO35_CONTROL_H
#define UNOR4_BL616CL_USER_UART_GPIO35_CONTROL_H

#include <stdint.h>

namespace bridge {

enum class UserUartGpio35Mode : uint8_t {
    kUart0Rx = 0U,
    kHighImpedanceInput = 1U,
    kUart0RxNoPullup = 2U,
    kPadDisabled = 3U,
};

constexpr uint8_t kUserUartGpio35UartPadFunction = 7U;
constexpr uint8_t kUserUartGpio35GpioPadFunction = 11U;
constexpr uint8_t kUserUartGpio35Uart0RxSignal = 3U;

constexpr bool user_uart_gpio35_state_is_expected(
    UserUartGpio35Mode mode,
    uint8_t pad_function,
    uint8_t uart_signal,
    bool input_enabled,
    bool output_enabled,
    bool pullup_enabled)
{
    if (mode == UserUartGpio35Mode::kUart0Rx) {
        return
            pad_function == kUserUartGpio35UartPadFunction &&
            uart_signal == kUserUartGpio35Uart0RxSignal &&
            input_enabled &&
            !output_enabled &&
            pullup_enabled;
    }

    if (mode == UserUartGpio35Mode::kHighImpedanceInput) {
        return
            pad_function == kUserUartGpio35GpioPadFunction &&
            input_enabled &&
            !output_enabled &&
            !pullup_enabled;
    }

    if (mode == UserUartGpio35Mode::kUart0RxNoPullup) {
        return
            pad_function == kUserUartGpio35UartPadFunction &&
            uart_signal == kUserUartGpio35Uart0RxSignal &&
            input_enabled &&
            !output_enabled &&
            !pullup_enabled;
    }

    return
        mode == UserUartGpio35Mode::kPadDisabled &&
        pad_function == kUserUartGpio35GpioPadFunction &&
        !input_enabled &&
        !output_enabled &&
        !pullup_enabled;
}

struct UserUartGpio35Status {
    uint32_t pad_config;
    uint32_t uart_signal_config;
    uint32_t sample_count;
    uint32_t anomaly_count;
    uint8_t pad_function;
    uint8_t uart_signal;
    bool input_enabled;
    bool output_enabled;
    bool pullup_enabled;
    bool expected;
    UserUartGpio35Mode mode;
};

class UserUartGpio35Control {
public:
    bool begin();
    void poll();
    bool set_mode(UserUartGpio35Mode mode);
    const UserUartGpio35Status &status() const;

private:
    void sample();

    UserUartGpio35Status status_ = {};
};

} // namespace bridge

#endif
