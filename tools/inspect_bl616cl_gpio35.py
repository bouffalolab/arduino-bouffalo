#!/usr/bin/env python3
"""Read paired BL616CL bridge diagnostic telemetry over hidraw."""

from __future__ import annotations

import argparse
import array
import fcntl
import os
import time
from pathlib import Path


EXPECTED_VENDOR = "2341"
EXPECTED_PRODUCT = "1002"
EXPECTED_SERIAL = "BL616CL-PROVISIONAL"
REPORT_BYTES = 64
FEATURE_BYTES = REPORT_BYTES + 1
CDC_IN_TELEMETRY_COMMAND = 0xD5
RA4_APPLICATION_RESET_COMMAND = 0xD4
RA4_SAMBA_PROBE_COMMAND = 0xD6
FEATURE_VERSION = 35
AP_START_STAGES = {
    0: "idle",
    1: "set_vif_unknown",
    2: "set_vif_ap",
    3: "ap_config",
    4: "create_network",
    5: "ap_scan",
    6: "vendor_elements",
    7: "enable_network",
    41: "add_vif",
    42: "add_network",
    43: "set_sae_pwe",
    44: "set_network_options",
    45: "set_network_ssid",
    46: "set_network_key",
    47: "interface_add_before_enqueue",
    48: "interface_add_enqueued",
    49: "interface_add_handler_enter",
    50: "interface_add_handler_return",
    51: "interface_add_before_add_iface",
    52: "interface_add_after_add_iface",
    53: "interface_init_before_driver",
    54: "interface_init_after_driver",
    55: "driver_init_before_macsw",
    56: "driver_init_after_macsw",
    67: "driver_set_param_before",
    68: "driver_set_param_after",
    69: "driver_get_ifname_before",
    70: "driver_get_ifname_after",
    71: "radio_name_before",
    72: "radio_name_after",
    73: "radio_add_before",
    74: "radio_add_after",
    75: "wpa_core_init_before",
    76: "wpa_core_init_after",
    77: "wpa_binding_before",
    78: "wpa_binding_after",
    79: "hardware_features_before",
    80: "hardware_features_after",
    81: "driver_capabilities_before",
    82: "driver_capabilities_after",
    83: "driver_init_before",
    84: "driver_init_after",
    85: "driver_mac_address_before",
    86: "driver_mac_address_after",
    87: "driver_clear_keys_before",
    88: "driver_clear_keys_after",
    89: "driver_countermeasures_before",
    90: "driver_countermeasures_after",
    91: "driver_flush_pmkid_before",
    92: "driver_flush_pmkid_after",
    93: "wps_init_before",
    94: "wps_init_after",
    95: "eapol_init_before",
    96: "eapol_init_after",
    97: "ctrl_iface_init_before",
    98: "ctrl_iface_init_after",
    99: "gas_query_init_before",
    100: "gas_query_init_after",
    101: "p2p_init_before",
    102: "p2p_init_after",
    103: "bss_init_before",
    104: "bss_init_after",
    105: "default_scan_ies_before",
    106: "default_scan_ies_after",
    107: "hardware_features_before_link_lock",
    108: "hardware_features_after_link_lock",
    109: "hardware_features_enqueued",
    110: "hardware_features_handler_enter",
    111: "hardware_features_response_header",
    112: "hardware_features_config_prepared",
    113: "hardware_features_response_before_allocate",
    114: "hardware_features_response_allocated",
    115: "hardware_features_response_copied",
    116: "hardware_features_response_queued",
    117: "hardware_features_response_received",
    118: "hardware_features_handler_enter",
    123: "hardware_features_after_response",
    124: "hardware_features_channel_pointer",
    125: "hardware_features_channel_count_before",
    126: "hardware_features_2g4_count_read",
    127: "hardware_features_after_2g4_mode_count",
    128: "hardware_features_5g_count_read",
    129: "hardware_features_mode_count_ready",
    130: "hardware_features_before_modes_allocate",
    131: "hardware_features_modes_allocated",
    132: "hardware_features_before_2g4_11g",
    133: "hardware_features_after_2g4_11g",
    134: "hardware_features_before_2g4_11b",
    135: "hardware_features_after_2g4_11b",
    136: "hardware_features_11g_enter",
    137: "hardware_features_11g_before_channels_allocate",
    138: "hardware_features_11g_channels_allocated",
    139: "hardware_features_11g_before_channels_copy",
    140: "hardware_features_11g_channels_copied",
    141: "hardware_features_11g_before_rates",
    142: "hardware_features_11g_rates_ready",
    143: "hardware_features_11g_before_ht",
    144: "hardware_features_11g_ht_ready",
    145: "hardware_features_11g_before_he",
    146: "hardware_features_11g_he_ready",
    147: "hardware_features_11g_ready",
    245: "hardware_features_before_2g4_11b",
    246: "hardware_features_11b_enter",
    247: "hardware_features_11b_channels_ready",
    248: "hardware_features_11b_before_channels_copy",
    249: "hardware_features_11b_channels_copied",
    251: "hardware_features_11b_before_rates",
    252: "hardware_features_11b_rates_ready",
    253: "hardware_features_11b_ready",
    254: "hardware_features_after_2g4_11b",
    153: "add_network_before_sync",
    154: "add_network_event_enqueued",
    155: "add_network_wait_begin",
    156: "add_network_eloop_selected",
    157: "add_network_eloop_handler_dispatch",
    158: "add_network_ctrl_handler_body",
    159: "add_network_ctrl_process_begin",
    160: "add_network_ctrl_process_return",
    161: "add_network_ctrl_reply_stored",
    162: "add_network_eloop_handler_return",
    163: "add_network_eloop_remove_request",
    164: "add_network_eloop_request_removed",
    165: "add_network_completion_signal",
    166: "add_network_sync_return",
    167: "add_network_global_process_enter",
    168: "add_network_ifname_redirect",
    169: "add_network_interface_matched",
    170: "add_network_interface_process_enter",
    171: "add_network_reply_buffer_allocated",
    172: "add_network_command_dispatch",
    173: "add_network_command_return",
    174: "add_network_helper_enter",
    175: "add_network_supplicant_enter",
    176: "add_network_supplicant_return",
    177: "add_network_config_enter",
    178: "add_network_config_allocate",
    179: "add_network_config_return",
    180: "add_network_notify_enter",
    181: "add_network_notify_return",
    182: "add_network_defaults_enter",
    183: "add_network_defaults_return",
    185: "add_network_reply_format",
    186: "add_network_helper_return",
    187: "add_network_config_list_walk",
    188: "add_network_config_list_walk_complete",
    189: "add_network_config_ssid_allocate",
    190: "add_network_config_ssid_allocated",
    191: "add_network_config_ssid_initialized",
    192: "add_network_config_ssid_linked",
    193: "add_network_config_priority_update",
    194: "add_network_config_priority_update_return",
    197: "add_network_priority_list_enter",
    198: "add_network_priority_list_old_free",
    199: "add_network_priority_list_reset",
    200: "add_network_priority_list_item",
    201: "add_network_priority_list_append",
    202: "add_network_priority_list_append_return",
    203: "add_network_priority_list_complete",
    204: "set_sae_pwe_before_request_allocate",
    205: "set_sae_pwe_request_allocated",
    206: "set_sae_pwe_before_sync",
    207: "set_sae_pwe_sync_return",
    208: "set_sae_pwe_event_allocated",
    209: "set_sae_pwe_event_enqueued",
    210: "set_sae_pwe_wait_begin",
    211: "set_sae_pwe_wait_return",
    212: "set_sae_pwe_eloop_selected",
    213: "set_sae_pwe_eloop_handler_dispatch",
    214: "set_sae_pwe_eloop_handler_return",
    215: "set_sae_pwe_eloop_remove_request",
    216: "set_sae_pwe_eloop_request_removed",
    217: "set_sae_pwe_completion_signal",
    219: "enable_network_wait_begin",
    220: "enable_network_sync_return",
    221: "enable_network_eloop_selected",
    222: "enable_network_eloop_handler_dispatch",
    223: "enable_network_eloop_handler_return",
    224: "enable_network_eloop_remove_request",
    225: "enable_network_completion_signal",
    226: "enable_network_ctrl_process_begin",
    227: "enable_network_ctrl_process_return",
    228: "enable_network_reply_stored",
    229: "enable_network_request_removed",
    230: "enable_network_before_mutex_unlock",
    231: "enable_network_before_completion_callback",
    232: "enable_network_completion_callback_return",
    233: "enable_network_before_sync_wait",
    234: "enable_network_before_signal",
    235: "enable_network_signal_return",
    236: "enable_network_completion_boundary_return",
    237: "enable_network_sync_wait_return",
    238: "set_network_execute_entry",
    239: "set_network_before_request_allocate",
    240: "set_network_request_allocated",
    241: "set_network_before_ifname_prefix",
    242: "set_network_ifname_prefix_formatted",
    243: "set_network_before_command_format",
    244: "set_network_command_formatted",
    57: "macsw_init_enter",
    58: "macsw_before_vif_update",
    59: "macsw_after_vif_update",
    60: "macsw_before_cfgmacsw",
    61: "macsw_after_cfgmacsw",
    62: "cfgmacsw_before_link_lock",
    63: "cfgmacsw_after_link_lock",
    64: "cfgmacsw_enqueued",
    65: "cfgmacsw_response_received",
    66: "interface_add_complete",
}

