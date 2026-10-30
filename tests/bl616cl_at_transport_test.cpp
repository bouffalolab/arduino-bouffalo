#include "../UNOR4BL616CLBridge/at_transport.h"
#include "../UNOR4BL616CLBridge/ble_hci_at_service.h"
#include "../UNOR4BL616CLBridge/ble_hci_service.h"
#include "../UNOR4BL616CLBridge/bouffalo_hci_packet_adapter.h"
#include "../UNOR4BL616CLBridge/byte_ring.h"
#include "../UNOR4BL616CLBridge/hci_idle_scan_disable_compat.h"
#include "../UNOR4BL616CLBridge/hci_scan_advertising_quiet_gate.h"
#include "../UNOR4BL616CLBridge/tcp_client_service.h"
#include "../UNOR4BL616CLBridge/tcp_server_service.h"
#include "../UNOR4BL616CLBridge/ping_service.h"
#include "../UNOR4BL616CLBridge/tls_client_service.h"
#include "../UNOR4BL616CLBridge/udp_runtime_diagnostic.h"
#include "../UNOR4BL616CLBridge/udp_service.h"
#include "../UNOR4BL616CLBridge/wifi_service.h"
#include "../UNOR4BL616CLBridge/wifi_status.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using bridge::at::AtTransport;
using bridge::at::Command;
using bridge::at::CommandMode;
using bridge::at::CommandResult;

static_assert(
    sizeof(bridge::TcpClientService) < bridge::at::kTransportRawPayloadCapacity,
    "TCP receive scratch storage must remain transport-owned");
static_assert(
    sizeof(bridge::TcpServerService) < bridge::at::kTransportRawPayloadCapacity,
    "TCP server receive scratch storage must remain transport-owned");
static_assert(
    sizeof(bridge::TlsClientService) < bridge::at::kTransportRawPayloadCapacity,
    "TLS receive scratch storage must remain transport-owned");
static_assert(
    sizeof(bridge::UdpService) <
        2U * bridge::at::kTransportRawPayloadCapacity + 1024U,
    "UDP receive and transmit buffers must remain bounded");

struct TestState {
    std::string output;
    std::string raw_payload;
    size_t output_limit = static_cast<size_t>(-1);
};

[[noreturn]] void fail(const char *message)
{
    std::cerr << "test failure: " << message << '\n';
    std::exit(1);
}

void expect(bool value, const char *message)
{
    if (!value) {
        fail(message);
    }
}

size_t output_callback(void *context, const uint8_t *data, size_t size)
{
    TestState *state = static_cast<TestState *>(context);
    const size_t accepted = size < state->output_limit ? size : state->output_limit;
    state->output.append(reinterpret_cast<const char *>(data), accepted);
    return accepted;
}

CommandResult raw_payload_callback(void *context, const uint8_t *data, size_t size)
{
    static_cast<TestState *>(context)->raw_payload.assign(reinterpret_cast<const char *>(data), size);
    return CommandResult::Ok;
}

CommandResult command_callback(void *context, const Command &command, AtTransport &server)
{
    TestState *state = static_cast<TestState *>(context);

    if (strcmp(command.name, "+ECHO") == 0 && command.mode == CommandMode::Write) {
        expect(command.argument_count == 2U, "quoted argument count");
        expect(strcmp(command.arguments[0], "a,b") == 0, "quoted argument value");
        expect(strcmp(command.arguments[1], "c,d") == 0, "escaped argument value");
        server.write_response(command, "echo");
        return CommandResult::Ok;
    }

    if (strcmp(command.name, "+RAW") == 0 && command.mode == CommandMode::Write) {
        expect(command.argument_count == 1U && strcmp(command.arguments[0], "4") == 0, "raw length argument");
        expect(server.request_raw_payload(4U, raw_payload_callback, state), "raw payload request");
        return CommandResult::Deferred;
    }

    if (strcmp(command.name, "+DEFER") == 0 && command.mode == CommandMode::Run) {
        return CommandResult::Deferred;
    }

    if (strcmp(command.name, "+FWVERSION") == 0 && command.mode == CommandMode::Read) {
        server.write_response(command, "0.6.0");
        return CommandResult::Ok;
    }

    return CommandResult::Error;
}

CommandResult golden_command_callback(void *, const Command &command, AtTransport &server)
{
    if (strcmp(command.name, "+GMR") == 0 && command.mode == CommandMode::Run) {
        server.write_cstr("<len>\r\n");
        return CommandResult::Ok;
    }
    if (strcmp(command.name, "+CMD") == 0 && command.mode == CommandMode::Read) {
        server.write_response(command, "<test>");
        return CommandResult::Ok;
    }
    if (strcmp(command.name, "+FWVERSION") == 0 && command.mode == CommandMode::Read) {
        server.write_response(command, "0.6.0");
        return CommandResult::Ok;
    }
    if (strcmp(command.name, "+FWVERSION_U32") == 0 && command.mode == CommandMode::Read) {
        static constexpr uint8_t kVersion[] = { 0U, 6U, 0U, 0U };
        server.write_cstr(command.name);
        server.write_cstr(": ");
        server.write_data(kVersion, sizeof(kVersion));
        server.write_cstr("\r\n");
        return CommandResult::Ok;
    }
    if (strcmp(command.name, "+GETSTATUS") == 0 && command.mode == CommandMode::Read) {
        server.write_response(command, "0");
        return CommandResult::Ok;
    }
    if (strcmp(command.name, "+GETTIME") == 0 &&
        command.mode == CommandMode::Write) {
        expect(
            command.argument_count == 0U,
            "parameterless write command has no arguments");
        server.write_response(command, "1790108705");
        return CommandResult::Ok;
    }
    return CommandResult::Error;
}

void feed(AtTransport &transport, const char *text, uint32_t now_ms)
{
    const size_t size = strlen(text);
    expect(
        transport.feed(reinterpret_cast<const uint8_t *>(text), size, now_ms) == size,
        "complete input consumption");
}

void feed_bytes(AtTransport &transport, const uint8_t *data, size_t size, uint32_t now_ms)
{
    expect(transport.feed(data, size, now_ms) == size, "complete binary input consumption");
}

void flush_all(AtTransport &transport)
{
    for (size_t count = 0U; transport.output_pending() && count < 1024U; ++count) {
        transport.flush_output();
    }
    expect(!transport.output_pending(), "output flush completed");
}

struct FakeHciController {
    bridge::BleHciController::ReceivePacket receive = nullptr;
    void *receive_context = nullptr;
    std::string sent_packets;
    bool start_succeeds = true;
    bool send_succeeds = true;
    uint32_t start_count = 0U;
    uint32_t stop_count = 0U;

    bool emit(const uint8_t *packet, size_t size)
    {
        if (receive == nullptr) {
            return false;
        }
        receive(receive_context, packet, size);
        return true;
    }
};

bool fake_hci_start(
    void *context,
    bridge::BleHciController::ReceivePacket receive,
    void *receive_context)
{
    FakeHciController *controller = static_cast<FakeHciController *>(context);
    ++controller->start_count;
    controller->receive = receive;
    controller->receive_context = receive_context;
    return controller->start_succeeds;
}

void fake_hci_stop(void *context)
{
    FakeHciController *controller = static_cast<FakeHciController *>(context);
    ++controller->stop_count;
    controller->receive = nullptr;
    controller->receive_context = nullptr;
}

bool fake_hci_send(void *context, const uint8_t *packet, size_t size)
{
    FakeHciController *controller = static_cast<FakeHciController *>(context);
    if (!controller->send_succeeds) {
        return false;
    }
    controller->sent_packets.assign(reinterpret_cast<const char *>(packet), size);
    return true;
}

struct FakeWaitClock {
    FakeHciController *controller = nullptr;
    uint32_t now_ms = 0U;
    uint32_t idle_count = 0U;
    uint32_t emit_on_idle = 0U;
    const uint8_t *packet = nullptr;
    size_t packet_size = 0U;
};

uint32_t fake_now_ms(void *context)
{
    return static_cast<FakeWaitClock *>(context)->now_ms;
}

void fake_idle(void *context)
{
    FakeWaitClock *clock = static_cast<FakeWaitClock *>(context);
    ++clock->idle_count;
    ++clock->now_ms;
    if (clock->idle_count == clock->emit_on_idle) {
        expect(clock->controller->emit(clock->packet, clock->packet_size), "fake controller delivers HCI packet");
    }
}

CommandResult hci_command_callback(void *context, const Command &command, AtTransport &transport)
{
    return static_cast<bridge::BleHciAtService *>(context)->handle_command(command, transport);
}

struct HciPacketSink {
    std::string packet;
    uint32_t receive_count = 0U;
};

void hci_packet_sink(void *context, const uint8_t *packet, size_t packet_size)
{
    HciPacketSink *sink = static_cast<HciPacketSink *>(context);
    sink->packet.assign(reinterpret_cast<const char *>(packet), packet_size);
    ++sink->receive_count;
}

void expect_packet(const HciPacketSink &sink, const uint8_t *expected, size_t expected_size, const char *message)
{
    expect(
        sink.packet == std::string(reinterpret_cast<const char *>(expected), expected_size),
        message);
}

void run_bouffalo_hci_packet_adapter_tests()
{
    using bridge::BouffaloHciPacketAdapter;
    using bridge::BouffaloHciPacketType;

    BouffaloHciPacketAdapter adapter;
    HciPacketSink sink;
    uint8_t packet[bridge::BleHciService::kMaxPacketBytes] = {};
    adapter.set_receiver(hci_packet_sink, &sink);

    static constexpr uint8_t kResetStatus[] = { 0x00U };
    static constexpr uint8_t kResetComplete[] = { 0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U };
    expect(
        adapter.receive_callback(
            BouffaloHciPacketType::CommandComplete,
            0x0C03U,
            kResetStatus,
            sizeof(kResetStatus),
            packet,
            sizeof(packet)),
        "Bouffalo command complete reconstructed");
    expect_packet(sink, kResetComplete, sizeof(kResetComplete), "Bouffalo command complete H4 bytes");

    static constexpr uint8_t kStatus[] = { 0x00U };
    static constexpr uint8_t kCommandStatus[] = { 0x04U, 0x0FU, 0x04U, 0x00U, 0x01U, 0x0CU, 0x20U };
    expect(
        adapter.receive_callback(
            BouffaloHciPacketType::CommandStatus,
            0x200CU,
            kStatus,
            sizeof(kStatus),
            packet,
            sizeof(packet)),
        "Bouffalo command status reconstructed");
    expect_packet(sink, kCommandStatus, sizeof(kCommandStatus), "Bouffalo command status H4 bytes");
    expect(
        !adapter.receive_callback(
            BouffaloHciPacketType::CommandStatus,
            0x200CU,
            kStatus,
            sizeof(kStatus),
            packet,
            6U),
        "Bouffalo command status requires complete H4 capacity");

    static constexpr uint8_t kLeParameters[] = { 0x02U, 0x01U, 0x06U };
    static constexpr uint8_t kLeEvent[] = { 0x04U, 0x3EU, 0x03U, 0x02U, 0x01U, 0x06U };
    expect(
        adapter.receive_callback(
            BouffaloHciPacketType::LeEvent,
            0U,
            kLeParameters,
            sizeof(kLeParameters),
            packet,
            sizeof(packet)),
        "Bouffalo LE event reconstructed");
    expect_packet(sink, kLeEvent, sizeof(kLeEvent), "Bouffalo LE event H4 bytes");

    static constexpr uint8_t kEventParameters[] = { 0x02U, 0x01U, 0x00U };
    static constexpr uint8_t kEvent[] = { 0x04U, 0x13U, 0x03U, 0x02U, 0x01U, 0x00U };
    expect(
        adapter.receive_callback(
            BouffaloHciPacketType::Event,
            0x13U,
            kEventParameters,
            sizeof(kEventParameters),
            packet,
            sizeof(packet)),
        "Bouffalo generic event reconstructed");
    expect_packet(sink, kEvent, sizeof(kEvent), "Bouffalo generic event H4 bytes");

    static constexpr uint8_t kDebugParameters[] = { 0xA5U, 0x5AU };
    static constexpr uint8_t kDebugEvent[] = { 0x04U, 0xFFU, 0x02U, 0xA5U, 0x5AU };
    expect(
        adapter.receive_callback(
            BouffaloHciPacketType::DebugEvent,
            0U,
            kDebugParameters,
            sizeof(kDebugParameters),
            packet,
            sizeof(packet)),
        "Bouffalo debug event reconstructed");
    expect_packet(sink, kDebugEvent, sizeof(kDebugEvent), "Bouffalo debug event H4 bytes");

    static constexpr uint8_t kAcl[] = { 0x01U, 0x20U, 0x02U, 0x00U, 0xAAU, 0xBBU };
    static constexpr uint8_t kH4Acl[] = { 0x02U, 0x01U, 0x20U, 0x02U, 0x00U, 0xAAU, 0xBBU };
    expect(adapter.receive_acl(kAcl, sizeof(kAcl), packet, sizeof(packet)), "Bouffalo ACL reconstructed");
    expect_packet(sink, kH4Acl, sizeof(kH4Acl), "Bouffalo ACL H4 bytes");
    memcpy(packet + 1U, kAcl, sizeof(kAcl));
    expect(
        adapter.receive_acl(packet + 1U, sizeof(kAcl), packet, sizeof(packet)),
        "Bouffalo overlapping ACL buffer reconstructed");
    expect_packet(sink, kH4Acl, sizeof(kH4Acl), "Bouffalo overlapping ACL H4 bytes");

    expect(
        !adapter.receive_callback(
            BouffaloHciPacketType::CommandStatus,
            0x200CU,
            kStatus,
            0U,
            packet,
            sizeof(packet)),
        "Bouffalo command status rejects missing status");
    expect(
        !adapter.receive_callback(
            BouffaloHciPacketType::Event,
            0x100U,
            kEventParameters,
            sizeof(kEventParameters),
            packet,
            sizeof(packet)),
        "Bouffalo generic event rejects oversized event code");

    adapter.clear_receiver();
    expect(
        !adapter.receive_acl(kAcl, sizeof(kAcl), packet, sizeof(packet)),
        "Bouffalo adapter rejects packets without receiver");
}

