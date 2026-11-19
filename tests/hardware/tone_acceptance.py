#!/usr/bin/env python3
"""Run the tone()/noTone() hardware acceptance on Ai-M64P-32S-Kit.

Upload tests/hardware/ToneAcceptance first. The board needs its GPIO8-GPIO22, GPIO14-GPIO15,
GPIO18-GPIO19 and GPIO20-GPIO23 jumpers. The CH340 port is opened once; opening it resets the
BL616CL, so the whole run happens in this process. The sketch samples each tone on its jumpered
partner, with interrupts on, and reports edge timing in CPU cycles. This script judges:
- frequency against xclk / (2 * round(xclk / (2 * f))) from 1 Hz to 20 kHz, and the 50 % duty;
- timed tones ending after the nearest whole number of half periods;
- noTone(), one tone at a time, frequency changes on the sounding pin;
- both timers held by BLTimer, invalid pins and frequencies;
- analogWrite(), BLTimer, digitalWrite(), pinMode() and analogRead() around a tone;
- the official toneMelody sequence.
Every edge comes from the timer interrupt and may be up to EDGE_US late; the sampling poll is also
delayed by other interrupts. No instrument is attached, so edge shape and an independent frequency
reference are outside this run. Each check is "acceptance" (counts toward the exit status) or
"exploratory" (recorded only). The log keeps every line exchanged with the board; --json writes
the judged results.

Usage: tone_acceptance.py --port /dev/ttyUSB0 --log run.log [--json run.json]
"""
import argparse
import json
import sys
import time

import serial

CONTROL_BAUD = 115200
CPU = 320000000  # CPU cycle counter rate; replaced by the board's INFO report.
XCLK = 40000000  # Timer clock; replaced by the board's INFO report.
EDGE_US = 12  # Declared bound on how late an edge can be; the first edge after a start is the latest.
DUTY_US = 0.5  # Mean high time within half a period plus this, beyond the edge bound spread over n.
CALL_US = 100  # tone() returns within this time.
SAMPLE_US = 2  # An edge made inside a call is seen within this time after the call returns.
PWM_US = 2  # Sampling delay allowed on hardware PWM edges, which interrupts can hold up.
FREQ_POINTS = [  # out, in, hz, capture ms, capture ms after noTone()
    (18, 19, 1, 4200, 1600), (19, 18, 31, 400, 60), (8, 22, 262, 100, 20), (22, 8, 440, 100, 20),
    (14, 15, 1000, 50, 20), (15, 14, 4186, 25, 20), (20, 23, 10000, 25, 20), (23, 20, 19999, 25, 20),
    (8, 22, 20000, 25, 20)]
DUR_POINTS = [  # out, in, hz, duration ms, capture ms
    (8, 22, 440, 100, 150), (22, 8, 20000, 10, 20), (14, 15, 1000, 1, 5), (18, 19, 31, 250, 320),
    (20, 23, 1, 1500, 1700), (23, 20, 1, 1, 700)]
CHANGES = [  # first hz, wait us, new hz, new duration ms, capture ms
    (100, 2000, 1000, 0, 30), (100, 1000, 400, 0, 30), (1000, 200, 100, 0, 40), (500, 300, 500, 20, 40)]
MELODY = [(262, 4), (196, 8), (196, 8), (220, 4), (196, 4), (0, 4), (247, 4), (262, 4)]  # toneMelody


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
        """Send one command; return its RESULT lines as {tag: fields}, wave lines keyed (tag, in).
        Lines without an input pin also collect in order under (tag, "all")."""
        self.note("> " + text)
        self.ser.write((text + "\n").encode())
        results = {}
        while True:
            line = self.wait("", timeout)
            if line == "DONE":
                return results
            if line.startswith("RESULT "):
                tag, f = line.split()[1], fields(line)
                base = tag[:-2] if tag.endswith(".t") else tag
                key = (base, f["in"]) if "in" in f else base
                results.setdefault(key, {}).update(f)
                if "in" not in f:
                    results.setdefault((base, "all"), []).append(f)


