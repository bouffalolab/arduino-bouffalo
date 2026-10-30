#include "user_uart_gpio35_control.h"

#include "Arduino.h"

extern "C" {
#include "bflb_gpio.h"
#include "hardware/gpio_reg.h"
}

namespace bridge {
namespace {

constexpr uint8_t kUserUartRxPin = PIN_SERIAL_RX;
constexpr uint8_t kUart0RxSignal = GPIO_UART_FUNC_UART0_RX;
constexpr uint32_t kUartCfg2Offset = 0x158U;
constexpr uint32_t kPadOutputEnableBit = 1U << 6U;
constexpr uint32_t kPadFunctionMask = 0x1fU << 8U;
constexpr uint8_t kPadFunctionShift = 8U;

uint32_t read_register(uint32_t address)
{
    return *(volatile uint32_t *)(uintptr_t)address;
}

struct bflb_device_s *gpio_device()
{
    return bflb_device_get_by_name("gpio");
}

bool configure_uart0_rx()
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        return false;
    }

    /*
     * This is the SDK UART RX setup: alternate UART function, input enabled,
     * and no GPIO output-enable request. It does not transmit a test level.
     */
    bflb_gpio_uart_init(gpio, kUserUartRxPin, kUart0RxSignal);
    return true;
}

bool configure_high_impedance_input()
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        return false;
    }

    /*
     * This intentionally detaches UART0 RX and removes the SDK pull-up, but
     * retains the digital input buffer. It is distinct from pad-disabled.
     */
    bflb_gpio_deinit(gpio, kUserUartRxPin);
    return true;
}

bool configure_uart0_rx_no_pullup()
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        return false;
    }

    /*
     * Keep the UART signal selector, input buffer, and Schmitt trigger, but
     * remove the SDK's fixed pull-up. This isolates pull strength as a
     * variable when the RX wire is driven through the UNO R4 TXB0108.
     */
    bflb_gpio_uart_init(gpio, kUserUartRxPin, kUart0RxSignal);
    bflb_gpio_init(
        gpio,
        kUserUartRxPin,
        (7U << GPIO_FUNC_SHIFT) | GPIO_ALTERNATE | GPIO_SMT_EN | GPIO_DRV_1);
    return true;
}

bool configure_pad_disabled()
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        return false;
    }

    /*
     * GPIO function with neither IE nor OE disables the BL616L digital input
     * buffer and output driver. UART signal selection is intentionally left
     * untouched because the pad no longer connects it to the external line.
     */
    bflb_gpio_init(gpio, kUserUartRxPin, GPIO_FLOAT);
    return true;
}

} // namespace

bool UserUartGpio35Control::begin()
{
    /*
     * GPIO35 receives RA4M1 TX through the UNO R4 TXB0108. Use the SDK's
     * normal UART RX setup, including its default internal pull-up, for the
     * improved short-wire relay topology.
     */
    const bool configured = configure_uart0_rx();
    status_.mode = UserUartGpio35Mode::kUart0Rx;
    sample();
    return configured && status_.expected;
}

void UserUartGpio35Control::poll()
{
    sample();
}

bool UserUartGpio35Control::set_mode(UserUartGpio35Mode mode)
{
    bool configured = false;
    switch (mode) {
    case UserUartGpio35Mode::kUart0Rx:
        configured = configure_uart0_rx();
        break;
    case UserUartGpio35Mode::kHighImpedanceInput:
        configured = configure_high_impedance_input();
        break;
    case UserUartGpio35Mode::kUart0RxNoPullup:
        configured = configure_uart0_rx_no_pullup();
        break;
    case UserUartGpio35Mode::kPadDisabled:
        configured = configure_pad_disabled();
        break;
    default:
        return false;
    }

    status_.mode = mode;
    sample();
    return configured && status_.expected;
}

const UserUartGpio35Status &UserUartGpio35Control::status() const
{
    return status_;
}

void UserUartGpio35Control::sample()
{
    struct bflb_device_s *gpio = gpio_device();
    if (gpio == nullptr) {
        status_.expected = false;
        ++status_.sample_count;
        ++status_.anomaly_count;
        return;
    }

    const uint32_t pad_config =
        read_register(gpio->reg_base + GLB_GPIO_CFG0_OFFSET + (kUserUartRxPin << 2U));
    const uint32_t uart_signal_config =
        read_register(gpio->reg_base + kUartCfg2Offset);

    status_.pad_config = pad_config;
    status_.uart_signal_config = uart_signal_config;
    /*
     * The generated BL616CL header puts GPIO35 OE/function macros under
     * #if 0, while the SDK GPIO driver configures this pad with the common
     * layout. Read the documented common bit positions explicitly.
     */
    status_.pad_function = static_cast<uint8_t>(
        (pad_config & kPadFunctionMask) >> kPadFunctionShift);
    status_.uart_signal = static_cast<uint8_t>((uart_signal_config >> 12U) & 0x0fU);
    status_.input_enabled = (pad_config & GLB_REG_GPIO_35_IE) != 0U;
    status_.output_enabled = (pad_config & kPadOutputEnableBit) != 0U;
    status_.pullup_enabled = (pad_config & GLB_REG_GPIO_0_PU) != 0U;

    status_.expected = user_uart_gpio35_state_is_expected(
        status_.mode,
        status_.pad_function,
        status_.uart_signal,
        status_.input_enabled,
        status_.output_enabled,
        status_.pullup_enabled);

    ++status_.sample_count;
    if (!status_.expected) {
        ++status_.anomaly_count;
    }
}

} // namespace bridge
