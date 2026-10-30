#!/usr/bin/env python3
"""Exercise dedicated R4 long GATT, Notify and Indicate from a Windows central."""

import asyncio
import argparse
import logging
from importlib.metadata import version
from pathlib import Path
import time

from bleak import BleakClient, BleakScanner

SERVICE = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c041"
CHARACTERISTIC = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c042"
CLIENT_OPTIONS = {}
ADDRESS_OPTIONS = {}


def enable_service_cache_control():
    from bleak.backends.winrt.client import BleakClientWinRT, BluetoothCacheMode

    class ServiceCacheClient(CLIENT_OPTIONS.get("backend", BleakClientWinRT)):
        async def _get_services(self, **kwargs):
            # Isolate the initial service-list cache policy. Characteristic /
            # descriptor cache policy and all data operations stay unchanged.
            kwargs["service_cache_mode"] = BluetoothCacheMode.CACHED
            print("WINDOWS_BLE_WINRT:SERVICE_LIST_CACHE=CACHED", flush=True)
            return await super()._get_services(**kwargs)

    CLIENT_OPTIONS["backend"] = ServiceCacheClient


def enable_release_session_control():
    from bleak.backends.winrt.client import BleakClientWinRT

    class ReleaseSessionClient(CLIENT_OPTIONS.get("backend", BleakClientWinRT)):
        async def disconnect(self):
            # Opt-in lifecycle experiment. Keep the original disposal path even
            # if a concurrent session close makes this property write fail.
            try:
                session = getattr(self, "_session", None)
                if session is not None:
                    session.maintain_connection = False
                    print("WINDOWS_BLE_WINRT:MAINTAIN_RELEASED", flush=True)
            finally:
                await super().disconnect()

    CLIENT_OPTIONS["backend"] = ReleaseSessionClient


def enable_services_changed_control():
    from bleak.backends.winrt.client import BleakClientWinRT

    class ServicesChangedClient(CLIENT_OPTIONS.get("backend", BleakClientWinRT)):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            # Opt-in experiment using Bleak's existing WinRT event handler.
            # The handler refreshes with CACHED only after ServicesChanged;
            # this does not retry Unreachable errors or payload operations.
            self._retry_on_services_changed = True

    CLIENT_OPTIONS["backend"] = ServicesChangedClient


def enable_failure_diagnostics(reject_service_unreachable=False):
    import importlib
    backend_module = importlib.import_module("bleak.backends.winrt.client")
    from bleak.backends.winrt.client import BleakClientWinRT
    import ble_winrt_discovery_diagnostics as stages

    # Existing mocked/backends without FutureLike retain numeric snapshots.
    observe_stages = hasattr(backend_module, "FutureLike")
    if reject_service_unreachable and (not observe_stages or
            int(backend_module.GattCommunicationStatus.UNREACHABLE) != 1):
        raise RuntimeError("Unsupported WinRT backend for no-retry control")
    if observe_stages:
        stages.install(backend_module)

    class FailureDiagnosticClient(CLIENT_OPTIONS.get("backend", BleakClientWinRT)):
        async def _get_services(self, *args, **kwargs):
            started = time.monotonic()
            state = stages.State(reject_service_unreachable=reject_service_unreachable)
            token = stages.CURRENT.set(state) if observe_stages else None
            try:
                return await super()._get_services(*args, **kwargs)
            except BaseException as error:
                # Sample this backend's objects before connect() disposes them.
                # Property reads only: no additional GATT request or retry.
                def numeric(obj, name):
                    try:
                        return int(getattr(obj, name))
                    except Exception:
                        return -1
                event = "DISCOVERY_CANCELLED" if isinstance(error, asyncio.CancelledError) else "DISCOVERY_FAILURE"
                if observe_stages:
                    print(state.line(), flush=True)
                print(f"WINDOWS_BLE_WINRT:{event}="
                      f"{int((time.monotonic() - started) * 1000)},"
                      f"{numeric(getattr(self, '_requester', None), 'connection_status')},"
                      f"{numeric(getattr(self, '_session', None), 'session_status')},"
                      f"{numeric(getattr(self, '_session', None), 'max_pdu_size')}", flush=True)
                raise
            finally:
                if token is not None:
                    stages.CURRENT.reset(token)

    CLIENT_OPTIONS["backend"] = FailureDiagnosticClient


