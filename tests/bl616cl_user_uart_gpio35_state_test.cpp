#include "../UNOR4BL616CLBridge/user_uart_gpio35_control.h"

#include <cstdlib>
#include <iostream>

namespace {

[[noreturn]] void fail(const char *message)
{
    std::cerr << "test failure: " << message << '\n';
    std::exit(1);
}

void expect(bool value, const char *message)
{
    if (!value) {
        fail(message);
    }
}

} // namespace

int main()
{
    using bridge::UserUartGpio35Mode;

    expect(
        static_cast<uint8_t>(UserUartGpio35Mode::kUart0Rx) == 0U,
        "UART RX feature mode value is stable");
    expect(
        static_cast<uint8_t>(UserUartGpio35Mode::kHighImpedanceInput) == 1U,
        "high impedance feature mode value is stable");
    expect(
        static_cast<uint8_t>(UserUartGpio35Mode::kUart0RxNoPullup) == 2U,
        "UART RX without pull-up feature mode value is stable");
    expect(
        static_cast<uint8_t>(UserUartGpio35Mode::kPadDisabled) == 3U,
        "pad-disabled feature mode value is stable");
    expect(
        bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kUart0Rx,
            bridge::kUserUartGpio35UartPadFunction,
            bridge::kUserUartGpio35Uart0RxSignal,
            true,
            false,
            true),
        "UART0 RX state is accepted");
    expect(
        !bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kUart0Rx,
            bridge::kUserUartGpio35UartPadFunction,
            bridge::kUserUartGpio35Uart0RxSignal,
            true,
            true,
            true),
        "GPIO35 output enable is rejected");
    expect(
        bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kHighImpedanceInput,
            bridge::kUserUartGpio35GpioPadFunction,
            0U,
            true,
            false,
            false),
        "high impedance input state is accepted");
    expect(
        !bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kHighImpedanceInput,
            bridge::kUserUartGpio35GpioPadFunction,
            0U,
            false,
            false,
            false),
        "input-disabled diagnostic state is rejected");
    expect(
        bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kUart0RxNoPullup,
            bridge::kUserUartGpio35UartPadFunction,
            bridge::kUserUartGpio35Uart0RxSignal,
            true,
            false,
            false),
        "UART0 RX without pull-up state is accepted");
    expect(
        !bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kUart0RxNoPullup,
            bridge::kUserUartGpio35UartPadFunction,
            bridge::kUserUartGpio35Uart0RxSignal,
            true,
            false,
            true),
        "default UART0 RX no-pull state rejects pull-up");
    expect(
        bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kPadDisabled,
            bridge::kUserUartGpio35GpioPadFunction,
            0U,
            false,
            false,
            false),
        "pad-disabled state is accepted");
    expect(
        !bridge::user_uart_gpio35_state_is_expected(
            UserUartGpio35Mode::kPadDisabled,
            bridge::kUserUartGpio35GpioPadFunction,
            0U,
            true,
            false,
            false),
        "pad-disabled mode rejects input enable");
    return 0;
}
