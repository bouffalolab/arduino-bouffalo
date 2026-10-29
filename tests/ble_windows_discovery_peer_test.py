"""Discovery comparison must preserve failures and physical disconnect ordering."""
import contextlib
import asyncio
import importlib.util
import io
import logging
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch

script = Path(__file__).resolve().parents[1] / "tools/bl616cl_windows_ble_long_gatt_peer.py"
sys.path.insert(0, str(script.parent))
spec = importlib.util.spec_from_file_location("discovery_peer", script)
peer = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {"bleak": types.SimpleNamespace(BleakClient=None, BleakScanner=None)}):
    spec.loader.exec_module(peer)


class ConnectedHoldTest(unittest.IsolatedAsyncioTestCase):
    async def exercise(self, *, disconnect=False, bad=False, cancel=False):
        clock = [10.0]
        client = types.SimpleNamespace(is_connected=True,
            read_gatt_char=AsyncMock(return_value=b"bad" if bad else peer.payload(7)))
        async def sleep(seconds):
            clock[0] += seconds
            if disconnect:
                client.is_connected = False
            if cancel:
                raise asyncio.CancelledError()
        fake_loop = types.SimpleNamespace(time=lambda: clock[0])
        with patch.object(peer.asyncio, "get_running_loop", return_value=fake_loop), \
                patch.object(peer.asyncio, "sleep", side_effect=sleep), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            try:
                await peer.hold_connected(client, 7, 3)
            except BaseException as error:
                return client, output.getvalue(), error
        return client, output.getvalue(), None

    async def test_holds_entire_duration_and_reads_uncached(self):
        client, output, error = await self.exercise()
        self.assertIsNone(error)
        client.read_gatt_char.assert_awaited_once_with(peer.CHARACTERISTIC, use_cached=False)
        self.assertIn("HOLD_PASS=7,3000", output)

    async def test_disconnect_bad_read_and_cancel_cannot_pass(self):
        for options in ({"disconnect": True}, {"bad": True}, {"cancel": True}):
            _, output, error = await self.exercise(**options)
            self.assertIsInstance(error, asyncio.CancelledError if options.get("cancel") else RuntimeError)
            self.assertNotIn("HOLD_PASS", output)

    async def test_default_zero_does_not_touch_client(self):
        await peer.hold_connected(None, 1, 0)


class DiscoveryTest(unittest.IsolatedAsyncioTestCase):
    async def exercise(self, *, bad_value=False, connect_error=False, snapshot=False):
        events = []
        calls = []

        class Client:
            def __init__(self, device, **kwargs):
                calls.append((device, kwargs))

            async def __aenter__(self):
                events.append("connect")
                if connect_error:
                    raise RuntimeError("Unreachable")
                return self

            async def read_gatt_char(self, characteristic, **kwargs):
                if characteristic != peer.CHARACTERISTIC or kwargs != {"use_cached": False}:
                    raise AssertionError("Unexpected characteristic or cached read")
                events.append("read")
                return b"bad" if bad_value else peer.payload(0)

            async def __aexit__(self, *exc):
                events.append("close")

        async def wait_signal(path):
            events.append(path.name)

        async def boundary(signals, session):
            events.append(f"snapshot-{session}")

        with patch.object(peer, "BleakClient", Client), \
                patch.object(peer, "find_coex_device", AsyncMock(return_value="again")), \
                patch.object(peer, "discovery_boundary", boundary), \
                patch.object(peer, "wait_signal", wait_signal), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            error = None
            try:
                await peer.discovery_gate("first", Path("signals"), 3, snapshot)
            except RuntimeError as caught:
                error = caught
        return calls, events, output.getvalue(), error

    async def test_uncached_each_session_and_disconnect_before_reconnect(self):
        calls, events, output, error = await self.exercise()
        self.assertIsNone(error)
        self.assertEqual([device for device, _ in calls], ["first", "again", "again"])
        for _, options in calls:
            self.assertEqual(options, {"timeout": 20, "winrt": {"use_cached_services": False}})
        self.assertEqual(events, [item for n in range(1, 4)
                                  for item in ("connect", "read", "close", f"disconnect-{n}.signal")])
        self.assertIn("WINDOWS_BLE_DISCOVERY:RESULT=PASS", output)

    async def test_bad_read_aborts_without_retry_or_pass(self):
        calls, events, output, error = await self.exercise(bad_value=True)
        self.assertRegex(str(error), "read mismatch")
        self.assertEqual(len(calls), 1)
        self.assertEqual(events, ["connect", "read", "close"])
        self.assertNotIn("RESULT=PASS", output)

    async def test_service_failure_aborts_without_retry_or_pass(self):
        calls, events, output, error = await self.exercise(connect_error=True)
        self.assertEqual(str(error), "Unreachable")
        self.assertEqual(len(calls), 1)
        self.assertEqual(events, ["connect"])
        self.assertNotIn("RESULT=PASS", output)

    async def test_snapshot_boundary_occurs_after_discovery_before_payload(self):
        _, events, _, error = await self.exercise(snapshot=True)
        self.assertIsNone(error)
        self.assertEqual(events, [item for n in range(1, 4)
                                  for item in ("connect", f"snapshot-{n}", "read", "close",
                                               f"disconnect-{n}.signal")])


