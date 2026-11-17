#!/usr/bin/env python3
"""Run the HardwareSerial hardware acceptance on Ai-M64P-32S-Kit.

Upload tests/hardware/UartAcceptance first. The board's CH340 port is opened once; opening it
resets the BL616CL, so the whole run happens in this process. Serial1 needs a GPIO14-GPIO15
jumper. Each check is "acceptance" (counts toward the exit status) or "exploratory" (recorded
only). The log keeps every line exchanged with the board; --json writes the judged results.

Usage: uart_acceptance.py --port /dev/ttyUSB0 --log run.log [--json run.json]
"""
import argparse
import bisect
import json
import sys
import time

import serial

CONTROL_BAUD = 115200
PARITY = {0x1: "E", 0x2: "O", 0x3: "N", 0x4: "M", 0x5: "S"}
STOP = {0x10: 1.0, 0x20: 1.5, 0x30: 2.0}
DATA = {0x100: 5, 0x200: 6, 0x300: 7, 0x400: 8}


def config(bits, parity, stop):
    """Compose an Arduino SERIAL_* value from data bits, parity letter and stop bits."""
    p = {v: k for k, v in PARITY.items()}[parity]
    s = {v: k for k, v in STOP.items()}[stop]
    return {5: 0x100, 6: 0x200, 7: 0x300, 8: 0x400}[bits] | p | s


def describe(cfg):
    """Return the conventional name of an Arduino SERIAL_* value, such as 8N1 or 8N1.5."""
    stop = STOP.get(cfg & 0xF0, 0)
    return "%d%s%s" % (DATA.get(cfg & 0xF00, 0), PARITY.get(cfg & 0xF, "?"), ("%g" % stop))


def prng(seed, n, mask):
    """Generate the xorshift32 stream used by the sketch."""
    s = seed & 0xFFFFFFFF or 1
    out = bytearray()
    for _ in range(n):
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        out.append(s & 0xFF & mask)
    return bytes(out)