AP_START_COMMANDS = {
    0: "none",
    1: "interface_add",
    2: "interface_remove",
    3: "add_network",
    4: "set_sae_pwe",
    5: "set_country",
    6: "set_non_pref_chan",
    7: "set_network_key_mgmt",
    8: "set_network_mode",
    9: "set_network_ssid",
    10: "set_network_psk",
    11: "set_network_generic",
    12: "ap_scan",
    13: "enable_network",
    14: "other_ap_command",
}

AP_START_COMMAND_PHASES = {
    0: "idle",
    1: "entered",
    2: "before_sync_wait",
    3: "sync_returned",
    4: "leaving",
}


def read_attribute(path: Path, name: str) -> str | None:
    try:
        return (path / name).read_text(encoding="ascii").strip()
    except OSError:
        return None


def hid_ioc(direction: int, number: int, length: int) -> int:
    return (
        (direction << 30)
        | (ord("H") << 8)
        | number
        | (length << 16)
    )


def find_hidraw() -> str:
    devices = [
        entry
        for entry in Path("/sys/bus/usb/devices").iterdir()
        if read_attribute(entry, "idVendor") == EXPECTED_VENDOR
        and read_attribute(entry, "idProduct") == EXPECTED_PRODUCT
        and read_attribute(entry, "serial") == EXPECTED_SERIAL
    ]
    if len(devices) != 1:
        raise RuntimeError(
            "expected exactly one paired BL616CL USB device, "
            f"found {len(devices)}"
        )

    prefix = f"{devices[0].name}:"
    interfaces = {
        interface.resolve()
        for interface in Path("/sys/bus/usb/devices").iterdir()
        if interface.name.startswith(prefix)
        and read_attribute(interface, "bInterfaceClass") == "03"
    }
    for hidraw in sorted(Path("/sys/class/hidraw").glob("hidraw*")):
        # USB/IP exposes hidraw through the HID-device directory rather than
        # below the USB interface, so compare the resolved parent directory.
        hid_device = (hidraw / "device").resolve()
        if hid_device.parent in interfaces:
            return f"/dev/{hidraw.name}"
    raise RuntimeError("paired BL616CL CMSIS-DAP HID node was not found")


def read_u32_le(report: bytes, offset: int) -> int:
    return int.from_bytes(report[offset : offset + 4], "little")


def select_feature_page(fd: int, page: int) -> None:
    feature = array.array("B", [0, CDC_IN_TELEMETRY_COMMAND, page])
    fcntl.ioctl(fd, hid_ioc(3, 0x06, len(feature)), feature, True)

def request_ra4_application_reset(fd: int) -> None:
    feature = array.array("B", [0, RA4_APPLICATION_RESET_COMMAND, 0])
    fcntl.ioctl(fd, hid_ioc(3, 0x06, len(feature)), feature, True)

def request_ra4_samba_probe(fd: int) -> None:
    feature = array.array("B", [0, RA4_SAMBA_PROBE_COMMAND, 1])
    fcntl.ioctl(fd, hid_ioc(3, 0x06, len(feature)), feature, True)


def read_feature(fd: int) -> bytes:
    feature = array.array("B", [0]) + array.array("B", bytes(REPORT_BYTES))
    fcntl.ioctl(fd, hid_ioc(3, 0x07, FEATURE_BYTES), feature, True)
    return bytes(feature[1:])


def print_cdc_in_telemetry(report: bytes) -> None:
    print(
        "CDC_IN="
        f"configured={report[6]} state={report[7]} "
        f"pending={int.from_bytes(report[10:12], 'little')} "
        f"submits={read_u32_le(report, 12)} "
        f"submit_failures={read_u32_le(report, 16)} "
        f"completions={read_u32_le(report, 20)} "
        f"completion_bytes={read_u32_le(report, 24)}"
    )
    print(
        "CDC_IN_RECOVERY="
        f"recoveries={read_u32_le(report, 28)} "
        f"zlp_submits={read_u32_le(report, 32)} "
        f"zlp_failures={read_u32_le(report, 36)} "
        f"submitted_bytes={read_u32_le(report, 56)}"
    )
    print(
        "USB_EVENTS="
        f"configured_generation={read_u32_le(report, 40)} "
        f"reset={read_u32_le(report, 44)} "
        f"disconnected={read_u32_le(report, 48)} "
        f"errors={read_u32_le(report, 52)} "
        f"sequence={read_u32_le(report, 60)}"
    )

def print_hid_data_path_telemetry(report: bytes) -> None:
    print(
        "HID_DATA_PATH="
        f"configured={report[6]} tx_busy={report[7]} rx_armed={report[8]} "
        f"last_out_length={report[9]} "
        f"last_request=0x{report[10]:02x} "
        f"last_response=0x{report[11]:02x} "
        f"last_in_submit_result={int.from_bytes(report[12:13], 'little', signed=True)} "
        f"last_read_rearm_result={int.from_bytes(report[13:14], 'little', signed=True)}"
    )
    print(
        "HID_DATA_PATH_COUNTS="
        f"out_callbacks={read_u32_le(report, 16)} "
        f"invalid_packets={read_u32_le(report, 20)} "
        f"busy_drops={read_u32_le(report, 24)} "
        f"dap_requests={read_u32_le(report, 28)} "
        f"in_submits={read_u32_le(report, 32)} "
        f"in_submit_failures={read_u32_le(report, 36)} "
        f"in_completions={read_u32_le(report, 40)} "
        f"read_rearms={read_u32_le(report, 44)} "
        f"read_rearm_failures={read_u32_le(report, 48)} "
        f"usb_event_sequence={read_u32_le(report, 52)}"
    )

def print_ap_start_telemetry(report: bytes) -> None:
    native_result = int.from_bytes(report[7:8], "little", signed=True)
    sta_channel_result = int.from_bytes(report[13:14], "little", signed=True)
    fhost_stage = report[15]
    print(
        "AP_START="
        f"stage={report[6]} native_result={native_result} "
        f"sta_vif={report[8]} ap_vif={report[9]} "
        f"sta_connected={report[10]} ap_started={report[11]}"
    )
    print(
        "AP_START_CHANNEL="
        f"requested={report[12]} sta_query_result={sta_channel_result} "
        f"sta_channel={report[14]} fhost_stage={fhost_stage} "
        f"fhost_stage_name={AP_START_STAGES.get(fhost_stage, 'unknown')} "
        f"fhost_result={int.from_bytes(report[31:32], 'little', signed=True)} "
        f"persisted={report[30]}"
    )
    print(
        "AP_START_TIMING="
        f"attempts={read_u32_le(report, 16)} "
        f"elapsed_ms={read_u32_le(report, 20)} "
        f"started_at_ms={read_u32_le(report, 24)} "
        f"completed_at_ms={read_u32_le(report, 28)}"
    )
    print(
        "AP_START_CHANNEL_POINTERS="
        f"fhost_global=0x{read_u32_le(report, 32):08x} "
        f"handler=0x{read_u32_le(report, 36):08x} "
        f"response_source=0x{read_u32_le(report, 40):08x} "
        f"response_copy=0x{read_u32_le(report, 44):08x} "
        f"response_received=0x{read_u32_le(report, 48):08x} "
        f"wpa_local=0x{read_u32_le(report, 52):08x}"
    )
    print(
        "AP_START_CHANNEL_COUNTS="
        f"chan2g4={read_u32_le(report, 56)} "
        f"chan5g={read_u32_le(report, 60)}"
    )

def parse_runtime_heap(report: bytes) -> list[dict[str, int]]:
    if len(report) != REPORT_BYTES or report[:6] != b"U35D" + bytes((FEATURE_VERSION, 39)):
        raise RuntimeError("unexpected runtime heap feature page")
    if read_u32_le(report, 6) == 0:
        raise RuntimeError("runtime heap snapshot not ready")
    rows = []
    for index in range(2):
        offset = 14 + index * 24
        row = {"flags": report[offset], "allocator": report[offset + 1],
               "free_nodes": int.from_bytes(report[offset + 2:offset + 4], "little")}
        for name, delta in (("counter_free", 4), ("walk_free", 8), ("max_block", 12),
                            ("used", 16), ("total", 20)):
            row[name] = read_u32_le(report, offset + delta)
        rows.append(row)
    return rows


def print_runtime_heap(report: bytes) -> None:
    rows = parse_runtime_heap(report)
    print(f"RUNTIME_HEAP:SEQUENCE={read_u32_le(report, 6)}:MILLIS={read_u32_le(report, 10)}")
    for name, row in zip(("OCRAM", "WRAM"), rows):
        print("RUNTIME_HEAP:" + name + "=" + ",".join(f"{key}:{value}" for key, value in row.items()))
        if row["flags"] != 63:
            raise RuntimeError("runtime heap allocator query unavailable")
        if row["counter_free"] != row["walk_free"]:
            raise RuntimeError("runtime heap counter and traversal disagree")
        if row["max_block"] > row["walk_free"] or row["walk_free"] + row["used"] != row["total"]:
            raise RuntimeError("runtime heap traversal inconsistent")
    print("RUNTIME_HEAP_TELEMETRY_PASS")


