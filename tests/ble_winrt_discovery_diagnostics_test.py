import asyncio
import contextvars
from pathlib import Path
import sys
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ble_winrt_discovery_diagnostics as diagnostic


class FutureLike:
    def __init__(self, result):
        self.value = result

    def result(self):
        if isinstance(self.value, BaseException):
            raise self.value
        return self.value


class Backend:
    def _get_services(self, requester):
        result = FutureLike(requester.get_gatt_services_async())
        return result


class DiagnosticTest(unittest.IsolatedAsyncioTestCase):
    def test_known_stages_only_and_multiline_calls(self):
        source = '''
async def discover(self):
    first = await FutureLike(
        self._requester.get_gatt_services_with_cache_mode_async(cache))
    second = await FutureLike(service.get_characteristics_async())
    third = await FutureLike(characteristic.get_descriptors_async())
    ignored = await FutureLike(self.read_private_payload())
'''
        self.assertEqual(diagnostic.operation_lines(source, 100),
                         {102: "SERVICES", 104: "CHARACTERISTICS", 105: "DESCRIPTORS"})

    async def test_original_result_and_error_preserved_and_scope_reset(self):
        module = types.SimpleNamespace(FutureLike=FutureLike, BleakClientWinRT=Backend)
        original_global = Backend._get_services.__globals__["FutureLike"]
        diagnostic.install(module)
        observer = module.FutureLike
        diagnostic.install(module)
        self.assertIs(module.FutureLike, observer)
        Backend._get_services.__globals__["FutureLike"] = observer
        try:
            result = types.SimpleNamespace(status=2, private_data="never log")
            requester = types.SimpleNamespace(get_gatt_services_async=lambda: result)
            state = diagnostic.State()
            token = diagnostic.CURRENT.set(state)
            try:
                future = Backend()._get_services(requester)
                self.assertIs(future.result(), result)
                self.assertEqual((state.stage, state.calls, state.last_status), ("SERVICES", 1, 2))
                error = asyncio.CancelledError()
                requester.get_gatt_services_async = lambda: error
                with self.assertRaises(asyncio.CancelledError) as caught:
                    Backend()._get_services(requester).result()
                self.assertIs(caught.exception, error)
                self.assertEqual((state.calls, state.last_status), (2, -1))
                self.assertNotIn("private", state.line())
            finally:
                diagnostic.CURRENT.reset(token)
            self.assertIs(Backend()._get_services(types.SimpleNamespace(
                get_gatt_services_async=lambda: result)).result(), result)
            self.assertEqual(state.calls, 2)
        finally:
            Backend._get_services.__globals__["FutureLike"] = original_global

    async def test_state_and_cleanup_survive_callback_context_change(self):
        state = diagnostic.State()
        token = diagnostic.CURRENT.set(state)
        empty = contextvars.Context()
        self.assertIs(empty.run(diagnostic.CURRENT.get), state)
        empty.run(diagnostic.CURRENT.reset, token)
        self.assertIsNone(diagnostic.CURRENT.get())

    async def test_strict_control_aborts_first_unreachable_before_retry(self):
        module = types.SimpleNamespace(FutureLike=FutureLike, BleakClientWinRT=Backend)
        original = Backend._get_services.__globals__["FutureLike"]
        diagnostic.install(module)
        Backend._get_services.__globals__["FutureLike"] = module.FutureLike
        try:
            result = types.SimpleNamespace(status=1, private_data="never log")
            requester = types.SimpleNamespace(get_gatt_services_async=lambda: result)
            for strict in (False, True):
                state = diagnostic.State(reject_service_unreachable=strict)
                token = diagnostic.CURRENT.set(state)
                try:
                    # Simulate a backend that retries only after reading status.
                    calls = 0
                    def discover():
                        nonlocal calls
                        for _ in range(10):
                            calls += 1
                            Backend()._get_services(requester).result()
                    if strict:
                        with self.assertRaisesRegex(RuntimeError, "first service discovery"):
                            discover()
                        self.assertEqual(calls, 1)
                        self.assertEqual(state.last_status, 1)
                    else:
                        discover()
                        self.assertEqual(calls, 10)
                    requester.get_gatt_services_async = lambda: types.SimpleNamespace(status=0)
                    self.assertEqual(Backend()._get_services(requester).result().status, 0)
                    requester.get_gatt_services_async = lambda: result
                finally:
                    diagnostic.CURRENT.reset(token)
            # A non-discovery FutureLike is outside the scoped operation.
            token = diagnostic.CURRENT.set(diagnostic.State(True))
            try:
                self.assertIs(module.FutureLike(result).result(), result)
            finally:
                diagnostic.CURRENT.reset(token)
        finally:
            Backend._get_services.__globals__["FutureLike"] = original

    async def test_concurrent_tasks_do_not_share_observations(self):
        async def task_work():
            state = diagnostic.State()
            token = diagnostic.CURRENT.set(state)
            try:
                await asyncio.sleep(0)
                self.assertIs(diagnostic.CURRENT.get(), state)
                return state
            finally:
                diagnostic.CURRENT.reset(token)
        first, second = await asyncio.gather(task_work(), task_work())
        self.assertIsNot(first, second)
        self.assertIsNone(diagnostic.CURRENT.get())


if __name__ == "__main__":
    unittest.main()