void run_ble_hci_at_tests()
{
    using bridge::BleHciAtService;
    using bridge::BleHciController;
    using bridge::BleHciService;
    using bridge::BleHciWaitHooks;

    FakeHciController attachable_fake;
    BleHciService attachable;
    expect(!attachable.begin(), "HCI begin rejects missing backend");
    expect(
        attachable.set_controller(
            {
                &attachable_fake,
                fake_hci_start,
                fake_hci_stop,
                fake_hci_send,
            }),
        "HCI backend attaches while inactive");
    expect(attachable.begin(), "HCI begins after backend attachment");
    expect(
        !attachable.set_controller({}),
        "HCI backend replacement rejects active transport");
    attachable.end();

    FakeHciController fake;
    BleHciService hci(
        {
            &fake,
            fake_hci_start,
            fake_hci_stop,
            fake_hci_send,
        });
    static constexpr uint8_t kCommandComplete[] = { 0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U };
    FakeWaitClock clock = {
        &fake,
        100U,
        0U,
        0U,
        kCommandComplete,
        sizeof(kCommandComplete),
    };
    BleHciAtService hci_at(
        hci,
        {
            &clock,
            fake_now_ms,
            fake_idle,
        });
    TestState state;
    AtTransport transport(
        {
            64U,
            BleHciService::kMaxPacketBytes,
            2048U,
            1000U,
        },
        output_callback,
        &state);
    transport.set_command_handler(hci_command_callback, &hci_at);

    feed(transport, "AT+HCIAVAILABLE?\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "inactive HCIAVAILABLE fails");
    state.output.clear();

    feed(transport, "AT+HCIBEGIN\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+HCIBEGIN: OK\r\n", "HCI begin transcript");
    expect(fake.start_count == 1U && hci.active(), "HCI controller started");
    state.output.clear();

    expect(fake.emit(kCommandComplete, sizeof(kCommandComplete)), "stale HCI packet queued before repeated begin");
    expect(hci.available() == sizeof(kCommandComplete), "stale HCI packet visible before repeated begin");
    feed(transport, "AT+HCIBEGIN\r\n", 3U);
    flush_all(transport);
    expect(state.output == "+HCIBEGIN: OK\r\n", "repeated HCI begin transcript");
    expect(
        fake.stop_count == 1U && fake.start_count == 2U && hci.active() && hci.available() == 0U,
        "repeated HCI begin resets the prior controller session");
    state.output.clear();

    feed(transport, "AT+HCIAVAILABLE?\r\n", 4U);
    flush_all(transport);
    expect(state.output == "+HCIAVAILABLE: 0\r\nOK\r\n", "empty HCI availability transcript");
    state.output.clear();

    static constexpr uint8_t kReset[] = { 0x01U, 0x03U, 0x0CU, 0x00U };
    static constexpr char kWriteHeader[] = "AT+HCIWRITE=4\r\n";
    feed(transport, kWriteHeader, 5U);
    expect(transport.raw_payload_pending(), "HCI raw write pending");
    feed_bytes(transport, kReset, sizeof(kReset), 5U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "HCI reset write transcript");
    expect(fake.sent_packets == std::string(reinterpret_cast<const char *>(kReset), sizeof(kReset)), "HCI reset bytes forwarded");
    state.output.clear();

    expect(fake.emit(kCommandComplete, sizeof(kCommandComplete)), "HCI command complete queued");
    feed(transport, "AT+HCIAVAILABLE?\r\n", 6U);
    flush_all(transport);
    expect(state.output == "+HCIAVAILABLE: 7\r\nOK\r\n", "queued HCI availability transcript");
    state.output.clear();

    feed(transport, "AT+HCIREAD\r\n", 7U);
    flush_all(transport);
    std::string expected("+HCIREAD: 7|");
    expected.append(reinterpret_cast<const char *>(kCommandComplete), sizeof(kCommandComplete));
    expected.append("OK\r\n");
    expect(state.output == expected, "HCI read raw transcript");
    expect(hci.available() == 0U, "HCI read drains queued bytes");
    state.output.clear();

    clock.now_ms = 200U;
    clock.idle_count = 0U;
    clock.emit_on_idle = 3U;
    feed(transport, "AT+HCIWAIT=250\r\n", 8U);
    flush_all(transport);
    expect(state.output == "+HCIWAIT: OK\r\n", "HCI wait transcript");
    expect(clock.idle_count == 3U && hci.available() == sizeof(kCommandComplete), "HCI wait yields until packet arrives");
    state.output.clear();

    feed(transport, "AT+HCIREAD\r\n", 9U);
    flush_all(transport);
    state.output.clear();

    static constexpr uint8_t kAdvertisingReport[] = {
        0x04U, 0x3EU, 0x01U, 0x02U,
    };
    for (size_t index = 0U; index < 70U; ++index) {
        expect(
            fake.emit(kAdvertisingReport, sizeof(kAdvertisingReport)),
            "advertising report queued");
    }
    expect(
        fake.emit(kCommandComplete, sizeof(kCommandComplete)),
        "control completion queues independently from advertising reports");
    feed(transport, "AT+HCIREAD\r\n", 10U);
    flush_all(transport);
    std::string chunked_expected("+HCIREAD: 258|");
    chunked_expected.append(
        reinterpret_cast<const char *>(kCommandComplete),
        sizeof(kCommandComplete));
    for (size_t index = 0U; index < 62U; ++index) {
        chunked_expected.append(
            reinterpret_cast<const char *>(kAdvertisingReport),
            sizeof(kAdvertisingReport));
    }
    chunked_expected.append(reinterpret_cast<const char *>(kAdvertisingReport), 3U);
    chunked_expected.append("OK\r\n");
    expect(
        state.output == chunked_expected,
        "HCI read prioritizes completion events and caps the ArduinoBLE transport chunk");
    expect(
        hci.available() == 29U,
        "HCI read leaves the remaining advertising reports queued");
    state.output.clear();

    static constexpr uint8_t kScanDisable[] = {
        0x01U, 0x0CU, 0x20U, 0x02U, 0x00U, 0x00U,
    };
    expect(
        hci.send_packet(kScanDisable, sizeof(kScanDisable)),
        "scan disable reaches the HCI controller");
    expect(
        hci.available() == 1U,
        "scan disable preserves the last byte of the already started report");
    uint8_t report_tail = 0U;
    expect(hci.read(&report_tail, 1U) == 1U && report_tail == 0x02U,
        "scan disable finishes the host-visible H4 report before command completion");
    expect(
        fake.sent_packets ==
            std::string(reinterpret_cast<const char *>(kScanDisable), sizeof(kScanDisable)),
        "scan disable H4 bytes forwarded");

    static constexpr uint8_t kInvalidReset[] = { 0x01U, 0x03U, 0x0CU, 0x01U };
    feed(transport, kWriteHeader, 11U);
    feed_bytes(transport, kInvalidReset, sizeof(kInvalidReset), 11U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "invalid H4 command rejected");
    state.output.clear();

    fake.send_succeeds = false;
    feed(transport, kWriteHeader, 12U);
    feed_bytes(transport, kReset, sizeof(kReset), 12U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "HCI backend send failure");
    state.output.clear();
    fake.send_succeeds = true;

    feed(transport, "AT+HCIEND\r\n", 13U);
    flush_all(transport);
    expect(state.output == "+HCIEND: OK\r\n", "HCI end transcript");
    expect(fake.stop_count == 2U && !hci.active() && hci.available() == 0U, "HCI end cleans up controller state");
    state.output.clear();

    feed(transport, "AT+HCIREAD\r\n", 14U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "inactive HCI read fails");
}

void run_ble_hci_large_acl_tests()
{
    using bridge::BleHciService;

    const auto make_acl = [](uint16_t handle_with_flags, size_t payload_size, uint8_t seed) {
        std::vector<uint8_t> packet(payload_size + 5U, 0U);
        packet[0] = 0x02U;
        packet[1] = static_cast<uint8_t>(handle_with_flags & 0xFFU);
        packet[2] = static_cast<uint8_t>(handle_with_flags >> 8U);
        packet[3] = static_cast<uint8_t>(payload_size & 0xFFU);
        packet[4] = static_cast<uint8_t>(payload_size >> 8U);
        for (size_t index = 5U; index < packet.size(); ++index) {
            packet[index] = static_cast<uint8_t>(seed + index);
        }
        return packet;
    };

    FakeHciController fake;
    BleHciService hci(
        {
            &fake,
            fake_hci_start,
            fake_hci_stop,
            fake_hci_send,
        });
    expect(hci.begin(), "large ACL test begins HCI controller");

    const std::vector<uint8_t> first_acl = make_acl(0x2001U, 695U, 0x00U);
    const std::vector<uint8_t> second_acl = make_acl(0x1002U, 175U, 0xA0U);

    expect(
        hci.receive_packet(first_acl.data(), first_acl.size()),
        "valid ACL larger than former control queue is accepted");
    expect(
        hci.receive_packet(second_acl.data(), second_acl.size()),
        "second ACL with a different connection handle is accepted");
    expect(
        hci.available() == first_acl.size() + second_acl.size(),
        "large ACL packets retain their complete queued byte count");

    std::vector<uint8_t> readback(hci.available(), 0U);
    expect(
        hci.read(readback.data(), readback.size()) == readback.size(),
        "large ACL packets can be read back completely");
    std::vector<uint8_t> expected;
    expected.insert(expected.end(), first_acl.begin(), first_acl.end());
    expected.insert(expected.end(), second_acl.begin(), second_acl.end());
    expect(readback == expected, "ACL packets preserve cross-handle FIFO ordering");

    const std::vector<uint8_t> first_handle_start = make_acl(0x2001U, 8U, 0x10U);
    const std::vector<uint8_t> second_handle_start = make_acl(0x2002U, 8U, 0x20U);
    const std::vector<uint8_t> first_handle_continuation = make_acl(0x1001U, 8U, 0x30U);
    const std::vector<uint8_t> second_handle_continuation = make_acl(0x1002U, 8U, 0x40U);
    expect(hci.receive_packet(first_handle_start.data(), first_handle_start.size()), "first handle start queues");
    expect(hci.receive_packet(second_handle_start.data(), second_handle_start.size()), "second handle start queues");
    expect(
        hci.receive_packet(first_handle_continuation.data(), first_handle_continuation.size()),
        "first handle continuation queues");
    expect(
        hci.receive_packet(second_handle_continuation.data(), second_handle_continuation.size()),
        "second handle continuation queues");
    std::vector<uint8_t> interleaved_readback(hci.available(), 0U);
    expect(
        hci.read(interleaved_readback.data(), interleaved_readback.size()) == interleaved_readback.size(),
        "interleaved ACL fragments are fully readable");
    std::vector<uint8_t> interleaved_expected;
    interleaved_expected.insert(
        interleaved_expected.end(),
        first_handle_start.begin(),
        first_handle_start.end());
    interleaved_expected.insert(
        interleaved_expected.end(),
        second_handle_start.begin(),
        second_handle_start.end());
    interleaved_expected.insert(
        interleaved_expected.end(),
        first_handle_continuation.begin(),
        first_handle_continuation.end());
    interleaved_expected.insert(
        interleaved_expected.end(),
        second_handle_continuation.begin(),
        second_handle_continuation.end());
    expect(
        interleaved_readback == interleaved_expected,
        "two-handle ACL first and continuation fragments preserve arrival order");

    std::vector<uint8_t> queued_event = { 0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U };
    expect(
        hci.receive_packet(queued_event.data(), queued_event.size()),
        "control event queues before capacity rejection test");
    std::vector<uint8_t> too_large(BleHciService::kControlRxQueueBytes, 0x5AU);
    too_large[0] = 0x02U;
    too_large[1] = 0x03U;
    too_large[2] = 0x20U;
    too_large[3] = static_cast<uint8_t>((too_large.size() - 5U) & 0xFFU);
    too_large[4] = static_cast<uint8_t>((too_large.size() - 5U) >> 8U);
    expect(
        !hci.receive_packet(too_large.data(), too_large.size()),
        "queue-full ACL is rejected as a whole packet");
    expect(
        hci.available() == queued_event.size(),
        "rejected ACL does not consume or corrupt existing queued data");

    std::vector<uint8_t> event_readback(queued_event.size(), 0U);
    expect(
        hci.read(event_readback.data(), event_readback.size()) == event_readback.size() &&
            event_readback == queued_event,
        "existing control event survives rejected ACL");

    hci.end();
}

void run_ble_hci_large_acl_at_read_tests()
{
    using bridge::BleHciAtService;
    using bridge::BleHciService;

    FakeHciController fake;
    BleHciService hci(
        {
            &fake,
            fake_hci_start,
            fake_hci_stop,
            fake_hci_send,
        });
    BleHciAtService hci_at(hci, {});
    TestState state;
    AtTransport transport(
        {
            64U,
            BleHciService::kMaxPacketBytes,
            2048U,
            1000U,
        },
        output_callback,
        &state);
    transport.set_command_handler(hci_command_callback, &hci_at);
    feed(transport, "AT+HCIBEGIN\r\n", 1U);
    flush_all(transport);
    state.output.clear();

    std::vector<uint8_t> acl(700U, 0U);
    acl[0] = 0x02U;
    acl[1] = 0x01U;
    acl[2] = 0x20U;
    acl[3] = static_cast<uint8_t>((acl.size() - 5U) & 0xFFU);
    acl[4] = static_cast<uint8_t>((acl.size() - 5U) >> 8U);
    for (size_t index = 5U; index < acl.size(); ++index) {
        acl[index] = static_cast<uint8_t>(index);
    }
    expect(fake.emit(acl.data(), acl.size()), "large ACL queues for chunked HCIREAD");

    size_t offset = 0U;
    for (const size_t expected_chunk : { 258U, 258U, 184U }) {
        feed(transport, "AT+HCIREAD\r\n", 2U + static_cast<uint32_t>(offset));
        flush_all(transport);
        std::string expected = "+HCIREAD: " + std::to_string(expected_chunk) + "|";
        expected.append(
            reinterpret_cast<const char *>(acl.data() + offset),
            expected_chunk);
        expected.append("OK\r\n");
        expect(state.output == expected, "large ACL HCIREAD chunk framing and byte order");
        state.output.clear();
        offset += expected_chunk;
    }
    expect(offset == acl.size() && hci.available() == 0U, "all large ACL HCIREAD chunks drain exactly once");

    hci.end();
}

void run_ble_hci_large_write_tests()
{
    using bridge::BleHciAtService;
    using bridge::BleHciService;

    FakeHciController fake;
    BleHciService hci(
        {
            &fake,
            fake_hci_start,
            fake_hci_stop,
            fake_hci_send,
        });
    BleHciAtService hci_at(hci, {});
    TestState state;
    AtTransport transport(
        {
            64U,
            BleHciService::kMaxPacketBytes,
            2048U,
            1000U,
        },
        output_callback,
        &state);
    transport.set_command_handler(hci_command_callback, &hci_at);
    feed(transport, "AT+HCIBEGIN\r\n", 1U);
    flush_all(transport);
    state.output.clear();

    std::vector<uint8_t> acl(BleHciService::kMaxPacketBytes, 0U);
    acl[0] = 0x02U;
    acl[1] = 0x04U;
    acl[2] = 0x20U;
    acl[3] = static_cast<uint8_t>((acl.size() - 5U) & 0xFFU);
    acl[4] = static_cast<uint8_t>((acl.size() - 5U) >> 8U);
    for (size_t index = 5U; index < acl.size(); ++index) {
        acl[index] = static_cast<uint8_t>(index ^ 0x5AU);
    }

    std::string header = "AT+HCIWRITE=" + std::to_string(acl.size()) + "\r\n";
    feed_bytes(
        transport,
        reinterpret_cast<const uint8_t *>(header.data()),
        header.size(),
        2U);
    expect(transport.raw_payload_pending(), "large HCI write waits for the complete raw payload");
    feed_bytes(transport, acl.data(), acl.size(), 2U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "large HCI write transcript");
    expect(
        fake.sent_packets == std::string(reinterpret_cast<const char *>(acl.data()), acl.size()),
        "large HCI write forwards the exact ACL packet");

    hci.end();
}

void run_ble_hci_fragment_priority_tests()
{
    FakeHciController fake;
    bridge::BleHciService hci({&fake, fake_hci_start, fake_hci_stop, fake_hci_send});
    expect(hci.begin(), "fragment priority HCI begin");
    const uint8_t report[] = {0x04U, 0x3EU, 0x02U, 0x02U, 0x00U};
    const uint8_t complete[] = {0x04U, 0x0EU, 0x04U, 0x01U, 0x03U, 0x0CU, 0x00U};
    const uint8_t disable[] = {0x01U, 0x0CU, 0x20U, 0x02U, 0x00U, 0x00U};
    for (unsigned cycle = 0U; cycle < 200U; ++cycle) {
        for (size_t split = 1U; split < sizeof(report); ++split) {
            hci.clear();
            expect(fake.emit(report, sizeof(report)) && fake.emit(report, sizeof(report)),
                "queue two reports across ring wrap");
            uint8_t output[32] = {};
            expect(hci.read(nullptr, split) == 0U && hci.read(output, 0U) == 0U,
                "empty HCI reads do not start a packet");
            size_t count = hci.read(output, split);
            expect(count == split, "read arbitrary advertising header or payload prefix");
            expect(fake.emit(complete, sizeof(complete)), "control event arrives after partial report");
            const bool discard = cycle % 2U == 0U;
            if (discard) expect(hci.send_packet(disable, sizeof(disable)), "disable scan mid report");
            while (hci.available() != 0U) {
                const size_t read = hci.read(output + count, 1U + cycle % 7U);
                expect(read != 0U, "fragment reader makes progress");
                count += read;
            }
            std::string expected(reinterpret_cast<const char *>(report), sizeof(report));
            expected.append(reinterpret_cast<const char *>(complete), sizeof(complete));
            if (!discard) expected.append(reinterpret_cast<const char *>(report), sizeof(report));
            expect(std::string(reinterpret_cast<const char *>(output), count) == expected,
                "control priority changes only between complete H4 frames");
        }
    }
    uint8_t prefix = 0U;
    expect(fake.emit(report, sizeof(report)) && hci.read(&prefix, 1U) == 1U,
        "leave a partial report before shutdown");
    hci.end();
    expect(hci.begin() && fake.emit(complete, sizeof(complete)), "restart after fragmented stream");
    uint8_t output[16] = {};
    expect(hci.read(output, sizeof(output)) == sizeof(complete) &&
        std::string(reinterpret_cast<const char *>(output), sizeof(complete)) ==
        std::string(reinterpret_cast<const char *>(complete), sizeof(complete)),
        "restart clears partial-frame state");
    hci.end();
}

void run_bouffalo_hci_controller_source_tests()
{
    const std::filesystem::path controller_source =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "UNOR4BL616CLBridge" / "bouffalo_hci_controller.cpp";
    std::ifstream source_file(controller_source);
    expect(source_file.good(), "Bouffalo HCI controller source is readable");

    const std::string source(
        (std::istreambuf_iterator<char>(source_file)),
        std::istreambuf_iterator<char>());
    expect(
        source.find("sdk_packet.p.hci_cmd.opcode = read_le16(packet + 1U);") != std::string::npos,
        "Bouffalo HCI command stores the H4 opcode in the SDK command field");
    expect(
        source.find("destination_id = read_le16(packet + 1U);") == std::string::npos,
        "Bouffalo HCI command does not misuse its opcode as a task destination");
}

void run_hci_idle_scan_disable_compat_tests()
{
    bridge::HciIdleScanDisableCompat compat;
    static constexpr uint8_t kIdleDisableWithFilterDuplicates[] = {
        0x01U, 0x0CU, 0x20U, 0x02U, 0x00U, 0x01U,
    };
    static constexpr uint8_t kIdleDisableInvalidFilterDuplicates[] = {
        0x01U, 0x0CU, 0x20U, 0x02U, 0x00U, 0x02U,
    };
    static constexpr uint8_t kSetScanParameters[] = {
        0x01U, 0x0BU, 0x20U, 0x07U, 0x01U, 0x20U, 0x00U, 0x20U, 0x00U, 0x00U, 0x00U,
    };
    static constexpr uint8_t kEnableScan[] = {
        0x01U, 0x0CU, 0x20U, 0x02U, 0x01U, 0x01U,
    };
    static constexpr uint8_t kReset[] = { 0x01U, 0x03U, 0x0CU, 0x00U };

    expect(
        compat.should_complete(
            kIdleDisableWithFilterDuplicates,
            sizeof(kIdleDisableWithFilterDuplicates)),
        "idle legacy scan disable receives the required no-op completion");
    compat.note_synthetic_idle_disable();
    expect(
        compat.take_scan_parameters_prime_request(
            kSetScanParameters,
            sizeof(kSetScanParameters)),
        "synthetic idle scan disable primes the next legacy scan configuration");
    expect(
        !compat.take_scan_parameters_prime_request(
            kSetScanParameters,
            sizeof(kSetScanParameters)),
        "scan configuration priming request is consumed once");
    expect(
        !compat.should_complete(
            kIdleDisableInvalidFilterDuplicates,
            sizeof(kIdleDisableInvalidFilterDuplicates)),
        "idle scan disable preserves invalid duplicate-filter errors");

    compat.observe_host_packet(kSetScanParameters, sizeof(kSetScanParameters));
    expect(
        !compat.should_complete(
            kIdleDisableWithFilterDuplicates,
            sizeof(kIdleDisableWithFilterDuplicates)),
        "configured scan disable is passed to the controller");

    compat.reset();
    compat.observe_host_packet(kEnableScan, sizeof(kEnableScan));
    expect(
        !compat.should_complete(
            kIdleDisableWithFilterDuplicates,
            sizeof(kIdleDisableWithFilterDuplicates)),
        "active scan disable is passed to the controller");

    compat.observe_host_packet(kReset, sizeof(kReset));
    expect(
        compat.should_complete(
            kIdleDisableWithFilterDuplicates,
            sizeof(kIdleDisableWithFilterDuplicates)),
        "controller reset restores the idle scan-disable compatibility state");
}

void run_hci_scan_advertising_quiet_gate_tests()
{
    bridge::HciScanAdvertisingQuietGate gate;
    expect(!gate.should_drop(100U), "idle advertising quiet gate passes reports");

    gate.arm(100U);
    expect(gate.should_drop(100U), "advertising quiet gate drops reports immediately after scan enable");
    expect(
        gate.should_drop(100U + bridge::HciScanAdvertisingQuietGate::kQuietDurationMs - 1U),
        "advertising quiet gate holds reports through its bounded quiet interval");
    expect(
        !gate.should_drop(100U + bridge::HciScanAdvertisingQuietGate::kQuietDurationMs),
        "advertising quiet gate releases reports when the interval ends");
    expect(!gate.should_drop(200U), "advertising quiet gate remains released");

    gate.arm(0xFFFFFFF0U);
    expect(gate.should_drop(0xFFFFFFF5U), "advertising quiet gate handles millisecond wrap before deadline");
    expect(
        !gate.should_drop(0xFFFFFFF0U + bridge::HciScanAdvertisingQuietGate::kQuietDurationMs),
        "advertising quiet gate handles millisecond wrap at deadline");

    bridge::HciScanAdvertisingRateGate rate_gate;
    expect(!rate_gate.should_drop(100U), "advertising rate gate delivers the first report");
    expect(
        rate_gate.should_drop(100U + bridge::HciScanAdvertisingRateGate::kMinimumIntervalMs - 1U),
        "advertising rate gate bounds reports during its delivery interval");
    expect(
        !rate_gate.should_drop(100U + bridge::HciScanAdvertisingRateGate::kMinimumIntervalMs),
        "advertising rate gate releases one report at the delivery interval");
    expect(
        rate_gate.should_drop(100U + bridge::HciScanAdvertisingRateGate::kMinimumIntervalMs + 1U),
        "advertising rate gate re-arms after each delivered report");

    rate_gate.reset();
    expect(!rate_gate.should_drop(0xFFFFFFF0U), "advertising rate gate starts before millisecond wrap");
    expect(
        !rate_gate.should_drop(
            0xFFFFFFF0U + bridge::HciScanAdvertisingRateGate::kMinimumIntervalMs),
        "advertising rate gate handles millisecond wrap at a delivery boundary");
}

struct GoldenVector {
    const char *input;
    const char *expected;
};

void run_golden_transcripts()
{
    static constexpr GoldenVector kVectors[] = {
        { "AT\r\n", "OK\r\n" },
        { "AT+GMR\r\n", "<len>\r\nOK\r\n" },
        { "AT+CMD?\r\n", "+CMD: <test>\r\nOK\r\n" },
        { "AT+FWVERSION?\r\n", "+FWVERSION: 0.6.0\r\nOK\r\n" },
        { "AT+FWVERSION\r\n", "ERROR\r\n" },
        { "AT+GETSTATUS?\r\n", "+GETSTATUS: 0\r\nOK\r\n" },
        { "AT+GETTIME=\r\n", "+GETTIME: 1790108705\r\nOK\r\n" },
        { "AT+GETSTATUS\r\n", "ERROR\r\n" },
        { "BAD\r\n", "ERROR\r\n" },
        { "AT+UNKNOWN\r\n", "ERROR\r\n" },
    };

    for (const GoldenVector &vector : kVectors) {
        TestState state;
        AtTransport transport(
            {
                32U,
                8U,
                64U,
                20U,
            },
            output_callback,
            &state);
        transport.set_command_handler(golden_command_callback, nullptr);

        feed(transport, vector.input, 1U);
        flush_all(transport);
        expect(state.output == vector.expected, "source-static golden transcript");
    }

    TestState state;
    AtTransport transport(
        {
            32U,
            8U,
            64U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(golden_command_callback, nullptr);

    feed(transport, "AT+FWVERSION_U32?\r\n", 2U);
    flush_all(transport);
    const char kVersionPrefix[] = "+FWVERSION_U32: ";
    const uint8_t kVersion[] = { 0U, 6U, 0U, 0U };
    std::string expected(kVersionPrefix);
    expected.append(reinterpret_cast<const char *>(kVersion), sizeof(kVersion));
    expected.append("\r\nOK\r\n");
    expect(state.output == expected, "source-static binary version golden transcript");
}

void run_wifi_status_tracker_tests()
{
    using bridge::WifiStatus;
    using bridge::WifiStatusEvent;
    using bridge::WifiStatusTracker;

    WifiStatusTracker tracker;
    expect(tracker.status() == WifiStatus::Idle, "wifi status initial idle");
    expect(strcmp(bridge::wifi_status_text(tracker.status()), "0") == 0, "wifi status initial text");

    tracker.on_event(WifiStatusEvent::ScanDone);
    expect(tracker.status() == WifiStatus::ScanCompleted, "wifi scan completed status");
    tracker.on_event(WifiStatusEvent::StaStarted);
    expect(tracker.status() == WifiStatus::Idle, "wifi sta start idle status");
    tracker.on_event(WifiStatusEvent::StaConnected);
    expect(tracker.status() == WifiStatus::Connected, "wifi sta connected status");
    tracker.on_event(WifiStatusEvent::StaDisconnected);
    expect(tracker.status() == WifiStatus::Idle, "wifi sta disconnected baseline status");
    tracker.on_event(WifiStatusEvent::StaStopped);
    expect(tracker.status() == WifiStatus::NoModule, "wifi sta stopped baseline status");
    expect(strcmp(bridge::wifi_status_text(tracker.status()), "255") == 0, "wifi no module text");

    tracker.on_event(WifiStatusEvent::ApStarted);
    expect(tracker.status() == WifiStatus::ApListening, "wifi ap started status");
    tracker.on_event(WifiStatusEvent::ApStaConnected);
    expect(tracker.status() == WifiStatus::ApConnected, "wifi ap station connected status");
    tracker.on_event(WifiStatusEvent::ApStaDisconnected);
    expect(tracker.status() == WifiStatus::ApConnected, "wifi ap station disconnect preserves status");
    tracker.on_event(WifiStatusEvent::ApStopped);
    expect(tracker.status() == WifiStatus::Idle, "wifi ap stopped status");

    tracker.set_status(WifiStatus::NoSsidAvailable);
    expect(strcmp(bridge::wifi_status_text(tracker.status()), "1") == 0, "wifi no ssid text");
    tracker.set_status(WifiStatus::ConnectFailed);
    expect(strcmp(bridge::wifi_status_text(tracker.status()), "4") == 0, "wifi connect failed text");
    tracker.set_status(WifiStatus::ConnectionLost);
    expect(strcmp(bridge::wifi_status_text(tracker.status()), "5") == 0, "wifi connection lost text");
    tracker.set_status(WifiStatus::Disconnected);
    expect(strcmp(bridge::wifi_status_text(tracker.status()), "6") == 0, "wifi disconnected text");
    tracker.set_status(WifiStatus::ApFailed);
    tracker.on_event(WifiStatusEvent::Ignored);
    expect(tracker.status() == WifiStatus::ApFailed, "wifi ignored event preserves status");
    expect(strcmp(bridge::wifi_status_text(tracker.status()), "9") == 0, "wifi ap failed text");
}

struct FakeWifiBackend {
    bridge::WifiScanResult results[2] = {};
    size_t result_count = 0U;
    bool start_succeeds = true;
    bool start_sta_succeeds = true;
    bool disconnect_succeeds = true;
    bool wifi_off_supported = true;
    bool reconnect_succeeds = true;
    bool sta_role_succeeds = true;
    bool sta_coexistence_succeeds = true;
    bool ap_start_succeeds = true;
    bool ap_stop_succeeds = true;
    bool ap_config_succeeds = true;
    bool sta_query_succeeds = true;
    uint32_t start_count = 0U;
    uint32_t start_sta_count = 0U;
    uint32_t disconnect_count = 0U;
    uint32_t reconnect_count = 0U;
    uint32_t sta_role_count = 0U;
    uint32_t sta_coexistence_count = 0U;
    uint32_t ap_start_count = 0U;
    uint32_t ap_stop_count = 0U;
    uint32_t ap_config_count = 0U;
    bool last_disconnect_wifi_off = false;
    bool last_auto_reconnect = true;
    bool last_sta_role_enabled = true;
    bool last_ap_hidden = false;
    uint8_t last_ap_channel = 0U;
    uint8_t last_ap_max_connections = 0U;
    uint8_t last_ap_ssid[bridge::kWifiApSsidBytes] = {};
    size_t last_ap_ssid_size = 0U;
    uint8_t last_ap_password[bridge::kWifiApPasswordBytes] = {};
    size_t last_ap_password_size = 0U;
    bridge::WifiApNetworkConfig last_ap_network;
    uint8_t ap_mac[6] = {0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
    uint8_t sta_mac[6] = {0x02U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU};
    uint8_t ap_ip[4] = {192U, 168U, 4U, 1U};
    uint8_t ap_gateway[4] = {192U, 168U, 4U, 1U};
    uint8_t ap_netmask[4] = {255U, 255U, 255U, 0U};
    bool hostname_succeeds = true;
    bool ipv6_enable_succeeds = true;
    bool ipv6_query_succeeds = true;
    uint32_t hostname_set_count = 0U;
    uint32_t ipv6_enable_count = 0U;
    uint8_t hostname[bridge::kWifiHostnameBytes + 1U] = "wlan";
    const char *ipv6_address = "fe80::1234";
    uint8_t last_ssid[bridge::kWifiStaSsidBytes] = {};
    size_t last_ssid_size = 0U;
    uint8_t last_password[bridge::kWifiStaPasswordBytes] = {};
    size_t last_password_size = 0U;
    bridge::WifiStaNetworkConfig last_sta_network;
    bool last_sta_auto_reconnect = true;
    bool sta_dns_succeeds = true;
    uint32_t sta_dns_set_count = 0U;
    uint8_t last_sta_dns1[4] = {};
    uint8_t last_sta_dns2[4] = {};
    uint8_t sta_ssid[bridge::kWifiStaSsidBytes] = {
        'f', 'i', 'x', 't', 'u', 'r', 'e', '-', 's', 't', 'a',
    };
    size_t sta_ssid_size = 11U;
    int sta_rssi = -58;
    uint8_t sta_bssid[6] = {0x0EU, 0x0FU, 0x10U, 0x11U, 0x12U, 0x13U};
    bridge::WifiStaIpInfo sta_ip_info = {
        {192U, 0U, 2U, 9U},
        {192U, 0U, 2U, 1U},
        {255U, 255U, 255U, 0U},
        {192U, 0U, 2U, 53U},
        {198U, 51U, 100U, 53U},
    };
};

bool fake_wifi_start_scan(void *context)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->start_count;
    return backend->start_succeeds;
}

size_t fake_wifi_copy_scan_results(
    void *context,
    bridge::WifiScanResult *results,
    size_t result_capacity)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    const size_t copied =
        backend->result_count < result_capacity ? backend->result_count : result_capacity;
    memcpy(results, backend->results, copied * sizeof(results[0]));
    return copied;
}

bool fake_wifi_start_sta_connect(
    void *context,
    const uint8_t *ssid,
    size_t ssid_size,
    const uint8_t *password,
    size_t password_size,
    const bridge::WifiStaNetworkConfig *network,
    bool auto_reconnect)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->start_sta_count;
    backend->last_ssid_size = ssid_size;
    backend->last_password_size = password_size;
    memcpy(backend->last_ssid, ssid, ssid_size);
    if (password_size != 0U) {
        memcpy(backend->last_password, password, password_size);
    }
    backend->last_sta_network = network != nullptr
        ? *network
        : bridge::WifiStaNetworkConfig{};
    backend->last_sta_auto_reconnect = auto_reconnect;
    return backend->start_sta_succeeds;
}

bool fake_wifi_set_sta_dns(
    void *context,
    const uint8_t dns1[4],
    const uint8_t dns2[4])
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->sta_dns_set_count;
    memcpy(backend->last_sta_dns1, dns1, sizeof(backend->last_sta_dns1));
    memcpy(backend->last_sta_dns2, dns2, sizeof(backend->last_sta_dns2));
    return backend->sta_dns_succeeds;
}

