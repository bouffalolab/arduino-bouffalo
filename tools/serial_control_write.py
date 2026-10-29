"""Send one test-control command without logging its private payload.

Keep pySerial's existing write deadline and never resend a partial/failed
command. Poll the host output queue instead of unbounded POSIX tcdrain().
A drained host queue is not evidence of device or RA4M1 acceptance.
"""
import math
import time

import serial


def write_control(link, payload, *, phase, drain_timeout=1.0):
    if not math.isfinite(drain_timeout) or drain_timeout <= 0:
        raise ValueError('drain timeout must be finite and positive')
    started = time.monotonic()
    print(f'SERIAL_CONTROL_WRITE_BEGIN={phase},bytes:{len(payload)}', flush=True)
    try:
        written = link.write(payload)
        if written != len(payload):
            raise serial.SerialTimeoutException('control command short write')
    except Exception as error:
        print(f'SERIAL_CONTROL_WRITE_FAILED={phase},elapsed_ms:{int((time.monotonic()-started)*1000)},type:{type(error).__name__}', flush=True)
        # POSIX pySerial can time out waiting for writability after os.write
        # already accepted bytes. Failure is not evidence of zero delivery.
        print(f'SERIAL_CONTROL_DELIVERY_UNKNOWN={phase}', flush=True)
        raise
    print(f'SERIAL_CONTROL_WRITE_PASS={phase},elapsed_ms:{int((time.monotonic()-started)*1000)}', flush=True)
    started = time.monotonic()
    print(f'SERIAL_CONTROL_DRAIN_BEGIN={phase}', flush=True)
    try:
        while True:
            pending = link.out_waiting
            if time.monotonic() - started >= drain_timeout:
                raise serial.SerialTimeoutException('control host output queue did not drain')
            if pending == 0:
                break
            time.sleep(min(0.01, max(0, drain_timeout-(time.monotonic()-started))))
    except Exception as error:
        print(f'SERIAL_CONTROL_DRAIN_FAILED={phase},elapsed_ms:{int((time.monotonic()-started)*1000)},type:{type(error).__name__}', flush=True)
        raise
    print(f'SERIAL_CONTROL_DRAIN_PASS={phase},elapsed_ms:{int((time.monotonic()-started)*1000)}', flush=True)
