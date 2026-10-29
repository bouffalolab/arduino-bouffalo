"""Observe WinRT discovery operation stages without logging identifiers or data."""
import ast
import asyncio
import inspect
import textwrap
import time
import weakref


class TaskStates:
    """Bleak FutureLike may resume a task under another callback Context."""
    def __init__(self):
        self.states = weakref.WeakKeyDictionary()

    def get(self):
        try:
            task = asyncio.current_task()
        except RuntimeError:
            return None
        return self.states.get(task) if task is not None else None

    def set(self, state):
        task = asyncio.current_task()
        if task is None:
            raise RuntimeError("Discovery diagnostics require an asyncio task")
        previous = self.states.get(task)
        self.states[task] = state
        return task, previous

    def reset(self, token):
        task, previous = token
        if previous is None:
            self.states.pop(task, None)
        else:
            self.states[task] = previous


CURRENT = TaskStates()
METHODS = {
    "get_gatt_services_async": "SERVICES",
    "get_gatt_services_with_cache_mode_async": "SERVICES",
    "get_gatt_services_for_uuid_async": "SERVICES",
    "get_gatt_services_for_uuid_with_cache_mode_async": "SERVICES",
    "get_characteristics_async": "CHARACTERISTICS",
    "get_characteristics_with_cache_mode_async": "CHARACTERISTICS",
    "get_descriptors_async": "DESCRIPTORS",
    "get_descriptors_with_cache_mode_async": "DESCRIPTORS",
}


def operation_lines(source, start_line):
    """Map only known FutureLike call sites, never evaluate source or payloads."""
    result = {}
    for call in ast.walk(ast.parse(textwrap.dedent(source))):
        if (isinstance(call, ast.Call) and isinstance(call.func, ast.Name)
                and call.func.id == "FutureLike" and call.args
                and isinstance(call.args[0], ast.Call)
                and isinstance(call.args[0].func, ast.Attribute)):
            stage = METHODS.get(call.args[0].func.attr)
            if stage:
                result[start_line + call.lineno - 1] = stage
    return result


class State:
    def __init__(self, reject_service_unreachable=False):
        self.reject_service_unreachable = reject_service_unreachable
        self.stage = "UNKNOWN"
        self.calls = 0
        self.last_status = -1
        self.started = time.monotonic()

    def line(self):
        return (f"WINDOWS_BLE_WINRT:DISCOVERY_STAGE={self.stage},{self.calls},"
                f"{self.last_status},{int((time.monotonic() - self.started) * 1000)}")


def install(backend_module):
    original = backend_module.FutureLike
    if getattr(original, "_ble_stage_observer", False):
        return
    method = backend_module.BleakClientWinRT._get_services
    source, start = inspect.getsourcelines(method)
    sites = operation_lines("".join(source), start)
    if not sites:
        raise RuntimeError("Unsupported WinRT discovery source for stage diagnostics")

    class ObservedFuture(original):
        _ble_stage_observer = True

        def __init__(self, *args, **kwargs):
            state = CURRENT.get()
            frame = inspect.currentframe().f_back
            try:
                stage = sites.get(frame.f_lineno) if frame.f_code is method.__code__ else None
            finally:
                del frame
            self._ble_observation = state if stage else None
            if self._ble_observation is not None:
                state.stage = stage
                state.calls += 1
                state.last_status = -1
                state.started = time.monotonic()
            super().__init__(*args, **kwargs)

        def result(self):
            result = super().result()
            state = self._ble_observation
            if state is not None:
                try:
                    state.last_status = int(result.status)
                except Exception:
                    state.last_status = -1
                if (state.reject_service_unreachable and state.stage == "SERVICES"
                        and state.last_status == 1):
                    # Abort before newer Bleak versions enter their internal
                    # retry loop. Other WinRT operations keep their semantics.
                    raise RuntimeError("Strict control: first service discovery returned Unreachable")
            return result

    backend_module.FutureLike = ObservedFuture