bool fake_wifi_disconnect_sta(void *context, bool wifi_off)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->disconnect_count;
    backend->last_disconnect_wifi_off = wifi_off;
    return !wifi_off || backend->wifi_off_supported
        ? backend->disconnect_succeeds
        : false;
}

bool fake_wifi_set_auto_reconnect(void *context, bool enabled)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->reconnect_count;
    backend->last_auto_reconnect = enabled;
    return backend->reconnect_succeeds;
}

bool fake_wifi_set_sta_role_enabled(void *context, bool enabled)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->sta_role_count;
    backend->last_sta_role_enabled = enabled;
    return backend->sta_role_succeeds;
}

bool fake_wifi_enable_sta_coexistence(void *context)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->sta_coexistence_count;
    return backend->sta_coexistence_succeeds;
}

bool fake_wifi_get_sta_ssid(
    void *context,
    uint8_t *ssid,
    size_t ssid_capacity,
    size_t *ssid_size)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    if (!backend->sta_query_succeeds ||
        ssid == nullptr ||
        ssid_size == nullptr ||
        backend->sta_ssid_size > ssid_capacity) {
        return false;
    }
    memcpy(ssid, backend->sta_ssid, backend->sta_ssid_size);
    *ssid_size = backend->sta_ssid_size;
    return true;
}

bool fake_wifi_get_sta_rssi(void *context, int *rssi)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    if (!backend->sta_query_succeeds || rssi == nullptr) {
        return false;
    }
    *rssi = backend->sta_rssi;
    return true;
}

bool fake_wifi_get_sta_bssid(void *context, uint8_t bssid[6])
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    if (!backend->sta_query_succeeds || bssid == nullptr) {
        return false;
    }
    memcpy(bssid, backend->sta_bssid, sizeof(backend->sta_bssid));
    return true;
}

bool fake_wifi_get_sta_ip_info(
    void *context,
    bridge::WifiStaIpInfo *info)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    if (!backend->sta_query_succeeds || info == nullptr) {
        return false;
    }
    *info = backend->sta_ip_info;
    return true;
}

bool fake_wifi_start_softap(
    void *context,
    const uint8_t *ssid,
    size_t ssid_size,
    const uint8_t *password,
    size_t password_size,
    uint8_t channel,
    bool hidden,
    uint8_t max_connections,
    const bridge::WifiApNetworkConfig *network)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->ap_start_count;
    backend->last_ap_ssid_size = ssid_size;
    backend->last_ap_password_size = password_size;
    backend->last_ap_channel = channel;
    backend->last_ap_hidden = hidden;
    backend->last_ap_max_connections = max_connections;
    backend->last_ap_network = *network;
    memcpy(backend->last_ap_ssid, ssid, ssid_size);
    if (password_size != 0U) {
        memcpy(backend->last_ap_password, password, password_size);
    }
    return backend->ap_start_succeeds;
}

bool fake_wifi_stop_softap(void *context, bool)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->ap_stop_count;
    return backend->ap_stop_succeeds;
}

bool fake_wifi_get_ap_mac(void *context, uint8_t mac[6])
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    memcpy(mac, backend->ap_mac, sizeof(backend->ap_mac));
    return true;
}

bool fake_wifi_get_sta_mac(void *context, uint8_t mac[6])
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    memcpy(mac, backend->sta_mac, sizeof(backend->sta_mac));
    return true;
}

bool fake_wifi_get_ap_ip(
    void *context,
    uint8_t ip[4],
    uint8_t gateway[4],
    uint8_t netmask[4])
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    memcpy(ip, backend->ap_ip, sizeof(backend->ap_ip));
    memcpy(gateway, backend->ap_gateway, sizeof(backend->ap_gateway));
    memcpy(netmask, backend->ap_netmask, sizeof(backend->ap_netmask));
    return true;
}

bool fake_wifi_set_ap_config(
    void *context,
    const uint8_t ip[4],
    const uint8_t gateway[4],
    const uint8_t netmask[4])
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->ap_config_count;
    backend->last_ap_network.configured = true;
    memcpy(backend->last_ap_network.ip, ip, sizeof(backend->last_ap_network.ip));
    memcpy(backend->last_ap_network.gateway, gateway, sizeof(backend->last_ap_network.gateway));
    memcpy(backend->last_ap_network.netmask, netmask, sizeof(backend->last_ap_network.netmask));
    return backend->ap_config_succeeds;
}

bool fake_wifi_set_sta_hostname(
    void *context,
    const uint8_t *hostname,
    size_t hostname_size)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->hostname_set_count;
    if (!backend->hostname_succeeds ||
        hostname == nullptr ||
        hostname_size == 0U ||
        hostname_size > bridge::kWifiHostnameBytes) {
        return false;
    }
    memcpy(backend->hostname, hostname, hostname_size);
    backend->hostname[hostname_size] = '\0';
    return true;
}

bool fake_wifi_get_sta_hostname(
    void *context,
    uint8_t *hostname,
    size_t hostname_capacity,
    size_t *hostname_size)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    const size_t size = strlen(reinterpret_cast<const char *>(backend->hostname));
    if (!backend->hostname_succeeds ||
        hostname == nullptr ||
        hostname_size == nullptr ||
        size > hostname_capacity) {
        return false;
    }
    memcpy(hostname, backend->hostname, size);
    *hostname_size = size;
    return true;
}

bool fake_wifi_enable_sta_ipv6(void *context)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    ++backend->ipv6_enable_count;
    return backend->ipv6_enable_succeeds;
}

bool fake_wifi_get_sta_ipv6(void *context, char *address, size_t address_capacity)
{
    FakeWifiBackend *backend = static_cast<FakeWifiBackend *>(context);
    if (!backend->ipv6_query_succeeds ||
        address == nullptr ||
        strlen(backend->ipv6_address) + 1U > address_capacity) {
        return false;
    }
    strcpy(address, backend->ipv6_address);
    return true;
}

struct FakeWifiClock {
    bridge::WifiService *service = nullptr;
    uint32_t now_ms = 0U;
    uint32_t idle_count = 0U;
    uint32_t complete_on_idle = 0U;
};

uint32_t fake_wifi_now_ms(void *context)
{
    return static_cast<FakeWifiClock *>(context)->now_ms;
}

void fake_wifi_idle(void *context)
{
    FakeWifiClock *clock = static_cast<FakeWifiClock *>(context);
    ++clock->idle_count;
    ++clock->now_ms;
    if (clock->idle_count == clock->complete_on_idle) {
        clock->service->on_wifi_event(
            static_cast<uint16_t>(bridge::WifiEventCode::ScanDone));
    }
}

CommandResult wifi_command_callback(
    void *context,
    const Command &command,
    AtTransport &transport)
{
    return static_cast<bridge::WifiService *>(context)->handle_command(command, transport);
}