def enable_wait_active_control():
    from bleak.backends.winrt.client import BleakClientWinRT, GattSessionStatus

    class WaitActiveClient(BleakClientWinRT):
        async def _get_services(self, *args, **kwargs):
            # Control experiment; keep installed Bleak and default path intact.
            if self._session.session_status != GattSessionStatus.ACTIVE:
                await asyncio.wait_for(self._session_active_events[-1].wait(), timeout=5)
            if self._session.session_status != GattSessionStatus.ACTIVE:
                raise RuntimeError("WinRT session closed before service discovery")
            print("WINDOWS_BLE_WINRT:ACTIVE_BEFORE_DISCOVERY", flush=True)
            return await super()._get_services(*args, **kwargs)

    CLIENT_OPTIONS["backend"] = WaitActiveClient


async def wait_stable_session(session, active_status, stable_seconds=1.0, timeout=5.0):
    """Opt-in lifecycle control: require an event-free ACTIVE interval."""
    loop = asyncio.get_running_loop()
    changed = asyncio.Event()

    def on_change(_sender, _args):
        loop.call_soon_threadsafe(changed.set)

    token = session.add_session_status_changed(on_change)
    deadline = loop.time() + timeout
    try:
        while True:
            changed.clear()
            active = session.session_status == active_status
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise TimeoutError("WinRT session did not become stable")
            interval = min(stable_seconds, remaining) if active else remaining
            try:
                await asyncio.wait_for(changed.wait(), interval)
            except asyncio.TimeoutError:
                if (active and interval == stable_seconds
                        and session.session_status == active_status):
                    return
                raise TimeoutError("WinRT session did not become stable")
    finally:
        session.remove_session_status_changed(token)


def enable_stable_active_control():
    from bleak.backends.winrt.client import BleakClientWinRT, GattSessionStatus

    class StableActiveClient(CLIENT_OPTIONS.get("backend", BleakClientWinRT)):
        async def _get_services(self, *args, **kwargs):
            await wait_stable_session(self._session, GattSessionStatus.ACTIVE)
            print("WINDOWS_BLE_WINRT:STABLE_ACTIVE_CONTROL=1000", flush=True)
            return await super()._get_services(*args, **kwargs)

    CLIENT_OPTIONS["backend"] = StableActiveClient


def enable_session_diagnostics():
    # LogRecord.relativeCreated uses logging's epoch. Export only its numeric
    # UTC anchor so ETW and WinRT timelines can be compared across processes.
    print(f"WINDOWS_BLE_WINRT:CLOCK_ORIGIN_UNIX_MS={logging._startTime * 1000:.3f}", flush=True)
    class SessionHandler(logging.Handler):
        retries = 0

        def emit(self, record):
            # Whitelist status enums; omit device IDs, addresses and payloads.
            if record.msg.startswith("session_status_changed_event_handler:"):
                print(f"WINDOWS_BLE_WINRT:SESSION={int(record.relativeCreated)},"
                      f"{int(record.args[1])},{int(record.args[2])}", flush=True)
            elif record.msg.startswith("getting services ("):
                print(f"WINDOWS_BLE_WINRT:GET_SERVICES={int(record.relativeCreated)}", flush=True)
            elif record.msg == "%s: services changed":
                print(f"WINDOWS_BLE_WINRT:SERVICES_CHANGED={int(record.relativeCreated)}", flush=True)
            elif record.msg == "%s: restarting get services due to services changed event":
                print(f"WINDOWS_BLE_WINRT:SERVICES_REFRESH={int(record.relativeCreated)}", flush=True)
            elif record.msg.startswith("%s: device unreachable when getting service"):
                self.retries += 1
                print(f"WINDOWS_BLE_WINRT:DISCOVERY_RETRY={self.retries}", flush=True)
    logger = logging.getLogger("bleak.backends.winrt.client")
    logger.setLevel(logging.DEBUG)
    logger.propagate = False
    logger.addHandler(SessionHandler())


def payload(generation):
    return bytes((0x31 + generation * 0x47 + offset * 0x13) & 255 for offset in range(512))