class ServiceCacheControlTest(unittest.IsolatedAsyncioTestCase):
    async def test_changes_only_service_list_policy_and_never_retries(self):
        calls = []
        failure = RuntimeError("unreachable")

        class Backend:
            async def _get_services(self, **kwargs):
                calls.append(kwargs)
                if len(calls) == 2:
                    raise failure
                return "services"

        module = types.SimpleNamespace(BleakClientWinRT=Backend,
                                      BluetoothCacheMode=types.SimpleNamespace(CACHED="cached"))
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, {"backend": Backend}, clear=True), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            peer.enable_service_cache_control()
            client = peer.CLIENT_OPTIONS["backend"]()
            self.assertIsInstance(client, Backend)
            self.assertEqual(await client._get_services(service_cache_mode="uncached", cache_mode="uncached"),
                             "services")
            with self.assertRaises(RuntimeError) as caught:
                await client._get_services(service_cache_mode="uncached", cache_mode="uncached")
            self.assertIs(caught.exception, failure)
        self.assertEqual(calls, [dict(service_cache_mode="cached", cache_mode="uncached")] * 2)
        self.assertEqual(output.getvalue().count("SERVICE_LIST_CACHE=CACHED"), 2)


class ReleaseSessionControlTest(unittest.IsolatedAsyncioTestCase):
    async def test_releases_before_original_cleanup_and_keeps_backend(self):
        events = []

        class Session:
            @property
            def maintain_connection(self):
                return True

            @maintain_connection.setter
            def maintain_connection(self, value):
                events.append(("release", value))

        class Backend:
            async def disconnect(self):
                events.append("cleanup")

        module = types.SimpleNamespace(BleakClientWinRT=Backend)
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, {"backend": Backend}, clear=True), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            peer.enable_release_session_control()
            client = peer.CLIENT_OPTIONS["backend"]()
            self.assertIsInstance(client, Backend)
            client._session = Session()
            await client.disconnect()
            self.assertEqual(events, [("release", False), "cleanup"])
            self.assertIn("MAINTAIN_RELEASED", output.getvalue())
            events.clear()
            client._session = None
            await client.disconnect()
            self.assertEqual(events, ["cleanup"])

    async def test_closed_session_error_still_cleans_up_and_fails_gate(self):
        events = []
        failure = RuntimeError("closed session")

        class Session:
            def __setattr__(self, name, value):
                raise failure

        class Backend:
            async def disconnect(self):
                events.append("cleanup")

        module = types.SimpleNamespace(BleakClientWinRT=Backend)
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, clear=True):
            peer.enable_release_session_control()
            client = peer.CLIENT_OPTIONS["backend"]()
            client._session = Session()
            with self.assertRaises(RuntimeError) as caught:
                await client.disconnect()
            self.assertIs(caught.exception, failure)
            self.assertEqual(events, ["cleanup"])


