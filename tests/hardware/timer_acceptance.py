#!/usr/bin/env python3
"""Run the BLTimer hardware acceptance on Ai-M64P-32S-Kit.

Upload tests/hardware/TimerAcceptance first. The board's CH340 port is opened once; opening it
resets the BL616CL, so the whole run happens in this process. The sketch derives each match time
from micros() minus the timer counter read in the callback (the latency): match drift proves the
tick count per period, gap counts expose lost or duplicate callbacks, and latency is checked
against the declared bound. micros() shares the crystal with the timers, so crystal ppm is not
measured. Each check is "acceptance" (counts toward the exit status) or "exploratory" (recorded
only). The log keeps every line exchanged with the board; --json writes the judged results.

Usage: timer_acceptance.py --port /dev/ttyUSB0 --log run.log [--json run.json]
"""
import argparse
import json
import random
import string
import sys
import time

import serial

CONTROL_BAUD = 115200
LATENCY_US = 20  # Declared bound from timer match to callback entry, without Wi-Fi or Serial traffic.
FIRST_SLACK_US = 100  # Allowed begin()/start() overhead before the first match.


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

    def echo(self, text):
        self.note("> ECHO " + text)
        self.ser.write(("ECHO " + text + "\n").encode())
        return self.wait("RESULT ECHO", 5.0)[len("RESULT ECHO "):]


class Report:
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail, kind="acceptance"):
        self.items.append({"name": name, "kind": kind, "pass": bool(ok), "detail": detail})
        print("%-4s %-11s %s  %s" % ("PASS" if ok else "FAIL", kind, name, detail))

    def failed(self):
        return [i for i in self.items if i["kind"] == "acceptance" and not i["pass"]]


def near(value, target, tol):
    return abs(value - target) <= tol


def first_ok(value, period):
    """The first match comes one full period after the call, plus the call's own overhead."""
    return period <= value <= period + FIRST_SLACK_US


def intervals_ok(lo, hi, period):
    """Callback-to-callback intervals differ from the period only by latency differences."""
    return period - LATENCY_US <= lo and hi <= period + LATENCY_US


def detail(r):
    return " ".join("%s=%d" % kv for kv in r.items())


def check_system(rep, board, label, baseline):
    """delay(), the FreeRTOS counter task, CPU share and Serial echo while a timer may be active."""
    r = board.command("SYS 1000")
    rep.add("%s delay(1000)" % label, 1000000 <= r["delay_us"] <= 1002000, detail(r))
    if baseline is None:
        rep.add("%s FreeRTOS task ticks" % label, near(r["task_ticks"], 1000, 3), detail(r))
    else:
        rep.add("%s FreeRTOS task ticks" % label, near(r["task_ticks"], baseline["task_ticks"], 3), detail(r))
        share = r["spin_loops_100ms"] / baseline["spin_loops_100ms"]
        rep.add("%s CPU share left to loop()" % label, share >= 0.5, "share=%.3f %s" % (share, detail(r)))
    rng = random.Random(label)
    bad = 0
    for _ in range(20):
        text = "".join(rng.choice(string.ascii_letters + string.digits) for _ in range(100))
        bad += board.echo(text) != text
    rep.add("%s Serial echo 20x100 B" % label, bad == 0, "mismatched=%d" % bad)
    return r