def print_ap_start_heap_telemetry(report: bytes) -> None:
    print(
        "AP_START_HEAP="
        f"fhost_stage={report[6]} "
        f"fhost_stage_name={AP_START_STAGES.get(report[6], 'unknown')} "
        f"fhost_result={int.from_bytes(report[7:8], 'little', signed=True)} "
        f"probe_stage={report[8]} persisted={report[9]} "
        f"malloc_failed={report[10]} allocator_stage={report[11]}"
    )
    print(
        "AP_START_HEAP_BEFORE="
        f"request={read_u32_le(report, 12)} "
        f"total={read_u32_le(report, 16)} "
        f"ocram={read_u32_le(report, 20)} "
        f"wram={read_u32_le(report, 24)}"
    )
    print(
        "AP_START_HEAP_AFTER="
        f"address=0x{read_u32_le(report, 28):08x} "
        f"total={read_u32_le(report, 32)} "
        f"ocram={read_u32_le(report, 36)} "
        f"wram={read_u32_le(report, 40)}"
    )
    print(
        "AP_START_HEAP_TIMING="
        f"attempts={read_u32_le(report, 44)} "
        f"elapsed_ms={read_u32_le(report, 48)}"
    )


def print_ap_start_notification_telemetry(report: bytes) -> None:
    event_mask = report[7]
    print(
        "AP_START_NOTIFICATION="
        f"persisted={report[6]} "
        f"events=0x{event_mask:02x} "
        f"callback_before={int(bool(event_mask & 0x01))} "
        f"callback_after={int(bool(event_mask & 0x02))} "
        f"wait_before={int(bool(event_mask & 0x04))} "
        f"wait_return={int(bool(event_mask & 0x08))} "
        f"last_event={report[8]} "
        f"notification_result={int.from_bytes(report[9:10], 'little', signed=True)} "
        f"wait_result={int.from_bytes(report[10:11], 'little', signed=True)} "
        f"attempts={read_u32_le(report, 12)} "
        f"last_sequence={read_u32_le(report, 16)} "
        f"waiting_task=0x{read_u32_le(report, 20):08x}"
    )

def print_ap_start_trace_telemetry(report: bytes) -> None:
    event_names = {
        1: "semaphore_wait_before",
        2: "semaphore_callback_before",
        3: "semaphore_callback_after",
        4: "semaphore_wait_return",
        5: "notification_callback_before",
        6: "notification_callback_after",
        7: "notification_wait_before",
        8: "notification_wait_return",
    }
    print(
        "AP_START_TRACE="
        f"persisted={report[6]} records={report[7]} "
        f"next_sequence={read_u32_le(report, 8)}"
    )
    for index in range(report[7]):
        offset = 12 + index * 14
        event = report[offset + 4]
        print(
            "AP_START_TRACE_RECORD="
            f"sequence={read_u32_le(report, offset)} "
            f"event={event} event_name={event_names.get(event, 'unknown')} "
            f"result={int.from_bytes(report[offset + 5:offset + 6], 'little', signed=True)} "
            f"handle=0x{read_u32_le(report, offset + 6):08x} "
            f"count={read_u32_le(report, offset + 10)}"
        )

def print_ap_start_notification_ab_telemetry(report: bytes) -> None:
    phase_names = {
        1: "request_classified",
        2: "message_initialized",
        3: "event_queue_before",
        4: "event_queue_returned",
        5: "callback_observed",
        6: "callback_notification_branch",
        7: "callback_semaphore_branch",
        8: "caller_notification_wait",
    }
    phase_mask = report[7]
    phases = ",".join(
        phase_names[phase]
        for phase in range(1, 9)
        if phase_mask & (1 << (phase - 1))
    )
    print(
        "AP_START_NOTIFICATION_AB="
        f"persisted={report[6]} phases=0x{phase_mask:02x} "
        f"phase_names={phases or 'none'} "
        f"last_phase={report[8]} "
        f"last_phase_name={phase_names.get(report[8], 'unknown')} "
        f"request_matches={report[14]} "
        f"last_result={int.from_bytes(report[13:14], 'little', signed=True)} "
        f"updates={read_u32_le(report, 16)}"
    )
    print(
        "AP_START_NOTIFICATION_AB_FLAGS="
        f"setup={report[9]} queued={report[10]} "
        f"callback={report[11]} wait={report[12]}"
    )
    print(
        "AP_START_NOTIFICATION_AB_MESSAGES="
        f"setup=0x{read_u32_le(report, 20):08x} "
        f"queued=0x{read_u32_le(report, 24):08x} "
        f"callback=0x{read_u32_le(report, 28):08x} "
        f"wait=0x{read_u32_le(report, 32):08x}"
    )
    print(
        "AP_START_NOTIFICATION_AB_TASKS="
        f"setup=0x{read_u32_le(report, 36):08x} "
        f"queued=0x{read_u32_le(report, 40):08x} "
        f"callback=0x{read_u32_le(report, 44):08x} "
        f"wait=0x{read_u32_le(report, 48):08x}"
    )

def print_ap_start_wpa_wait_telemetry(report: bytes) -> None:
    phase_names = {
        1: "register_enter",
        2: "target_initialized",
        3: "callback_registered",
        4: "before_enable_network",
        5: "enable_network_return",
        6: "before_final_wait",
        7: "callback_enter",
        8: "before_callback_unregister",
        9: "callback_unregister_return",
        10: "before_task_notify",
        11: "task_notify_return",
        12: "final_wait_return",
    }
    phase_mask = int.from_bytes(report[7:9], "little")
    phases = ",".join(
        phase_names[phase]
        for phase in range(1, 17)
        if phase_mask & (1 << (phase - 1))
    )
    print(
        "AP_START_WPA_WAIT="
        f"persisted={report[6]} phases=0x{phase_mask:03x} "
        f"phase_names={phases or 'none'} "
        f"last_phase={report[9]} "
        f"last_phase_name={phase_names.get(report[9], 'unknown')} "
        f"result={int.from_bytes(report[10:11], 'little', signed=True)} "
        f"vif={int.from_bytes(report[11:12], 'little', signed=True)} "
        f"expected_event={report[12]} observed_event={report[13]} "
        f"wpa_state={report[14]} updates={read_u32_le(report, 16)}"
    )
    print(
        "AP_START_WPA_WAIT_HANDLES="
        f"target=0x{read_u32_le(report, 20):08x} "
        f"waiting_task=0x{read_u32_le(report, 24):08x} "
        f"callback_task=0x{read_u32_le(report, 28):08x}"
    )

def print_ap_start_wpa_event_path_telemetry(report: bytes) -> None:
    phase_names = {
        1: "ap_configured_enter",
        2: "ap_configured_disabled",
        3: "before_wpa_completed",
        4: "after_wpa_completed",
        5: "wpa_state_enter",
        6: "before_operstate",
        7: "after_operstate",
        8: "driver_operstate_enter",
        9: "before_connected_event",
        10: "after_connected_event",
        11: "send_event_enter",
        12: "before_event_process",
        13: "event_process_enter",
        14: "event_state_updated",
        15: "before_callback_dispatch",
        16: "callback_dispatch_enter",
        17: "callback_slot_match",
        18: "callback_slot_return",
        19: "callback_dispatch_return",
        20: "event_process_return",
        21: "send_event_return",
        22: "enable_one_enter",
        23: "enable_one_after",
        24: "enable_network_continuation",
        25: "before_fast_associate",
        26: "after_fast_associate",
        27: "before_request_scan",
        28: "after_request_scan",
        29: "before_create_ap",
        30: "after_create_ap",
    }
    phase_mask = read_u32_le(report, 7)
    phases = ",".join(
        phase_names[phase]
        for phase in range(1, 31)
        if phase_mask & (1 << (phase - 1))
    )
    print(
        "AP_START_WPA_EVENT_PATH="
        f"persisted={report[6]} phases=0x{phase_mask:06x} "
        f"phase_names={phases or 'none'} last_phase={report[11]} "
        f"last_phase_name={phase_names.get(report[11], 'unknown')} "
        f"vif={int.from_bytes(report[12:13], 'little', signed=True)} "
        f"event={report[13]} fhost_wpa_state={report[14]} "
        f"operstate={int.from_bytes(report[15:16], 'little', signed=True)} "
        f"old_wpa_state={int.from_bytes(report[16:17], 'little', signed=True)} "
        f"new_wpa_state={int.from_bytes(report[17:18], 'little', signed=True)} "
        f"result={int.from_bytes(report[18:19], 'little', signed=True)} "
        f"callback_slot={report[19]} updates={read_u32_le(report, 20)}"
    )
    print(
        "AP_START_WPA_EVENT_PATH_CALLBACKS="
        f"matching_slot_mask=0x{read_u32_le(report, 24):08x}"
    )
    if report[11] >= 22:
        print(
            "AP_START_ENABLE_NETWORK_STATE="
            f"ssid_disabled={report[13]} wpa_state={report[14]} "
            f"current_ssid={int.from_bytes(report[15:16], 'little', signed=True)} "
            f"disconnected={int.from_bytes(report[16:17], 'little', signed=True)} "
            f"reassociate={int.from_bytes(report[17:18], 'little', signed=True)} "
            f"result={int.from_bytes(report[18:19], 'little', signed=True)}"
        )

