"""Bound host cleanup when a USB/IP tty close is stuck in the kernel.

The daemon owns the old Serial object until close returns. Callers must discard
that object after this function, including on timeout; never reuse its fd or
retry close directly. A timeout remains a gate failure. This does not cancel a
kernel syscall or promise that the USB transport itself has recovered.
"""
import math
import threading
import time


class SerialCloseTimeout(RuntimeError):
    pass


_start_lock = threading.Lock()


def close_serial_bounded(link, *, phase, timeout=2.0):
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('serial close timeout must be finite and positive')
    started = time.monotonic()
    # Use __dict__, not getattr: some test doubles synthesize arbitrary attrs.
    # Retain the job on the old object so an error path cannot start a second
    # close while the first syscall is pending (and fd numbers may be reused).
    with _start_lock:
        state = vars(link).get('_bridge_close_state')
        if state is None:
            state = dict(done=threading.Event(), error=None)
            vars(link)['_bridge_close_state'] = state

            def close_owned():
                try:
                    link.close()
                except BaseException as error:
                    state['error'] = error
                    # pySerial inherits IOBase: its destructor may call close
                    # again after a failed os.close. That fd number may already
                    # have been released by Linux (e.g. EINTR). Seal the old
                    # object against destructor retry; the caller discards it.
                    if 'is_open' in vars(link):
                        vars(link)['is_open'] = False
                finally:
                    state['done'].set()

            worker = threading.Thread(target=close_owned, name='bridge-serial-close', daemon=True)
            print(f'SERIAL_CLOSE_BEGIN={phase}', flush=True)
            worker.start()
    completed = state['done'].wait(timeout)
    elapsed = int((time.monotonic() - started) * 1000)
    if not completed:
        print(f'SERIAL_CLOSE_TIMEOUT={phase},elapsed_ms:{elapsed}', flush=True)
        raise SerialCloseTimeout(f'serial close exceeded {timeout:g}s during {phase}')
    if state['error'] is not None:
        print(f'SERIAL_CLOSE_FAILED={phase},elapsed_ms:{elapsed}', flush=True)
        raise RuntimeError(f'serial close failed during {phase}') from state['error']
    print(f'SERIAL_CLOSE_PASS={phase},elapsed_ms:{elapsed}', flush=True)