def uart_div(clk, baud):
    """Mirror the Core/LHAL divisor rounding."""
    return (clk * 10 // baud + 5) // 10


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

    def lines(self, timeout):
        """Yield decoded lines until timeout; undecodable bytes are kept as replacement text."""
        end = time.time() + timeout
        while time.time() < end:
            self.pending += self.ser.read(4096)
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip("\r")
                self.note("< " + text)
                yield text

    def wait(self, prefix, timeout=10.0):
        for text in self.lines(timeout):
            if text.startswith(prefix):
                return text
        raise TimeoutError("no %r within %.1fs" % (prefix, timeout))

    def command(self, text, timeout=20.0):
        self.note("> " + text)
        self.ser.write((text + "\n").encode())
        result = self.wait("RESULT ", timeout)
        return dict(kv.split("=", 1) for kv in result.split()[2:] if "=" in kv)

    def apply(self, baud, cfg):
        stop = STOP[cfg & 0xF0]
        self.ser.apply_settings({
            "baudrate": baud,
            "bytesize": DATA[cfg & 0xF00],
            "parity": PARITY[cfg & 0xF],
            "stopbits": serial.STOPBITS_TWO if stop == 2 else serial.STOPBITS_ONE,
        })
        self.note("= host port %d %s" % (baud, describe(cfg)))


class Report:
    def __init__(self):
        self.items = []

    def add(self, name, ok, detail, kind="acceptance"):
        self.items.append({"name": name, "kind": kind, "pass": bool(ok), "detail": detail})
        print("%-4s %-11s %s  %s" % ("PASS" if ok else "FAIL", kind, name, detail))

    def failed(self):
        return [i for i in self.items if i["kind"] == "acceptance" and not i["pass"]]


def ints(r, *keys):
    return [int(r.get(k, "-99999"), 0) for k in keys]


def decode_frames(init, edges, period, bits, parity, frames):
    """Sample frames from GPIO edge timestamps; return start, data, parity bit and first stop bit."""
    def level(t):
        return init ^ (bisect.bisect_right(edges, t) & 1)

    out = []
    pos = 0.0
    for _ in range(frames):
        idx = bisect.bisect_left(edges, pos)
        while idx < len(edges) and level(edges[idx]) != 0:
            idx += 1
        if idx >= len(edges):
            break
        start = edges[idx]
        samples = [level(start + (k + 0.5) * period) for k in range(bits + parity + 2)]
        data = sum(samples[1 + k] << k for k in range(bits))
        out.append({
            "start": start,
            "start_low": samples[0] == 0,
            "data": data,
            "parity": samples[1 + bits] if parity else None,
            "stop1": samples[1 + bits + parity],
        })
        pos = start + (1 + bits + parity + 0.5) * period
    return out


def check_frame(rep, board, info, baud, cfg, payload):
    bits, par, stop = DATA[cfg & 0xF00], PARITY[cfg & 0xF], STOP[cfg & 0xF0]
    mask = (1 << bits) - 1
    r = board.command("FRAME %d 0x%x %s" % (baud, cfg, " ".join("0x%x" % b for b in payload)))
    name = "frame %s @%d" % (describe(cfg), baud)
    if r.get("begin") != "1":
        rep.add(name, False, "begin failed")
        return
    edges = [int(x) for x in r["t"].split(",")] if r.get("t") else []
    clk = info["uart1_clk"]
    period = info["cpu_hz"] * uart_div(clk, baud) / clk  # CPU cycles per bit at the actual baud
    frames = decode_frames(int(r["init"]), edges, period, bits, par != "N", 1 + len(payload))[1:]
    problems = []
    if len(frames) != len(payload):
        problems.append("decoded %d/%d frames" % (len(frames), len(payload)))
    for f, want in zip(frames, payload):
        want &= mask
        ones = bin(want).count("1")
        exp_par = {"E": ones & 1, "O": 1 - (ones & 1)}.get(par)
        if not f["start_low"] or f["data"] != want or f["stop1"] != 1 or f["parity"] != exp_par:
            problems.append("byte 0x%x read %r" % (want, f))
    # Back-to-back frames: start-to-start distance is (1 + data + parity + stop) bit periods.
    starts = [f["start"] for f in frames]
    measured = [(b - a) / period - (1 + bits + (par != "N")) for a, b in zip(starts, starts[1:])]
    if not measured or any(abs(m - stop) > 0.1 for m in measured):
        problems.append("stop bits %s, want %g" % (["%.3f" % m for m in measured], stop))
    # Bit period from the first payload frame's data edges against the expected actual baud.
    first = starts[0] if starts else 0
    spans = [(e - first) for e in edges if first < e < first + (1 + bits) * period + period / 2]
    est = [s / round(s / period) for s in spans if round(s / period) > 0]
    err = (sum(est) / len(est) / period - 1) if est else 1
    if abs(err) > 0.01:
        problems.append("bit period error %.3f%%" % (err * 100))
    rep.add(name, not problems, "; ".join(problems) or "data/parity/stop ok, stop=%s, period err %.3f%%"
            % ("/".join("%.2f" % m for m in measured), err * 100))


def check_u0(rep, board, baud, cfg, n, kind="acceptance"):
    bits = DATA[cfg & 0xF00]
    mask = (1 << bits) - 1
    name = "U0/CH340 %s @%d x%d" % (describe(cfg), baud, n)
    board.note("> U0 %d 0x%x %d" % (baud, cfg, n))
    board.ser.write(("U0 %d 0x%x %d\n" % (baud, cfg, n)).encode())
    board.wait("SWITCH", 5)
    time.sleep(0.05)
    board.apply(baud, cfg)
    time.sleep(0.05)
    board.ser.reset_input_buffer()
    board.ser.write(bytes([0x0A & mask]))
    want = prng(0x51ED2705 ^ baud ^ cfg, n, mask)
    got = bytearray()
    end = time.time() + 3 + n * 13 / baud
    while len(got) < n and time.time() < end:
        got += board.ser.read(n - len(got))
    host_bad = sum(1 for a, b in zip(got, want) if (a & mask) != b) + (n - len(got))
    sent_at = time.time()
    board.ser.write(prng(0x2545F491 ^ baud ^ cfg, n, mask))
    board.ser.flush()
    # flush() only drains the kernel buffer; wait until the CH340 has clocked out every frame
    # before changing its baud rate.
    frame_bits = 1 + bits + (PARITY[cfg & 0xF] != "N") + STOP[cfg & 0xF0]
    time.sleep(max(0.0, sent_at + n * frame_bits / baud - time.time()) + 0.05)
    board.apply(CONTROL_BAUD, 0x413)
    board.pending = b""
    board.ser.reset_input_buffer()
    for text in board.lines(10):
        if text.startswith("RESULT U0"):
            r = dict(kv.split("=", 1) for kv in text.split()[2:] if "=" in kv)
            break
    else:
        rep.add(name, False, "no RESULT", kind)
        return
    dut = ints(r, "begin", "sync", "got", "mismatch", "overflow")
    ok = dut == [1, 0x0A & mask, n, 0, 0] and host_bad == 0
    rep.add(name, ok, "DUT->host bad=%d/%d; host->DUT got=%d mismatch=%d overflow=%d"
            % (host_bad, n, dut[2], dut[3], dut[4]), kind)


def main():
    ap = argparse.ArgumentParser(description="Run the HardwareSerial hardware acceptance on Ai-M64P-32S-Kit.")
    ap.add_argument("--port", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--json")
    args = ap.parse_args()

    rep = Report()
    with open(args.log, "a") as log:
        board = Board(args.port, log)
        board.wait("READY", 15)
        r = board.command("INFO")
        info = {k: int(v) for k, v in r.items()}
        clk = info["uart1_clk"]
        rep.add("info", clk > 0 and info["cpu_hz"] > 0, " ".join("%s=%s" % kv for kv in r.items()),
                "exploratory")

        r = board.command("JUMPER")
        rep.add("jumper GPIO14->GPIO15", r["idle"] == "0" and r["bad"] == "0", str(r))

        r = board.command("API")
        rep.add("peek/read/available", r == {"zero": "0", "a0": "3", "p0": "65", "p1": "65", "a1": "3",
                                             "r": "65,66,67,-1", "p2": "-1", "open": "1"}, str(r))

        for baud in (9600, 115200, 2000000):
            r = board.command("FLUSH %d" % baud)
            fifo, length, w, wus, fus, afw, rx = ints(r, "fifo", "len", "w", "write_us", "flush_us",
                                                      "afw", "rx")
            actual = clk / uart_div(clk, baud)
            expect_us = length * 10 * 1e6 / actual
            ok = (r.get("begin") == "1" and length > 0 and w == length and afw == fifo
                  and rx == length and wus < expect_us / 2 and expect_us * 0.97 <= fus <= expect_us * 1.1 + 200)
            rep.add("flush @%d" % baud, ok, "fifo=%d write_us=%d flush_us=%d expect_us=%.0f afw=%d rx=%d"
                    % (fifo, wus, fus, expect_us, afw, rx))

        r = board.command("OVF")
        rep.add("rx overflow accounting", r == {"resized": "1", "active_resize": "0", "avail": "63",
                                                "overflow": "137", "first_bad": "-1", "restored": "1"}, str(r))

        r = board.command("END")
        rep.add("end() frees pins, begin() restores", r == {
            "pre": "3", "closed": "1", "w_closed": "0", "afw_closed": "0", "avail_closed": "0",
            "gpio_bad": "0", "post": "4"}, str(r))

        for baud, n, kind in [(b, 4096, "acceptance") for b in (
                9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 1500000, 2000000)] + \
                [(b, 4096, "exploratory") for b in (2500000, 3000000, 4000000, 5000000, 8000000, 10000000)]:
            r = board.command("LOOP %d 0x413 %d" % (baud, n), timeout=30)
            begin, got, bad, ovf, us = ints(r, "begin", "got", "mismatch", "overflow", "us")
            rep.add("loop 8N1 @%d x%d" % (baud, n), begin == 1 and got == n and bad == 0 and ovf == 0,
                    "begin=%d got=%d mismatch=%d overflow=%d us=%d actual_baud=%.0f"
                    % (begin, got, bad, ovf, us, clk / max(uart_div(clk, baud), 1)), kind)

        formats = [config(b, p, s) for b in (5, 6, 7, 8) for p in "NEO" for s in (1.0, 2.0)]
        formats += [config(5, "N", 1.5), config(8, "N", 1.5), config(8, "E", 1.5)]
        for cfg in formats:
            r = board.command("LOOP 115200 0x%x 1024" % cfg)
            begin, got, bad, ovf = ints(r, "begin", "got", "mismatch", "overflow")
            rep.add("loop %s @115200 x1024" % describe(cfg), begin == 1 and got == 1024 and bad == 0 and ovf == 0,
                    "begin=%d got=%d mismatch=%d overflow=%d" % (begin, got, bad, ovf))
        for cfg in formats:
            check_frame(rep, board, info, 9600, cfg, [0x55, 0x00, 0x13])

        rejected = [(115200, config(8, "M", 1.0)), (115200, config(8, "S", 1.0)),
                    (115200, config(7, "M", 2.0)), (115200, 0), (115200, 0x403), (0, 0x413)]
        for baud, cfg in rejected:
            r = board.command("BEGIN %d 0x%x" % (baud, cfg))
            rep.add("begin rejects baud=%d cfg=0x%x" % (baud, cfg), r["ok"] == "0" and r["w"] == "0", str(r))
        # Divisor limits: 2..0xfffe. Divisor 1 corrupted loopback data, so begin() must refuse it.
        for baud in (300, 600, 1200, clk // 2, clk):
            r = board.command("BEGIN %d 0x413" % baud)
            div = uart_div(clk, baud)
            want = "1" if 2 <= div < 0xFFFF else "0"
            rep.add("begin limit baud=%d div=%d" % (baud, div), r["ok"] == want and r["w"] == want, str(r))
        for baud in (5000000, 10000000, 20000000):
            r = board.command("LOOP %d 0x413 24" % baud)
            rep.add("burst 8N1 @%d x24 (div=%d)" % (baud, uart_div(clk, baud)),
                    r.get("got") == "24" and r.get("mismatch") == "0", str(r), "exploratory")

        for cfg in [config(b, p, s) for b in (5, 6, 7, 8) for p in "NEO" for s in (1.0, 2.0)]:
            check_u0(rep, board, 115200, cfg, 256)
        for baud in (9600, 19200, 57600, 115200, 230400, 460800, 921600, 1000000, 1500000, 2000000):
            check_u0(rep, board, baud, 0x413, 4096)
        board.ser.close()

    failed = rep.failed()
    print("acceptance: %d checks, %d failed; exploratory: %d" % (
        sum(i["kind"] == "acceptance" for i in rep.items), len(failed),
        sum(i["kind"] == "exploratory" for i in rep.items)))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"port": args.port, "info": info, "results": rep.items}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