def print_ap_start_create_ap_telemetry(report: bytes) -> None:
    phase_names = {
        1: "create_ap_enter",
        2: "after_ap_deinit",
        3: "before_driver_associate",
        4: "after_driver_associate",
        5: "after_hostapd_iface_allocate",
        6: "after_hostapd_config_defaults",
        7: "after_ap_configuration",
        8: "after_bss_array_allocate",
        9: "before_bss_data_allocate",
        10: "after_bss_data_allocate",
        11: "after_bss_callback_setup",
        12: "after_bss_setup",
        13: "after_bss_driver_setup",
        14: "before_hostapd_setup",
        15: "after_hostapd_setup",
        16: "create_ap_return",
    }
    phase_mask = int.from_bytes(report[7:9], "little")
    detail = read_u32_le(report, 16)
    detail_names = {
        0: "iface_allocated",
        1: "iface_metadata_copied",
        2: "before_config_defaults",
    }
    phases = ",".join(
        phase_names[phase]
        for phase in range(1, 13)
        if phase_mask & (1 << (phase - 1))
    )
    print(
        "AP_START_CREATE_AP="
        f"persisted={report[6]} phases=0x{phase_mask:03x} "
        f"phase_names={phases or 'none'} last_phase={report[9]} "
        f"last_phase_name={phase_names.get(report[9], 'unknown')} "
        f"result={int.from_bytes(report[10:11], 'little', signed=True)} "
        f"wpa_state={report[11]} detail={detail} "
        f"detail_name={detail_names.get(detail, 'other')} "
        f"updates={read_u32_le(report, 20)}"
    )


def print_ap_start_hostapd_lifecycle_telemetry(report: bytes) -> None:
    phase_names = {
        42: "setup_bss_entry",
        43: "setup_bss_started",
        44: "setup_bss_station_flush",
        45: "setup_bss_get_ssid",
        46: "setup_bss_wpa_psk",
        47: "setup_bss_set_ssid",
        48: "setup_bss_wpa",
        49: "setup_bss_beacon_return",
        50: "setup_bss_wpa_keys",
        51: "setup_bss_complete",
        52: "setup_bss_before_accounting",
        53: "setup_bss_accounting_return",
        54: "setup_bss_before_beacon",
        55: "beacon_before_ap_params",
        56: "beacon_ap_params_return",
        57: "beacon_before_extra_ies",
        58: "beacon_extra_ies_return",
        59: "beacon_before_driver_set_ap",
        60: "beacon_driver_set_ap_return",
        61: "driver_set_ap_entry",
        62: "driver_before_beacon_build",
        63: "driver_beacon_build_return",
        64: "driver_before_cfgmacsw_start_ap",
        65: "driver_cfgmacsw_start_ap_return",
    }
    record_count = min(report[32], 3)
    print(
        "AP_START_HOSTAPD_LIFECYCLE="
        f"persisted={report[6]} phase_mask=0x{read_u32_le(report, 7):08x} "
        f"last_phase={report[11]} "
        f"last_phase_name={phase_names.get(report[11], 'other')} "
        f"result={int.from_bytes(report[12:13], 'little', signed=True)} "
        f"iface_state={report[13]} wait_channel_update={report[14]} "
        f"detail0=0x{read_u32_le(report, 16):08x} "
        f"detail1=0x{read_u32_le(report, 20):08x} "
        f"records={record_count}"
    )
    for index in range(record_count):
        offset = 33 + index * 8
        phase = report[offset]
        print(
            "AP_START_HOSTAPD_LIFECYCLE_RECORD="
            f"index={index} phase={phase} "
            f"phase_name={phase_names.get(phase, 'other')} "
            f"result={int.from_bytes(report[offset + 1:offset + 2], 'little', signed=True)} "
            f"iface_state={report[offset + 2]} "
            f"wait_channel_update={report[offset + 3]} "
            f"detail0=0x{read_u32_le(report, offset + 4):08x}"
        )


def print_ap_start_status_code_telemetry(report: bytes) -> None:
    print(
        "AP_START_STATUS_CODE="
        f"persisted={report[6]} phase_mask=0x{read_u16_le(report, 7):04x} "
        f"last_phase={report[9]} "
        f"vif={int.from_bytes(report[10:11], 'little', signed=True)} "
        f"message_kind={report[11]} message_length={read_u16_le(report, 12)} "
        f"raw_state={report[14]} vif_state={report[15]} "
        f"result={int.from_bytes(report[16:17], 'little', signed=True)} "
        f"updates={read_u32_le(report, 20)}"
    )


def print_ap_start_hostapd_state_update_telemetry(report: bytes) -> None:
    record_count = min(report[7], 3)
    print(
        "AP_START_HOSTAPD_STATE_UPDATE="
        f"persisted={report[6]} records={record_count} "
        f"next_sequence={read_u32_le(report, 8)}"
    )
    for index in range(record_count):
        offset = 12 + index * 12
        print(
            "AP_START_HOSTAPD_STATE_UPDATE_RECORD="
            f"index={index} sequence={read_u32_le(report, offset)} "
            f"hostapd_state={report[offset + 4]} enabled={report[offset + 5]} "
            f"raw_state_before={report[offset + 6]} "
            f"vif_state_before={report[offset + 7]} "
            f"raw_state_after={report[offset + 8]} "
            f"vif_state_after={report[offset + 9]}"
        )


AP_START_ALLOCATOR_STAGES = {
    1: "os_zalloc_enter",
    2: "os_malloc_enter",
    3: "before_rtos_malloc",
    4: "after_rtos_malloc",
    5: "rtos_malloc_enter",
    6: "before_kmalloc",
    7: "after_kmalloc",
    8: "kmalloc_enter",
    9: "kmalloc_locked",
    10: "kmalloc_heap_selected",
    11: "kmalloc_allocated",
    12: "kmalloc_unlocked",
    13: "tlsf_malloc_enter",
    14: "tlsf_before_allocate",
    15: "tlsf_after_allocate",
    16: "tlsf_after_zero",
    17: "os_zalloc_before_zero",
    18: "os_zalloc_return",
    19: "kmalloc_auto_heap_attempt",
    20: "kmalloc_auto_return",
}


def print_ap_start_allocator_trace_telemetry(report: bytes) -> None:
    record_count = min(report[7], 3)
    print(
        "AP_START_ALLOCATOR_TRACE="
        f"fhost_stage={report[6]} "
        f"fhost_stage_name={AP_START_STAGES.get(report[6], 'unknown')} "
        f"records={record_count} next_sequence={read_u32_le(report, 8)}"
    )
    for index in range(record_count):
        offset = 12 + index * 17
        stage = report[offset + 4]
        value0 = read_u32_le(report, offset + 5)
        value1 = read_u32_le(report, offset + 9)
        value2 = read_u32_le(report, offset + 13)
        prefix = (
            "AP_START_ALLOCATOR_RECORD="
            f"sequence={read_u32_le(report, offset)} "
            f"stage={stage} "
            f"stage_name={AP_START_ALLOCATOR_STAGES.get(stage, 'unknown')} "
        )
        if stage == 19:
            print(
                prefix +
                f"order={(value0 >> 24) & 0x7f} "
                f"heap_id={value0 & 0xff} "
                f"result={'success' if not value0 & 0x80000000 else 'failed'} "
                f"free_size={value1} max_free_size={value2}"
            )
        elif stage == 20:
            print(
                prefix +
                f"result_ptr=0x{value0:08x} "
                f"size={value1} flags=0x{value2:08x}"
            )
        else:
            print(
                prefix +
                f"value0=0x{value0:08x} "
                f"value1=0x{value1:08x} "
                f"value2=0x{value2:08x}"
            )


def print_ap_start_allocator_heap_walk_telemetry(report: bytes) -> None:
    record_count = min(report[8], 2)
    print(
        "AP_START_ALLOCATOR_HEAP_WALK="
        f"persisted={report[6]} "
        f"result={int.from_bytes(report[7:8], 'little', signed=True)} "
        f"records={record_count}"
    )
    for index in range(record_count):
        offset = 12 + index * 20
        print(
            "AP_START_ALLOCATOR_HEAP="
            f"heap_id={report[offset]} "
            f"allocated_blocks={int.from_bytes(report[offset + 2:offset + 4], 'little')} "
            f"allocated_bytes={read_u32_le(report, offset + 4)} "
            f"largest_blocks="
            f"{read_u32_le(report, offset + 8)},"
            f"{read_u32_le(report, offset + 12)},"
            f"{read_u32_le(report, offset + 16)}"
        )


def print_ap_start_eloop_post_telemetry(report: bytes) -> None:
    phase_names = {
        1: "caller_before_post",
        2: "callee_enter",
        3: "callee_ifname_prefix",
        4: "callee_enable_network_match",
        5: "event_allocated",
        6: "before_mutex_lock",
        7: "mutex_acquired",
        8: "event_enqueued",
        9: "mutex_released",
        10: "before_event_trigger",
        11: "event_trigger_returned",
    }
    print(
        "AP_START_ELOOP_POST="
        f"persisted={report[6]} last_phase={report[7]} "
        f"last_phase_name={phase_names.get(report[7], 'unknown')} "
        f"caller_event={report[8]} callee_event={report[9]} "
        f"caller_enable_network={report[10]} "
        f"callee_ifname_prefix={report[11]} "
        f"callee_enable_network={report[12]} "
        f"updates={read_u32_le(report, 40)}"
    )
    print(
        "AP_START_ELOOP_POST_CALLER="
        f"body=0x{read_u32_le(report, 16):08x} "
        f"body_length={read_u32_le(report, 20)}"
    )
    print(
        "AP_START_ELOOP_POST_CALLEE="
        f"body=0x{read_u32_le(report, 24):08x} "
        f"body_length={read_u32_le(report, 28)} "
        f"request=0x{read_u32_le(report, 32):08x} "
        f"request_word=0x{read_u32_le(report, 36):08x}"
    )