void run_wifi_service_tests()
{
    using bridge::WifiEventCode;
    using bridge::WifiService;
    using bridge::WifiStatus;
    using bridge::WifiStatusTracker;

    FakeWifiBackend backend;
    backend.result_count = 2U;
    backend.results[0] = {
        { 'f', 'i', 'x', 't', 'u', 'r', 'e', '-', 'o', 'p', 'e', 'n' },
        12U,
        { 0x0AU, 0x0BU, 0x00U, 0x00U, 0x00U, 0x01U },
        -42,
        1U,
        0U,
    };
    backend.results[1] = {
        { 'f', 'i', 'x', 't', 'u', 'r', 'e', '-', 'w', 'p', 'a', '2' },
        12U,
        { 0x0CU, 0x0DU, 0x00U, 0x00U, 0x00U, 0x02U },
        -67,
        11U,
        3U,
    };
    WifiStatusTracker status;
    FakeWifiClock clock;
    WifiService service(
        status,
        {
            &backend,
            fake_wifi_start_scan,
            fake_wifi_copy_scan_results,
            fake_wifi_start_sta_connect,
            fake_wifi_set_sta_dns,
            fake_wifi_disconnect_sta,
            fake_wifi_set_auto_reconnect,
            fake_wifi_set_sta_role_enabled,
            fake_wifi_enable_sta_coexistence,
            fake_wifi_get_sta_ssid,
            fake_wifi_get_sta_rssi,
            fake_wifi_get_sta_bssid,
            fake_wifi_get_sta_ip_info,
            fake_wifi_start_softap,
            fake_wifi_stop_softap,
            fake_wifi_get_ap_mac,
            fake_wifi_get_ap_ip,
            fake_wifi_set_ap_config,
            fake_wifi_set_sta_hostname,
            fake_wifi_get_sta_hostname,
            fake_wifi_enable_sta_ipv6,
            fake_wifi_get_sta_ipv6,
            fake_wifi_get_sta_mac,
        },
        {
            &clock,
            fake_wifi_now_ms,
            fake_wifi_idle,
        });
    clock.service = &service;
    clock.complete_on_idle = 3U;

    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::InitDone));
    expect(status.status() == WifiStatus::Idle, "wifi init event maps idle");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::Connected));
    expect(status.status() == WifiStatus::Idle, "wifi association waits for ipv4");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::GotIp));
    expect(status.status() == WifiStatus::Connected, "wifi got-ip event maps status");
    expect(
        backend.sta_coexistence_count == 0U,
        "wifi event callback defers STA coexistence control to service polling");
    expect(service.poll(), "wifi got-ip queues STA coexistence enable");
    expect(
        backend.sta_coexistence_count == 1U,
        "wifi got-ip enables STA coexistence once");
    expect(
        !service.poll() && backend.sta_coexistence_count == 1U,
        "wifi coexistence enable does not repeat while the STA stays connected");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::Disconnected));
    expect(status.status() == WifiStatus::Idle, "wifi disconnect event maps status");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::GotIp));
    expect(service.poll(), "wifi reconnect queues a fresh STA coexistence enable");
    expect(
        backend.sta_coexistence_count == 2U,
        "wifi reconnect re-enables STA coexistence after disconnect");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::Disconnected));

    TestState state;
    AtTransport transport(
        {
            128U,
            8U,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(wifi_command_callback, &service);

    feed(transport, "AT+HOSTNAME?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+HOSTNAME: wlan\r\nOK\r\n",
        "wifi hostname default read transcript");
    state.output.clear();

    feed(transport, "AT+HOSTNAME=bridge-fixture\r\n", 1U);
    flush_all(transport);
    expect(
        backend.hostname_set_count == 1U &&
            strcmp(reinterpret_cast<const char *>(backend.hostname), "bridge-fixture") == 0,
        "wifi hostname write invokes backend");
    expect(
        state.output == "+HOSTNAME: \r\nOK\r\n",
        "wifi hostname write transcript");
    state.output.clear();

    feed(transport, "AT+HOSTNAME?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+HOSTNAME: bridge-fixture\r\nOK\r\n",
        "wifi hostname readback transcript");
    state.output.clear();

    feed(transport, "AT+HOSTNAME\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi hostname rejects run mode");
    state.output.clear();

    feed(transport, "AT+HOSTNAME=\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi hostname rejects empty value");
    state.output.clear();

    feed(transport, "AT+HOSTNAME=12345678901234567890123456789012\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi hostname rejects oversized value");
    state.output.clear();

    backend.hostname_succeeds = false;
    feed(transport, "AT+HOSTNAME=backend-failure\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi hostname propagates write backend failure");
    state.output.clear();
    feed(transport, "AT+HOSTNAME?\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi hostname propagates read backend failure");
    state.output.clear();
    backend.hostname_succeeds = true;

    feed(transport, "AT+IPV6\r\n", 1U);
    flush_all(transport);
    expect(
        backend.ipv6_enable_count == 1U && state.output == "OK\r\n",
        "wifi ipv6 run invokes backend without response prefix");
    state.output.clear();

    feed(transport, "AT+IPV6?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+IPV6: fe80::1234\r\nOK\r\n",
        "wifi ipv6 read transcript");
    state.output.clear();

    feed(transport, "AT+IPV6=1\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi ipv6 rejects write mode");
    state.output.clear();

    backend.ipv6_enable_succeeds = false;
    feed(transport, "AT+IPV6\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi ipv6 propagates enable backend failure");
    state.output.clear();
    backend.ipv6_enable_succeeds = true;
    backend.ipv6_query_succeeds = false;
    feed(transport, "AT+IPV6?\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi ipv6 propagates read backend failure");
    state.output.clear();
    backend.ipv6_query_succeeds = true;

    feed(transport, "AT+WIFIMODE?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+WIFIMODE: 1\r\n\r\nOK\r\n",
        "wifi mode default read transcript");
    state.output.clear();

    feed(transport, "AT+WIFIMODE=4\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi mode rejects unsupported role");
    expect(backend.sta_role_count == 0U, "wifi mode does not mutate unsupported role");
    state.output.clear();

    feed(transport, "AT+WIFIMODE=0\r\n", 1U);
    flush_all(transport);
    expect(
        backend.sta_role_count == 1U && !backend.last_sta_role_enabled,
        "wifi mode null disables sta role");
    expect(status.status() == WifiStatus::NoModule, "wifi mode null status");
    expect(
        state.output == "+WIFIMODE: \r\nOK\r\n",
        "wifi mode null write transcript");
    state.output.clear();

    feed(transport, "AT+WIFIMODE?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+WIFIMODE: 0\r\n\r\nOK\r\n",
        "wifi mode null read transcript");
    state.output.clear();

    feed(transport, "AT+WIFISCAN\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "ERROR\r\n",
        "wifi mode null rejects scan");
    expect(backend.start_count == 0U, "wifi mode null does not start scan");
    state.output.clear();

    feed(transport, "AT+BEGINSTA=fixture-disabled\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "ERROR\r\n",
        "wifi mode null rejects station connect");
    expect(backend.start_sta_count == 0U, "wifi mode null does not start station connect");
    state.output.clear();

    feed(transport, "AT+WIFIMODE=1,1\r\n", 1U);
    flush_all(transport);
    expect(
        backend.sta_role_count == 2U && backend.last_sta_role_enabled,
        "wifi mode sta enables sta role");
    expect(
        backend.reconnect_count == 0U,
        "wifi mode sta does not touch reconnect before a station exists");
    expect(status.status() == WifiStatus::Idle, "wifi mode sta status");
    expect(
        state.output == "+WIFIMODE: \r\nOK\r\n",
        "wifi mode sta write transcript");
    state.output.clear();
    backend.reconnect_count = 0U;

    feed(transport, "AT+WIFIMODE=2\r\n", 1U);
    flush_all(transport);
    expect(
        backend.sta_role_count == 3U && !backend.last_sta_role_enabled,
        "wifi mode ap disables station role");
    expect(state.output == "+WIFIMODE: \r\nOK\r\n", "wifi mode ap write transcript");
    state.output.clear();

    feed(
        transport,
        "AT+BEGINSOFTAP=fixture-ap,fixture-password,6,1,2\r\n",
        1U);
    flush_all(transport);
    expect(backend.ap_start_count == 0U, "softap start waits for later service poll");
    expect(state.output == "+BEGINSOFTAP: 1\r\nOK\r\n", "softap start transcript");
    state.output.clear();
    expect(!service.poll(), "softap start respects the UART response grace interval");
    clock.now_ms += WifiService::kSoftApResponseGraceMs;
    expect(service.poll(), "softap start consumes queued request");
    expect(backend.ap_start_count == 1U, "softap start invokes backend after response");
    expect(
        backend.last_ap_ssid_size == sizeof("fixture-ap") - 1U &&
            memcmp(backend.last_ap_ssid, "fixture-ap", backend.last_ap_ssid_size) == 0,
        "softap start ssid");
    expect(
        backend.last_ap_password_size == sizeof("fixture-password") - 1U &&
            memcmp(
                backend.last_ap_password,
                "fixture-password",
                backend.last_ap_password_size) == 0,
        "softap start password");
    expect(
        backend.last_ap_channel == 6U &&
            backend.last_ap_hidden &&
            backend.last_ap_max_connections == 2U,
        "softap start optional arguments");
    expect(
        status.status() == WifiStatus::Idle,
        "softap request does not claim listening before native event");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::ApStarted));
    expect(
        status.status() == WifiStatus::ApListening,
        "softap start becomes listening on native event");

    feed(transport, "AT+GETSOFTAPSSID?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+GETSOFTAPSSID: fixture-ap\r\nOK\r\n",
        "softap ssid read transcript");
    state.output.clear();

    feed(transport, "AT+MACSOFTAP?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+MACSOFTAP: 02:11:22:33:44:55\r\nOK\r\n",
        "softap mac read transcript");
    state.output.clear();

    feed(transport, "AT+MACSTA?\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+MACSTA: 02:aa:bb:cc:dd:ee\r\nOK\r\n",
        "station mac read transcript");
    state.output.clear();
    feed(transport, "AT+MACSTA\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "station mac rejects run form");
    state.output.clear();

    feed(transport, "AT+IPSOFTAP\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+IPSOFTAP: 192.168.4.1\r\nOK\r\n",
        "softap ip run transcript");
    state.output.clear();

    feed(transport, "AT+SOFTAPCONFIG=192.168.5.1,192.168.5.1,255.255.255.0\r\n", 1U);
    flush_all(transport);
    expect(backend.ap_config_count == 1U, "softap config invokes backend");
    expect(
        backend.last_ap_network.ip[0] == 192U &&
            backend.last_ap_network.ip[1] == 168U &&
            backend.last_ap_network.ip[2] == 5U &&
            backend.last_ap_network.ip[3] == 1U,
        "softap config parses ip");
    expect(
        state.output == "+SOFTAPCONFIG: \r\nOK\r\n",
        "softap config transcript");
    state.output.clear();

    feed(transport, "AT+WIFIMODE=3\r\n", 1U);
    flush_all(transport);
    expect(
        backend.sta_role_count == 4U && backend.last_sta_role_enabled,
        "wifi mode apsta enables station role");
    expect(
        backend.reconnect_count == 0U,
        "wifi mode apsta does not touch reconnect before a station exists");
    expect(state.output == "+WIFIMODE: \r\nOK\r\n", "wifi mode apsta transcript");
    state.output.clear();
    backend.reconnect_count = 0U;

    feed(transport, "AT+DISCONNECTSOFTAP\r\n", 1U);
    flush_all(transport);
    expect(backend.ap_stop_count == 1U, "softap stop invokes backend");
    expect(state.output == "OK\r\n", "softap stop run transcript");
    state.output.clear();

    feed(transport, "AT+BEGINSOFTAP=fixture-default,fixture-password\r\n", 1U);
    flush_all(transport);
    expect(backend.ap_start_count == 1U, "second softap start remains queued before service poll");
    expect(!service.poll(), "default softap start respects the UART response grace interval");
    clock.now_ms += WifiService::kSoftApResponseGraceMs;
    expect(service.poll(), "default softap start consumes queued request");
    expect(
        backend.ap_start_count == 2U && backend.last_ap_max_connections == 4U,
        "softap defaults to bundle station limit");
    expect(
        state.output == "+BEGINSOFTAP: 1\r\nOK\r\n",
        "softap default station limit transcript");
    state.output.clear();
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::ApStarted));

    feed(transport, "AT+BEGINSOFTAP=fixture-ap,short,6,0,1\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "softap rejects short password");
    state.output.clear();

    feed(transport, "AT+BEGINSOFTAP=fixture-ap,fixture-password,6,0,5\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "softap rejects unsupported max stations");
    state.output.clear();

    feed(transport, "AT+WIFIMODE=1\r\n", 1U);
    flush_all(transport);
    expect(backend.ap_stop_count == 2U, "wifi mode sta stops active softap");
    expect(state.output == "+WIFIMODE: \r\nOK\r\n", "wifi mode sta after ap");
    state.output.clear();
    backend.reconnect_count = 0U;

    feed(transport, "AT+GETSSID?\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+GETSSID: fixture-sta\r\nOK\r\n",
        "wifi station ssid read transcript");
    state.output.clear();

    feed(transport, "AT+GETRSSI?\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+GETRSSI: -58\r\nOK\r\n",
        "wifi station rssi read transcript");
    state.output.clear();

    feed(transport, "AT+GETBSSID?\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+GETBSSID: 0e:0f:10:11:12:13\r\nOK\r\n",
        "wifi station bssid read transcript");
    state.output.clear();

    feed(transport, "AT+IPSTA=0\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+IPSTA: 192.0.2.9\r\nOK\r\n",
        "wifi station ip read transcript");
    state.output.clear();

    feed(transport, "AT+IPSTA=1\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+IPSTA: 192.0.2.1\r\nOK\r\n",
        "wifi station gateway read transcript");
    state.output.clear();

    feed(transport, "AT+IPSTA=2\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+IPSTA: 255.255.255.0\r\nOK\r\n",
        "wifi station netmask read transcript");
    state.output.clear();

    feed(transport, "AT+IPSTA=3\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+IPSTA: 192.0.2.53\r\nOK\r\n",
        "wifi station dns1 read transcript");
    state.output.clear();

    feed(transport, "AT+IPSTA=4\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+IPSTA: 198.51.100.53\r\nOK\r\n",
        "wifi station dns2 read transcript");
    state.output.clear();

    feed(transport, "AT+IPSTA=5\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi station ip rejects invalid selector");
    state.output.clear();

    feed(transport, "AT+GETSSID\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi station ssid rejects run mode");
    state.output.clear();

    backend.sta_query_succeeds = false;
    feed(transport, "AT+GETRSSI?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi station rssi propagates backend failure");
    state.output.clear();
    feed(transport, "AT+IPSTA=0\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi station ip propagates backend failure");
    state.output.clear();
    backend.sta_query_succeeds = true;

    feed(transport, "AT+WIFISCAN\r\n", 1U);
    flush_all(transport);
    expect(backend.start_count == 1U, "wifi scan invokes backend");
    const std::string scan_payload =
        "fixture-open | 0a:0b:00:00:00:01 | -42 | 1 | open\r\n"
        "fixture-wpa2 | 0c:0d:00:00:00:02 | -67 | 11 | WPA2\r\n";
    expect(
        state.output == "+WIFISCAN: " + std::to_string(scan_payload.size()) + "|" +
            scan_payload + "OK\r\n",
        "wifi scan transcript");
    expect(status.status() == WifiStatus::ScanCompleted, "wifi scan event updates status");
    state.output.clear();

    feed(transport, "AT+SOFTRESETWIFI\r\n", 2U);
    flush_all(transport);
    expect(
        backend.disconnect_count == 0U,
        "wifi soft reset skips the native disconnect while idle");
    expect(status.status() == WifiStatus::Idle, "wifi soft reset returns idle status");
    expect(
        state.output == "+SOFTRESETWIFI: \r\nOK\r\n",
        "wifi soft reset transcript");
    state.output.clear();

    feed(transport, "AT+SOFTRESETWIFI?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi soft reset rejects read mode");
    state.output.clear();

    status.set_status(WifiStatus::Idle);
    feed(transport, "AT+BEGINSTA=fixture-open\r\n", 2U);
    flush_all(transport);
    expect(backend.start_sta_count == 1U, "wifi beginsta invokes backend");
    expect(
        backend.reconnect_count == 0U,
        "wifi beginsta does not issue a reconnect callback");
    expect(
        backend.last_sta_auto_reconnect,
        "wifi beginsta passes the stored reconnect policy to backend");
    expect(
        backend.last_ssid_size == sizeof("fixture-open") - 1U &&
            memcmp(backend.last_ssid, "fixture-open", backend.last_ssid_size) == 0,
        "wifi beginsta ssid");
    expect(backend.last_password_size == 0U, "wifi beginsta open network");
    expect(
        state.output == "+BEGINSTA: 0\r\nOK\r\n",
        "wifi beginsta immediate idle status transcript");
    state.output.clear();

    feed(transport, "AT+SOFTRESETWIFI\r\n", 2U);
    flush_all(transport);
    expect(
        backend.disconnect_count == 1U && !backend.last_disconnect_wifi_off,
        "wifi soft reset disconnects a pending station join");
    state.output.clear();

    status.set_status(WifiStatus::ScanCompleted);
    feed(transport, "AT+BEGINSTA=fixture-wpa2,fixture-password\r\n", 2U);
    flush_all(transport);
    expect(backend.start_sta_count == 2U, "wifi beginsta protected invokes backend");
    expect(
        backend.reconnect_count == 0U,
        "wifi protected beginsta does not issue a reconnect callback");
    expect(
        backend.last_sta_auto_reconnect,
        "wifi protected beginsta passes the stored reconnect policy to backend");
    expect(
        backend.last_password_size == sizeof("fixture-password") - 1U &&
            memcmp(backend.last_password, "fixture-password", backend.last_password_size) == 0,
        "wifi beginsta password");
    expect(
        state.output == "+BEGINSTA: 2\r\nOK\r\n",
        "wifi beginsta preserves immediate status snapshot");
    state.output.clear();

    backend.start_sta_succeeds = false;
    feed(transport, "AT+BEGINSTA=fixture-failure\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+BEGINSTA: 4\r\nOK\r\n",
        "wifi beginsta backend failure follows esp32 status framing");
    state.output.clear();
    backend.start_sta_succeeds = true;

    feed(transport, "AT+BEGINSTA=\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi beginsta rejects empty ssid");
    state.output.clear();

    feed(transport, "AT+BEGINSTA=fixture,\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi beginsta rejects empty password");
    state.output.clear();

    feed(
        transport,
        "AT+BEGINSTA=123456789012345678901234567890123\r\n",
        2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi beginsta rejects oversized ssid");
    state.output.clear();

    feed(transport, "AT+BEGINSTA?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi beginsta rejects read mode");
    state.output.clear();

    feed(
        transport,
        "AT+SETIP=192.0.2.9,192.0.2.1,255.255.255.0,192.0.2.53,198.51.100.53\r\n",
        2U);
    flush_all(transport);
    expect(
        state.output == "+SETIP: \r\nOK\r\n",
        "wifi setip accepts static IPv4 and DNS");
    state.output.clear();

    feed(transport, "AT+BEGINSTA=fixture-static,fixture-password\r\n", 2U);
    flush_all(transport);
    expect(
        backend.last_sta_network.configured &&
            backend.last_sta_network.ip[0] == 192U &&
            backend.last_sta_network.ip[1] == 0U &&
            backend.last_sta_network.ip[2] == 2U &&
            backend.last_sta_network.ip[3] == 9U &&
            backend.last_sta_network.dns1[3] == 53U &&
            backend.last_sta_network.dns2[0] == 198U &&
            backend.last_sta_network.dns2[3] == 53U,
        "wifi beginsta forwards static IPv4 and DNS");
    expect(
        backend.reconnect_count == 0U,
        "wifi static beginsta does not issue a reconnect callback");
    expect(
        backend.last_sta_auto_reconnect,
        "wifi static beginsta passes the stored reconnect policy to backend");
    state.output.clear();

    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::Connected));
    expect(
        status.status() == WifiStatus::Connected,
        "wifi static IPv4 publishes connected on association");
    expect(
        backend.reconnect_count == 0U,
        "wifi static association does not issue a reconnect callback");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::Disconnected));
    expect(
        status.status() == WifiStatus::Idle,
        "wifi static IPv4 disconnect returns idle");

    feed(
        transport,
        "AT+SETIP=192.0.2.9,invalid,255.255.255.0\r\n",
        2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi setip rejects invalid IPv4");
    state.output.clear();

    feed(transport, "AT+SOFTRESETWIFI\r\n", 2U);
    flush_all(transport);
    feed(transport, "AT+BEGINSTA=fixture-dhcp,fixture-password\r\n", 2U);
    flush_all(transport);
    expect(
        !backend.last_sta_network.configured,
        "wifi soft reset clears static IPv4 configuration");
    state.output.clear();
    backend.disconnect_count = 1U;
    expect(
        backend.reconnect_count == 0U,
        "wifi DHCP beginsta does not issue a reconnect callback");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::GotIp));
    expect(
        backend.reconnect_count == 0U,
        "wifi DHCP association does not issue a reconnect callback");

    /*
     * WiFiS3::setDNS() serializes its current DHCP lease through +SETIP.
     * That must update only the resolver, retaining DHCP for reconnects.
     */
    feed(
        transport,
        "AT+SETIP=192.0.2.9,192.0.2.1,255.255.255.0,203.0.113.53,0.0.0.0\r\n",
        2U);
    flush_all(transport);
    expect(
        state.output == "+SETIP: \r\nOK\r\n",
        "wifi setdns through current DHCP lease transcript");
    expect(
        backend.sta_dns_set_count == 1U &&
            backend.last_sta_dns1[0] == 203U &&
            backend.last_sta_dns1[1] == 0U &&
            backend.last_sta_dns1[2] == 113U &&
            backend.last_sta_dns1[3] == 53U &&
            backend.last_sta_dns2[0] == 0U,
        "wifi setdns applies an immediate DHCP resolver override");
    state.output.clear();

    feed(transport, "AT+BEGINSTA=fixture-dhcp-dns,fixture-password\r\n", 2U);
    flush_all(transport);
    expect(
        !backend.last_sta_network.configured &&
            backend.last_sta_network.dns_configured &&
            backend.last_sta_network.dns1[0] == 203U &&
            backend.last_sta_network.dns1[3] == 53U,
        "wifi reconnect retains DHCP mode with the DNS override");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::GotIp));
    expect(
        backend.sta_dns_set_count == 1U,
        "wifi DHCP callback defers the blocking DNS backend");
    expect(service.poll(), "wifi bridge task consumes deferred DNS restore");
    expect(
        backend.sta_dns_set_count == 2U,
        "wifi DHCP reconnect reapplies the resolver after lease acquisition");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::GotIp));
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::Disconnected));
    (void)service.poll();
    expect(backend.sta_dns_set_count == 2U,
           "wifi disconnect cancels a pending DNS restore");
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::GotIp));
    expect(service.disconnect_for_bootstrap(),
           "wifi bootstrap disconnects a lease awaiting DNS restore");
    expect(backend.disconnect_count == 2U,
           "wifi pending DNS lease still owns a native STA connection");
    (void)service.poll();
    expect(backend.sta_dns_set_count == 2U,
           "wifi bootstrap cancels pending DNS restore");
    backend.disconnect_count = 1U;
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::GotIp));
    expect(service.poll(), "wifi next lease restores deferred DNS");
    expect(backend.sta_dns_set_count == 3U,
           "wifi next lease recovers after cancellation");
    state.output.clear();

    feed(transport, "AT+RECONNECT?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+RECONNECT: 1\r\nOK\r\n", "wifi reconnect default readback");
    state.output.clear();

    feed(transport, "AT+RECONNECT=0\r\n", 2U);
    flush_all(transport);
    expect(backend.reconnect_count == 1U && !backend.last_auto_reconnect, "wifi reconnect disable backend");
    expect(state.output == "OK\r\n", "wifi reconnect disable transcript");
    state.output.clear();

    feed(transport, "AT+RECONNECT?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+RECONNECT: 0\r\nOK\r\n", "wifi reconnect disabled readback");
    state.output.clear();

    backend.reconnect_succeeds = false;
    feed(transport, "AT+RECONNECT=1\r\n", 2U);
    flush_all(transport);
    expect(
        backend.reconnect_count == 2U && backend.last_auto_reconnect,
        "wifi reconnect enable backend request");
    expect(state.output == "OK\r\n", "wifi reconnect keeps esp32 write framing");
    state.output.clear();
    backend.reconnect_succeeds = true;

    feed(transport, "AT+RECONNECT?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+RECONNECT: 1\r\nOK\r\n", "wifi reconnect enabled readback");
    state.output.clear();

    feed(transport, "AT+RECONNECT=2\r\n", 2U);
    flush_all(transport);
    expect(
        backend.reconnect_count == 3U && backend.last_auto_reconnect,
        "wifi reconnect atoi-two backend request");
    expect(state.output == "OK\r\n", "wifi reconnect retains esp32 atoi write behavior");
    state.output.clear();

    feed(transport, "AT+RECONNECT=abc\r\n", 2U);
    flush_all(transport);
    expect(
        backend.reconnect_count == 4U && !backend.last_auto_reconnect,
        "wifi reconnect atoi-zero backend request");
    expect(state.output == "OK\r\n", "wifi reconnect accepts nonempty atoi-zero value");
    state.output.clear();

    feed(transport, "AT+RECONNECT?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+RECONNECT: 0\r\nOK\r\n", "wifi reconnect atoi-zero readback");
    state.output.clear();

    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::Disconnected));
    feed(transport, "AT+RECONNECT=0\r\n", 2U);
    flush_all(transport);
    expect(backend.reconnect_count == 5U && !backend.last_auto_reconnect,
           "wifi reconnect disable reaches live backend while disconnected");
    expect(state.output == "OK\r\n", "wifi disconnected reconnect write framing");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+AUTOCONNECT: 0\r\nOK\r\n", "wifi autoconnect fixed readback");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT=1\r\n", 2U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "wifi autoconnect enable no-op transcript");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT=0\r\n", 2U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "wifi autoconnect disable no-op transcript");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT=abc\r\n", 2U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "wifi autoconnect accepts nonempty atoi-zero value");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+AUTOCONNECT: 0\r\nOK\r\n", "wifi autoconnect readback remains zero");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT=\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi autoconnect rejects empty argument");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT=1,0\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi autoconnect rejects multiple arguments");
    state.output.clear();

    feed(transport, "AT+AUTOCONNECT\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi autoconnect rejects run mode");
    state.output.clear();

    feed(transport, "AT+DISCONNECT\r\n", 2U);
    flush_all(transport);
    expect(
        backend.disconnect_count == 2U && !backend.last_disconnect_wifi_off,
        "wifi disconnect run backend request");
    expect(state.output == "+DISCONNECT: \r\nOK\r\n", "wifi disconnect run transcript");
    state.output.clear();

    backend.disconnect_succeeds = false;
    feed(transport, "AT+DISCONNECT=0\r\n", 2U);
    flush_all(transport);
    expect(
        backend.disconnect_count == 3U && !backend.last_disconnect_wifi_off,
        "wifi disconnect write backend request");
    expect(state.output == "OK\r\n", "wifi disconnect ignores native result like esp32");
    state.output.clear();
    backend.disconnect_succeeds = true;

    backend.wifi_off_supported = false;
    feed(transport, "AT+DISCONNECT=1\r\n", 2U);
    flush_all(transport);
    expect(
        backend.disconnect_count == 4U && backend.last_disconnect_wifi_off,
        "wifi disconnect wifi-off backend request");
    expect(state.output == "ERROR\r\n", "wifi disconnect wifi-off requires real radio support");
    state.output.clear();
    backend.wifi_off_supported = true;

    feed(transport, "AT+DISCONNECT?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi disconnect rejects read mode");
    state.output.clear();

    feed(transport, "AT+WIFISCAN?\r\n", 2U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi scan rejects read mode");
    state.output.clear();

    backend.start_succeeds = false;
    feed(transport, "AT+WIFISCAN\r\n", 3U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi scan start failure");
    state.output.clear();
    backend.start_succeeds = true;

    clock.now_ms = 0U;
    clock.idle_count = 0U;
    clock.complete_on_idle = 0U;
    feed(transport, "AT+WIFISCAN\r\n", 4U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "wifi scan timeout");
    expect(!service.scan_active() && !service.scan_complete(), "scan timeout invalidates active results");
    state.output.clear();
    service.on_wifi_event(static_cast<uint16_t>(WifiEventCode::ScanDone));
    expect(!service.scan_complete(), "late scan completion after timeout is ignored");
    expect(service.scan_result_count() == 0U, "timed-out scan cannot expose previous results");

    backend.result_count = 0U;
    clock.idle_count = 0U;
    clock.complete_on_idle = 3U;
    feed(transport, "AT+WIFISCAN\r\n", 5U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "empty WiFi scan succeeds without a payload prefix");
    expect(service.scan_complete() && service.scan_result_count() == 0U, "empty scan replaces cached results");
    state.output.clear();

    backend.result_count = 1U;
    clock.idle_count = 0U;
    clock.complete_on_idle = 3U;
    clock.now_ms = 0xfffffff0U;
    feed(transport, "AT+WIFISCAN\r\n", 6U);
    flush_all(transport);
    expect(service.scan_result_count() == 1U, "scan recovers from an empty result");
    expect(state.output.find("fixture-open") != std::string::npos &&
           state.output.find("fixture-wpa2") == std::string::npos,
           "scan cache replaces previous rows rather than appending");
    state.output.clear();

    feed(transport, "AT+WIFIMODE=0\r\n", 7U);
    flush_all(transport);
    expect(!service.scan_complete() && service.scan_result_count() == 0U, "mode off invalidates scan cache");
    state.output.clear();
    const uint32_t starts_before_off_scan = backend.start_count;
    feed(transport, "AT+WIFISCAN\r\n", 8U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && backend.start_count == starts_before_off_scan,
           "mode off rejects scan without touching the backend");
    state.output.clear();
    feed(transport, "AT+WIFIMODE=1\r\n", 9U);
    flush_all(transport);
    state.output.clear();
    clock.idle_count = 0U;
    clock.complete_on_idle = 0U;
    clock.now_ms = 0xfffffff0U;
    feed(transport, "AT+WIFISCAN\r\n", 10U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && !service.scan_active(), "scan deadline survives millis wrap");
}

struct FakeNetworkScanWindow {
    bool allowed = true;
    bool active = false;
    unsigned pauses = 0U;
    unsigned resumes = 0U;
    static bool pause(void *context) {
        auto &window = *static_cast<FakeNetworkScanWindow *>(context);
        ++window.pauses;
        window.active = window.allowed;
        return window.active;
    }
    static void resume(void *context) {
        auto &window = *static_cast<FakeNetworkScanWindow *>(context);
        expect(window.active, "network resumes only its own active window");
        window.active = false;
        ++window.resumes;
    }
};

struct FakeTcpClientBackend {
    FakeNetworkScanWindow *scan_window = nullptr;
    bool connect_success = true;
    bool resolve_success = true;
    bool accept_success = true;
    int next_handle = 100;
    uint32_t create_count = 0U;
    uint32_t create_listener_count = 0U;
    uint32_t close_count = 0U;
    uint32_t flush_count = 0U;
    uint16_t listener_port = 0U;
    uint8_t resolved_address[4] = { 203U, 0U, 113U, 9U };
    uint8_t connected_address[4] = {};
    uint16_t connected_port = 0U;
    uint32_t connected_timeout_ms = 0U;
    bool connected = true;
    std::string sent;
    std::string received = "reply";
    uint8_t *last_receive_buffer = nullptr;
    std::vector<int> pending_accept_handles;
};

bool fake_tcp_create(void *context, int *handle)
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    if (handle == nullptr) {
        return false;
    }
    *handle = backend->next_handle++;
    ++backend->create_count;
    return true;
}

