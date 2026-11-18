# BLTimer (experimental)

`BLTimer` runs a callback periodically or once from one of the two BL616CL
general-purpose hardware timers, TIMER0 and TIMER1. Callbacks do not depend on
`loop()` polling. ArduinoCore-API has no hardware-timer interface, so this is a
platform library: only sketches that include `BLTimer.h` compile and link it.
It requires the matching `bl616cl-libs` 0.12.0 package.

```cpp
#include <BLTimer.h>

BLTimer timer;
volatile uint32_t ticks;

void onTick() { ticks++; }

void setup() {
  if (!timer.begin(onTick, 1000)) {  // every 1 ms
    // no free timer or invalid request
  }
}
```

`examples/TimerBlink` blinks `LED_BUILTIN` from a periodic timer and reports a
one-shot timer from `loop()`.

## Interface

| Method | Behavior |
| --- | --- |
| `begin(callback, period_us)` / `begin(callback, arg, period_us)` | Claim a free timer (TIMER0 first, then TIMER1), or reuse the one this object holds, and call the callback every `period_us`. The first callback comes one full period after the call. |
| `beginOnce(callback, delay_us)` / `beginOnce(callback, arg, delay_us)` | Same, but call the callback once. The object keeps the timer until `end()`. |
| `setPeriod(period_us)` | Change the period or delay. A running timer restarts a full period from the call; a stopped timer stays stopped. |
| `start()` | Restart counting from zero; re-arms a one-shot timer that has fired. |
| `stop()` | Stop counting and callbacks, keeping the timer and configuration. |
| `end()` | Stop and release the timer. Also called by the destructor. |
| `running()` | Whether a callback is pending; false after `stop()`, `end()` or a one-shot callback. |
| `timer()` | Held timer, 0 or 1, or -1 when none is held. |

Every method that can fail returns `false` and leaves the object unchanged.
Failures:
- both timers are already held by other objects;
- a period below `BLTimer::MIN_PERIOD_US`, or a null callback;
- `setPeriod()`, `start()` or `stop()` on an object that holds no timer;
- any call from a callback or another interrupt (`end()` does nothing there).

Periods are never rounded or truncated to make an invalid request succeed.

## Range, resolution and timing

- **Clock and resolution:** the timers count the 40 MHz crystal divided to
  1 MHz, so the resolution is 1 µs.
- **Range:** periods and delays run from 10 µs (`MIN_PERIOD_US`) to
  4,294,967,295 µs (`MAX_PERIOD_US`, about 71.6 minutes).
- **Clock check:** `begin()` fails if the crystal clock is not a whole number of
  MHz between 1 and 256 MHz.
- **Period accuracy:** each period is exactly the requested number of ticks.
  `micros()` uses the same crystal, so periods are exact relative to
  `micros()`. The crystal's own tolerance was not measured.
- **Latency:** the delay from the timer match to callback entry was at most
  5 µs without other traffic, and at most 9 µs with continuous 115200-baud
  `Serial` traffic (Ai-M64P-32S-Kit; Wi-Fi off; no flash writes). The first
  callback after `begin()` can take a few microseconds longer, because its code
  is fetched from flash.
- **Missed matches:** callbacks are not queued. If other interrupts delay a
  callback past the next match, the two matches produce one callback. In the
  tests, 100 µs periods lost none of about 22,000 callbacks during `Serial`
  traffic. 10 µs periods lost 1 to 4 of about 221,000.
- **When every callback counts:** use periods of at least 100 µs. Avoid flash
  writes (LittleFS, Preferences, OTA) and heavy Wi-Fi activity; their effect
  was not measured.
- **CPU cost:** with the acceptance callback, a 10 µs period took about 15% of
  the CPU and a 100 µs period about 1.5%.

## Callback rules

Callbacks run in interrupt context:
- keep them short;
- do not block, call `delay()`, print with `Serial`, allocate memory, or call
  `BLTimer` methods;
- set `volatile` variables, or use FreeRTOS `...FromISR` functions, to hand
  work to a task.

Call `BLTimer` methods from task context and from one task at a time, for
example only from `setup()` and `loop()`. Objects cannot be copied.

## Resources

- **TIMER0/TIMER1:** used only by `BLTimer`. A third object fails while two
  timers are held.
- **PWM:** a separate peripheral; it is not affected.
- **Watchdog:**
  - It shares the timers' clock gate and two configuration registers.
  - `BLTimer` never disables the shared clock gate and changes only the
    TIMER0/TIMER1 fields of those registers, from task context.
  - A future watchdog library must follow the same rule.
- **`tone()`:** planned on PWM, not on these timers. Any future platform feature
  that needs TIMER0/TIMER1 must claim it through `BLTimer`, and fails when both
  are held.
- **Not offered:** pin output, TIMER0 GPIO capture and timer DMA requests.

## Verification

Hardware acceptance ran on an Ai-M64P-32S-Kit with `tests/hardware/TimerAcceptance` and
`tests/hardware/timer_acceptance.py`. Covered:
- periods of 10 µs, 37 µs, 100 µs, 1 ms, 12.345 ms and 1 s, checked for count,
  drift, lost or duplicate callbacks, and latency;
- one-shot delays;
- stop, start and `setPeriod()`, including whether the first period is full;
- release and re-acquire;
- two timers at once, and a third claim failing;
- rejected arguments, and calls from inside a callback;
- `delay()`, a FreeRTOS task and `Serial` while a timer runs at 100 µs and
  10 µs.

Not measured:
- the period on a pin with a logic analyzer or oscilloscope (BLTimer has no pin
  output);
- crystal ppm;
- the UNO R4 WiFi board, which uses the same code.