def print_ap_start_command_telemetry(report: bytes) -> None:
    command = report[6]
    phase = report[7]
    vif = int.from_bytes(report[8:9], "little", signed=True)
    network_id = int.from_bytes(report[10:11], "little", signed=True)
    fhost_result = int.from_bytes(report[13:14], "little", signed=True)
    print(
        "AP_START_COMMAND="
        f"command={command} command_name={AP_START_COMMANDS.get(command, 'unknown')} "
        f"phase={phase} phase_name={AP_START_COMMAND_PHASES.get(phase, 'unknown')} "
        f"vif={vif} wpa_state={report[9]} network_id={network_id} "
        f"sequence={report[11]}"
    )
    print(
        "AP_START_COMMAND_CONTEXT="
        f"fhost_stage={report[12]} "
        f"fhost_stage_name={AP_START_STAGES.get(report[12], 'unknown')} "
        f"fhost_result={fhost_result} bridge_stage={report[14]} "
        f"persisted={report[15]}"
    )
    print(
        "AP_START_COMMAND_TIMING="
        f"attempts={read_u32_le(report, 16)} "
        f"elapsed_ms={read_u32_le(report, 20)} "
        f"started_at_ms={read_u32_le(report, 24)} "
        f"completed_at_ms={read_u32_le(report, 28)}"
    )


AP_START_WATCHDOG_STATES = {
    0: "disabled",
    1: "armed",
    2: "disarmed",
    3: "reset_observed",
    4: "recovery_complete",
}

RA4_BOOTSTRAP_OPERATIONS = {
    0: "idle",
    1: "release",
    2: "enter_program_mode",
    3: "reset_with_boot_low",
    4: "reset_to_application",
    5: "fail_safe_release",
    6: "normal_boot",
}

RA4_BOOTSTRAP_PIN_STATES = {
    0: "unknown",
    1: "high",
    2: "low",
    3: "released",
}


def print_ap_start_watchdog_telemetry(report: bytes) -> None:
    timeout_ms = int.from_bytes(report[10:12], "little")
    print(
        "AP_START_WATCHDOG="
        f"enabled={report[6]} state={report[7]} "
        f"state_name={AP_START_WATCHDOG_STATES.get(report[7], 'unknown')} "
        f"reset_reason={report[8]} persisted={report[9]} "
        f"timeout_ms={timeout_ms} arm_count={read_u32_le(report, 12)}"
    )
    print(
        "AP_START_RETAINED="
        f"liveness_phase={report[16]} "
        f"liveness_events={report[17]} "
        f"fhost_stage={report[18]} "
        f"fhost_stage_name={AP_START_STAGES.get(report[18], 'unknown')} "
        f"bridge_stage={report[19]} "
        f"fhost_result={int.from_bytes(report[20:21], 'little', signed=True)} "
        f"native_result={int.from_bytes(report[21:22], 'little', signed=True)} "
        f"diagnostic_persisted={report[22]}"
    )

def print_ra4_bootstrap_telemetry(report: bytes) -> None:
    record_count = min(report[9], 8)
    print(
        "RA4_BOOTSTRAP="
        f"operation={RA4_BOOTSTRAP_OPERATIONS.get(report[6], 'unknown')} "
        f"active={report[7]} succeeded={report[8]} "
        f"sequence={read_u32_le(report, 10)} records={record_count}"
    )
    for index in range(record_count):
        offset = 14 + index * 6
        boot_level = int.from_bytes(
            report[offset + 3 : offset + 4], "little", signed=True)
        reset_level = int.from_bytes(
            report[offset + 4 : offset + 5], "little", signed=True)
        print(
            "RA4_BOOTSTRAP_STEP="
            f"index={index} step={report[offset]} "
            f"boot={RA4_BOOTSTRAP_PIN_STATES.get(report[offset + 1], 'unknown')} "
            f"reset={RA4_BOOTSTRAP_PIN_STATES.get(report[offset + 2], 'unknown')} "
            f"boot_level={boot_level} reset_level={reset_level} "
            f"succeeded={report[offset + 5]}"
        )

def read_u16_le(report: bytes, offset: int) -> int:
    return int.from_bytes(report[offset : offset + 2], "little")

def print_ra4_uart_recovery_snapshot(
    report: bytes,
    offset: int,
    label: str,
) -> None:
    print(
        "RA4_UART_RECOVERY_"
        f"{label}="
        f"initialized={report[offset]} "
        f"rx_fifo={report[offset + 1]} "
        f"int_status=0x{read_u16_le(report, offset + 2):04x} "
        f"int_mask=0x{read_u16_le(report, offset + 4):04x} "
        f"urx_config=0x{read_u32_le(report, offset + 6):08x} "
        f"bit_period=0x{read_u32_le(report, offset + 10):08x} "
        f"received={read_u32_le(report, offset + 14)} "
        f"overflows={read_u32_le(report, offset + 18)}"
    )

def print_ra4_uart_recovery_telemetry(report: bytes, uart_name: str) -> None:
    flags = report[7]
    print(
        f"RA4_{uart_name}_UART_RECOVERY="
        f"persisted={report[6]} "
        f"before_valid={int(bool(flags & 0x01))} "
        f"after_valid={int(bool(flags & 0x02))} "
        f"reset_reason={report[8]} "
        f"cdc_before={read_u32_le(report, 9)} "
        f"cdc_after={read_u32_le(report, 13)}"
    )
    print_ra4_uart_recovery_snapshot(report, 17, f"{uart_name}_BEFORE")
    print_ra4_uart_recovery_snapshot(report, 39, f"{uart_name}_AFTER")

def print_ra4_uart_live_telemetry(report: bytes, uart_name: str) -> None:
    print_ra4_uart_recovery_snapshot(report, 6, f"{uart_name}_LIVE")

def print_ra4_at_transport_telemetry(report: bytes) -> None:
    print(
        "RA4_AT_TRANSPORT="
        f"initialized={report[6]} "
        f"received_bytes={read_u32_le(report, 8)} "
        f"buffered_rx_bytes={read_u32_le(report, 12)} "
        f"rx_overflows={read_u32_le(report, 16)} "
        f"buffered_tx_bytes={read_u32_le(report, 20)} "
        f"tx_overflows={read_u32_le(report, 24)} "
        f"rx_error_status_seen={read_u32_le(report, 28)} "
        f"rx_fifo_errors_seen={read_u32_le(report, 32)}"
    )

def print_ra4_ota_download_telemetry(report: bytes) -> None:
    read_results = {
        0: "data",
        1: "pending",
        2: "closed",
        3: "error",
    }
    timeout_reasons = {
        0: "none",
        1: "header",
        2: "body",
    }
    print(
        "RA4_OTA_DOWNLOAD="
        f"last_read={read_results.get(report[6], 'unknown')} "
        f"timeout={timeout_reasons.get(report[7], 'unknown')} "
        f"secure={report[8]} last_read_valid={report[9]} "
        f"last_read_bytes={read_u32_le(report, 12)} "
        f"content_length={read_u32_le(report, 16)} "
        f"downloaded={read_u32_le(report, 20)} "
        f"polls={read_u32_le(report, 24)} data={read_u32_le(report, 28)} "
        f"pending={read_u32_le(report, 32)} closed={read_u32_le(report, 36)} "
        f"errors={read_u32_le(report, 40)} "
        f"last_progress_ms={read_u32_le(report, 44)} "
        f"last_poll_ms={read_u32_le(report, 48)}"
    )

def print_ra4_ota_download_backend_telemetry(report: bytes) -> None:
    print(
        "RA4_OTA_DOWNLOAD_BACKEND="
        f"open={report[6]} secure={report[7]} "
        f"handle={int.from_bytes(report[8:12], 'little', signed=True)} "
        f"available_calls={read_u32_le(report, 12)} "
        f"available_ready={read_u32_le(report, 16)} "
        f"available_pending={read_u32_le(report, 20)} "
        f"available_failures={read_u32_le(report, 24)} "
        f"last_available={read_u32_le(report, 28)} "
        f"receive_calls={read_u32_le(report, 32)} "
        f"receive_failures={read_u32_le(report, 36)} "
        f"last_receive_bytes={read_u32_le(report, 40)} "
        f"last_receive_ok={report[44]}"
    )

def print_ra4_samba_telemetry(report: bytes) -> None:
    state_names = {
        0: "idle",
        1: "wait_bootloader",
        2: "wait_auto_baud_second",
        3: "wait_auto_baud_confirm",
        4: "wait_binary_reply",
        5: "wait_version_reply",
        6: "wait_application",
        7: "recovery",
    }
    error_names = {
        0: "none",
        1: "busy",
        2: "suspend_user_uart",
        3: "enter_program_mode",
        4: "set_samba_baud",
        5: "write",
        6: "binary_reply_timeout",
        7: "binary_reply_invalid",
        8: "version_reply_timeout",
        9: "version_reply_invalid",
        10: "restore_user_uart",
        11: "release_bootstrap",
    }
    version = report[30:].split(b"\0", 1)[0].decode("ascii", errors="replace")
    binary_reply = (
        f"0x{report[60]:02x} 0x{report[61]:02x}"
        if report[7] in (6, 7)
        else "n/a"
    )
    print(
        "RA4_SAMBA="
        f"state={state_names.get(report[6], 'unknown')} "
        f"error={error_names.get(report[7], 'unknown')} "
        f"active={report[8]} succeeded={report[9]} "
        f"sequence={read_u32_le(report, 10)} "
        f"started_ms={read_u32_le(report, 14)} "
        f"completed_ms={read_u32_le(report, 18)} "
        f"tx_bytes={read_u32_le(report, 22)} "
        f"rx_bytes={read_u32_le(report, 26)} "
        f"binary_reply={binary_reply} "
        f"version={version!r}"
    )

