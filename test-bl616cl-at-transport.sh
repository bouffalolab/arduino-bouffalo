#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build/host-tests"
OUTPUT="${BUILD_DIR}/bl616cl_at_transport_test"

mkdir -p "${BUILD_DIR}"

g++ \
  -std=gnu++17 \
  -Wall \
  -Wextra \
  -Werror \
  "${ROOT_DIR}/UNOR4BL616CLBridge/at_transport.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/ble_hci_service.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/ble_hci_at_service.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/bouffalo_hci_packet_adapter.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/tcp_client_service.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/tcp_server_service.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/tcp_runtime_diagnostic.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/ping_service.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/udp_runtime_diagnostic.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/udp_service.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/tls_client_service.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/wifi_status.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/wifi_service.cpp" \
  "${ROOT_DIR}/tests/bl616cl_at_transport_test.cpp" \
  -o "${OUTPUT}"

"${OUTPUT}"
python3 "${ROOT_DIR}/tests/bl616cl_hci_fragment_gate_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_record_profile_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_available_bytes_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_cached_available_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_connect_completion_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_peer_failure_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_mtls_direct_ip_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_negative_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_resource_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_resource_peer_test.py"
python3 "${ROOT_DIR}/tests/ra4_tls_resource_owned_peer_test.py"
python3 "${ROOT_DIR}/tests/ra4_tls_resource_usb_reopen_test.py"
python3 "${ROOT_DIR}/tests/ra4_tls_resource_deadline_boundary_test.py"
python3 "${ROOT_DIR}/tests/ra4_tls_receive_budget_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_coalesce_read_test.py"
python3 "${ROOT_DIR}/tests/wifis3_sized_response_reserve_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_resource_capture_test.py"
python3 "${ROOT_DIR}/tests/ra4_nano_heap_diagnostic_test.py"
python3 "${ROOT_DIR}/tests/ble_tls_heap_boundaries_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_ap_scan_restore_test.py"
python3 "${ROOT_DIR}/tests/ra4_bossac_handoff_proxy_test.py"
python3 "${ROOT_DIR}/tests/ra4_bossac_uart_handoff_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_ap_join_window_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_graceful_close_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_close_peer_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_close_stability_runner_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_gettime_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tcp_resource_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_udp_resource_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_udp_resource_runner_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_network_throughput_source_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_peer_failure_recovery_source_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_generic_at_source_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_generic_usb_recovery_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_hostname_ipv6_source_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_ipv6_poll_response_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_wifis3_api_coverage_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_sta_recovery_source_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_live_reconnect_policy_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_udp_explicit_interface_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_udp_hid_deferred_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_controlled_ap_recovery_test.py"

compile_and_run() {
  local name="$1"
  shift
  g++ -std=gnu++17 -Wall -Wextra -Werror \
    -I"${ROOT_DIR}/UNOR4BL616CLBridge" "$@" -o "${BUILD_DIR}/${name}"
  "${BUILD_DIR}/${name}"
}

compile_and_run \
  bl616cl_self_fota_test \
  -I"${ROOT_DIR}/tests/fakes" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/at_transport.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/self_fota_service.cpp" \
  "${ROOT_DIR}/tests/bl616cl_self_fota_test.cpp"

compile_and_run \
  bl616cl_runtime_allocation_test \
  "${ROOT_DIR}/tests/bl616cl_runtime_allocation_test.cpp"

compile_and_run \
  bl616cl_cdc_line_coding_control_test \
  "${ROOT_DIR}/UNOR4BL616CLBridge/cdc_line_coding_control.cpp" \
  "${ROOT_DIR}/tests/bl616cl_cdc_line_coding_control_test.cpp"

compile_and_run \
  bl616cl_line_coding_request_gate_test \
  "${ROOT_DIR}/UNOR4BL616CLBridge/line_coding_request_gate.cpp" \
  "${ROOT_DIR}/tests/bl616cl_line_coding_request_gate_test.cpp"

compile_and_run \
  bl616cl_line_coding_request_queue_test \
  "${ROOT_DIR}/UNOR4BL616CLBridge/line_coding_request_queue.cpp" \
  "${ROOT_DIR}/tests/bl616cl_line_coding_request_queue_test.cpp"

compile_and_run \
  bl616cl_tcp_runtime_diagnostic_test \
  "${ROOT_DIR}/UNOR4BL616CLBridge/tcp_runtime_diagnostic.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/ap_packet_diagnostic.cpp" \
  "${ROOT_DIR}/tests/bl616cl_tcp_runtime_diagnostic_test.cpp"

compile_and_run \
  bl616cl_time_sync_protocol_test \
  "${ROOT_DIR}/UNOR4BL616CLBridge/time_sync_protocol.cpp" \
  "${ROOT_DIR}/tests/bl616cl_time_sync_protocol_test.cpp"

compile_and_run \
  bl616cl_recovery_control_test \
  -I"${ROOT_DIR}/tests/fakes" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/bl616cl_recovery_control.cpp" \
  "${ROOT_DIR}/tests/bl616cl_recovery_control_test.cpp"