async def event_gate(device, mode, characteristic):
    if mode == "long-notify":
        expected = [bytes((0x2d + sequence * 0x3b + offset * 0x11) & 255
                          for offset in range(60)) for sequence in range(3)]
    else:
        expected = [bytes([value]) for value in
                    ((0x19, 0x3b, 0x5d) if mode == "indicate" else (0x31, 0x52, 0x73))]
    async with BleakClient(device, timeout=20, winrt=ADDRESS_OPTIONS, **CLIENT_OPTIONS) as client:
        for subscription in range(1, 3):
            queue = asyncio.Queue()

            def received(_characteristic, data):
                queue.put_nowait(bytes(data))

            await client.start_notify(characteristic, received)
            for index, value in enumerate(expected, 1):
                if await asyncio.wait_for(queue.get(), timeout=10) != value:
                    raise RuntimeError(f"{mode} payload mismatch")
                print(f"WINDOWS_BLE_EVENTS:RECEIVED={subscription},{index},{len(value)}", flush=True)
            await client.stop_notify(characteristic)
            await asyncio.sleep(0.5)
            if not queue.empty():
                raise RuntimeError("Unexpected extra BLE events")
    print("WINDOWS_BLE_EVENTS:RESULT=PASS", flush=True)


async def wait_signal(path, timeout=45):
    deadline = asyncio.get_running_loop().time() + timeout
    while not path.exists():
        if asyncio.get_running_loop().time() >= deadline:
            raise RuntimeError("Peripheral confirmation timed out: " + path.name)
        await asyncio.sleep(0.1)


async def discovery_boundary(signals, session):
    (signals / f"connected-{session}.signal").write_text("CONNECTED\n", encoding="ascii")
    await wait_signal(signals / f"discovery-snapshot-{session}.signal")


async def hold_connected(client, generation, seconds):
    if not seconds:
        return
    loop = asyncio.get_running_loop()
    started = loop.time()
    deadline = started + seconds
    print(f"WINDOWS_BLE_COEX:HOLD_BEGIN={generation},{seconds}", flush=True)
    while loop.time() < deadline:
        if not client.is_connected:
            raise RuntimeError("BLE disconnected during hold")
        await asyncio.sleep(min(1, deadline - loop.time()))
    if not client.is_connected:
        raise RuntimeError("BLE disconnected after hold")
    if bytes(await client.read_gatt_char(CHARACTERISTIC, use_cached=False)) != payload(generation):
        raise RuntimeError("Connected hold readback mismatch")
    print(f"WINDOWS_BLE_COEX:HOLD_PASS={generation},{int((loop.time() - started) * 1000)}", flush=True)