def main():
    ap = argparse.ArgumentParser(description="Run the BLTimer hardware acceptance on Ai-M64P-32S-Kit.")
    ap.add_argument("--port", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    rep = Report()
    with open(args.log, "a") as log:
        board = Board(args.port, log)
        board.wait("READY", 15)
        info = board.command("INFO")
        rep.add("info", info["xclk"] == 40000000 and info["min"] == 10, detail(info), "exploratory")

        r = board.command("CLR")
        ok = (r["ok"] == 1 and 300000 <= r["running"] <= 302000 and r["after_start"] < 100
              and 100000 <= r["after_100ms"] <= 102000)
        rep.add("start() restarts the counter from zero", ok, detail(r))
        # BLTimer relies on this: disabling a timer resets its counter, so re-enabling starts at 0.
        rep.add("stop resets the counter; raw lhal re-enable starts at 0", r["stopped"] == 0
                and r["raw_start"] < 100, detail(r))

        for period, n in ((10, 100000), (37, 27000), (100, 10000), (1000, 2000), (12345, 100), (1000000, 5)):
            r = board.command("RATE %d %d" % (period, n), timeout=period * n * 1.5 / 1e6 + 10)
            ok = (r["ok"] == 1 and r["count"] == n and r["lost"] == 0 and r["dup"] == 0 and abs(r["drift"]) <= 1
                  and first_ok(r["first_match"], period) and r["lat_max"] <= LATENCY_US)
            rep.add("periodic %d us x%d" % (period, n), ok, detail(r))

        for delay_us in (10, 1000, 100000):
            r = board.command("ONCE %d" % delay_us)
            ok = (r["ok"] == 1 and r["count1"] == 1 and first_ok(r["lat1"], delay_us) and r["running1"] == 0
                  and r["restart"] == 1 and r["armed"] == 1 and r["count2"] == 2
                  and first_ok(r["lat2"], delay_us) and r["running2"] == 0)
            rep.add("one-shot %d us fires once, start() re-arms" % delay_us, ok, detail(r))

        a = board.command("LIFE")  # The sketch prints LIFE, then LIFE2.
        b = fields(board.wait("RESULT LIFE2 ", 5))
        rep.add("lifecycle: run, stop holds callbacks", a["begin"] == 1 and 48 <= a["run_count"] <= 51
                and a["stop"] == 1 and a["run_after_stop"] == 0 and a["stopped_delta"] == 0, detail(a))
        rep.add("lifecycle: start() gives a full first period", a["start"] == 1 and first_ok(a["start_first"], 1000)
                and 28 <= a["start_count"] <= 31 and intervals_ok(a["start_min"], a["start_max"], 1000), detail(a))
        rep.add("lifecycle: setPeriod() while running", a["set"] == 1 and first_ok(a["set_first"], 2000)
                and 19 <= a["set_count"] <= 21 and intervals_ok(a["set_min"], a["set_max"], 2000), detail(a))
        rep.add("lifecycle: setPeriod() while stopped", b["set_stopped"] == 1 and b["run_set_stopped"] == 0
                and b["set_stopped_count"] == 0 and first_ok(b["restart_first"], 500)
                and 38 <= b["restart_count"] <= 41 and intervals_ok(b["restart_min"], b["restart_max"], 500),
                detail(b))
        rep.add("lifecycle: end() releases, re-acquire works", b["end_count"] == 0 and b["end_index"] == -1
                and b["end_running"] == 0 and b["end_calls_ok"] == 0 and b["again"] == 1
                and b["again_index"] in (0, 1) and b["again_count"] >= 8, detail(b))

        for p0, p1 in ((100, 1000), (1000, 700)):
            r = board.command("DUAL %d %d 1000" % (p0, p1))
            ok = (r["a"] == 1 and r["b"] == 1 and r["c"] == 0 and r["ia"] == 0 and r["ib"] == 1 and r["ic"] == -1
                  and near(r["count_a"], r["window_a"] // p0, 2) and near(r["count_b"], r["window_b"] // p1, 2)
                  and abs(r["drift_a"]) <= 1 and abs(r["drift_b"]) <= 1 and r["lost"] == 0 and r["dup"] == 0
                  and r["lat_max_a"] <= LATENCY_US and r["lat_max_b"] <= LATENCY_US
                  and r["c_count"] == 0 and r["c2"] == 1 and r["ic2"] == 0)
            rep.add("two timers %d us + %d us, third claim fails" % (p0, p1), ok, detail(r))

        r = board.command("REJECT")
        ok = (r["r0"] == 0 and r["r9"] == 0 and r["rnull"] == 0 and r["rnull_plain"] == 0 and r["ronce9"] == 0
              and r["idx_after"] == -1 and r["unheld"] == 0 and r["r10"] == 1 and r["set9"] == 0
              and r["run10"] == 1 and 995 <= r["mean10_x100"] <= 1005 and r["set_max"] == 1 and r["run_max"] == 1
              and r["begin_max"] == 1 and r["plain"] == 1 and 18 <= r["plain_count"] <= 21)
        rep.add("invalid requests fail and keep state; max period accepted", ok, detail(r))

        r = board.command("ISRCALL")
        ok = (r["ok"] == 1 and r["count"] >= 18 and r["stop"] == 0 and r["start"] == 0 and r["set"] == 0
              and r["begin"] == 0 and r["idx_after_end"] == r["idx_before"] and r["running"] == 1)
        rep.add("methods called in a callback fail", ok, detail(r))

        baseline = check_system(rep, board, "idle", None)
        for period in (100, 10):
            r = board.command("LOAD %d" % period)
            rep.add("background timer %d us starts" % period, r["ok"] == 1, detail(r))
            check_system(rep, board, "timer %d us" % period, baseline)
            r = board.command("LOAD 0")
            matches = r["elapsed"] // period
            rep.add("background timer %d us: no callbacks beyond timer matches" % period,
                    r["count"] <= matches + 1, "matches=%d %s" % (matches, detail(r)))
            # Callbacks are not queued: interrupt latency near the period merges matches. Gap
            # classification is exact only while latency stays well below the period.
            ok = (r["lost"] == 0 and r["dup"] == 0 and r["drift"] == 0
                  and near(r["count"], matches, FIRST_SLACK_US // period + 2))
            rep.add("background timer %d us: no lost callbacks during Serial traffic" % period, ok,
                    "matches=%d %s" % (matches, detail(r)), "acceptance" if period >= 100 else "exploratory")
            rep.add("background timer %d us: latency during Serial traffic" % period, r["lat_max"] <= LATENCY_US,
                    detail(r), "exploratory")
        board.ser.close()

    failed = rep.failed()
    print("acceptance: %d checks, %d failed; exploratory: %d" % (
        sum(i["kind"] == "acceptance" for i in rep.items), len(failed),
        sum(i["kind"] == "exploratory" for i in rep.items)))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"port": args.port, "info": info, "latency_us": LATENCY_US, "results": rep.items}, f,
                      indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
