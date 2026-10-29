#!/usr/bin/env python3
"""Inject control completions between real HCIREAD advertising fragments."""
import argparse
import re
import time

import run_ra4_ota_download_final_carrier as proxy
from run_bl616cl_generic_at_final_carrier import CLI, ROOT, restore_usb, run
from uno_r4_usb_identity import resolve_paired_bl616cl_port


class Events:
    """Validate framing without logging addresses or advertising contents."""
    def __init__(self):
        self.buffer = bytearray()
        self.reports = 0
        self.completions = []

    def feed(self, data):
        self.buffer.extend(data)
        while self.buffer:
            if self.buffer[0] != 4:
                raise RuntimeError("H4 event stream lost packet boundary")
            if len(self.buffer) < 3 or len(self.buffer) < 3 + self.buffer[2]:
                return
            size = 3 + self.buffer[2]
            packet = self.buffer[:size]
            del self.buffer[:size]
            if packet[1] == 0x0e:
                if size < 7:
                    raise RuntimeError("Truncated command completion")
                self.completions.append((packet[4] | packet[5] << 8, packet[6]))
            elif packet[1] == 0x3e and size >= 5 and packet[3] == 2:
                offset = 5
                for _ in range(packet[4]):
                    if offset + 10 > size:
                        raise RuntimeError("Truncated advertising report header")
                    offset += 10 + packet[offset + 8]
                    if offset > size:
                        raise RuntimeError("Truncated advertising report data")
                if offset != size:
                    raise RuntimeError("Advertising report length disagrees with H4 frame")
                self.reports += packet[4]
            else:
                raise RuntimeError("Unexpected HCI event in isolated scan gate")

    def partial_advertisement(self):
        return len(self.buffer) >= 4 and self.buffer[:2] == b"\x04\x3e" and self.buffer[3] == 2


class HciAt:
    def __init__(self, link):
        self.link = link
        self.events = Events()

    def write(self, opcode, parameters=b""):
        packet = bytes((1, opcode & 255, opcode >> 8, len(parameters))) + parameters
        response = proxy.transact_raw_payload(self.link,
            f"AT+HCIWRITE={len(packet)}\r\n".encode(), packet, 64, .001, 5)
        proxy.require_exact(response, b"OK\r\n", "HCI command write")

    def read_exact(self, size):
        result = bytearray()
        deadline = time.monotonic() + 5
        while len(result) < size and time.monotonic() < deadline:
            result.extend(self.link.read(size - len(result)))
        if len(result) != size:
            raise RuntimeError("HCIREAD timed out")
        return bytes(result)

    def pump(self):
        response = proxy.transact(self.link, b"AT+HCIAVAILABLE?\r\n", 5)
        match = re.fullmatch(rb"\+HCIAVAILABLE: (\d+)\r\nOK\r\n", response)
        if match is None:
            raise RuntimeError("Invalid HCIAVAILABLE framing")
        if int(match[1]) == 0:
            time.sleep(.005)
            return False
        self.link.write(b"AT+HCIREAD\r\n"); self.link.flush()
        header = bytearray()
        while len(header) < 32:
            header.extend(self.read_exact(1))
            if header.endswith(b"|"):
                break
        match = re.fullmatch(rb"\+HCIREAD: (\d+)\|", header)
        if match is None or not 0 < int(match[1]) <= 258:
            raise RuntimeError("Invalid HCIREAD length header")
        payload = self.read_exact(int(match[1]))
        if self.read_exact(4) != b"OK\r\n":
            raise RuntimeError("Invalid HCIREAD terminal")
        self.events.feed(payload)
        return True

    def complete(self, opcode):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.events.completions:
                if self.events.completions.pop(0) != (opcode, 0):
                    raise RuntimeError("HCI completion opcode/status mismatch")
                return
            self.pump()
        raise RuntimeError("HCI command completion timed out")

    def command(self, opcode, parameters=b""):
        self.write(opcode, parameters)
        self.complete(opcode)

    def partial(self):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.events.partial_advertisement():
                return
            # Accumulate enough reports for a 258-byte AT response to split a frame.
            time.sleep(.8)
            self.pump()
        raise RuntimeError(f"No real partial advertising frame observed; reports={self.events.reports}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--injections", type=int, default=32)
    args = parser.parse_args()
    if not 1 <= args.injections <= 100:
        parser.error("injections must be 1..100")
    opts = argparse.Namespace(bridge_port=resolve_paired_bl616cl_port("auto"),
        arduino_cli=CLI, runtime_settle_ms=1000)
    proxy.final_scan.require_final_carrier_bridge(opts.bridge_port)
    image = proxy.compile_proxy(opts)
    link = None
    modified = False
    try:
        modified = True
        proxy.upload_proxy(opts, image)
        link = proxy.open_bridge(opts.bridge_port)
        proxy.require_exact(proxy.transact(link, b"AT+HCIBEGIN\r\n", 5), b"+HCIBEGIN: OK\r\n", "HCI begin")
        hci = HciAt(link)
        hci.command(0x0c03)  # Reset
        hci.command(0x0c01, (0x3fffffffffffffff).to_bytes(8, "little"))
        hci.command(0x2001, (0x3ff).to_bytes(8, "little"))
        hci.command(0x200b, bytes((1, 0x20, 0, 0x20, 0, 0, 0)))
        hci.command(0x200c, bytes((1, 0)))  # active scan, duplicate reports allowed
        for count in range(1, args.injections + 1):
            hci.partial()
            hci.write(0x1009)  # Read BD_ADDR; never log the response's address.
            time.sleep(.03)
            hci.complete(0x1009)
            print(f"BL_HCI_FRAGMENT:CONTROL_PASS={count}", flush=True)
        hci.partial()
        hci.write(0x200c, bytes((0, 0)))  # Disable while host has an unfinished report.
        time.sleep(.03)
        hci.complete(0x200c)
        deadline = time.monotonic() + 3
        while hci.pump():
            if time.monotonic() > deadline:
                raise RuntimeError("HCI scan did not stop producing reports")
        if hci.events.buffer or hci.events.reports == 0:
            raise RuntimeError("Incomplete H4 stream after scan disable")
        proxy.require_exact(proxy.transact(link, b"AT+HCIEND\r\n", 5), b"+HCIEND: OK\r\n", "HCI end")
        print(f"BL_HCI_FRAGMENT:DATA_PASS injections={args.injections} reports={hci.events.reports}", flush=True)
    finally:
        if link is not None:
            link.close()
        if modified:
            restore_usb(physical_reset=True)
            run("python3", str(ROOT / "tools/run_ra4_wifis3_scan_final_carrier.py"))
    print("BL616CL_HCI_FRAGMENT_GATE_PASS", flush=True)


if __name__ == "__main__":
    main()