python3 "${ROOT_DIR}/tests/apsta_fixture_control_test.py"

python3 "${ROOT_DIR}/tests/bl616cl_sta_disconnect_wait_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_runtime_heap_report_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_runtime_allocation_report_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_heap_ownership_report_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_initialization_heap_order_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_runtime_heap_policy_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_minimal_cxx_fault_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_setup_allocation_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_ip4_fragment_error_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_passive_tx_confirm_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_coex_beacon_priority_test.py"
python3 "${ROOT_DIR}/tests/ra4_wifis3_tls_negative_diagnostic_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_scan_window_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_window_timing_test.py"
python3 "${ROOT_DIR}/tests/tls_window_timing_decoder_test.py"
compile_and_run \
  bl616cl_at_response_storage_test \
  "${ROOT_DIR}/UNOR4BL616CLBridge/at_transport.cpp" \
  "${ROOT_DIR}/tests/bl616cl_at_response_storage_test.cpp" \
  -Wl,--wrap=malloc -Wl,--wrap=free

compile_and_run \
  bl616cl_tls_ca_sharing_test \
  "${ROOT_DIR}/UNOR4BL616CLBridge/at_transport.cpp" \
  "${ROOT_DIR}/UNOR4BL616CLBridge/tls_client_service.cpp" \
  "${ROOT_DIR}/tests/bl616cl_tls_ca_sharing_test.cpp" \
  -Wl,--wrap=malloc -Wl,--wrap=free

python3 "${ROOT_DIR}/tests/bl616cl_tls_stop_lifecycle_test.py"

python3 "${ROOT_DIR}/tests/ra4_wifis3_udp_write_trace_test.py"

python3 "${ROOT_DIR}/tests/bl616cl_ap_station_observer_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_uart_error_observer_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_uart_dma_rx_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_softap_session_preparation_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_listen_observer_test.py"
python3 "${ROOT_DIR}/tests/ra4_ota_peer_test.py"
python3 "${ROOT_DIR}/tests/ra4_ota_association_snapshot_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_mac_rx_diagnostic_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_sta_rx_ownership_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_sta_rx_chunks_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_pbuf_release_lifecycle_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_sta_tcp_release_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_receive_window_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_rx_credit_pool_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_rx_native_policy_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_ipv6_reassembly_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_ipv6_tcp_input_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_ipv6_tcp_peer_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_rx_budget_diagnostic_test.py"
python3 "${ROOT_DIR}/tests/tcp_rx_budget_decoder_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_rx_memory_decoder_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_rx_isolation_peer_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_prepared_usb_session_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_ap_lifecycle_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_lwip_allocation_failure_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_tx_preallocation_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tcp_flow_diagnostic_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_tls_flow_decoder_test.py"
python3 "${ROOT_DIR}/tests/ra4_all_tcp_slots_peer_test.py"
python3 "${ROOT_DIR}/tests/bl616cl_usb_mapping_test.py"
python3 "${ROOT_DIR}/tests/ra4_tls_throughput_usb_reopen_test.py"
python3 "${ROOT_DIR}/tests/bounded_serial_close_test.py"
python3 "${ROOT_DIR}/tests/serial_control_write_test.py"
python3 "${ROOT_DIR}/tests/ra4_at_roundtrip_timing_test.py"
python3 "${ROOT_DIR}/tests/ra4_tls_throughput_arm_preparation_test.py"
python3 "${ROOT_DIR}/tests/ra4_windows_tls_peer_cleanup_test.py"
python3 "${ROOT_DIR}/tests/ra4_tls_throughput_owned_peer_test.py"
g++ -std=c++11 -Wall -Wextra -Werror -I"${ROOT_DIR}/UNOR4BL616CLBridge" \
  "${ROOT_DIR}/tests/bl616cl_sta_tcp_diagnostic_test.cpp" -o "${BUILD_DIR}/sta_tcp_diagnostic_test"
"${BUILD_DIR}/sta_tcp_diagnostic_test"
g++ -std=c++11 -Wall -Wextra -Werror -DBL616CL_STA_TCP_DIAGNOSTIC_PEER_PORT=18444 \
  -I"${ROOT_DIR}/UNOR4BL616CLBridge" \
  "${ROOT_DIR}/tests/bl616cl_sta_tcp_diagnostic_test.cpp" -o "${BUILD_DIR}/sta_tcp_tls_port_test"
"${BUILD_DIR}/sta_tcp_tls_port_test"

g++ -std=gnu++17 -Wall -Wextra -Werror \
  -I "${ROOT_DIR}/UNOR4BL616CLBridge" \
  "${ROOT_DIR}/tests/bl616cl_udp_fragment_content_test.cpp" \
  -o "${BUILD_DIR}/bl616cl_udp_fragment_content_test"
"${BUILD_DIR}/bl616cl_udp_fragment_content_test"