async def connected_coex_gate(device, signals, cycles, sessions, targeted_services=False,
                              snapshot_discovery=False, cycle_hold_seconds=0):
    generation = 0
    discovery = {"services": [SERVICE]} if targeted_services else {}
    print("WINDOWS_BLE_COEX:DISCOVERY=" + ("TARGETED" if targeted_services else "ALL"), flush=True)
    for session in range(1, sessions + 1):
        if session > 1:
            device = await find_coex_device()
        async with BleakClient(device, timeout=20, winrt={"use_cached_services": False, **ADDRESS_OPTIONS}, **discovery, **CLIENT_OPTIONS) as client:
            if snapshot_discovery:
                await discovery_boundary(signals, session)
            for _ in range(cycles // sessions):
                if bytes(await client.read_gatt_char(CHARACTERISTIC, use_cached=False)) != payload(generation):
                    raise RuntimeError("Connected coexistence read mismatch")
                generation += 1
                await client.write_gatt_char(CHARACTERISTIC, payload(generation), response=True)
                await wait_signal(signals / f"cycle-{generation}.signal")
                if bytes(await client.read_gatt_char(CHARACTERISTIC, use_cached=False)) != payload(generation):
                    raise RuntimeError("Connected coexistence readback mismatch")
                await hold_connected(client, generation, cycle_hold_seconds)
                print(f"WINDOWS_BLE_COEX:CYCLE_PASS={generation}", flush=True)
            # Set this before closing so a discovery retry's earlier link loss
            # cannot acknowledge the end of this logical data session.
            (signals / f"disconnect-request-{session}.signal").write_text("REQUEST\n", encoding="ascii")
        await wait_signal(signals / f"disconnect-{session}.signal")
        print(f"WINDOWS_BLE_COEX:SESSION_PASS={session}", flush=True)
    device = await find_coex_device()
    async with BleakClient(device, timeout=20, winrt={"use_cached_services": False, **ADDRESS_OPTIONS}, **discovery, **CLIENT_OPTIONS) as client:
        if snapshot_discovery:
            await discovery_boundary(signals, sessions + 1)
        if bytes(await client.read_gatt_char(CHARACTERISTIC, use_cached=False)) != payload(cycles):
            raise RuntimeError("Connected coexistence reconnect readback mismatch")
        (signals / f"disconnect-request-{sessions + 1}.signal").write_text("REQUEST\n", encoding="ascii")
    print("WINDOWS_BLE_COEX:RESULT=PASS", flush=True)


async def find_coex_device():
    device = await BleakScanner.find_device_by_filter(
        lambda _device, advert: SERVICE in [uuid.lower() for uuid in advert.service_uuids],
        timeout=30,
    )
    if device is None:
        raise RuntimeError("Connected peripheral did not resume advertising")
    print("WINDOWS_BLE_COEX:ADVERTISEMENT_REACQUIRED", flush=True)
    return device


async def discovery_gate(device, signals, sessions, snapshot_discovery=False):
    """Repeat uncached full discovery without Wi-Fi or GATT writes/retries."""
    for session in range(1, sessions + 1):
        if session > 1:
            device = await find_coex_device()
        print(f"WINDOWS_BLE_DISCOVERY:CONNECT_START={session}", flush=True)
        async with BleakClient(device, timeout=20, winrt={"use_cached_services": False, **ADDRESS_OPTIONS}, **CLIENT_OPTIONS) as client:
            if snapshot_discovery:
                await discovery_boundary(signals, session)
            if bytes(await client.read_gatt_char(CHARACTERISTIC, use_cached=False)) != payload(0):
                raise RuntimeError("Discovery-only read mismatch")
        await wait_signal(signals / f"disconnect-{session}.signal")
        print(f"WINDOWS_BLE_DISCOVERY:SESSION_PASS={session}", flush=True)
    print("WINDOWS_BLE_DISCOVERY:RESULT=PASS", flush=True)


async def main(disconnect_signal, mode, coex_signals=None, cycles=8, sessions=1, targeted_services=False,
               discovery_sessions=0, snapshot_discovery=False, cycle_hold_seconds=0):
    suffixes = {"long-gatt": "4", "notify": "1", "indicate": "2", "long-notify": "3"}
    service = "9d1f1c20-7df8-4f79-9b7c-5a2b2bc9c0" + suffixes[mode] + "1"
    characteristic = service[:-1] + "2"
    device = await BleakScanner.find_device_by_filter(
        lambda _device, advert: service in [uuid.lower() for uuid in advert.service_uuids],
        timeout=30,
    )
    if device is None:
        raise RuntimeError("Dedicated long-GATT service was not found")
    if ADDRESS_OPTIONS:
        raw = device.details.adv or device.details.scan
        kind = getattr(raw, "bluetooth_address_type", None)
        print("WINDOWS_BLE_WINRT:ADVERTISED_ADDRESS_TYPE="
              + (str(int(kind)) if kind is not None else "UNAVAILABLE"), flush=True)
    if mode != "long-gatt":
        await event_gate(device, mode, characteristic)
        return
    print("WINDOWS_BLE_LONG_GATT:TARGET_FOUND", flush=True)
    if discovery_sessions:
        await discovery_gate(device, coex_signals, discovery_sessions, snapshot_discovery)
        return
    if coex_signals is not None:
        await connected_coex_gate(device, coex_signals, cycles, sessions, targeted_services, snapshot_discovery, cycle_hold_seconds)
        return
    async with BleakClient(device, timeout=20, winrt=ADDRESS_OPTIONS, **CLIENT_OPTIONS) as client:
        for generation in range(8):
            actual = await client.read_gatt_char(CHARACTERISTIC, use_cached=False)
            if bytes(actual) != payload(generation):
                raise RuntimeError(f"512-byte read mismatch in cycle {generation + 1}")
            await client.write_gatt_char(CHARACTERISTIC, payload(generation + 1), response=True)
            await asyncio.sleep(0.2)
            actual = await client.read_gatt_char(CHARACTERISTIC, use_cached=False)
            if bytes(actual) != payload(generation + 1):
                raise RuntimeError(f"512-byte readback mismatch in cycle {generation + 1}")
            print(f"WINDOWS_BLE_LONG_GATT:CYCLE_PASS={generation + 1}", flush=True)
    deadline = asyncio.get_running_loop().time() + 45
    while not disconnect_signal.exists():
        if asyncio.get_running_loop().time() >= deadline:
            raise RuntimeError("Peripheral did not confirm physical disconnect")
        await asyncio.sleep(0.2)
    async with BleakClient(device, timeout=20, winrt=ADDRESS_OPTIONS, **CLIENT_OPTIONS) as client:
        if bytes(await client.read_gatt_char(CHARACTERISTIC, use_cached=False)) != payload(8):
            raise RuntimeError("Reconnect readback mismatch")
    print("WINDOWS_BLE_LONG_GATT:RECONNECT_PASS", flush=True)
    print("WINDOWS_BLE_LONG_GATT:RESULT=PASS", flush=True)


if __name__ == "__main__":
    print("WINDOWS_BLE_HOST:BLEAK_VERSION=" + version("bleak"), flush=True)
    parser = argparse.ArgumentParser()
    parser.add_argument("--disconnect-signal", type=Path, required=True)
    parser.add_argument("--mode", choices=("long-gatt", "notify", "indicate", "long-notify"), default="long-gatt")
    parser.add_argument("--coex-signals", type=Path)
    parser.add_argument("--cycle-hold-seconds", type=int, default=0)
    parser.add_argument("--cycles", type=int, default=8)
    parser.add_argument("--sessions", type=int, default=1)
    parser.add_argument("--targeted-service-discovery", action="store_true")
    parser.add_argument("--discovery-sessions", type=int, default=0)
    parser.add_argument("--snapshot-discovery", action="store_true")
    parser.add_argument("--winrt-session-diagnostics", action="store_true")
    parser.add_argument("--winrt-failure-diagnostics", action="store_true")
    parser.add_argument("--no-discovery-retry-control", action="store_true")
    parser.add_argument("--services-changed-control", action="store_true")
    parser.add_argument("--release-session-control", action="store_true")
    parser.add_argument("--service-cache-control", action="store_true")
    parser.add_argument("--wait-active-control", action="store_true")
    parser.add_argument("--stable-active-control", action="store_true")
    parser.add_argument("--winrt-address-type", choices=("public", "random"))
    args = parser.parse_args()
    if not 0 <= args.cycle_hold_seconds <= 60 or (args.cycle_hold_seconds and (
            args.mode != "long-gatt" or args.coex_signals is None or args.discovery_sessions)):
        parser.error("cycle hold needs connected coexistence and 0..60 seconds")
    if args.no_discovery_retry_control and (not args.winrt_failure_diagnostics
            or not args.discovery_sessions or args.services_changed_control
            or args.service_cache_control or args.targeted_service_discovery):
        parser.error("no-retry control needs full discovery sessions and failure diagnostics only")
    if args.winrt_session_diagnostics:
        enable_session_diagnostics()
    if args.wait_active_control:
        enable_wait_active_control()
    if args.stable_active_control:
        enable_stable_active_control()
    if args.winrt_failure_diagnostics:
        enable_failure_diagnostics(reject_service_unreachable=args.no_discovery_retry_control)
    if args.no_discovery_retry_control:
        print("WINDOWS_BLE_WINRT:NO_DISCOVERY_RETRY_CONTROL=ON", flush=True)
    if args.services_changed_control:
        enable_services_changed_control()
        print("WINDOWS_BLE_WINRT:SERVICES_CHANGED_CONTROL=ON", flush=True)
    if args.release_session_control:
        enable_release_session_control()
        print("WINDOWS_BLE_WINRT:RELEASE_SESSION_CONTROL=ON", flush=True)
    if args.service_cache_control:
        enable_service_cache_control()
        print("WINDOWS_BLE_WINRT:SERVICE_CACHE_CONTROL=ON", flush=True)
    if args.winrt_address_type:
        ADDRESS_OPTIONS["address_type"] = args.winrt_address_type
        print("WINDOWS_BLE_WINRT:REQUESTED_ADDRESS_TYPE=" + args.winrt_address_type, flush=True)
    if args.snapshot_discovery and args.coex_signals is None:
        parser.error("discovery snapshots need coex-signals")
    if args.discovery_sessions and (args.mode != "long-gatt" or args.coex_signals is None
                                    or not 1 <= args.discovery_sessions <= 200):
        parser.error("discovery-only needs long-gatt, coex-signals and 1..200 sessions")
    if args.coex_signals is not None and (
            args.mode != "long-gatt" or not 1 <= args.cycles <= 200
            or args.sessions < 1 or args.cycles % args.sessions != 0):
        parser.error("coexistence needs long-gatt, 1..200 cycles divisible by sessions")
    asyncio.run(main(args.disconnect_signal, args.mode, args.coex_signals, args.cycles,
                     args.sessions, args.targeted_service_discovery, args.discovery_sessions,
                     args.snapshot_discovery, args.cycle_hold_seconds))