bool fake_tcp_close(void *context, int)
{
    ++static_cast<FakeTcpClientBackend *>(context)->close_count;
    return true;
}

bool fake_tcp_create_listener(void *context, uint16_t port, int *handle)
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    if (backend->scan_window != nullptr && backend->scan_window->allowed) {
        expect(backend->scan_window->active, "TCP listener setup owns the RF window");
    }
    if (handle == nullptr) {
        return false;
    }
    *handle = backend->next_handle++;
    backend->listener_port = port;
    ++backend->create_listener_count;
    return true;
}

bool fake_tcp_accept(void *context, int, int *client_handle)
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    if (backend->scan_window != nullptr && backend->scan_window->allowed) {
        expect(backend->scan_window->active, "TCP accept owns the RF window");
    }
    if (!backend->accept_success) return false;
    if (client_handle == nullptr) {
        return false;
    }
    if (backend->pending_accept_handles.empty()) {
        *client_handle = -1;
        return true;
    }
    *client_handle = backend->pending_accept_handles.front();
    backend->pending_accept_handles.erase(backend->pending_accept_handles.begin());
    return true;
}

bool fake_tcp_resolve(void *context, const char *hostname, uint8_t address[4])
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    if (hostname == nullptr || address == nullptr || strcmp(hostname, "fixture.test") != 0) {
        return false;
    }
    if (backend->scan_window != nullptr && backend->scan_window->allowed) {
        expect(backend->scan_window->active, "TCP hostname lookup owns the RF window");
    }
    if (!backend->resolve_success) return false;
    memcpy(address, backend->resolved_address, sizeof(backend->resolved_address));
    return true;
}

bool fake_tcp_get_hostname_diagnostic(
    void *,
    bridge::TcpHostnameDiagnostic *diagnostic)
{
    if (diagnostic == nullptr) {
        return false;
    }
    diagnostic->server_address[0] = 192U;
    diagnostic->server_address[1] = 0U;
    diagnostic->server_address[2] = 2U;
    diagnostic->server_address[3] = 53U;
    diagnostic->h_error = 0;
    diagnostic->flags = 3U;
    return true;
}

bool fake_tcp_connect(
    void *context,
    int,
    const uint8_t address[4],
    uint16_t port,
    uint32_t timeout_ms)
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    memcpy(backend->connected_address, address, sizeof(backend->connected_address));
    backend->connected_port = port;
    backend->connected_timeout_ms = timeout_ms;
    if (backend->scan_window != nullptr && backend->scan_window->allowed) {
        expect(backend->scan_window->active, "TCP connect owns the RF window");
    }
    backend->connected = backend->connect_success;
    return backend->connect_success;
}

bool fake_tcp_send(
    void *context,
    int,
    const uint8_t *data,
    size_t size,
    size_t *sent)
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    if (data == nullptr || sent == nullptr) {
        return false;
    }
    backend->sent.append(reinterpret_cast<const char *>(data), size);
    *sent = size;
    return true;
}

bool fake_tcp_receive(
    void *context,
    int,
    uint8_t *data,
    size_t capacity,
    size_t *received)
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    if (data == nullptr || received == nullptr) {
        return false;
    }
    backend->last_receive_buffer = data;
    const size_t count = backend->received.size() < capacity
        ? backend->received.size()
        : capacity;
    memcpy(data, backend->received.data(), count);
    backend->received.erase(0U, count);
    *received = count;
    return true;
}

bool fake_tcp_available(void *context, int, size_t *bytes)
{
    if (bytes == nullptr) {
        return false;
    }
    *bytes = static_cast<FakeTcpClientBackend *>(context)->received.size();
    return true;
}

bool fake_tcp_peek(void *context, int, int *value)
{
    FakeTcpClientBackend *backend = static_cast<FakeTcpClientBackend *>(context);
    if (value == nullptr) {
        return false;
    }
    *value = backend->received.empty()
        ? -1
        : static_cast<unsigned char>(backend->received[0]);
    return true;
}

bool fake_tcp_connected(void *context, int, bool *connected)
{
    if (connected == nullptr) {
        return false;
    }
    *connected = static_cast<FakeTcpClientBackend *>(context)->connected;
    return true;
}

bool fake_tcp_state(
    void *,
    int,
    uint8_t address[4],
    uint16_t *remote_port,
    uint16_t *local_port)
{
    if (address == nullptr || remote_port == nullptr || local_port == nullptr) {
        return false;
    }
    address[0] = 198U;
    address[1] = 51U;
    address[2] = 100U;
    address[3] = 7U;
    *remote_port = 4321U;
    *local_port = 54321U;
    return true;
}

bool fake_tcp_flush(void *context, int)
{
    ++static_cast<FakeTcpClientBackend *>(context)->flush_count;
    return true;
}

bool fake_tcp_local_address(void *, int, uint8_t address[4])
{
    if (address == nullptr) {
        return false;
    }
    address[0] = 192U;
    address[1] = 168U;
    address[2] = 42U;
    address[3] = 2U;
    return true;
}

CommandResult tcp_client_command_callback(
    void *context,
    const Command &command,
    AtTransport &transport)
{
    return static_cast<bridge::TcpClientService *>(context)->handle_command(command, transport);
}

CommandResult tcp_server_command_callback(
    void *context,
    const Command &command,
    AtTransport &transport)
{
    bridge::TcpServerService *service = static_cast<bridge::TcpServerService *>(context);
    if (service->handles_server_command(command)) {
        return service->handle_server_command(command, transport);
    }
    if (service->handles_client_command(command)) {
        return service->handle_client_command(command, transport);
    }
    return CommandResult::Error;
}

struct FakePingBackend {
    uint8_t address[4] = { 192U, 0U, 2U, 1U };
    bridge::PingResult result = bridge::PingResult::Success;
    uint32_t average_rtt_ms = 17U;
    uint8_t observed_ttl = 0U;
    uint8_t observed_count = 0U;
};

bool fake_ping_resolve(void *context, const char *hostname, uint8_t address[4])
{
    FakePingBackend *backend = static_cast<FakePingBackend *>(context);
    if (hostname == nullptr || address == nullptr ||
        strcmp(hostname, "fixture.test") != 0) {
        return false;
    }
    memcpy(address, backend->address, sizeof(backend->address));
    return true;
}

bridge::PingResult fake_ping_echo(
    void *context,
    const uint8_t address[4],
    uint8_t ttl,
    uint8_t count,
    uint32_t *average_rtt_ms)
{
    FakePingBackend *backend = static_cast<FakePingBackend *>(context);
    if (address == nullptr || average_rtt_ms == nullptr ||
        memcmp(address, backend->address, sizeof(backend->address)) != 0) {
        return bridge::PingResult::Error;
    }
    backend->observed_ttl = ttl;
    backend->observed_count = count;
    *average_rtt_ms = backend->average_rtt_ms;
    return backend->result;
}

CommandResult ping_command_callback(
    void *context,
    const Command &command,
    AtTransport &transport)
{
    return static_cast<bridge::PingService *>(context)->handle_command(command, transport);
}

