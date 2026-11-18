#!/usr/bin/env python3
"""Run the analogWrite hardware acceptance on Ai-M64P-32S-Kit.

Upload tests/hardware/PwmAcceptance first. The board needs its GPIO8-GPIO22, GPIO14-GPIO15,
GPIO18-GPIO19 and GPIO20-GPIO23 jumpers. The CH340 port is opened once; opening it resets the
BL616CL, so the whole run happens in this process. The sketch samples each PWM output on its
jumpered partner and reports edge timing in CPU cycles; this script judges frequency (1 kHz),
duty against round(value * 40000 / (2^bits - 1)) / 40000, constant levels at 0 and full scale,
the GPIO % 4 channel map (GPADC sampling on GPIO0-7/12/13), simultaneous channels, conflicts,
invalid pins, the return to digital use and coexistence with BLTimer. No instrument is attached,
so edge shape and an independent frequency reference are outside this run. Each check is
"acceptance" (counts toward the exit status) or "exploratory" (recorded only). The log keeps every
line exchanged with the board; --json writes the judged results.

Usage: pwm_acceptance.py --port /dev/ttyUSB0 --log run.log [--json run.json]
"""
import argparse
import json
import sys
import time

import serial

CONTROL_BAUD = 115200
PERIOD_COUNTS = 40000
FREQ_TOL = 1e-4  # Mean period within 0.01 % of 1 kHz against the CPU cycle counter.
JITTER_CYCLES = 64  # Each period within 0.2 us of nominal: about three polling intervals.
DUTY_TOL_PPM = 100  # Mean duty within 0.01 % of full scale (one 8-bit step is 3922 ppm).
MIN_PERIODS = 20  # Rising edges expected in a 25 ms window.
ADC_DUTY = [26, 77, 153, 217]  # Channel duties used by ADCMAP, 8-bit values.
ADC_TOL = 0.03
JUMPERS = [(23, 20), (20, 23), (8, 22), (22, 8), (14, 15), (15, 14), (18, 19), (19, 18)]


def fields(line):
    """Return the key=value fields of a RESULT line as integers."""
    return {k: int(v, 0) for k, v in (kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)}


class Board:
    """Line-oriented access to the sketch over the CH340 port, with a raw transcript."""

    def __init__(self, port, log):
        self.log = log
        self.ser = serial.Serial(port=None, baudrate=CONTROL_BAUD, timeout=0.05)
        self.ser.dtr = True  # Run mode on this board: DTR drives BOOT, RTS drives EN.
        self.ser.rts = False
        self.ser.port = port
        self.ser.open()
        self.pending = b""

    def note(self, text):
        self.log.write("%.3f %s\n" % (time.time(), text))
        self.log.flush()

    def wait(self, prefix, timeout):
        end = time.time() + timeout
        while time.time() < end:
            self.pending += self.ser.read(4096)
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip("\r")
                self.note("< " + text)
                if text.startswith(prefix):
                    return text
        raise TimeoutError("no %r within %.1fs" % (prefix, timeout))

    def command(self, text, timeout=30.0):
        """Send one command; return its RESULT lines as (tag, fields) in order."""
        self.note("> " + text)
        self.ser.write((text + "\n").encode())
        results = []
        while True:
            line = self.wait("", timeout)
            if line == "DONE":
                return results
            if line.startswith("RESULT "):
                results.append((line.split()[1], fields(line)))


class Report:
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail, kind="acceptance"):
        self.items.append({"name": name, "kind": kind, "pass": bool(ok), "detail": detail})
        print("%-4s %-11s %s  %s" % ("PASS" if ok else "FAIL", kind, name, detail))

    def failed(self):
        return [i for i in self.items if i["kind"] == "acceptance" and not i["pass"]]


