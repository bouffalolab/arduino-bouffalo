#!/usr/bin/env python3
"""Run the analogRead hardware acceptance on Ai-M64P-32S-Kit without external wiring.

Upload tests/hardware/AdcAcceptance first. The board's CH340 port is opened once; opening it
resets the BL616CL, so the whole run happens in this process. No known external voltage is
connected, so zero/full scale per channel and intermediate voltages are not judged here; the
script checks the channel mapping (driven-pin matrix), resolution scaling and zero padding on the
steady A2/A3 inputs, noise against the 12 effective bits, invalid pins, return to digital use and
readings while Wi-Fi runs. Each check is "acceptance" (counts toward the exit status) or
"exploratory" (recorded only). The log keeps every line exchanged with the board; --json writes
the judged results.

Usage: adc_acceptance.py --port /dev/ttyUSB0 --log run.log [--json run.json]
"""
import argparse
import json
import sys
import time

import serial

CONTROL_BAUD = 115200
WL_AP_LISTENING = 7
N_PINS = 10
DRIVEN_LIMIT = 512  # A driven-high pin reads the top code (about 1021 at 10 bits).
OWN_MARGIN = 64  # A[x] pre-charged high must read this much above its pre-charged-low readings.


def fields(line):
    """Return the key=value fields of a RESULT line as integers."""
    return {k: int(v) for k, v in (kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)}


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
        """Send one command and return the key=value fields of its RESULT line as integers."""
        self.note("> " + text)
        self.ser.write((text + "\n").encode())
        return fields(self.wait("RESULT ", timeout))


class Report:
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail, kind="acceptance"):
        self.items.append({"name": name, "kind": kind, "pass": bool(ok), "detail": detail})
        print("%-4s %-11s %s  %s" % ("PASS" if ok else "FAIL", kind, name, detail))

    def failed(self):
        return [i for i in self.items if i["kind"] == "acceptance" and not i["pass"]]


def detail(r):
    return " ".join("%s=%d" % kv for kv in r.items())


def check_map(rep, board):
    """Each row drives one A pin high and the rest low, then reads all ten at 10 bits."""
    rows = [board.command("MAP %d" % x) for x in range(N_PINS)]
    v = [[r["v%d" % y] for y in range(N_PINS)] for r in rows]
    for x, r in enumerate(rows):
        others = [v[x][y] for y in range(N_PINS) if y != x]
        rep.add("A%d high: no other A pin reads a driven pin" % x,
                all(0 <= o < DRIVEN_LIMIT for o in others), detail(r))
    for x in range(N_PINS):
        own_low = max(v[x2][x] for x2 in range(N_PINS) if x2 != x)
        rep.add("A%d reads its own pin" % x, v[x][x] >= own_low + OWN_MARGIN,
                "high=%d max_when_low=%d" % (v[x][x], own_low))


def check_res(rep, board, pin, n):
    """Interleaved reads of a steady pin at several resolutions; means compared in 12-bit units."""
    r = board.command("RES %d %d" % (pin, n))
    d = detail(r)
    m12 = r["m12"]
    rep.add("A%d resolution reads never fail" % pin, r["neg"] == 0, d)
    rep.add("A%d 10-bit = 12-bit / 4" % pin, abs(r["m10"] * 4 - m12) <= 400, d)
    rep.add("A%d 8-bit = 12-bit / 16" % pin, abs(r["m8"] * 16 - m12) <= 1600, d)
    rep.add("A%d 1-bit reads 0 below half scale" % pin, r["m1"] == 0 and r["max1"] == 0, d)
    rep.add("A%d 14/16/31-bit pad 12 bits with zeros" % pin, r["pad_bad"] == 0
            and all(abs(r[k] - m12) <= 100 for k in ("m14", "m16", "m31")), d)
    rep.add("A%d invalid resolutions keep 12 bits" % pin, abs(r["inv_mean_x100"] - m12) <= 300, d)
    return r


def main():
    ap = argparse.ArgumentParser(description="Run the analogRead hardware acceptance on Ai-M64P-32S-Kit.")
    ap.add_argument("--port", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    rep = Report()
    with open(args.log, "a") as log:
        board = Board(args.port, log)
        board.wait("READY", 15)
        info = board.command("INFO")
        rep.add("NUM_ANALOG_INPUTS is 10", info["num"] == N_PINS, detail(info))
        rep.add("analogRead takes at most 300 us after the first call", info["call_us_max"] <= 300, detail(info))
        rep.add("first call (ADC init and calibration)", info["boot_value"] >= 0, detail(info), "exploratory")

        check_map(rep, board)

        res = {pin: check_res(rep, board, pin, 200) for pin in (2, 3)}
        m10 = res[2]["m10"] / 100.0
        rep.add("boot default resolution is 10 bits", abs(info["boot_value"] - m10) <= 3,
                "boot_value=%d A2 m10=%.2f" % (info["boot_value"], m10))

        r = board.command("NOISE 1000")
        rep.add("steady-input noise <= 1 LSB at 12 bits", r["fail"] == 0 and r["a2_sd_x100"] <= 100
                and r["a3_sd_x100"] <= 100, detail(r))

        r = board.command("INVALID")
        rep.add("non-ADC and reserved pins return -1", r["checked"] == 30 and r["not_minus1"] == 0, detail(r))
        rep.add("rejected pin keeps its digital output", r["r9"] == -1 and r["level9_high"] == 1
                and r["level9_low"] == 0, detail(r))
        rep.add("analogReference ignores unsupported modes",
                abs(r["ref_other_mean_x100"] - r["ref_default_mean_x100"]) <= 100, detail(r))

        r = board.command("RESTORE")
        rep.add("pinMode returns an analog pin to digital use", r["high"] == 1 and r["low"] == 0
                and r["pullup"] == 1 and r["again"] >= 0, detail(r))

        for mode, n in ((0, 2000), (1, 2000), (2, 2000), (0, 2000)):
            r = board.command("WIFI %d %d" % (mode, n), timeout=120)
            ok = r["fail"] == 0
            if mode == 1:
                ok = ok and r["ap_status"] == WL_AP_LISTENING
            elif mode == 2:
                ok = ok and r["scans"] >= 1 and r["found"] >= 0
            name = {0: "Wi-Fi idle", 1: "Wi-Fi soft AP running", 2: "Wi-Fi STA scanning"}[mode]
            rep.add("%s: readings recorded without failures" % name, ok, detail(r))

        r = board.command("RAILS")
        ok = all(r["hi%d" % i] >= 65000 and r["lo%d" % i] <= 64 for i in range(N_PINS))
        rep.add("LHAL: pins held at 3V3/GND read top/zero codes", ok, detail(r), "exploratory")

    failed = rep.failed()
    n_acc = sum(1 for i in rep.items if i["kind"] == "acceptance")
    print("acceptance: %d/%d passed" % (n_acc - len(failed), n_acc))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"items": rep.items, "failed": len(failed), "acceptance": n_acc}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