class ServicesChangedControlTest(unittest.TestCase):
    def test_opt_in_keeps_constructor_options_and_existing_backend(self):
        calls = []
        class Backend:
            def __init__(self, *args, **kwargs):
                calls.append((args, kwargs))
                self._retry_on_services_changed = False
        module = types.SimpleNamespace(BleakClientWinRT=Backend)
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, {"backend": Backend}, clear=True):
            peer.enable_services_changed_control()
            client = peer.CLIENT_OPTIONS["backend"]("device", winrt={"use_cached_services": False})
            self.assertIsInstance(client, Backend)
            self.assertTrue(client._retry_on_services_changed)
        self.assertEqual(calls, [(("device",), {"winrt": {"use_cached_services": False}})])


class FailureDiagnosticsTest(unittest.IsolatedAsyncioTestCase):
    async def test_event_driven_cancellation_is_labelled_and_propagated(self):
        class Backend:
            async def _get_services(self, **kwargs):
                raise asyncio.CancelledError()
        module = types.SimpleNamespace(BleakClientWinRT=Backend)
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, clear=True), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            peer.enable_failure_diagnostics()
            with self.assertRaises(asyncio.CancelledError):
                await peer.CLIENT_OPTIONS["backend"]()._get_services()
        self.assertIn("DISCOVERY_CANCELLED=", output.getvalue())
        self.assertNotIn("DISCOVERY_FAILURE=", output.getvalue())

    async def test_failure_preserves_exception_and_cache_policy_without_retry(self):
        failure = RuntimeError("private error message")
        calls = []

        class Backend:
            async def _get_services(self, **kwargs):
                calls.append(kwargs)
                raise failure

        module = types.SimpleNamespace(BleakClientWinRT=Backend)
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, clear=True), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            peer.enable_failure_diagnostics()
            client = peer.CLIENT_OPTIONS["backend"]()
            client._requester = types.SimpleNamespace(connection_status=1, name="private-device")
            client._session = types.SimpleNamespace(session_status=1, max_pdu_size=64)
            with self.assertRaises(RuntimeError) as caught:
                await client._get_services(service_cache_mode="uncached", cache_mode="uncached")
            self.assertIs(caught.exception, failure)
        self.assertEqual(calls, [dict(service_cache_mode="uncached", cache_mode="uncached")])
        self.assertRegex(output.getvalue(), r"DISCOVERY_FAILURE=\d+,1,1,64")
        self.assertNotIn("private", output.getvalue())

    async def test_success_calls_original_once_and_emits_no_failure(self):
        class Backend:
            async def _get_services(self, **kwargs):
                return "services"

        module = types.SimpleNamespace(BleakClientWinRT=Backend)
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, clear=True), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            peer.enable_failure_diagnostics()
            self.assertEqual(await peer.CLIENT_OPTIONS["backend"]()._get_services(), "services")
        self.assertEqual(output.getvalue(), "")


class SessionDiagnosticsTest(unittest.TestCase):
    def test_only_status_enums_are_emitted(self):
        logger = logging.getLogger("bleak.backends.winrt.client")
        handlers, level, propagate = logger.handlers[:], logger.level, logger.propagate
        try:
            logger.handlers = []
            with contextlib.redirect_stdout(io.StringIO()) as output:
                peer.enable_session_diagnostics()
                logger.debug("session_status_changed_event_handler: id: %s, error: %r, status: %r",
                             "private-device-id", 0, 1)
                logger.debug("getting services (service_cache_mode=%r, cache_mode=%r)...", None, None)
                logger.debug("Read payload: %s", "private-value")
                logger.debug("%s: device unreachable when getting services, retrying in 1 second...",
                             "private-device-id")
                logger.debug("%s: device unreachable when getting service %s, retrying in 1 second...",
                             "private-device-id", "private-service-id")
            text = output.getvalue()
            self.assertIn(",0,1", text)
            self.assertIn("GET_SERVICES=", text)
            self.assertIn("DISCOVERY_RETRY=1", text)
            self.assertIn("DISCOVERY_RETRY=2", text)
            self.assertNotIn("private", text)
        finally:
            logger.handlers, logger.level, logger.propagate = handlers, level, propagate