def expected_counts(bits, value):
    full = (1 << bits) - 1
    level = min(max(value, 0), full)
    return (level * PERIOD_COUNTS + full // 2) // full


def judge_wave(w, counts):
    """Return (ok, detail) for a captured wave against the expected high count per period."""
    if counts == 0 or counts == PERIOD_COUNTS:
        level = 1 if counts else 0
        ok = w["rises"] == 0 and w["falls"] == 0 and w["lvl0"] == level and w["lvl1"] == level
        return ok, "constant %s: rises=%d falls=%d lvl0=%d lvl1=%d" % (
            "high" if level else "low", w["rises"], w["falls"], w["lvl0"], w["lvl1"])
    nominal = w["cpu"] // 1000
    per = w["per"]
    duty = w["hi"] * 1e6 / per if per else 0.0
    want = counts * 1e6 / PERIOD_COUNTS
    ok = (w["rises"] >= MIN_PERIODS and abs(per - nominal) <= nominal * FREQ_TOL
          and w["per_min"] >= nominal - JITTER_CYCLES and w["per_max"] <= nominal + JITTER_CYCLES
          and abs(duty - want) <= DUTY_TOL_PPM)
    return ok, "freq=%.4f Hz duty=%.1f ppm want=%.1f ppm per=[%d,%d] hi=[%d,%d] rises=%d" % (
        w["cpu"] / per if per else 0.0, duty, want, w["per_min"], w["per_max"], w["hi_min"], w["hi_max"],
        w["rises"])


def by_tag(results, tag, pin=None):
    for t, f in results:
        if t == tag and (pin is None or f.get("in") == pin):
            return f
    raise KeyError("%s in=%s missing" % (tag, pin))


def check_wave(rep, name, w, counts):
    ok, d = judge_wave(w, counts)
    rep.add(name, ok, d)


def main():
    ap = argparse.ArgumentParser(description="Run the analogWrite hardware acceptance on Ai-M64P-32S-Kit.")
    ap.add_argument("--port", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    rep = Report()
    with open(args.log, "a") as log:
        board = Board(args.port, log)
        board.wait("READY", 15)

        info = by_tag(board.command("INFO"), "INFO")
        has = info["has_lo"] | (info["has_hi"] << 32)
        rep.add("variant declares GPIO0-31 as PWM pins, not GPIO32-36 or pins 37-40",
                has == (1 << 32) - 1, "has=0x%x" % has)
        rep.add("clocks: CPU 320 MHz, XCLK 40 MHz", info["cpu"] == 320000000 and info["xclk"] == 40000000,
                "cpu=%d xclk=%d" % (info["cpu"], info["xclk"]), "exploratory")

        r = board.command("BOOT")
        cfg = by_tag(r, "BOOTCFG")
        rep.add("first analogWrite: single-ended routing, 1000 Hz, GPIO23 in PWM function",
                cfg["io_sel"] == 0 and cfg["freq"] == 1000 and cfg["func23"] == 16,
                "io_sel=%d freq=%d func23=%d" % (cfg["io_sel"], cfg["freq"], cfg["func23"]))
        check_wave(rep, "default resolution is 8 bits: analogWrite(23, 128) after reset", by_tag(r, "BOOT", 20),
                   expected_counts(8, 128))

        for _, f in board.command("SHORTS"):
            rep.add("jumper GPIO%d-GPIO%d passes both levels both ways" % (f["a"], f["b"]), f["ok"] == 4,
                    "ok=%d/4" % f["ok"])

        for out, inp in JUMPERS:
            for v in (0, 64, 128, 191, 255):
                w = by_tag(board.command("DUTY %d %d 8 %d" % (out, inp, v)), "DUTY", inp)
                check_wave(rep, "GPIO%d (CH%d) value %d/255 seen on GPIO%d" % (out, out % 4, v, inp), w,
                           expected_counts(8, v))

        for bits, v in ((10, 256), (10, 512), (10, 768), (12, 1024), (12, 2048), (12, 3072), (16, 16384),
                        (16, 32768), (16, 49152), (31, 1 << 29), (31, 1 << 30), (31, (1 << 31) - 1), (1, 0),
                        (1, 1), (8, 300), (8, -5)):
            w = by_tag(board.command("DUTY 23 20 %d %d" % (bits, v)), "DUTY", 20)
            check_wave(rep, "resolution %d bits, value %d" % (bits, v), w, expected_counts(bits, v))
        for bad in (0, 32, -1, 33):
            w = by_tag(board.command("RESINV %d" % bad), "RESINV", 20)
            check_wave(rep, "analogWriteResolution(%d) is ignored (12 bits kept)" % bad, w,
                       expected_counts(12, 2048))

        for p in (0, 1, 2, 3, 4, 5, 6, 7, 12, 13):
            f = by_tag(board.command("ADCMAP %d" % p), "ADCMAP")
            frac = f["high"] / f["n"]
            want = [d / 255 for d in ADC_DUTY]
            nearest = min(range(4), key=lambda c: abs(want[c] - frac))
            rep.add("GPIO%d drives channel %d (GPADC sees %.1f %%, want %.1f %%)" % (p, p % 4, frac * 100,
                    want[p % 4] * 100), nearest == p % 4 and abs(frac - want[p % 4]) <= ADC_TOL
                    and f["func"] == 16, "high=%d/%d func=%d" % (f["high"], f["n"], f["func"]))

        board.command("RELEASE")
        rounds = ((64, 191, 128), (191, 191, 128), (191, 64, 128))
        for k, (a, b, c) in enumerate(rounds):
            r = board.command("MULTI %d %d %d" % (a, b, c))
            waves = {pin: by_tag(r, "MULTI", pin) for pin in (20, 22, 15)}
            for pin, out, v in ((20, 23, a), (22, 8, b), (15, 14, c)):
                check_wave(rep, "round %d: GPIO%d (CH%d) at %d with four channels running" % (k + 1, out, out % 4, v),
                           waves[pin], expected_counts(8, v))
            pers = [waves[p]["per"] for p in waves]
            rep.add("round %d: all channels share one period" % (k + 1), max(pers) - min(pers) <= 16,
                    "per=%s" % pers)
            rises = [waves[p]["first_rise"] for p in waves]
            rep.add("round %d: rising edges aligned within 0.2 us" % (k + 1), max(rises) - min(rises) <= 64,
                    "first_rise=%s" % rises, "exploratory")
        board.command("RELEASE")

        r = board.command("CONFLICT")
        cfg = by_tag(r, "CONFLICTCFG")
        rep.add("same-channel request (GPIO20 while GPIO8 holds CH0) leaves GPIO20 a GPIO",
                cfg["same"] == 1 and cfg["func20"] == 11, "same=%d func20=%d" % (cfg["same"], cfg["func20"]))
        check_wave(rep, "refused GPIO20 keeps its digital LOW (seen on GPIO23)", by_tag(r, "CONFLICT_REFUSED", 23), 0)
        check_wave(rep, "channel owner GPIO8 keeps its duty during the refusal", by_tag(r, "CONFLICT_REFUSED", 22),
                   expected_counts(8, 128))
        check_wave(rep, "refused GPIO20 still follows digitalWrite(HIGH)", by_tag(r, "CONFLICT_DIGITAL", 23),
                   PERIOD_COUNTS)
        check_wave(rep, "GPIO20 gets CH0 after pinMode(8, INPUT)", by_tag(r, "CONFLICT_FREED", 23),
                   expected_counts(8, 64))

        r = board.command("INVALID")
        f = by_tag(r, "INVALID")
        rep.add("requests on GPIO32-36 and pins 37/40/100/255 change no pin or PWM register",
                f["checked"] == 9 and f["changed"] == 0, "checked=%d changed=0x%x" % (f["checked"], f["changed"]))
        check_wave(rep, "running GPIO23 unaffected by invalid requests", by_tag(r, "INVALID_RUNNING", 20),
                   expected_counts(8, 64))

        r = board.command("BACK")
        f = by_tag(r, "BACKCFG_DW")
        rep.add("digitalWrite(HIGH) on a PWM pin makes it a GPIO output", f["func"] == 11 and f["oe"] == 1,
                "func=%d oe=%d" % (f["func"], f["oe"]))
        check_wave(rep, "after digitalWrite(HIGH): constant high, no residual PWM", by_tag(r, "BACK_DW_HIGH", 20),
                   PERIOD_COUNTS)
        check_wave(rep, "after digitalWrite(LOW): constant low", by_tag(r, "BACK_DW_LOW", 20), 0)
        f = by_tag(r, "BACKCFG_PINMODE")
        rep.add("pinMode(INPUT) on a PWM pin makes it a GPIO input", f["func"] == 11 and f["oe"] == 0,
                "func=%d oe=%d" % (f["func"], f["oe"]))
        f = by_tag(r, "BACKINPUT")
        rep.add("released pin follows its partner's drive (no output left)", f["follow_hi"] == 1 and f["follow_lo"] == 0,
                "follow_hi=%d follow_lo=%d" % (f["follow_hi"], f["follow_lo"]))
        check_wave(rep, "CH3 free again after pinMode(23): GPIO19 outputs", by_tag(r, "BACK_CH3_FREE", 18),
                   expected_counts(8, 128))
        f = by_tag(r, "BACKADC")
        rep.add("analogRead on a PWM pin switches it to analog input", f["value"] >= 0 and f["func4"] == 10,
                "value=%d func4=%d" % (f["value"], f["func4"]))
        check_wave(rep, "CH0 free again after analogRead(A4): GPIO8 outputs", by_tag(r, "BACK_CH0_FREE", 22),
                   expected_counts(8, 64))

        r = board.command("TIMER")
        for pin, out, v in ((20, 23, 64), (22, 8, 191), (15, 14, 128)):
            check_wave(rep, "with BLTimer at 100 us: GPIO%d at %d" % (out, v), by_tag(r, "TIMER_PWM", pin),
                       expected_counts(8, v))
        f = by_tag(r, "TIMER")
        want = f["us"] / 100
        rep.add("BLTimer keeps its 100 us rate while PWM runs", f["began"] == 1 and f["idx_free"] == 1
                and abs(f["ticks"] - want) <= max(2, want * 0.01), "ticks=%d want=%.1f" % (f["ticks"], want))

    failed = rep.failed()
    n_acc = sum(1 for i in rep.items if i["kind"] == "acceptance")
    print("acceptance: %d/%d passed" % (n_acc - len(failed), n_acc))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"items": rep.items, "failed": len(failed), "acceptance": n_acc}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