def print_ra4_samba_programmer_telemetry(report: bytes) -> None:
    state_names = {
        0: "idle",
        1: "wait_bootloader",
        2: "wait_auto_baud_second",
        3: "wait_auto_baud_confirm",
        4: "wait_binary_reply",
        5: "wait_version_reply",
        6: "wait_applet_staged",
        7: "wait_applet_words",
        8: "wait_applet_stack",
        9: "wait_erase_reply",
        10: "wait_chunk_staged",
        11: "wait_select_buffer_reply",
        12: "wait_write_chunk_reply",
        13: "wait_checksum_reply",
        14: "wait_application",
        15: "recovery",
        16: "send_chunk_payload",
        17: "send_applet_payload",
    }
    error_names = {
        0: "none",
        1: "busy",
        2: "image_source",
        3: "image_size",
        4: "suspend_user_uart",
        5: "enter_program_mode",
        6: "set_samba_baud",
        7: "write",
        8: "binary_reply_timeout",
        9: "binary_reply_invalid",
        10: "version_reply_timeout",
        11: "version_reply_invalid",
        12: "missing_capabilities",
        13: "applet_source",
        14: "erase_reply_timeout",
        15: "erase_reply_invalid",
        16: "select_reply_timeout",
        17: "select_reply_invalid",
        18: "write_reply_timeout",
        19: "write_reply_invalid",
        20: "checksum_reply_timeout",
        21: "checksum_reply_invalid",
        22: "checksum_mismatch",
        23: "restore_user_uart",
        24: "release_bootstrap",
    }
    version = report[42:].split(b"\0", 1)[0].decode("ascii", errors="replace")
    print(
        "RA4_SAMBA_PROGRAMMER="
        f"state={state_names.get(report[6], 'unknown')} "
        f"error={error_names.get(report[7], 'unknown')} "
        f"active={report[8]} succeeded={report[9]} "
        f"sequence={read_u32_le(report, 10)} "
        f"started_ms={read_u32_le(report, 14)} "
        f"completed_ms={read_u32_le(report, 18)} "
        f"tx_bytes={read_u32_le(report, 22)} "
        f"rx_bytes={read_u32_le(report, 26)} "
        f"image_bytes={read_u32_le(report, 30)} "
        f"verified_bytes={read_u32_le(report, 34)} "
        f"chunks={int.from_bytes(report[38:40], 'little')} "
        f"version_bytes={report[40]} "
        f"terminator_first=0x{report[41]:02x} "
        f"version={version!r}"
    )

def print_time_service_telemetry(report: bytes) -> None:
    failure_names = {
        0: "none",
        1: "task_create",
        2: "station_unavailable",
        3: "dns",
        4: "socket",
        5: "socket_options",
        6: "send",
        7: "receive",
        8: "packet",
        9: "rtc",
    }
    print(
        "TIME_SERVICE="
        f"station_ready={report[6]} running={report[7]} synced={report[8]} "
        f"failure={failure_names.get(report[9], 'unknown')} "
        f"got_ip={read_u32_le(report, 10)} "
        f"task_create={read_u32_le(report, 14)} "
        f"task_create_fail={read_u32_le(report, 18)} "
        f"attempts={read_u32_le(report, 22)} "
        f"dns_fail={read_u32_le(report, 26)} "
        f"socket_fail={read_u32_le(report, 30)} "
        f"send_fail={read_u32_le(report, 34)} "
        f"recv_fail={read_u32_le(report, 38)} "
        f"packet_fail={read_u32_le(report, 42)} "
        f"rtc_fail={read_u32_le(report, 46)} "
        f"success={read_u32_le(report, 50)} "
        f"last_dns={int.from_bytes(report[54:56], 'little', signed=True)} "
        f"last_recv={int.from_bytes(report[56:58], 'little', signed=True)}"
    )

def print_time_service_readback_telemetry(report: bytes) -> None:
    print(
        "TIME_SERVICE_READBACK="
        f"station_ready={report[6]} synced={report[7]} "
        f"rtc_year={read_u16_le(report, 8)} "
        f"ntp_epoch={read_u32_le(report, 10)} "
        f"rtc_epoch={read_u32_le(report, 14)} "
        f"published_epoch={read_u32_le(report, 18)} "
        f"epoch_reads={read_u32_le(report, 22)} "
        f"rtc_fail={read_u32_le(report, 26)} "
        f"success={read_u32_le(report, 30)}"
    )