class StableActiveControlTest(unittest.IsolatedAsyncioTestCase):
    class Session:
        session_status = 1
        removed = False

        def add_session_status_changed(self, callback):
            self.callback = callback
            return 17

        def remove_session_status_changed(self, token):
            assert token == 17
            self.removed = True

        def change(self, status):
            self.session_status = status
            self.callback(self, None)

    async def test_close_and_reactivation_restart_quiet_interval(self):
        session = self.Session()
        loop = asyncio.get_running_loop()
        started = loop.time()
        loop.call_later(.01, session.change, 0)
        loop.call_later(.02, session.change, 1)
        await peer.wait_stable_session(session, 1, stable_seconds=.04, timeout=.3)
        self.assertGreaterEqual(loop.time() - started, .055)
        self.assertTrue(session.removed)

    async def test_closed_session_timeout_and_cancellation_remove_handler(self):
        session = self.Session()
        session.session_status = 0
        with self.assertRaises(TimeoutError):
            await peer.wait_stable_session(session, 1, stable_seconds=.01, timeout=.02)
        self.assertTrue(session.removed)
        session.removed = False
        task = asyncio.create_task(peer.wait_stable_session(session, 1))
        await asyncio.sleep(0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(session.removed)

    async def test_original_discovery_failure_is_never_retried(self):
        calls = []
        failure = RuntimeError("unreachable")

        class Backend:
            async def _get_services(self, *args, **kwargs):
                calls.append((args, kwargs))
                raise failure

        module = types.SimpleNamespace(BleakClientWinRT=Backend,
                                       GattSessionStatus=types.SimpleNamespace(ACTIVE=1))
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, clear=True), \
                patch.object(peer, "wait_stable_session", AsyncMock()), \
                contextlib.redirect_stdout(io.StringIO()):
            peer.enable_stable_active_control()
            client = peer.CLIENT_OPTIONS["backend"]()
            client._session = self.Session()
            with self.assertRaises(RuntimeError) as caught:
                await client._get_services(service_cache_mode="uncached")
            self.assertIs(caught.exception, failure)
        self.assertEqual(calls, [((), dict(service_cache_mode="uncached"))])


class WaitActiveControlTest(unittest.IsolatedAsyncioTestCase):
    async def exercise(self, status, activate=False):
        calls = []

        class Backend:
            async def _get_services(self, **kwargs):
                calls.append(kwargs)
                return "services"

        module = types.SimpleNamespace(BleakClientWinRT=Backend,
                                       GattSessionStatus=types.SimpleNamespace(ACTIVE=1))
        with patch.dict(sys.modules, {"bleak.backends.winrt.client": module}), \
                patch.dict(peer.CLIENT_OPTIONS, clear=True), \
                contextlib.redirect_stdout(io.StringIO()):
            peer.enable_wait_active_control()
            client = peer.CLIENT_OPTIONS["backend"]()
            client._session = types.SimpleNamespace(session_status=status)
            event = asyncio.Event()
            client._session_active_events = [event]
            if activate:
                async def activation():
                    await asyncio.sleep(0)
                    client._session.session_status = 1
                    event.set()
                activation_task = asyncio.create_task(activation())
            else:
                event.set()
            try:
                result = await client._get_services(service_cache_mode="uncached")
            except RuntimeError:
                result = "closed"
            if activate:
                await activation_task
        return result, calls

    async def test_active_enumerates_once_with_original_cache_policy(self):
        result, calls = await self.exercise(1)
        self.assertEqual(result, "services")
        self.assertEqual(calls, [{"service_cache_mode": "uncached"}])

    async def test_waits_for_activation_without_retrying(self):
        result, calls = await self.exercise(0, activate=True)
        self.assertEqual(result, "services")
        self.assertEqual(len(calls), 1)

    async def test_stale_active_signal_does_not_enumerate_closed_session(self):
        result, calls = await self.exercise(0)
        self.assertEqual(result, "closed")
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