void run_ping_service_tests()
{
    FakePingBackend backend;
    bridge::PingService service(
        {
            &backend,
            fake_ping_resolve,
            fake_ping_echo,
        });
    TestState state;
    AtTransport transport(
        {
            128U,
            64U,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(ping_command_callback, &service);

    feed(transport, "AT+PING=0,fixture.test,64,3\r\n", 1U);
    flush_all(transport);
    expect(state.output == "+PING: 17\r\nOK\r\n", "ping success transcript");
    expect(
        backend.observed_ttl == 64U && backend.observed_count == 3U,
        "ping backend receives ttl and count");
    state.output.clear();

    backend.result = bridge::PingResult::Timeout;
    feed(transport, "AT+PING=0,fixture.test,128,0\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+PING: -2\r\nOK\r\n", "ping timeout transcript");
    expect(backend.observed_count == 1U, "ping normalizes zero count");
    state.output.clear();

    feed(transport, "AT+PING=0,missing.test,64,1\r\n", 3U);
    flush_all(transport);
    expect(state.output == "+PING: -3\r\nOK\r\n", "ping dns failure transcript");
    state.output.clear();

    feed(transport, "AT+PING=0,fixture.test,256,1\r\n", 4U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "ping rejects invalid ttl");
}

struct FakeTlsBackend {
    bool connect_success = true;
    bool scan_paused = false;
    bool scan_pause_allowed = true;
    uint32_t scan_pause_count = 0U;
    uint32_t scan_resume_count = 0U;
    int next_handle = 70;
    uint32_t create_count = 0U;
    uint32_t close_count = 0U;
    uint32_t connect_count = 0U;
    uint32_t flush_count = 0U;
    std::string hostname;
    std::string certificate;
    std::string client_certificate;
    std::string client_private_key;
    std::string sent;
    std::string received = "tls";
    uint8_t *last_receive_buffer = nullptr;
    uint16_t port = 0U;
    uint32_t timeout_ms = 0U;
    bool connected = true;
};

bool fake_tls_create(void *context, int *handle)
{
    FakeTlsBackend *backend = static_cast<FakeTlsBackend *>(context);
    if (handle == nullptr) {
        return false;
    }
    *handle = backend->next_handle++;
    ++backend->create_count;
    return true;
}

bool fake_tls_close(void *context, int)
{
    ++static_cast<FakeTlsBackend *>(context)->close_count;
    return true;
}

bool fake_tls_connect_hostname(
    void *context,
    int,
    const char *hostname,
    uint16_t port,
    uint32_t timeout_ms,
    const uint8_t *certificate,
    size_t certificate_size,
    const uint8_t *client_certificate,
    size_t client_certificate_size,
    const uint8_t *client_private_key,
    size_t client_private_key_size)
{
    FakeTlsBackend *backend = static_cast<FakeTlsBackend *>(context);
    if (hostname == nullptr || certificate == nullptr) {
        return false;
    }
    backend->hostname = hostname;
    backend->certificate.assign(
        reinterpret_cast<const char *>(certificate),
        certificate_size);
    backend->client_certificate.assign(
        client_certificate == nullptr
            ? ""
            : reinterpret_cast<const char *>(client_certificate),
        client_certificate_size);
    backend->client_private_key.assign(
        client_private_key == nullptr
            ? ""
            : reinterpret_cast<const char *>(client_private_key),
        client_private_key_size);
    backend->port = port;
    backend->timeout_ms = timeout_ms;
    ++backend->connect_count;
    return backend->connect_success;
}

bool fake_tls_connect_ip(
    void *,
    int,
    const uint8_t *,
    uint16_t,
    uint32_t,
    const uint8_t *,
    size_t,
    const uint8_t *,
    size_t,
    const uint8_t *,
    size_t)
{
    return true;
}

struct FakeTlsPrivateKeyResolver {
    bool available = true;
    uint32_t call_count = 0U;
    std::string observed_slot;
    std::string private_key = "private-key";
};

bool fake_tls_private_key_resolver(
    void *context,
    const char *slot,
    uint8_t *destination,
    size_t capacity,
    size_t *size)
{
    FakeTlsPrivateKeyResolver *resolver =
        static_cast<FakeTlsPrivateKeyResolver *>(context);
    if (!resolver->available ||
        slot == nullptr ||
        destination == nullptr ||
        size == nullptr ||
        resolver->private_key.size() > capacity) {
        return false;
    }
    ++resolver->call_count;
    resolver->observed_slot = slot;
    memcpy(
        destination,
        resolver->private_key.data(),
        resolver->private_key.size());
    *size = resolver->private_key.size();
    return true;
}

bool fake_tls_send(
    void *context,
    int,
    const uint8_t *data,
    size_t size,
    size_t *sent)
{
    FakeTlsBackend *backend = static_cast<FakeTlsBackend *>(context);
    if (data == nullptr || sent == nullptr) {
        return false;
    }
    backend->sent.assign(reinterpret_cast<const char *>(data), size);
    *sent = size;
    return true;
}

bool fake_tls_receive(
    void *context,
    int,
    uint8_t *data,
    size_t capacity,
    size_t *received)
{
    FakeTlsBackend *backend = static_cast<FakeTlsBackend *>(context);
    if (data == nullptr || received == nullptr) {
        return false;
    }
    backend->last_receive_buffer = data;
    const size_t count = backend->received.size() < capacity
        ? backend->received.size()
        : capacity;
    memcpy(data, backend->received.data(), count);
    *received = count;
    return true;
}

bool fake_tls_available(void *context, int, size_t *bytes)
{
    if (bytes == nullptr) {
        return false;
    }
    *bytes = static_cast<FakeTlsBackend *>(context)->received.size();
    return true;
}

bool fake_tls_peek(void *context, int, int *value)
{
    FakeTlsBackend *backend = static_cast<FakeTlsBackend *>(context);
    if (value == nullptr) {
        return false;
    }
    *value = backend->received.empty()
        ? -1
        : static_cast<unsigned char>(backend->received[0]);
    return true;
}

bool fake_tls_connected(void *context, int, bool *connected)
{
    if (connected == nullptr) {
        return false;
    }
    *connected = static_cast<FakeTlsBackend *>(context)->connected;
    return true;
}

bool fake_tls_state(
    void *,
    int,
    uint8_t address[4],
    uint16_t *remote_port,
    uint16_t *local_port)
{
    if (address == nullptr || remote_port == nullptr || local_port == nullptr) {
        return false;
    }
    address[0] = 198U;
    address[1] = 51U;
    address[2] = 100U;
    address[3] = 9U;
    *remote_port = 443U;
    *local_port = 54321U;
    return true;
}

bool fake_tls_local_address(void *, int, uint8_t address[4])
{
    if (address == nullptr) {
        return false;
    }
    address[0] = 192U;
    address[1] = 168U;
    address[2] = 42U;
    address[3] = 3U;
    return true;
}

bool fake_tls_flush(void *context, int)
{
    ++static_cast<FakeTlsBackend *>(context)->flush_count;
    return true;
}

bool fake_tls_get_verify_flags(void *, int, uint32_t *flags)
{
    if (flags == nullptr) {
        return false;
    }
    *flags = 0x808U;
    return true;
}

bool fake_tls_get_diagnostic(
    void *,
    int,
    uint8_t *phase,
    int32_t *error)
{
    if (phase == nullptr || error == nullptr) {
        return false;
    }
    *phase = 4U;
    *error = -19840;
    return true;
}

bool fake_tls_get_heap_diagnostic(
    void *,
    int,
    bridge::TlsHeapDiagnostic *diagnostic)
{
    if (diagnostic == nullptr) {
        return false;
    }
    diagnostic->before_handshake_free_bytes = 180000U;
    diagnostic->failure_free_bytes = 160000U;
    diagnostic->after_cleanup_free_bytes = 179000U;
    diagnostic->connection_timeout_ms = 250U;
    diagnostic->handshake_elapsed_ms = 253U;
    diagnostic->before_setup_ocram_free_bytes = 12000U;
    diagnostic->before_setup_wram_free_bytes = 169000U;
    diagnostic->failure_ocram_free_bytes = 7000U;
    diagnostic->failure_wram_free_bytes = 153000U;
    diagnostic->after_cleanup_ocram_free_bytes = 11000U;
    diagnostic->after_cleanup_wram_free_bytes = 168000U;
    diagnostic->observed_ocram_free_bytes = 9000U;
    diagnostic->observed_wram_free_bytes = 164000U;
    diagnostic->before_setup_wram_max_block_bytes = 120000U;
    diagnostic->failure_wram_max_block_bytes = 90000U;
    diagnostic->after_cleanup_wram_max_block_bytes = 118000U;
    diagnostic->observed_wram_max_block_bytes = 110000U;
    diagnostic->tcp_state_count[4] = 8U;
    diagnostic->tcp_state_count[5] = 2U;
    diagnostic->tcp_state_count[10] = 1U;
    diagnostic->tcp_unacked_pbuf_bytes = 1536U;
    diagnostic->tcp_unsent_pbuf_bytes = 2048U;
    diagnostic->tcp_ooseq_pbuf_bytes = 64U;
    diagnostic->tcp_max_retransmissions = 2U;
    return true;
}

bool fake_tls_get_endpoint_diagnostic(
    void *,
    int,
    bridge::TlsEndpointDiagnostic *diagnostic)
{
    if (diagnostic == nullptr) {
        return false;
    }
    diagnostic->resolved_address[0] = 192U;
    diagnostic->resolved_address[1] = 0U;
    diagnostic->resolved_address[2] = 2U;
    diagnostic->resolved_address[3] = 10U;
    diagnostic->peer_address[0] = 192U;
    diagnostic->peer_address[1] = 0U;
    diagnostic->peer_address[2] = 2U;
    diagnostic->peer_address[3] = 10U;
    diagnostic->peer_port = 443U;
    diagnostic->resolved_valid = true;
    diagnostic->peer_valid = true;
    return true;
}

CommandResult tls_client_command_callback(
    void *context,
    const Command &command,
    AtTransport &transport)
{
    return static_cast<bridge::TlsClientService *>(context)->handle_command(command, transport);
}

bool fake_tls_scan_pause(void *context)
{
    auto &backend = *static_cast<FakeTlsBackend *>(context);
    ++backend.scan_pause_count;
    backend.scan_paused = backend.scan_pause_allowed;
    return backend.scan_paused;
}

void fake_tls_scan_resume(void *context)
{
    auto &backend = *static_cast<FakeTlsBackend *>(context);
    expect(backend.scan_paused, "TLS resumes only its own scan pause");
    ++backend.scan_resume_count;
    backend.scan_paused = false;
}

void run_tls_client_service_tests()
{
    FakeTlsBackend backend;
    FakeTlsPrivateKeyResolver private_key_resolver;
    bridge::TlsClientService service(
        {
            &backend,
            fake_tls_create,
            fake_tls_close,
            fake_tls_connect_hostname,
            fake_tls_connect_ip,
            fake_tls_send,
            fake_tls_receive,
            fake_tls_available,
            fake_tls_peek,
            fake_tls_connected,
            fake_tls_state,
            fake_tls_flush,
            fake_tls_get_diagnostic,
            fake_tls_get_verify_flags,
            fake_tls_get_heap_diagnostic,
            fake_tls_get_endpoint_diagnostic,
            fake_tls_local_address,
        },
        fake_tls_private_key_resolver,
        &private_key_resolver);
    TestState state;
    AtTransport transport(
        {
            256U,
            64U,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(tls_client_command_callback, &service);

    feed(transport, "AT+SSLBEGINCLIENT\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+SSLBEGINCLIENT: 2000\r\nOK\r\n",
        "tls begin transcript");
    state.output.clear();

    backend.connected = false;
    feed(transport, "AT+SSLIPCLIENT=2000\r\n", 1U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tls disconnected local ip transcript");
    state.output.clear();
    backend.connected = true;
    feed(transport, "AT+SSLIPCLIENT=2000\r\n", 1U);
    flush_all(transport);
    expect(state.output == "+SSLIPCLIENT: 192.168.42.3\r\n\r\nOK\r\n",
           "tls local ip differs from remote ip");
    state.output.clear();

    feed_bytes(
        transport,
        reinterpret_cast<const uint8_t *>("AT+SETCAROOT=2000,3\r\nabc"),
        sizeof("AT+SETCAROOT=2000,3\r\nabc") - 1U,
        2U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tls ca raw transcript");
    state.output.clear();

    feed(transport, "AT+SETCAROOT=2000,4\r\nx", 2U);
    expect(transport.raw_payload_pending(), "partial CA replacement waits for the full payload");
    expect(state.output.empty(), "partial CA replacement emits no premature success");
    transport.tick(23U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && !transport.raw_payload_pending(),
           "CA replacement timeout releases raw framing");
    state.output.clear();
    feed(transport, "AT+SETCAROOT=2008,4\r\n", 24U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && !transport.raw_payload_pending(),
           "invalid TLS slot cannot consume a CA payload");
    state.output.clear();
    feed(transport, "AT+SETCAROOT=2000,65\r\n", 25U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && !transport.raw_payload_pending(),
           "oversized CA payload is rejected before raw collection");
    state.output.clear();

    feed_bytes(
        transport,
        reinterpret_cast<const uint8_t *>("AT+SETECCSLOT=2000,7,4\r\ncert"),
        sizeof("AT+SETECCSLOT=2000,7,4\r\ncert") - 1U,
        3U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tls client credential raw transcript");
    expect(
        private_key_resolver.observed_slot == "7",
        "tls client credential resolves the requested SoftSE slot");
    state.output.clear();

    service.set_scan_control(fake_tls_scan_pause, fake_tls_scan_resume, &backend);
    feed(transport, "AT+SSLCLIENTCONNECT=2000,fixture.test,443,250\r\n", 4U);
    flush_all(transport);
    expect(
        state.output == "+SSLCLIENTCONNECT: \r\nOK\r\n",
        "tls connect transcript");
    expect(
        backend.hostname == "fixture.test" &&
            backend.certificate.size() == 4U &&
            backend.certificate.compare(0U, 3U, "abc") == 0 &&
            backend.certificate[3] == '\0' &&
        backend.client_certificate == "cert" &&
        backend.client_private_key == "private-key" &&
            private_key_resolver.call_count == 2U &&
        backend.port == 443U &&
        backend.timeout_ms == 250U &&
        backend.connect_count == 1U,
        "tls connect resolves the SoftSE key only while configuring TLS");
    state.output.clear();

    expect(backend.scan_pause_count == 1U && backend.scan_resume_count == 1U &&
           !backend.scan_paused, "successful TLS connect restores scan");
    backend.connect_success = false;
    feed(transport, "AT+SSLCLIENTCONNECT=2000,fixture.test,443,250\r\n", 4U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && backend.scan_resume_count == 2U &&
           !backend.scan_paused, "failed TLS connect restores scan");
    state.output.clear();
    backend.connect_success = true;
    backend.scan_pause_allowed = false;
    feed(transport, "AT+SSLCLIENTCONNECT=2000,fixture.test,443,250\r\n", 4U);
    flush_all(transport);
    expect(state.output == "+SSLCLIENTCONNECT: \r\nOK\r\n" &&
           backend.scan_pause_count == 3U && backend.scan_resume_count == 2U,
           "idle or rejected scan pause does not start scanning on TLS return");
    state.output.clear();
    backend.scan_pause_allowed = true;

    feed(transport, "AT+SSLAVAILABLE=2000\r\n", 5U);
    flush_all(transport);
    expect(state.output == "+SSLAVAILABLE: 3\r\nOK\r\n", "tls available transcript");
    state.output.clear();

    feed(transport, "AT+SSLPEEK=2000\r\n", 6U);
    flush_all(transport);
    expect(state.output == "+SSLPEEK: 116\r\nOK\r\n", "tls peek transcript");
    state.output.clear();

    feed(transport, "AT+SSLCLIENTRECEIVE=2000,3\r\n", 7U);
    flush_all(transport);
    expect(
        state.output == "+SSLCLIENTRECEIVE: 3|tls\r\nOK\r\n",
        "tls receive transcript");
    expect(
        backend.last_receive_buffer == transport.temporary_workspace(1U),
        "tls receive borrows the transport workspace");
    state.output.clear();

    expect(backend.scan_pause_count == 6U && backend.scan_resume_count == 5U,
           "TLS available/peek/receive each restore their scan window");
    feed(transport, "AT+SSLCLIENTSEND=2000,4\r\npi", 8U);
    expect(transport.raw_payload_pending() && backend.scan_pause_count == 6U &&
           !backend.scan_paused, "partial raw TLS payload leaves scanning active");
    feed(transport, "ng", 8U);
    flush_all(transport);
    expect(state.output == "OK\r\n" && backend.sent == "ping", "tls send transcript");
    state.output.clear();
    expect(backend.scan_pause_count == 7U && backend.scan_resume_count == 6U &&
           !backend.scan_paused, "raw TLS send restores its scan window");
    feed(transport, "AT+SSLCLIENTRECEIVE=2008,3\r\n", 8U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && backend.scan_pause_count == 8U &&
           backend.scan_resume_count == 7U && !backend.scan_paused,
           "receive argument error releases the scoped scan window");
    state.output.clear();
    service.set_scan_control(nullptr, nullptr, nullptr);


    feed(transport, "AT+SSLCLIENTSTATE=2000\r\n", 9U);
    flush_all(transport);
    expect(
        state.output == "+SSLCLIENTSTATE: 198.51.100.9,443,54321\r\n\r\nOK\r\n",
        "tls state transcript");
    state.output.clear();

    backend.connected = false;
    feed(transport, "AT+SSLCLIENTSTATE=2000\r\n", 9U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tls disconnected state transcript");
    state.output.clear();
    backend.connected = true;

    feed(transport, "AT+SSLCLIENTFLUSH=2000\r\n", 10U);
    flush_all(transport);
    expect(state.output == "+SSLCLIENTFLUSH: \r\nOK\r\n", "tls flush transcript");
    state.output.clear();

    feed(transport, "AT+SSLCLIENTDIAG=2000\r\n", 11U);
    flush_all(transport);
    expect(
        state.output ==
            "+SSLCLIENTDIAG: 4,-19840,180000,160000,179000,250,253,"
            "12000,169000,7000,153000,11000,168000,9000,164000,"
            "120000,90000,118000,110000,0,0,0,0,0,0,0,0,0,0,"
            "0,0,0,0,8,2,0,0,0,0,1,1536,2048,64,2\r\n"
            "OK\r\n",
        "tls diagnostic includes total and per-heap lifecycle snapshots");
    state.output.clear();

    feed(transport, "AT+SSLCLIENTENDPOINT=2000\r\n", 11U);
    flush_all(transport);
    expect(
        state.output ==
            "+SSLCLIENTENDPOINT: 192.0.2.10,192.0.2.10,443\r\n"
            "OK\r\n",
        "tls endpoint diagnostic transcript");
    state.output.clear();

    feed(transport, "AT+SSLCLIENTVERIFYFLAGS=2000\r\n", 11U);
    flush_all(transport);
    expect(
        state.output == "+SSLCLIENTVERIFYFLAGS: 2056\r\nOK\r\n",
        "tls verify flags transcript");
    state.output.clear();

    feed(transport, "AT+SSLCLIENTCLOSE=2000\r\n", 11U);
    flush_all(transport);
    expect(
        state.output == "+SSLCLIENTCLOSE: \r\nOK\r\n" &&
            backend.close_count == 1U,
        "tls close transcript and resource release");
    state.output.clear();

    for (int socket = 2000; socket < 2008; ++socket) {
        feed(transport, "AT+SSLBEGINCLIENT\r\n", 1U);
        flush_all(transport);
        expect(
            state.output ==
                "+SSLBEGINCLIENT: " + std::to_string(socket) + "\r\nOK\r\n",
            "tls allocation fills each documented client slot");
        state.output.clear();
    }
    feed(transport, "AT+SSLBEGINCLIENT\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "ERROR\r\n",
        "tls allocation rejects a ninth simultaneous client");
    state.output.clear();

    feed(transport, "AT+SSLCLIENTCLOSE=2003\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+SSLCLIENTCLOSE: \r\nOK\r\n",
        "tls close releases a middle client slot");
    state.output.clear();

    feed(transport, "AT+SSLBEGINCLIENT\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+SSLBEGINCLIENT: 2003\r\nOK\r\n",
        "tls allocation reuses a released client slot");
}

void run_tcp_client_service_tests()
{
    FakeTcpClientBackend backend;
    bridge::TcpClientService service(
        {
            &backend,
            fake_tcp_create,
            fake_tcp_close,
            fake_tcp_create_listener,
            fake_tcp_accept,
            fake_tcp_resolve,
            fake_tcp_get_hostname_diagnostic,
            fake_tcp_connect,
            fake_tcp_send,
            fake_tcp_receive,
            fake_tcp_available,
            fake_tcp_peek,
            fake_tcp_connected,
            fake_tcp_state,
            fake_tcp_flush,
            fake_tcp_local_address,
        });
    TestState state;
    AtTransport transport(
        {
            128U,
            64U,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(tcp_client_command_callback, &service);

    feed(transport, "AT+HOSTBYNAME=fixture.test\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+HOSTBYNAME: 203.0.113.9\r\nOK\r\n",
        "tcp hostname transcript");
    state.output.clear();

    feed(transport, "AT+HOSTBYNAMEDIAG\r\n", 1U);
    flush_all(transport);
    expect(
        state.output ==
            "+HOSTBYNAMEDIAG: 1,1,0,0,192.0.2.53\r\n"
            "OK\r\n",
        "tcp hostname diagnostic transcript");
    state.output.clear();

    feed(transport, "AT+BEGINCLIENT\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+BEGINCLIENT: 0\r\nOK\r\n",
        "tcp automatic slot transcript");
    expect(backend.create_count == 1U, "tcp automatic slot creates handle");
    state.output.clear();

    feed(transport, "AT+IPCLIENT=0\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+IPCLIENT: 192.168.42.2\r\nOK\r\n",
        "tcp local ip transcript");
    state.output.clear();

    backend.connected = false;
    feed(transport, "AT+IPCLIENT=0\r\n", 2U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tcp disconnected local ip transcript");
    state.output.clear();
    backend.connected = true;

    feed(transport, "AT+CLIENTSTATUS=0\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+CLIENTSTATUS: \r\nOK\r\n", "tcp client status transcript");
    state.output.clear();

    FakeNetworkScanWindow window;
    backend.scan_window = &window;
    service.set_scan_control(FakeNetworkScanWindow::pause,
                             FakeNetworkScanWindow::resume, &window);
    feed(transport, "AT+CLIENTCONNECTIP=0,192.0.2.10,1234\r\n", 3U);
    flush_all(transport);
    expect(
        state.output == "+CLIENTCONNECTIP: \r\nOK\r\n",
        "tcp ipv4 connect transcript");
    expect(
        backend.connected_address[0] == 192U &&
            backend.connected_address[1] == 0U &&
            backend.connected_address[2] == 2U &&
            backend.connected_address[3] == 10U &&
            backend.connected_port == 1234U &&
            backend.connected_timeout_ms == 3000U,
        "tcp ipv4 connect backend arguments");
    state.output.clear();

    expect(window.pauses == 1U && window.resumes == 1U && !window.active,
           "TCP IP connection restores BLE scanning");
    feed(transport, "AT+CLIENTCONNECT=0,fixture.test,443,250\r\n", 3U);
    flush_all(transport);
    expect(state.output == "+CLIENTCONNECT: \r\nOK\r\n" &&
           window.pauses == 2U && window.resumes == 2U,
           "TCP hostname connection restores scanning after DNS and connect");
    state.output.clear();
    backend.connect_success = false;
    feed(transport, "AT+CLIENTCONNECTIP=0,192.0.2.10,1234\r\n", 3U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && window.resumes == 3U && !window.active,
           "TCP connection failure restores BLE scanning");
    state.output.clear();
    backend.connect_success = true;
    backend.resolve_success = false;
    feed(transport, "AT+CLIENTCONNECT=0,fixture.test,443,250\r\n", 3U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && window.resumes == 4U && !window.active,
           "TCP DNS failure restores BLE scanning");
    state.output.clear();
    backend.resolve_success = true;
    backend.connected = true;
    backend.scan_window = nullptr;
    service.set_scan_control(nullptr, nullptr, nullptr);

    feed(transport, "AT+AVAILABLE=0\r\n", 4U);
    flush_all(transport);
    expect(state.output == "+AVAILABLE: 5\r\nOK\r\n", "tcp available transcript");
    state.output.clear();

    feed(transport, "AT+PEEK=0\r\n", 5U);
    flush_all(transport);
    expect(state.output == "+PEEK: 114\r\nOK\r\n", "tcp peek transcript");
    state.output.clear();

    feed(transport, "AT+CLIENTRECEIVE=0,3\r\n", 6U);
    flush_all(transport);
    expect(state.output == "+CLIENTRECEIVE: 3|repOK\r\n", "tcp receive raw transcript");
    expect(
        backend.last_receive_buffer == transport.temporary_workspace(1U),
        "tcp receive borrows the transport workspace");
    state.output.clear();

    feed(transport, "AT+CLIENTSEND=0,4\r\npi", 7U);
    expect(transport.raw_payload_pending(), "tcp send waits for raw payload");
    feed(transport, "ng", 8U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tcp send raw transcript");
    expect(backend.sent == "ping", "tcp send forwards raw payload");
    state.output.clear();

    feed(transport, "AT+CLIENTCONNECTED=0\r\n", 9U);
    flush_all(transport);
    expect(state.output == "+CLIENTCONNECTED: 1\r\nOK\r\n", "tcp connected transcript");
    state.output.clear();

    feed(transport, "AT+CLIENTSTATE=0\r\n", 10U);
    flush_all(transport);
    expect(
        state.output == "+CLIENTSTATE: 198.51.100.7,4321,54321\r\nOK\r\n",
        "tcp state transcript");
    state.output.clear();

    backend.connected = false;
    feed(transport, "AT+CLIENTSTATE=0\r\n", 10U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tcp disconnected state transcript");
    state.output.clear();
    backend.connected = true;

    feed(transport, "AT+REMOTEIP=0\r\n", 11U);
    flush_all(transport);
    expect(state.output == "+REMOTEIP: 198.51.100.7\r\nOK\r\n", "tcp remote ip transcript");
    state.output.clear();

    feed(transport, "AT+REMOTEPORT=0\r\n", 12U);
    flush_all(transport);
    expect(state.output == "+REMOTEPORT: 4321\r\nOK\r\n", "tcp remote port transcript");
    state.output.clear();

    feed(transport, "AT+FLUSH=0\r\n", 13U);
    flush_all(transport);
    expect(state.output == "+FLUSH: \r\nOK\r\n", "tcp flush transcript");
    expect(backend.flush_count == 1U, "tcp flush invokes backend");
    state.output.clear();

    feed(transport, "AT+CLIENTCLOSE=0\r\n", 14U);
    flush_all(transport);
    expect(state.output == "+CLIENTCLOSE: \r\nOK\r\n", "tcp close transcript");
    expect(backend.close_count == 1U, "tcp close invokes backend");
    state.output.clear();

    feed(transport, "AT+CLIENTCONNECTED=0\r\n", 15U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "tcp closed slot rejects connected query");
    state.output.clear();

    feed(transport, "AT+BEGINCLIENT=3\r\n", 16U);
    flush_all(transport);
    expect(state.output == "+BEGINCLIENT: \r\nOK\r\n", "tcp explicit slot transcript");
    state.output.clear();

    feed(transport, "AT+CLIENTCONNECT=3,fixture.test,80,250\r\n", 17U);
    flush_all(transport);
    expect(state.output == "+CLIENTCONNECT: \r\nOK\r\n", "tcp name connect transcript");
    expect(
        backend.connected_address[0] == 203U &&
            backend.connected_address[1] == 0U &&
            backend.connected_address[2] == 113U &&
            backend.connected_address[3] == 9U &&
            backend.connected_port == 80U &&
            backend.connected_timeout_ms == 250U,
        "tcp hostname connect backend arguments");
    state.output.clear();

    for (size_t slot = 0U; slot < bridge::kTcpClientSlotCount; ++slot) {
        const std::string command = "AT+BEGINCLIENT=" + std::to_string(slot) + "\r\n";
        feed(transport, command.c_str(), static_cast<uint32_t>(20U + slot));
        flush_all(transport);
        state.output.clear();
    }
    feed(transport, "AT+BEGINCLIENT\r\n", 30U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "tcp slot exhaustion transcript");
    state.output.clear();

    feed(transport, "AT+CLIENTCONNECTIP=0,invalid,80\r\n", 31U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "tcp invalid ipv4 rejected");
}

void run_tcp_server_service_tests()
{
    FakeTcpClientBackend backend;
    bridge::TcpServerService service(
        {
            &backend,
            fake_tcp_create,
            fake_tcp_close,
            fake_tcp_create_listener,
            fake_tcp_accept,
            fake_tcp_resolve,
            fake_tcp_get_hostname_diagnostic,
            fake_tcp_connect,
            fake_tcp_send,
            fake_tcp_receive,
            fake_tcp_available,
            fake_tcp_peek,
            fake_tcp_connected,
            fake_tcp_state,
            fake_tcp_flush,
            fake_tcp_local_address,
        });
    TestState state;
    AtTransport transport(
        {
            128U,
            64U,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(tcp_server_command_callback, &service);
    FakeNetworkScanWindow window;
    backend.scan_window = &window;
    service.set_scan_control(FakeNetworkScanWindow::pause,
                             FakeNetworkScanWindow::resume, &window);

    feed(transport, "AT+BEGINSERVER=5000\r\n", 1U);
    flush_all(transport);
    expect(
        state.output == "+BEGINSERVER: 0\r\nOK\r\n",
        "tcp server create transcript");
    expect(
        backend.create_listener_count == 1U && backend.listener_port == 5000U,
        "tcp server listener backend arguments");
    expect(window.pauses == 1U && window.resumes == 1U && !window.active,
           "listener setup restores BLE scanning");
    state.output.clear();
    feed(transport, "AT+BEGINSERVER=0\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && window.pauses == 1U,
           "invalid listener request does not pause scanning");
    state.output.clear();
    backend.accept_success = false;
    feed(transport, "AT+SERVERACCEPT=0\r\n", 1U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && window.pauses == 2U &&
           window.resumes == 2U && !window.active,
           "failed accept restores BLE scanning");
    backend.accept_success = true;
    state.output.clear();

    feed(transport, "AT+SERVERAVAILABLE=0\r\n", 2U);
    flush_all(transport);
    expect(
        state.output == "+SERVERAVAILABLE: -1\r\nOK\r\n",
        "tcp server no pending client transcript");
    expect(window.pauses == 3U && window.resumes == 3U && !window.active,
           "empty nonblocking accept restores BLE scanning");
    state.output.clear();

    backend.pending_accept_handles.push_back(200);
    backend.received = "hello";
    feed(transport, "AT+SERVERAVAILABLE=0\r\n", 3U);
    flush_all(transport);
    expect(
        state.output == "+SERVERAVAILABLE: 1000\r\nOK\r\n",
        "tcp server available unaccepted client transcript");
    state.output.clear();

    feed(transport, "AT+AVAILABLE=1000\r\n", 4U);
    flush_all(transport);
    expect(
        state.output == "+AVAILABLE: 5\r\nOK\r\n",
        "tcp server client available transcript");
    state.output.clear();

    feed(transport, "AT+IPCLIENT=1000\r\n", 4U);
    flush_all(transport);
    expect(state.output == "+IPCLIENT: 192.168.42.2\r\nOK\r\n",
           "tcp server client local ip transcript");
    state.output.clear();

    feed(transport, "AT+CLIENTSTATUS=1000\r\n", 4U);
    flush_all(transport);
    expect(state.output == "+CLIENTSTATUS: \r\nOK\r\n",
           "tcp server client status transcript");
    state.output.clear();

    feed(transport, "AT+CLIENTRECEIVE=1000,3\r\n", 5U);
    flush_all(transport);
    expect(
        state.output == "+CLIENTRECEIVE: 3|helOK\r\n",
        "tcp server client receive transcript");
    expect(
        backend.last_receive_buffer == transport.temporary_workspace(1U),
        "tcp server receive borrows the transport workspace");
    state.output.clear();

    feed(transport, "AT+SERVERWRITE=0,4\r\npu", 6U);
    expect(transport.raw_payload_pending(), "tcp server write waits for raw payload");
    feed(transport, "sh", 7U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tcp server broadcast raw transcript");
    expect(backend.sent == "push", "tcp server broadcasts to unaccepted client");
    state.output.clear();

    backend.pending_accept_handles.push_back(201);
    feed(transport, "AT+SERVERACCEPT=0\r\n", 8U);
    flush_all(transport);
    expect(
        state.output == "+SERVERACCEPT: 1001\r\nOK\r\n",
        "tcp server accept client transcript");
    state.output.clear();

    feed(transport, "AT+SERVERWRITE=0,4\r\nmore", 9U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tcp server broadcast after accept transcript");
    expect(
        backend.sent == "pushmore",
        "tcp server broadcast excludes accepted client");
    state.output.clear();

    feed(transport, "AT+CLIENTSEND=1001,4\r\npong", 10U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tcp server accepted client send transcript");
    expect(
        backend.sent == "pushmorepong",
        "tcp server accepted client send backend data");
    state.output.clear();

    feed(transport, "AT+CLIENTSTATE=1001\r\n", 11U);
    flush_all(transport);
    expect(
        state.output == "+CLIENTSTATE: 198.51.100.7,4321,54321\r\nOK\r\n",
        "tcp server client state transcript");
    state.output.clear();

    backend.connected = false;
    feed(transport, "AT+CLIENTSTATE=1001\r\n", 11U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "tcp server disconnected state transcript");
    state.output.clear();
    backend.connected = true;

    feed(transport, "AT+CLIENTCLOSE=1001\r\n", 12U);
    flush_all(transport);
    expect(state.output == "+CLIENTCLOSE: \r\nOK\r\n", "tcp server client close transcript");
    expect(backend.close_count == 1U, "tcp server client close backend call");
    state.output.clear();

    feed(transport, "AT+SERVEREND=0\r\n", 13U);
    flush_all(transport);
    expect(state.output == "+SERVEREND: \r\nOK\r\n", "tcp server end transcript");
    expect(
        backend.close_count == 3U,
        "tcp server end closes remaining client and listener");
    state.output.clear();

    feed(transport, "AT+SERVERAVAILABLE=0\r\n", 14U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "tcp ended server rejects availability query");
}

struct FakeUdpBackend {
    FakeNetworkScanWindow *scan_window = nullptr;
    bool receive_success = true;
    int next_handle = 300;
    uint32_t open_count = 0U;
    uint32_t join_count = 0U;
    uint32_t close_count = 0U;
    uint16_t opened_port = 0U;
    uint8_t opened_address[4] = {};
    uint8_t joined_group[4] = {};
    uint16_t sent_port = 0U;
    uint8_t sent_address[4] = {};
    std::string sent;
    std::string pending = "udp-reply";
    uint8_t pending_source[4] = { 198U, 51U, 100U, 8U };
    uint16_t pending_source_port = 6000U;
};

bool fake_udp_open(
    void *context,
    uint16_t port,
    const uint8_t local_address[4],
    int *handle)
{
    FakeUdpBackend *backend = static_cast<FakeUdpBackend *>(context);
    if (handle == nullptr) {
        return false;
    }
    *handle = backend->next_handle++;
    backend->opened_port = port;
    if (local_address != nullptr) {
        memcpy(backend->opened_address, local_address, 4U);
    } else {
        memset(backend->opened_address, 0, 4U);
    }
    ++backend->open_count;
    return true;
}

bool fake_udp_join(void *context, int, const uint8_t group_address[4])
{
    FakeUdpBackend *backend = static_cast<FakeUdpBackend *>(context);
    if (group_address == nullptr) {
        return false;
    }
    memcpy(backend->joined_group, group_address, 4U);
    ++backend->join_count;
    return true;
}

bool fake_udp_close(void *context, int)
{
    ++static_cast<FakeUdpBackend *>(context)->close_count;
    return true;
}

bool fake_udp_resolve(void *context, const char *hostname, uint8_t address[4])
{
    auto *backend = static_cast<FakeUdpBackend *>(context);
    if (backend->scan_window != nullptr && backend->scan_window->allowed) {
        expect(backend->scan_window->active, "UDP DNS owns the RF window");
    }
    if (hostname == nullptr || address == nullptr ||
        strcmp(hostname, "udp.fixture.test") != 0) {
        return false;
    }
    address[0] = 203U;
    address[1] = 0U;
    address[2] = 113U;
    address[3] = 11U;
    return true;
}

bool fake_udp_send(
    void *context,
    int,
    const uint8_t destination[4],
    uint16_t port,
    const uint8_t *data,
    size_t size,
    size_t *sent)
{
    FakeUdpBackend *backend = static_cast<FakeUdpBackend *>(context);
    if (backend->scan_window != nullptr && backend->scan_window->allowed) {
        expect(backend->scan_window->active, "UDP send owns the RF window");
    }
    if (destination == nullptr || data == nullptr || sent == nullptr) {
        return false;
    }
    memcpy(backend->sent_address, destination, 4U);
    backend->sent_port = port;
    backend->sent.append(reinterpret_cast<const char *>(data), size);
    *sent = size;
    return true;
}

bool fake_udp_receive(
    void *context,
    int,
    uint8_t *data,
    size_t capacity,
    size_t *received,
    uint8_t source_address[4],
    uint16_t *source_port)
{
    FakeUdpBackend *backend = static_cast<FakeUdpBackend *>(context);
    if (backend->scan_window != nullptr && backend->scan_window->allowed) {
        expect(backend->scan_window->active, "UDP receive owns the RF window");
    }
    if (!backend->receive_success) return false;
    if (data == nullptr || received == nullptr || source_address == nullptr ||
        source_port == nullptr) {
        return false;
    }
    const size_t count = backend->pending.size() < capacity
        ? backend->pending.size()
        : capacity;
    memcpy(data, backend->pending.data(), count);
    backend->pending.erase(0U, count);
    memcpy(source_address, backend->pending_source, 4U);
    *source_port = backend->pending_source_port;
    *received = count;
    return true;
}

bool fake_udp_diagnostics(void *, int, bridge::UdpDiagnostics *diagnostics)
{
    if (diagnostics == nullptr) {
        return false;
    }
    diagnostics->flags = 1023U;
    diagnostics->last_send_result = 4;
    diagnostics->last_errno = 0;
    diagnostics->sta_address[0] = 192U;
    diagnostics->sta_address[1] = 168U;
    diagnostics->sta_address[2] = 4U;
    diagnostics->sta_address[3] = 2U;
    diagnostics->sta_gateway[0] = 192U;
    diagnostics->sta_gateway[1] = 168U;
    diagnostics->sta_gateway[2] = 4U;
    diagnostics->sta_gateway[3] = 1U;
    diagnostics->default_address[0] = 192U;
    diagnostics->default_address[1] = 168U;
    diagnostics->default_address[2] = 4U;
    diagnostics->default_address[3] = 2U;
    diagnostics->local_address[0] = 192U;
    diagnostics->local_address[1] = 168U;
    diagnostics->local_address[2] = 4U;
    diagnostics->local_address[3] = 2U;
    diagnostics->local_port = 5001U;
    diagnostics->lwip_udp_xmit_delta = 4U;
    diagnostics->lwip_udp_route_error_delta = 1U;
    diagnostics->lwip_ip_xmit_delta = 4U;
    diagnostics->lwip_ip_route_error_delta = 1U;
    diagnostics->lwip_ip_drop_delta = 2U;
    diagnostics->wifi_output_packets = 5U;
    diagnostics->wifi_output_bytes = 512U;
    diagnostics->wifi_output_ipv4_packets = 4U;
    diagnostics->wifi_output_udp_packets = 4U;
    diagnostics->wifi_output_arp_packets = 1U;
    diagnostics->wifi_linkoutput_calls = 5U;
    diagnostics->wifi_tx_queued = 5U;
    diagnostics->wifi_tx_confirmed = 5U;
    diagnostics->wifi_tx_acknowledged = 4U;
    diagnostics->wifi_tx_unacknowledged = 1U;
    diagnostics->last_udp_source_port = 5001U;
    diagnostics->last_udp_destination_port = 6000U;
    diagnostics->last_udp_ipv4_total_length = 32U;
    diagnostics->last_udp_length = 12U;
    diagnostics->last_udp_ipv4_checksum = 0x1234U;
    diagnostics->last_udp_ipv4_checksum_residual = 0xFFFFU;
    diagnostics->last_udp_checksum = 0x5678U;
    diagnostics->last_udp_checksum_residual = 0xFFFFU;
    diagnostics->last_udp_checksum_flags = 15U;
    return true;
}

CommandResult udp_command_callback(
    void *context,
    const Command &command,
    AtTransport &transport)
{
    return static_cast<bridge::UdpService *>(context)->handle_command(command, transport);
}

struct FakeUdpRuntimeClock {
    uint32_t now = 0U;
};

uint32_t fake_udp_runtime_now(void *context)
{
    return static_cast<FakeUdpRuntimeClock *>(context)->now;
}

void fake_udp_runtime_idle(void *context)
{
    static_cast<FakeUdpRuntimeClock *>(context)->now += 100U;
}

CommandResult udp_runtime_command_callback(
    void *context,
    const Command &command,
    AtTransport &transport)
{
    return static_cast<bridge::UdpRuntimeDiagnostic *>(context)->handle_command(
        command,
        transport);
}

void run_udp_runtime_diagnostic_tests()
{
    FakeUdpBackend backend;
    backend.pending = "bl616cl-udp-reply";
    backend.pending_source[0] = 203U;
    backend.pending_source[1] = 0U;
    backend.pending_source[2] = 113U;
    backend.pending_source[3] = 10U;
    backend.pending_source_port = 4244U;
    FakeUdpRuntimeClock clock;
    bridge::UdpRuntimeDiagnostic service(
        {
            &backend,
            fake_udp_open,
            fake_udp_join,
            fake_udp_close,
            fake_udp_resolve,
            fake_udp_send,
            fake_udp_receive,
            fake_udp_diagnostics,
        },
        {
            &clock,
            fake_udp_runtime_now,
            fake_udp_runtime_idle,
        });
    TestState state;
    AtTransport transport(
        {
            128U,
            64U,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(udp_runtime_command_callback, &service);

    feed(transport, "AT+UDPRUNTIME=4245,4244,203.0.113.10\r\n", 1U);
    flush_all(transport);
    expect(
        state.output ==
            "+UDPRUNTIME: 1,0,1,17,1023,4,0,CB00710A,4244,4,1,4,1,2,5,512,4,4,1,5,5,5,4,1,5001,6000,32,12,4660,65535,22136,65535,15\r\nOK\r\n",
        "direct udp runtime successful transcript");
    expect(
        backend.opened_port == 4245U &&
            backend.sent == "bl616cl-udp-echo" &&
            backend.sent_port == 4244U &&
            backend.sent_address[0] == 203U &&
            backend.sent_address[3] == 10U &&
            backend.close_count == 1U,
        "direct udp runtime bypasses packet service");

    state.output.clear();
    backend.pending.clear();
    clock.now = 0U;
    feed(transport, "AT+UDPRUNTIME=4245,4244,203.0.113.10\r\n", 2U);
    flush_all(transport);
    expect(
        state.output ==
            "+UDPRUNTIME: 0,3,8,0,1023,4,0,00000000,0,4,1,4,1,2,5,512,4,4,1,5,5,5,4,1,5001,6000,32,12,4660,65535,22136,65535,15\r\nOK\r\n",
        "direct udp runtime bounded timeout transcript");
    expect(backend.close_count == 2U, "direct udp runtime closes timeout socket");
}

void run_udp_service_tests()
{
    FakeUdpBackend backend;
    bridge::UdpService service(
        {
            &backend,
            fake_udp_open,
            fake_udp_join,
            fake_udp_close,
            fake_udp_resolve,
            fake_udp_send,
            fake_udp_receive,
            fake_udp_diagnostics,
        });
    TestState state;
    AtTransport transport(
        {
            128U,
            64U,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(udp_command_callback, &service);
    FakeNetworkScanWindow window;
    backend.scan_window = &window;
    service.set_scan_control(FakeNetworkScanWindow::pause,
                             FakeNetworkScanWindow::resume, &window);

    feed(transport, "AT+UDPBEGIN=5001,192.0.2.2\r\n", 1U);
    flush_all(transport);
    expect(state.output == "+UDPBEGIN: 0\r\nOK\r\n", "udp begin transcript");
    expect(
        backend.open_count == 1U && backend.opened_port == 5001U &&
            backend.opened_address[0] == 192U &&
            backend.opened_address[1] == 0U &&
            backend.opened_address[2] == 2U &&
            backend.opened_address[3] == 2U,
        "udp bind arguments");
    state.output.clear();

    feed(transport, "AT+UDPBEGINPACKETIP=0,6001,203.0.113.10\r\n", 2U);
    flush_all(transport);
    expect(state.output == "+UDPBEGINPACKETIP: \r\nOK\r\n", "udp packet begin ip transcript");
    state.output.clear();

    feed(transport, "AT+UDPWRITE=0,4\r\nping", 3U);
    flush_all(transport);
    expect(state.output == "OK\r\n", "udp write raw transcript");
    expect(window.pauses == 0U, "buffering UDP raw data leaves scanning active");
    state.output.clear();

    feed(transport, "AT+UDPENDPACKET=0\r\n", 4U);
    flush_all(transport);
    expect(state.output == "+UDPENDPACKET: \r\nOK\r\n", "udp end packet transcript");
    expect(
        backend.sent == "ping" && backend.sent_port == 6001U &&
            backend.sent_address[0] == 203U &&
            backend.sent_address[3] == 10U,
        "udp datagram destination and payload");
    expect(window.pauses == 1U && window.resumes == 1U && !window.active,
           "UDP send restores scanning");
    state.output.clear();

    feed(transport, "AT+UDPDEBUG=0\r\n", 5U);
    flush_all(transport);
    expect(
        state.output ==
            "+UDPDEBUG: 1023,4,0,C0A80402,C0A80401,C0A80402,C0A80402,5001,0,0,0,0,0,0,0,0,0,4,0,0,0,0\r\nOK\r\n",
        "udp diagnostic transcript");
    state.output.clear();

    feed(transport, "AT+UDPPARSE=0\r\n", 6U);
    flush_all(transport);
    expect(state.output == "+UDPPARSE: 9\r\nOK\r\n", "udp parse transcript");
    expect(window.pauses == 2U && window.resumes == 2U && !window.active,
           "UDP receive restores scanning");
    state.output.clear();
    feed(transport, "AT+UDPAVAILABLE=0\r\n", 7U);
    flush_all(transport);
    expect(state.output == "+UDPAVAILABLE: 9\r\nOK\r\n", "udp available transcript");
    state.output.clear();
    feed(transport, "AT+UDPPEEK=0\r\n", 8U);
    flush_all(transport);
    expect(state.output == "+UDPPEEK: 117\r\nOK\r\n", "udp peek transcript");
    state.output.clear();
    feed(transport, "AT+UDPREAD=0,4\r\n", 9U);
    flush_all(transport);
    expect(state.output == "+UDPREAD: 4|udp-OK\r\n", "udp read raw transcript");
    state.output.clear();
    feed(transport, "AT+UDPAVAILABLE=0\r\n", 10U);
    flush_all(transport);
    expect(state.output == "+UDPAVAILABLE: 5\r\nOK\r\n", "udp partial read preserves remainder");
    state.output.clear();
    feed(transport, "AT+UDPREMOTEIP=0\r\n", 11U);
    flush_all(transport);
    expect(state.output == "+UDPREMOTEIP: 198.51.100.8\r\nOK\r\n", "udp remote ip transcript");
    state.output.clear();
    feed(transport, "AT+UDPREMOTEPORT=0\r\n", 12U);
    flush_all(transport);
    expect(state.output == "+UDPREMOTEPORT: 6000\r\nOK\r\n", "udp remote port transcript");
    state.output.clear();

    feed(transport, "AT+UDPFLUSH=0\r\n", 13U);
    flush_all(transport);
    expect(state.output == "+UDPFLUSH: \r\nOK\r\n", "udp flush transcript");
    state.output.clear();
    backend.receive_success = false;
    feed(transport, "AT+UDPPARSE=0\r\n", 13U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n" && window.pauses == 3U &&
           window.resumes == 3U && !window.active,
           "failed UDP receive restores scanning");
    state.output.clear();
    backend.receive_success = true;
    feed(transport, "AT+UDPPARSE=0\r\n", 13U);
    flush_all(transport);
    expect(state.output == "+UDPPARSE: 0\r\nOK\r\n" && window.pauses == 4U &&
           window.resumes == 4U && !window.active,
           "empty UDP receive restores scanning");
    state.output.clear();
    feed(transport, "AT+UDPSTOP=0\r\n", 14U);
    flush_all(transport);
    expect(state.output == "+UDPSTOP: \r\nOK\r\n", "udp stop transcript");
    expect(backend.close_count == 1U, "udp stop closes socket");
    state.output.clear();

    feed(transport, "AT+UDPBEGINMULTI=5002,239.1.2.3\r\n", 15U);
    flush_all(transport);
    expect(state.output == "+UDPBEGINMULTI: 0\r\nOK\r\n", "udp multicast begin transcript");
    expect(
        backend.join_count == 1U && backend.joined_group[0] == 239U &&
            backend.joined_group[3] == 3U,
        "udp multicast group");
    state.output.clear();
    feed(transport, "AT+BEGINPACKETMULTI=0\r\n", 16U);
    flush_all(transport);
    expect(state.output == "+BEGINPACKETMULTI: \r\nOK\r\n", "udp multicast packet begin transcript");
    state.output.clear();
    feed(transport, "AT+UDPWRITE=0,3\r\nabc", 17U);
    flush_all(transport);
    feed(transport, "AT+UDPENDPACKET=0\r\n", 18U);
    flush_all(transport);
    expect(
        backend.sent == "pingabc" && backend.sent_port == 5002U &&
            backend.sent_address[0] == 239U,
        "udp multicast destination and port");
    state.output.clear();

    feed(transport, "AT+UDPBEGINPACKETADD=0,6002,udp.fixture.test\r\n", 19U);
    flush_all(transport);
    expect(state.output == "+UDPBEGINPACKETADD: \r\nOK\r\n", "udp hostname packet begin transcript");
    expect(
        backend.sent_address[0] == 239U &&
            backend.sent_port == 5002U,
        "udp hostname begin does not send prematurely");
    state.output.clear();

    service.close_all();
    expect(backend.close_count == 2U, "udp close all releases remaining sockets");
}

void run_udp_max_payload_test()
{
    FakeUdpBackend backend;
    bridge::UdpService service(
        {
            &backend,
            fake_udp_open,
            fake_udp_join,
            fake_udp_close,
            fake_udp_resolve,
            fake_udp_send,
            fake_udp_receive,
            fake_udp_diagnostics,
        });
    TestState state;
    AtTransport transport(
        {
            128U,
            bridge::at::kTransportRawPayloadCapacity,
            512U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(udp_command_callback, &service);

    feed(transport, "AT+UDPBEGIN=5001\r\n", 1U);
    flush_all(transport);
    state.output.clear();
    const std::string payload(bridge::at::kTransportRawPayloadCapacity, 'x');
    for (uint32_t cycle = 0U; cycle < 2U; ++cycle) {
        feed(transport, "AT+UDPBEGINPACKETIP=0,6001,203.0.113.10\r\n", 2U + cycle * 3U);
        flush_all(transport);
        state.output.clear();
        const std::string write = "AT+UDPWRITE=0,16384\r\n" + payload;
        feed_bytes(
            transport,
            reinterpret_cast<const uint8_t *>(write.data()),
            write.size(),
            3U + cycle * 3U);
        flush_all(transport);
        expect(state.output == "OK\r\n", "udp maximum raw payload accepted");
        state.output.clear();
        feed(transport, "AT+UDPENDPACKET=0\r\n", 4U + cycle * 3U);
        flush_all(transport);
        expect(
            state.output == "+UDPENDPACKET: \r\nOK\r\n" &&
                backend.sent.size() == (cycle + 1U) * payload.size(),
            "udp maximum datagram sent after buffer allocation and reuse");
        state.output.clear();
    }

    backend.pending = "before-send";
    feed(transport, "AT+UDPPARSE=0\r\n", 9U);
    flush_all(transport);
    expect(state.output == "+UDPPARSE: 11\r\nOK\r\n", "udp pending receive");
    state.output.clear();
    feed(transport, "AT+UDPBEGINPACKETIP=0,6001,203.0.113.10\r\n", 10U);
    flush_all(transport);
    feed(transport, "AT+UDPWRITE=0,4\r\nping", 11U);
    flush_all(transport);
    feed(transport, "AT+UDPENDPACKET=0\r\n", 12U);
    flush_all(transport);
    expect(backend.sent.substr(2U * payload.size()) == "ping",
           "udp send keeps unread datagram independent");
    state.output.clear();
    feed(transport, "AT+UDPREAD=0,11\r\n", 13U);
    flush_all(transport);
    expect(state.output == "+UDPREAD: 11|before-sendOK\r\n",
           "udp unread datagram survives send");
    state.output.clear();

    backend.pending = "during-send";
    feed(transport, "AT+UDPBEGINPACKETIP=0,6001,203.0.113.10\r\n", 14U);
    flush_all(transport);
    feed(transport, "AT+UDPWRITE=0,4\r\npong", 15U);
    flush_all(transport);
    state.output.clear();
    feed(transport, "AT+UDPPARSE=0\r\n", 16U);
    flush_all(transport);
    expect(state.output == "+UDPPARSE: 11\r\nOK\r\n",
           "udp receive migrates in-progress send buffer");
    state.output.clear();
    feed(transport, "AT+UDPENDPACKET=0\r\n", 17U);
    flush_all(transport);
    expect(backend.sent.substr(2U * payload.size()) == "pingpong",
           "udp migrated send payload remains intact");
    state.output.clear();
    feed(transport, "AT+UDPREAD=0,11\r\n", 18U);
    flush_all(transport);
    expect(state.output == "+UDPREAD: 11|during-sendOK\r\n",
           "udp concurrent receive payload remains intact");
}

} // namespace

int main()
{
    bridge::ByteRing<4U> ring;
    expect(ring.push(1U), "ring first push");
    expect(ring.push(2U), "ring second push");
    expect(ring.push(3U), "ring third push");
    expect(!ring.push(4U), "ring reserves one slot");
    uint8_t ring_data[2] = {};
    expect(ring.read(ring_data, sizeof(ring_data)) == 2U, "ring read size");
    expect(ring_data[0] == 1U && ring_data[1] == 2U, "ring read order");
    expect(ring.push(4U), "ring wrap first push");
    expect(ring.push(5U), "ring wrap second push");
    uint8_t ring_value = 0U;
    expect(ring.pop(ring_value) && ring_value == 3U, "ring wrap first pop");
    expect(ring.pop(ring_value) && ring_value == 4U, "ring wrap second pop");
    expect(ring.pop(ring_value) && ring_value == 5U, "ring wrap third pop");
    expect(!ring.pop(ring_value), "ring empty pop");

    TestState state;
    AtTransport transport(
        {
            32U,
            8U,
            64U,
            20U,
        },
        output_callback,
        &state);
    transport.set_command_handler(command_callback, &state);

    feed(transport, "AT+ECHO=\"a,b\",c\\,d\r\n", 1U);
    flush_all(transport);
    expect(state.output == "+ECHO: echo\r\nOK\r\n", "quoted command response");
    state.output.clear();

    feed(transport, "AT+RAW=4\r\nAB", 2U);
    expect(transport.raw_payload_pending(), "raw payload pending");
    expect(
        transport.temporary_workspace(1U) == nullptr,
        "raw payload owns the temporary workspace");
    expect(state.output.empty(), "raw response deferred");

    static constexpr char kRawAndCommand[] = "CDAT+FWVERSION?\r\n";
    const size_t consumed = transport.feed(
        reinterpret_cast<const uint8_t *>(kRawAndCommand),
        sizeof(kRawAndCommand) - 1U,
        3U);
    expect(consumed == 2U, "input pauses after completed raw payload response");
    expect(state.raw_payload == "ABCD", "raw payload bytes");
    expect(transport.output_pending(), "raw payload response pending");
    flush_all(transport);
    expect(state.output == "OK\r\n", "raw payload response");
    expect(
        transport.temporary_workspace(8U) != nullptr,
        "temporary workspace releases after raw payload completion");
    state.output.clear();
    expect(
        transport.feed(
            reinterpret_cast<const uint8_t *>(kRawAndCommand) + consumed,
            sizeof(kRawAndCommand) - 1U - consumed,
            4U) == sizeof(kRawAndCommand) - 1U - consumed,
        "remaining input accepted after raw response");
    flush_all(transport);
    expect(state.output == "+FWVERSION: 0.6.0\r\nOK\r\n", "input retained after raw response");
    state.output.clear();

    feed(transport, "AT+DEFER\r\n", 5U);
    expect(transport.response_deferred(), "deferred response state");
    expect(state.output.empty(), "deferred response is not emitted early");
    expect(
        !transport.complete_deferred_response(CommandResult::Deferred),
        "deferred response cannot complete as deferred");
    expect(
        transport.complete_deferred_response(CommandResult::Ok),
        "deferred response completion");
    flush_all(transport);
    expect(state.output == "OK\r\n", "deferred response transcript");
    state.output.clear();

    feed(transport, "AT+RAW=4\r\nA", 10U);
    expect(transport.raw_payload_pending(), "second raw payload pending");
    transport.tick(31U);
    expect(!transport.raw_payload_pending(), "raw payload timeout reset");
    const auto raw_timeout = transport.raw_payload_timeout_diagnostic();
    expect(raw_timeout.count == 1U && raw_timeout.expected == 4U &&
           raw_timeout.received == 1U, "raw timeout preserves exact byte progress");
    transport.tick(32U);
    expect(transport.raw_payload_timeout_diagnostic().count == 1U,
           "finished timeout is counted once");
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "raw payload timeout response");
    state.output.clear();

    feed(transport, "AT+FWVERSION?\r\n", 32U);
    flush_all(transport);
    expect(state.output == "+FWVERSION: 0.6.0\r\nOK\r\n", "parser recovery after timeout");
    state.output.clear();

    feed(transport, "AT+UNKNOWN\r\n", 33U);
    flush_all(transport);
    expect(state.output == "ERROR\r\n", "unknown command response");
    state.output.clear();

    feed(transport, "AT+RAW=4\r\nA", 34U);
    transport.reset();
    expect(!transport.raw_payload_pending(), "explicit transport reset");
    feed(transport, "AT+FWVERSION?\r\n", 35U);
    flush_all(transport);
    expect(state.output == "+FWVERSION: 0.6.0\r\nOK\r\n", "parser recovery after explicit reset");

    state.output.clear();
    state.output_limit = 3U;
    feed(transport, "AT+FWVERSION?\r\n", 36U);
    expect(transport.output_pending(), "partial output pending");
    flush_all(transport);
    expect(state.output == "+FWVERSION: 0.6.0\r\nOK\r\n", "first response complete under backpressure");
    state.output.clear();
    state.output_limit = static_cast<size_t>(-1);
    feed(transport, "AT+FWVERSION?\r\n", 37U);
    flush_all(transport);
    expect(state.output == "+FWVERSION: 0.6.0\r\nOK\r\n", "second response ordered after backpressure");

    run_golden_transcripts();
    run_wifi_status_tracker_tests();
    run_wifi_service_tests();
    run_tcp_client_service_tests();
    run_tcp_server_service_tests();
    run_ping_service_tests();
    run_tls_client_service_tests();
    run_udp_service_tests();
    run_udp_max_payload_test();
    run_udp_runtime_diagnostic_tests();
    run_bouffalo_hci_packet_adapter_tests();
    run_ble_hci_at_tests();
    run_ble_hci_large_acl_tests();
    run_ble_hci_large_acl_at_read_tests();
    run_ble_hci_large_write_tests();
    run_ble_hci_fragment_priority_tests();
    run_bouffalo_hci_controller_source_tests();
    run_hci_idle_scan_disable_compat_tests();
    run_hci_scan_advertising_quiet_gate_tests();

    return 0;
}