def print_tcp_listener_close_telemetry(report: bytes) -> None:
    origin_names = {
        0: "none",
        1: "tcp_client_slot",
        2: "tcp_server_client_slot",
        3: "tcp_server_end",
        4: "softresetwifi",
        5: "listener_setup_failure",
    }
    origin = report[18]
    print(
        "TCP_LISTENER_CLOSE="
        f"requests={read_u32_le(report, 6)} "
        f"listener_requests={read_u32_le(report, 10)} "
        f"last_handle={int.from_bytes(report[14:18], 'little', signed=True)} "
        f"origin={origin} "
        f"origin_name={origin_names.get(origin, 'unknown')} "
        f"result={int.from_bytes(report[20:24], 'little', signed=True)} "
        f"errno={int.from_bytes(report[24:28], 'little', signed=True)} "
        f"close_generation={read_u32_le(report, 28)} "
        f"active_generation={read_u32_le(report, 32)} "
        f"listener_handle={int.from_bytes(report[36:40], 'little', signed=True)}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hidraw", help="override automatic paired-BL616CL HID discovery")
    parser.add_argument(
        "--cdc-in",
        action="store_true",
        help="read CDC IN transfer/recovery telemetry instead of UART RX state",
    )
    parser.add_argument(
        "--ap-start",
        action="store_true",
        help="read the native AP-start diagnostic state instead of UART RX state",
    )
    parser.add_argument(
        "--ap-start-heap",
        action="store_true",
        help="read AP-start allocator diagnostic state instead of UART RX state",
    )
    parser.add_argument(
        "--ap-start-command",
        action="store_true",
        help="read AP-start command progress instead of UART RX state",
    )
    parser.add_argument(
        "--ap-start-notification",
        action="store_true",
        help="read persistent ENABLE_NETWORK task-notification outcome",
    )
    parser.add_argument(
        "--ap-start-trace",
        action="store_true",
        help="read persistent ENABLE_NETWORK semaphore/notification trace",
    )
    parser.add_argument(
        "--ap-start-notification-ab",
        action="store_true",
        help="read ENABLE_NETWORK task-notification branch-state telemetry",
    )
    parser.add_argument(
        "--ap-start-wpa-wait",
        action="store_true",
        help="read final AP WPA-event wait telemetry",
    )
    parser.add_argument(
        "--ap-start-wpa-event-path",
        action="store_true",
        help="read AP WPA completion-to-FHOST event-path telemetry",
    )
    parser.add_argument(
        "--ap-start-create-ap",
        action="store_true",
        help="read AP create_ap boundary telemetry",
    )
    parser.add_argument(
        "--ap-start-allocator-trace",
        action="store_true",
        help="read retained allocator boundary trace for AP create_ap",
    )
    parser.add_argument(
        "--ap-start-allocator-heap-walk",
        action="store_true",
        help="read retained allocated-block summary before AP BSS allocation",
    )
    parser.add_argument(
        "--ap-start-eloop-post",
        action="store_true",
        help="read persistent ENABLE_NETWORK eloop_post_event telemetry",
    )
    parser.add_argument(
        "--ap-start-hostapd-lifecycle",
        action="store_true",
        help="read retained hostapd setup-BSS lifecycle telemetry",
    )
    parser.add_argument(
        "--ap-start-status-code",
        action="store_true",
        help="read retained AP status-code telemetry",
    )
    parser.add_argument(
        "--ap-start-hostapd-state-update",
        action="store_true",
        help="read retained hostapd state-update telemetry",
    )
    parser.add_argument(
        "--hid-data-path",
        action="store_true",
        help="read CMSIS-DAP HID interrupt endpoint diagnostic telemetry",
    )
    parser.add_argument(
        "--ap-start-watchdog",
        action="store_true",
        help="read AP-start diagnostic watchdog/reset telemetry",
    )
    parser.add_argument(
        "--ra4-bootstrap",
        action="store_true",
        help="read RA4M1 BOOT/RESET control-sequence telemetry",
    )
    parser.add_argument(
        "--ra4-uart-recovery",
        action="store_true",
        help="read UART0 watchdog-recovery snapshots",
    )
    parser.add_argument(
        "--ra4-at-uart-recovery",
        action="store_true",
        help="read UART1 AT watchdog-recovery snapshots",
    )
    parser.add_argument(
        "--ra4-uart-live",
        action="store_true",
        help="read the current UART0 state and software RX counters",
    )
    parser.add_argument(
        "--ra4-at-uart-live",
        action="store_true",
        help="read the current UART1 AT state and software RX counters",
    )
    parser.add_argument("--sta-disconnect", action="store_true",
                        help="read persistent native STA teardown stages")
    parser.add_argument(
        "--tcp-listener-close",
        action="store_true",
        help="read TCP listener close-path telemetry",
    )
    parser.add_argument(
        "--ra4-at-transport",
        action="store_true",
        help="read the active UartTransport counters used by the RA4M1 AT service",
    )
    parser.add_argument(
        "--ra4-ota-download",
        action="store_true",
        help="read RA4 OTA downloader state/read-result telemetry",
    )
    parser.add_argument(
        "--ra4-ota-download-backend",
        action="store_true",
        help="read RA4 OTA download backend availability/receive telemetry",
    )
    parser.add_argument(
        "--ra4-samba",
        action="store_true",
        help="read the read-only RA4M1 SAM-BA handshake telemetry",
    )
    parser.add_argument(
        "--ra4-samba-programmer",
        action="store_true",
        help="read project-owned RA4M1 SAM-BA programmer telemetry",
    )
    parser.add_argument(
        "--time-service",
        action="store_true",
        help="read credential-safe WiFiS3 getTime NTP/RTC service telemetry",
    )
    parser.add_argument(
        "--time-service-readback",
        action="store_true",
        help="read WiFiS3 getTime RTC write/readback telemetry",
    )
    parser.add_argument(
        "--ra4-application-reset",
        action="store_true",
        help="pulse RA4M1 reset using the existing HID D4 control request",
    )
    parser.add_argument(
        "--trigger-ra4-samba-probe",
        action="store_true",
        help="request a read-only RA4M1 SAM-BA handshake through HID D6",
    )
    parser.add_argument("--runtime-heap", action="store_true", help="request exact OCRAM/WRAM allocator snapshot")
    args = parser.parse_args()
    selected_pages = sum(
        (
            args.runtime_heap,
            args.cdc_in,
            args.ap_start,
            args.ap_start_heap,
            args.ap_start_command,
            args.ap_start_notification,
            args.ap_start_trace,
            args.ap_start_notification_ab,
            args.ap_start_wpa_wait,
            args.ap_start_wpa_event_path,
            args.ap_start_create_ap,
            args.ap_start_allocator_trace,
            args.ap_start_allocator_heap_walk,
            args.ap_start_eloop_post,
            args.ap_start_hostapd_lifecycle,
            args.ap_start_status_code,
            args.ap_start_hostapd_state_update,
            args.hid_data_path,
            args.ap_start_watchdog,
            args.ra4_bootstrap,
            args.ra4_uart_recovery,
            args.ra4_at_uart_recovery,
            args.ra4_uart_live,
            args.ra4_at_uart_live,
            args.ra4_at_transport,
            args.ra4_ota_download,
            args.ra4_ota_download_backend,
            args.ra4_samba,
            args.ra4_samba_programmer,
            args.time_service,
            args.time_service_readback,
            args.tcp_listener_close,
            args.sta_disconnect,
            args.ra4_application_reset,
            args.trigger_ra4_samba_probe,
        )
    )
    if selected_pages > 1:
        raise RuntimeError(
            "--cdc-in, --ap-start, --ap-start-heap, --ap-start-command, "
            "--ap-start-notification, "
            "--ap-start-trace, --ap-start-notification-ab, "
            "--ap-start-wpa-wait, --ap-start-wpa-event-path, "
            "--ap-start-create-ap, "
            "--ap-start-allocator-trace, --ap-start-allocator-heap-walk, "
            "--ap-start-eloop-post, --ap-start-hostapd-lifecycle, "
            "--ap-start-status-code, --ap-start-hostapd-state-update, "
            "--hid-data-path, --ap-start-watchdog, --ra4-bootstrap, and "
            "--ra4-uart-recovery, --ra4-at-uart-recovery, --ra4-uart-live, "
            "--ra4-at-uart-live, --ra4-at-transport, --ra4-ota-download, "
            "--ra4-ota-download-backend, --ra4-samba, --tcp-listener-close, "
            "--ra4-samba-programmer, --time-service, --time-service-readback, "
            "--ra4-application-reset, and --trigger-ra4-samba-probe "
            "are mutually exclusive"
        )

    hidraw = args.hidraw or find_hidraw()
    if not Path(hidraw).is_char_device():
        raise RuntimeError(f"missing HID device: {hidraw}")

    fd = os.open(hidraw, os.O_RDWR | os.O_NONBLOCK)
    try:
        if args.ra4_application_reset:
            request_ra4_application_reset(fd)
            print(f"HIDRAW={hidraw}")
            print("RA4_APPLICATION_RESET_REQUEST_ACCEPTED")
            return 0
        if args.trigger_ra4_samba_probe:
            request_ra4_samba_probe(fd)
            print(f"HIDRAW={hidraw}")
            print("RA4_SAMBA_PROBE_REQUEST_ACCEPTED")
            return 0
        select_feature_page(
            fd,
            4
            if args.ap_start_command
            else 15
            if args.ap_start_notification
            else 5
            if args.ap_start_trace
            else 16
            if args.ap_start_notification_ab
            else 18
            if args.ap_start_wpa_wait
            else 19
            if args.ap_start_wpa_event_path
            else 20
            if args.ap_start_create_ap
            else 6
            if args.ap_start_allocator_trace
            else 21
            if args.ap_start_allocator_heap_walk
            else 17
            if args.ap_start_eloop_post
            else 24
            if args.ap_start_hostapd_lifecycle
            else 25
            if args.ap_start_status_code
            else 26
            if args.ap_start_hostapd_state_update
            else 3
            if args.ap_start_heap
            else 2
            if args.ap_start
            else 1
            if args.cdc_in
            else 7
            if args.hid_data_path
            else 8
            if args.ap_start_watchdog
            else 9
            if args.ra4_bootstrap
            else 10
            if args.ra4_uart_recovery
            else 11
            if args.ra4_at_uart_recovery
            else 12
            if args.ra4_uart_live
            else 13
            if args.ra4_at_uart_live
            else 14
            if args.ra4_at_transport
            else 32
            if args.ra4_ota_download
            else 33
            if args.ra4_ota_download_backend
            else 34
            if args.ra4_samba
            else 35
            if args.ra4_samba_programmer
            else 36
            if args.time_service
            else 37
            if args.time_service_readback
            else 31
            if args.tcp_listener_close
            else 38
            if args.sta_disconnect
            else 39
            if args.runtime_heap
            else 0,
        )
        report = read_feature(fd)
        if args.runtime_heap:
            previous = read_u32_le(report, 6) if report[5] == 39 else 0
            select_feature_page(fd, 39)
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                report = read_feature(fd)
                if report[5] == 39 and read_u32_le(report, 6) != previous:
                    break
                time.sleep(0.01)
            else:
                raise RuntimeError("runtime heap sample timed out")
    finally:
        if (
            args.runtime_heap
            or args.cdc_in
            or args.ap_start
            or args.ap_start_heap
            or args.ap_start_command
            or args.ap_start_notification
            or args.ap_start_trace
            or args.ap_start_notification_ab
            or args.ap_start_wpa_wait
            or args.ap_start_wpa_event_path
            or args.ap_start_create_ap
            or args.ap_start_allocator_trace
            or args.ap_start_allocator_heap_walk
            or args.ap_start_eloop_post
            or args.ap_start_hostapd_lifecycle
            or args.ap_start_status_code
            or args.ap_start_hostapd_state_update
            or args.hid_data_path
            or args.ap_start_watchdog
            or args.ra4_bootstrap
            or args.ra4_uart_recovery
            or args.ra4_at_uart_recovery
            or args.ra4_uart_live
            or args.ra4_at_uart_live
            or args.ra4_at_transport
            or args.ra4_ota_download
            or args.ra4_ota_download_backend
            or args.ra4_samba
            or args.ra4_samba_programmer
            or args.time_service
            or args.time_service_readback
            or args.tcp_listener_close
            or args.sta_disconnect
        ):
            try:
                select_feature_page(fd, 0)
            except OSError:
                pass
        os.close(fd)

    expected_signature = b"U35D" + bytes((FEATURE_VERSION,))
    if report[:5] != expected_signature:
        raise RuntimeError(
            "unexpected user-UART diagnostic feature signature: "
            f"got {report[:8].hex()}, expected {expected_signature.hex()}"
        )

    if args.sta_disconnect:
        if report[5] != 38:
            raise RuntimeError("unexpected STA disconnect diagnostic page")
        print(f"HIDRAW={hidraw}")
        signed = lambda value: value if value < 128 else value - 256
        print(f"STA_DISCONNECT=attempts={read_u32_le(report, 6)} "
              f"events={read_u32_le(report, 10)} stage={report[14]} "
              f"result={signed(report[15])}")
        next_index, count = report[16:18]
        if next_index >= 16 or count > 16:
            raise RuntimeError("invalid STA disconnect ring bounds")
        history = [((next_index - count + i) % 16) for i in range(count)]
        print("STA_DISCONNECT_HISTORY=" + ",".join(
            f"{report[18 + i]}:{signed(report[34 + i])}" for i in history))
        print("STA_DISCONNECT_TELEMETRY_PASS")
        return 0

    if args.cdc_in:
        if report[5] != 1:
            raise RuntimeError("unexpected CDC IN telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_cdc_in_telemetry(report)
        print("CDC_IN_TELEMETRY_PASS")
        return 0

    if args.ap_start:
        if report[5] != 2:
            raise RuntimeError("unexpected AP-start diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_telemetry(report)
        print("AP_START_TELEMETRY_PASS")
        return 0

    if args.ap_start_notification:
        if report[5] != 15:
            raise RuntimeError("unexpected AP-start notification feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_notification_telemetry(report)
        print("AP_START_NOTIFICATION_TELEMETRY_PASS")
        return 0

    if args.ap_start_trace:
        if report[5] != 5:
            raise RuntimeError("unexpected AP-start trace feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_trace_telemetry(report)
        print("AP_START_TRACE_TELEMETRY_PASS")
        return 0

    if args.ap_start_notification_ab:
        if report[5] != 16:
            raise RuntimeError("unexpected AP-start notification A/B feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_notification_ab_telemetry(report)
        print("AP_START_NOTIFICATION_AB_TELEMETRY_PASS")
        return 0

    if args.ap_start_wpa_wait:
        if report[5] != 18:
            raise RuntimeError("unexpected AP-start WPA wait feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_wpa_wait_telemetry(report)
        print("AP_START_WPA_WAIT_TELEMETRY_PASS")
        return 0

    if args.ap_start_wpa_event_path:
        if report[5] != 19:
            raise RuntimeError("unexpected AP-start WPA event-path feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_wpa_event_path_telemetry(report)
        print("AP_START_WPA_EVENT_PATH_TELEMETRY_PASS")
        return 0

    if args.ap_start_create_ap:
        if report[5] != 20:
            raise RuntimeError("unexpected AP create_ap feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_create_ap_telemetry(report)
        print("AP_START_CREATE_AP_TELEMETRY_PASS")
        return 0

    if args.ap_start_allocator_trace:
        if report[5] != 6:
            raise RuntimeError("unexpected AP-start allocator trace feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_allocator_trace_telemetry(report)
        print("AP_START_ALLOCATOR_TRACE_TELEMETRY_PASS")
        return 0

    if args.ap_start_allocator_heap_walk:
        if report[5] != 21:
            raise RuntimeError("unexpected AP-start allocator heap-walk feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_allocator_heap_walk_telemetry(report)
        print("AP_START_ALLOCATOR_HEAP_WALK_TELEMETRY_PASS")
        return 0

    if args.ap_start_eloop_post:
        if report[5] != 17:
            raise RuntimeError("unexpected AP-start eloop-post feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_eloop_post_telemetry(report)
        print("AP_START_ELOOP_POST_TELEMETRY_PASS")
        return 0

    if args.ap_start_hostapd_lifecycle:
        if report[5] != 24:
            raise RuntimeError("unexpected AP-start hostapd lifecycle feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_hostapd_lifecycle_telemetry(report)
        print("AP_START_HOSTAPD_LIFECYCLE_TELEMETRY_PASS")
        return 0

    if args.ap_start_status_code:
        if report[5] != 25:
            raise RuntimeError("unexpected AP-start status-code feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_status_code_telemetry(report)
        print("AP_START_STATUS_CODE_TELEMETRY_PASS")
        return 0

    if args.ap_start_hostapd_state_update:
        if report[5] != 26:
            raise RuntimeError("unexpected AP-start hostapd state-update feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_hostapd_state_update_telemetry(report)
        print("AP_START_HOSTAPD_STATE_UPDATE_TELEMETRY_PASS")
        return 0

    if args.runtime_heap:
        print_runtime_heap(report)
        return 0

    if args.ap_start_heap:
        if report[5] != 3:
            raise RuntimeError("unexpected AP-start heap diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_heap_telemetry(report)
        print("AP_START_HEAP_TELEMETRY_PASS")
        return 0

    if args.ap_start_command:
        if report[5] != 4:
            raise RuntimeError("unexpected AP-start command telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_command_telemetry(report)
        print("AP_START_COMMAND_TELEMETRY_PASS")
        return 0

    if args.hid_data_path:
        if report[5] != 7:
            raise RuntimeError("unexpected HID data-path diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_hid_data_path_telemetry(report)
        print("HID_DATA_PATH_TELEMETRY_PASS")
        return 0

    if args.ap_start_watchdog:
        if report[5] != 8:
            raise RuntimeError("unexpected AP-start watchdog telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_ap_start_watchdog_telemetry(report)
        print("AP_START_WATCHDOG_TELEMETRY_PASS")
        return 0

    if args.ra4_bootstrap:
        if report[5] != 9:
            raise RuntimeError("unexpected RA4 bootstrap diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_bootstrap_telemetry(report)
        print("RA4_BOOTSTRAP_TELEMETRY_PASS")
        return 0

    if args.ra4_uart_recovery:
        if report[5] != 10:
            raise RuntimeError("unexpected RA4 UART recovery diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_uart_recovery_telemetry(report, "USER")
        print("RA4_UART_RECOVERY_TELEMETRY_PASS")
        return 0

    if args.ra4_at_uart_recovery:
        if report[5] != 11:
            raise RuntimeError("unexpected RA4 AT UART recovery diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_uart_recovery_telemetry(report, "AT")
        print("RA4_AT_UART_RECOVERY_TELEMETRY_PASS")
        return 0

    if args.ra4_uart_live:
        if report[5] != 12:
            raise RuntimeError("unexpected RA4 UART live diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_uart_live_telemetry(report, "USER")
        print("RA4_UART_LIVE_TELEMETRY_PASS")
        return 0

    if args.ra4_at_uart_live:
        if report[5] != 13:
            raise RuntimeError("unexpected RA4 AT UART live diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_uart_live_telemetry(report, "AT")
        print("RA4_AT_UART_LIVE_TELEMETRY_PASS")
        return 0

    if args.ra4_at_transport:
        if report[5] != 14:
            raise RuntimeError("unexpected RA4 AT transport diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_at_transport_telemetry(report)
        print("RA4_AT_TRANSPORT_TELEMETRY_PASS")
        return 0

    if args.ra4_ota_download:
        if report[5] != 32:
            raise RuntimeError("unexpected RA4 OTA download diagnostic feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_ota_download_telemetry(report)
        print("RA4_OTA_DOWNLOAD_TELEMETRY_PASS")
        return 0

    if args.ra4_ota_download_backend:
        if report[5] != 33:
            raise RuntimeError("unexpected RA4 OTA download backend feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_ota_download_backend_telemetry(report)
        print("RA4_OTA_DOWNLOAD_BACKEND_TELEMETRY_PASS")
        return 0

    if args.ra4_samba:
        if report[5] != 34:
            raise RuntimeError("unexpected RA4 SAM-BA telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_samba_telemetry(report)
        print("RA4_SAMBA_TELEMETRY_PASS")
        return 0

    if args.ra4_samba_programmer:
        if report[5] != 35:
            raise RuntimeError("unexpected RA4 SAM-BA programmer telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_ra4_samba_programmer_telemetry(report)
        print("RA4_SAMBA_PROGRAMMER_TELEMETRY_PASS")
        return 0

    if args.time_service:
        if report[5] != 36:
            raise RuntimeError("unexpected WiFiS3 getTime telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_time_service_telemetry(report)
        print("TIME_SERVICE_TELEMETRY_PASS")
        return 0

    if args.time_service_readback:
        if report[5] != 37:
            raise RuntimeError("unexpected WiFiS3 getTime readback telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_time_service_readback_telemetry(report)
        print("TIME_SERVICE_READBACK_TELEMETRY_PASS")
        return 0

    if args.tcp_listener_close:
        if report[5] != 31:
            raise RuntimeError("unexpected TCP listener close telemetry feature page")
        print(f"HIDRAW={hidraw}")
        print_tcp_listener_close_telemetry(report)
        print("TCP_LISTENER_CLOSE_TELEMETRY_PASS")
        return 0

    if report[5] != 0:
        raise RuntimeError("unexpected UART RX diagnostic feature page")

    mode = report[5]
    flags = report[6]
    gpio = report[9]
    print(f"HIDRAW={hidraw}")
    print(
        "GPIO35="
        f"mode={mode} ie={int(bool(flags & 0x01))} "
        f"oe={int(bool(flags & 0x02))} expected={int(bool(flags & 0x04))} "
        f"pu={int(bool(flags & 0x08))} function={report[7]} "
        f"uart_signal={report[8]} gpio={gpio}"
    )
    print(
        "GPIO35_REGISTERS="
        f"pad=0x{read_u32_le(report, 12):08x} "
        f"uart_cfg2=0x{read_u32_le(report, 16):08x} "
        f"samples={read_u32_le(report, 20)} "
        f"anomalies={read_u32_le(report, 24)}"
    )
    print(
        "USER_UART_TRANSPORT="
        f"cdc_packets={read_u32_le(report, 28)} "
        f"cdc_bytes={read_u32_le(report, 32)} "
        f"queue_drops={read_u32_le(report, 36)} "
        f"uart_tx_bytes={read_u32_le(report, 40)} "
        f"uart_tx_failures={read_u32_le(report, 44)}"
    )
    tx_flags = report[48]
    print(
        "USER_UART_RX="
        f"received_bytes={read_u32_le(report, 48)} "
        f"ring_overflows={read_u32_le(report, 52)} "
        f"buffered_bytes={int.from_bytes(report[56:58], 'little')} "
        f"program_1200={report[58]} reset_2400={report[59]} "
        f"normal={report[60]} line_coding_drops={report[61]}"
    )
    if (
        mode != 0
        or not flags & 0x01
        or flags & 0x02
        or not flags & 0x04
        or not flags & 0x08
        or report[7] != 7
        or report[8] != 3
        or gpio != 35
    ):
        raise RuntimeError("GPIO35 is not in the expected UART0 RX pull-up state")
    print("GPIO35_UART0_RX_PULLUP_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