class Report:
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail, kind="acceptance"):
        self.items.append({"name": name, "kind": kind, "pass": bool(ok), "detail": detail})
        print("%-4s %-11s %s  %s" % ("PASS" if ok else "FAIL", kind, name, detail))

    def failed(self):
        return [i for i in self.items if i["kind"] == "acceptance" and not i["pass"]]


def cyc(us):
    return us * CPU / 1e6


def half_counts(hz):
    """Timer counts per half period: round(xclk / (2 * hz))."""
    return (XCLK + hz) // (2 * hz)


def half_cycles(hz):
    return half_counts(hz) * CPU / XCLK


def matches(dur_ms, hz, extra_counts=0):
    """Matches before a timed tone ends: nearest whole half periods, at least one."""
    h = half_counts(hz)
    return max(1, (dur_ms * XCLK // 1000 + extra_counts + h // 2) // h)


def last_fall(n, hz):
    """Cycles from the start to the last falling edge of a tone that ends at match n."""
    return (n if n % 2 else n - 1) * half_cycles(hz)


def wave_errors(w, hz, win):
    """Check a running tone: edge count, mean period, every half period and the mean duty."""
    period, half, j = 2 * half_cycles(hz), half_cycles(hz), cyc(EDGE_US)
    errs = []
    if w["overflow"]:
        errs.append("edge buffer overflow")
    if w["rises"] < int(win / period) - 1 or w["per_n"] < 1:
        errs.append("rises=%d" % w["rises"])
        return errs, ""
    per = w["per_sum"] / w["per_n"]
    if abs(per - period) > j / w["per_n"] + 1e-5 * period:
        errs.append("mean period %.1f" % per)
    if w["per_min"] < period - j or w["per_max"] > period + j:
        errs.append("period %d..%d" % (w["per_min"], w["per_max"]))
    for k in ("hi", "lo"):
        if w[k + "_n"] and (w[k + "_min"] < half - j or w[k + "_max"] > half + j):
            errs.append("%s %d..%d" % (k, w[k + "_min"], w[k + "_max"]))
    hi = w["hi_sum"] / w["hi_n"] if w["hi_n"] else 0
    if not w["hi_n"] or abs(hi - half) > j / w["hi_n"] + cyc(DUTY_US):
        errs.append("mean high %.1f" % hi)
    worst = max(abs(w[k] - half) for k in ("hi_min", "hi_max", "lo_min", "lo_max") if w[k[:2] + "_n"])
    detail = "f=%.4f Hz (nominal %.4f) duty=%.3f%% worst half %+.2f us rises=%d" % (
        CPU / per, XCLK / (2 * half_counts(hz)), 100 * hi / per, worst / cyc(1), w["rises"])
    return errs, detail


def quiet(w):
    return w["rises"] == 0 and w["falls"] == 0


def check_wave(rep, name, w, hz, win, kind="acceptance"):
    errs, detail = wave_errors(w, hz, win)
    rep.add(name, not errs, "; ".join(errs) + (" | " if errs else "") + detail, kind)


def check_end(rep, name, w, hz, n, start, extra=0.0):
    """A timed tone: ceil(n/2) pulses, the last fall at start + last_fall(n), low and quiet after.
    A pulse already high when sampling began shows no rising edge."""
    pulses = (n + 1) // 2
    want = start + last_fall(n, hz)
    err = w["last_fall"] - want
    ok = (w["rises"] == pulses - w["lvl0"] and w["falls"] == pulses and w["lvl1"] == 0
          and w["last_edge"] == w["last_fall"] and abs(err) <= cyc(EDGE_US) + extra)
    rep.add(name, ok, "n=%d pulses=%d/%d end %+.2f us (sound %.3f ms)" % (
        n, w["rises"], pulses, err / cyc(1), n * half_counts(hz) * 1000 / XCLK))


def check_cfg(rep, name, c, func=11, oe=1, level=None):
    ok = c["func"] == func and c["oe"] == oe and (level is None or c["i"] == level)
    rep.add(name, ok, "func=%d oe=%d level=%d" % (c["func"], c["oe"], c["i"]))


def pwm_errors(w, value):
    """A 1 kHz analogWrite() wave at 8-bit value: period and high time within PWM_US."""
    period = 40000 * CPU / XCLK
    high = (value * 40000 + 127) // 255 * CPU / XCLK
    tol = cyc(PWM_US)
    errs = []
    if w["per_n"] < 20 or abs(w["per_sum"] / w["per_n"] - period) > 1e-4 * period:
        errs.append("period")
    if w["hi_n"] < 20 or w["hi_min"] < high - tol or w["hi_max"] > high + tol:
        errs.append("hi %d..%d (want %.0f)" % (w["hi_min"], w["hi_max"], high))
    if w["lo_n"] < 20 or w["lo_min"] < period - high - tol or w["lo_max"] > period - high + tol:
        errs.append("lo %d..%d" % (w["lo_min"], w["lo_max"]))
    return errs


def run(board, rep):
    global CPU, XCLK
    info = board.command("INFO")["INFO"]
    CPU, XCLK = info["cpu"], info["xclk"]
    rep.add("info", XCLK == 40000000, "cpu=%d xclk=%d" % (CPU, XCLK))

    shorts = board.command("SHORTS")
    ok = len(shorts) == 8 and all(v["hi"] == 1 and v["lo"] == 0 for v in shorts.values())
    rep.add("jumpers 8-22 14-15 18-19 20-23", ok, " ".join("%d->%d:%d%d" % (v["out"], v["in"], v["hi"], v["lo"])
                                                          for v in shorts.values()))

    calls, steady = [], []
    for out, inp, hz, ms, stop_ms in FREQ_POINTS:
        r = board.command("FREQ %d %d %d %d %d" % (out, inp, hz, ms, stop_ms))
        w, stop = r[("FREQ", inp)], r[("FREQSTOP", inp)]
        name = "freq %d Hz GPIO%d->%d" % (hz, out, inp)
        calls.append(w["call"])
        check_wave(rep, name, w, hz, w["win"])
        rep.add(name + " starts high in tone()", w["lvl0"] == 0 and w["first_rise"] <= w["call"] + cyc(SAMPLE_US)
                and w["call"] <= cyc(CALL_US), "call %.1f us first rise %.1f us" % (
                    w["call"] / cyc(1), w["first_rise"] / cyc(1)))
        check_cfg(rep, name + " pin is a GPIO output", r["FREQCFG"])
        rep.add(name + " noTone() stops low", stop["rises"] == 0 and stop["falls"] <= 1 and stop["lvl1"] == 0
                and (stop["falls"] == 0 or stop["first_fall"] <= stop["call"] + cyc(SAMPLE_US)),
                "falls=%d level=%d over %d ms" % (stop["falls"], stop["lvl1"], stop_ms))
        check_cfg(rep, name + " pin after noTone()", r["FREQSTOPCFG"], level=0)

    for out, inp, hz, dur, ms in DUR_POINTS:
        r = board.command("DUR %d %d %d %d %d" % (out, inp, hz, dur, ms), timeout=60)
        w = r[("DUR", inp)]
        name = "duration %d Hz %d ms GPIO%d->%d" % (hz, dur, out, inp)
        calls.append(w["call"])
        n = matches(dur, hz)
        if dur * XCLK // 1000 >= half_counts(hz) // 2:
            rep.add(name + " rounds to half periods", abs(n * half_counts(hz) - dur * XCLK // 1000)
                    <= half_counts(hz) // 2, "n=%d" % n)
        check_end(rep, name, w, hz, n, w["first_rise"], cyc(SAMPLE_US))
        rep.add(name + " returns at once", w["call"] <= cyc(CALL_US) and w["lvl0"] == 0,
                "call %.1f us" % (w["call"] / cyc(1)))
        check_cfg(rep, name + " pin rests low", r["DURCFG"], level=0)
    r = board.command("DUR 19 18 3000 4294967295 30")
    w = r[("DUR", 18)]
    check_wave(rep, "duration 4294967295 ms keeps sounding", w, 3000, w["win"])

    r = board.command("NOTONE")
    rep.add("noTone() while high", r[("NT_HIGH", 22)]["lvl1"] == 1 and r[("NT_STOP", 22)]["falls"] == 1
            and r[("NT_STOP", 22)]["rises"] == 0 and r[("NT_STOP", 22)]["lvl1"] == 0,
            "falls=%d level=%d" % (r[("NT_STOP", 22)]["falls"], r[("NT_STOP", 22)]["lvl1"]))
    check_cfg(rep, "noTone() leaves a low output", r["NT_STOPCFG"], level=0)
    rep.add("noTone() returns the timer", r["NT_TIMERS"]["ok"] == 1, "%s" % r["NT_TIMERS_CLAIM"])
    rep.add("noTone() on another pin: no effect", r["NT_OTHER"]["same"] == 1, "")
    check_wave(rep, "noTone() on another pin: tone goes on", r[("NT_OTHER_WAVE", 22)], 1000,
               r[("NT_OTHER_WAVE", 22)]["win"])
    steady.append(r[("NT_OTHER_WAVE", 22)] | {"hz": 1000})
    rep.add("noTone() without a tone: no effect", r["NT_IDLE"]["same"] == 1, "")

    r = board.command("ONE")
    rep.add("tone() on another pin while one sounds: pin unchanged", r["ONE_OTHER"]["same"] == 1
            and quiet(r[("ONE_A", 19)]), "")
    check_wave(rep, "first tone goes on", r[("ONE_A", 22)], 1000, r[("ONE_A", 22)]["win"])
    steady.append(r[("ONE_A", 22)] | {"hz": 1000})
    check_wave(rep, "tone() on the sounding pin sets the frequency", r[("ONE_SAME", 22)], 2000,
               r[("ONE_SAME", 22)]["win"])
    rep.add("after noTone() the other pin may sound", quiet(r[("ONE_NEXT", 22)]) and r[("ONE_NEXT", 22)]["lvl1"] == 0,
            "")
    check_wave(rep, "second pin sounds", r[("ONE_NEXT", 19)], 2000, r[("ONE_NEXT", 19)]["win"])
    check_wave(rep, "after a timed tone ended another pin may sound", r[("ONE_AFTER", 19)], 3000,
               r[("ONE_AFTER", 19)]["win"])

    for f1, wait, f2, dur, ms in CHANGES:
        r = board.command("CHANGE %d %d %d %d %d" % (f1, wait, f2, dur, ms))
        w, since = r[("CHANGE", 22)], r["CHANGE_SINCE"]
        name = "change %d->%d Hz after %d us%s" % (f1, f2, wait, " for %d ms" % dur if dur else "")
        elapsed = since["since"] - since["started"]  # Cycles of the current half period at the call.
        now = elapsed * XCLK / CPU
        past = now > half_counts(f2) - 2
        want = 0 if past else half_cycles(f2) - elapsed
        err = w["first_edge"] - want
        rep.add(name + (" ends the half period at once" if past else " ends it at the new length"),
                -cyc(SAMPLE_US) <= err <= w["call"] + cyc(EDGE_US) + cyc(SAMPLE_US),
                "first edge %+.2f us from expected" % (err / cyc(1)))
        if dur:
            n = matches(dur, f2, int(now))
            check_end(rep, name + " ends on time", w, f2, n, -elapsed, w["call"])
        else:
            check_wave(rep, name + " then runs at the new frequency", w, f2, w["win"] - w["first_edge"])

    r = board.command("PITCH 200 400 410")
    w = r[("PITCH", 22)]
    j = cyc(EDGE_US)
    lo, hi = half_cycles(410), half_cycles(400)
    ok = (all(w[k + "_min"] >= lo - j and w[k + "_max"] <= hi + j for k in ("hi", "lo")) and w["rises"] >= 75
          and w["lvl1"] == 0 and cyc(209000) - hi - j <= w["last_edge"] <= cyc(210000) + hi + j)
    rep.add("tone(8, 400/410 Hz, 10 ms) every 1 ms keeps the wave and ends 10 ms after the last call", ok,
            "rises=%d half %d..%d (want %.0f..%.0f) last edge %.2f ms" % (
                w["rises"], min(w["hi_min"], w["lo_min"]), max(w["hi_max"], w["lo_max"]), lo, hi,
                w["last_edge"] / cyc(1000)))

    r = board.command("BUSY")
    rep.add("both timers held by BLTimer: tone() ignored", r["BUSY_FULL"]["a"] == 1 and r["BUSY_FULL"]["b"] == 1
            and r["BUSY_FULL"]["same"] == 1 and quiet(r[("BUSY_FULL_WAVE", 22)]), "%s" % r["BUSY_FULL"])
    check_wave(rep, "one timer freed: tone() sounds", r[("BUSY_FREE_WAVE", 22)], 1000,
               r[("BUSY_FREE_WAVE", 22)]["win"])
    rep.add("tone holds a timer until noTone()", r["BUSY_SHARE"]["while_tone"] == 0
            and r["BUSY_SHARE"]["after_notone"] == 1, "%s" % r["BUSY_SHARE"])
    rep.add("a timed tone that ended gives its timer to BLTimer", r["BUSY_RECLAIM"]["claim"] == 1
            and r["BUSY_RECLAIM"]["same"] == 1 and quiet(r[("BUSY_RECLAIM_WAVE", 22)]), "%s" % r["BUSY_RECLAIM"])
    check_cfg(rep, "pin after the reclaimed tone", r["BUSY_RECLAIM_CFG"], level=0)

    r = board.command("INVALID")
    rep.add("invalid pins and frequencies change no pin or timer", r["INV_IDLE"]["changed"] == 0
            and r["INV_IDLE"]["tcer_same"] == 1 and r["INV_TIMERS"]["ok"] == 1, "%s" % r["INV_IDLE"])
    check_wave(rep, "invalid calls leave a running tone alone", r[("INV_RUNNING", 22)], 1000,
               r[("INV_RUNNING", 22)]["win"])
    steady.append(r[("INV_RUNNING", 22)] | {"hz": 1000})

    r = board.command("PWM")
    for tag, inp, value in (("PWM_A", 19, 128), ("PWM_A", 23, 64), ("PWM_B", 19, 128), ("PWM_B", 23, 64),
                            ("PWM_B", 14, 191), ("PWM_D", 23, 64), ("PWM_E", 19, 64), ("PWM_E", 23, 64)):
        errs = pwm_errors(r[(tag, inp)], value)
        rep.add("%s analogWrite %d on GPIO%d's partner" % (tag, value, inp), not errs, "; ".join(errs))
    check_wave(rep, "PWM_A tone 3 kHz beside two PWM channels", r[("PWM_A", 14)], 3000, r[("PWM_A", 14)]["win"])
    rep.add("PWM_B tone stops when analogWrite() starts", r[("PWM_BX", 14)]["rises"] <= 4,
            "rises=%d in 3 ms (the 3 kHz tone gives 9)" % r[("PWM_BX", 14)]["rises"])
    check_cfg(rep, "PWM_B analogWrite() takes the tone pin", r["PWM_BCFG"], func=16, oe=0)
    rep.add("PWM_B analogWrite() returns the timer", r["PWM_B_TIMERS"]["ok"] == 1, "")
    check_wave(rep, "PWM_C refused analogWrite() leaves the tone", r[("PWM_C", 22)], 2000, r[("PWM_C", 22)]["win"])
    check_cfg(rep, "PWM_C tone pin stays a GPIO", r["PWM_CCFG"])
    check_wave(rep, "PWM_D tone() takes a PWM pin", r[("PWM_D", 19)], 2000, r[("PWM_D", 19)]["win"])
    check_cfg(rep, "PWM_D pin is a GPIO output", r["PWM_DCFG"])

    for hz, ms in ((5000, 200), (20000, 80)):
        r = board.command("TIMERCO %d %d" % (hz, ms))
        t, w = r["TCO_TIMER"], r[("TCO", 22)]
        want = w["win"] / cyc(100)
        rep.add("BLTimer 100 us beside a %d Hz tone" % hz, t["ok"] == 1 and abs(t["ticks"] - want) <= 1,
                "ticks=%d want %.0f timer=%d" % (t["ticks"], want, t["timer"]))
        check_wave(rep, "%d Hz tone beside BLTimer 100 us" % hz, w, hz, w["win"])
        steady.append(w | {"hz": hz})

    r = board.command("DIGITAL")
    w = r[("DIG_WRITE", 22)]
    rep.add("digitalWrite(HIGH) stops the tone at high", w["falls"] == 0 and w["lvl1"] == 1
            and w["last_edge"] <= w["call"] + cyc(SAMPLE_US), "rises=%d falls=%d" % (w["rises"], w["falls"]))
    check_cfg(rep, "digitalWrite() pin", r["DIG_WRITECFG"], level=1)
    rep.add("digitalWrite() returns the timer", r["DIG_WRITE_TIMERS"]["ok"] == 1, "")
    w = r[("DIG_INPUT", 22)]
    rep.add("pinMode(INPUT) stops the tone", w["rises"] == 0 and w["lvl1"] == 0
            and w["last_edge"] <= w["call"] + cyc(SAMPLE_US), "falls=%d" % w["falls"])
    check_cfg(rep, "pinMode(INPUT) pin", r["DIG_INPUTCFG"], oe=0)
    rep.add("pinMode() returns the timer", r["DIG_INPUT_TIMERS"]["ok"] == 1, "")
    check_wave(rep, "tone on GPIO2 (own input)", r[("DIG_ADC_TONE", 2)], 1000, r[("DIG_ADC_TONE", 2)]["win"])
    check_cfg(rep, "analogRead() takes the tone pin", r["DIG_ADCCFG"], func=10, oe=0)
    rep.add("analogRead() returns the timer", r["DIG_ADC_TIMERS"]["ok"] == 1 and r["DIG_ADC"]["value"] >= 0,
            "value=%d" % r["DIG_ADC"]["value"])

    r = board.command("MELODY", timeout=60)
    for i, (hz, div) in enumerate(MELODY):
        w = r[("MEL%d" % i, 22)]
        dur = 1000 // div
        if hz == 0:
            rep.add("toneMelody note %d: tone(8, 0, %d) is silent" % (i, dur), quiet(w) and w["lvl1"] == 0, "")
            continue
        calls.append(w["call"])
        check_end(rep, "toneMelody note %d: %d Hz for %d ms" % (i, hz, dur), w, hz, matches(dur, hz),
                  w["first_rise"], cyc(SAMPLE_US))
        n = matches(dur, hz)
        errs, detail = wave_errors(w, hz, last_fall(n, hz))
        rep.add("toneMelody note %d frequency" % i, not errs, "; ".join(errs) + detail)

    worst = max(max(abs(w[k] - half_cycles(w["hz"])) for k in ("hi_min", "hi_max", "lo_min", "lo_max"))
                for w in steady)
    rep.add("edge spread on running tones", True, "worst half period %+.2f us" % (worst / cyc(1)), "exploratory")
    rep.add("tone() call time", True, "first after boot %.1f us, others up to %.1f us" % (
        calls[0] / cyc(1), max(calls[1:]) / cyc(1)), "exploratory")
    load = board.command("LOAD")[("LOAD", "all")]
    rates = {hz: [x["iters"] / x["win"] for x in load if x["hz"] == hz] for hz in (1, 20000)}
    shares = [1 - b / a for a, b in zip(rates[1], rates[20000])]
    rep.add("CPU share of a 20 kHz tone", True, " ".join("%.1f%%" % (100 * x) for x in shares), "exploratory")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()
    rep = Report()
    with open(args.log, "w") as log:
        board = Board(args.port, log)
        board.wait("READY ToneAcceptance", 15)
        try:
            run(board, rep)
        except (TimeoutError, KeyError) as exc:
            rep.add("run", False, "aborted: %r" % exc)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rep.items, f, indent=1, ensure_ascii=False)
    acc = [i for i in rep.items if i["kind"] == "acceptance"]
    print("acceptance: %d/%d passed; exploratory: %d" % (
        len(acc) - len(rep.failed()), len(acc), len(rep.items) - len(acc)))
    return 1 if rep.failed() else 0


if __name__ == "__main__":
    sys.exit(main())
