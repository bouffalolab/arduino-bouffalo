#include "../UNOR4BL616CLBridge/ra4_samba_service.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct FakeBootstrapIo {
    std::string events;
    int8_t boot_level = 1;
    int8_t reset_level = 1;
};

struct FakeLink {
    std::string events;
    std::vector<uint8_t> writes;
    std::deque<uint8_t> reads;
    bool suspended = false;
    bool resumed = false;
    uint32_t baud = 115200U;
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

bool input_float(void *context, uint8_t pin)
{
    FakeBootstrapIo *io = static_cast<FakeBootstrapIo *>(context);
    io->events += "I" + std::to_string(pin) + ";";
    return true;
}

bool output(void *context, uint8_t pin, bool high)
{
    FakeBootstrapIo *io = static_cast<FakeBootstrapIo *>(context);
    io->events += "O" + std::to_string(pin) + (high ? "H;" : "L;");
    if (pin == 14U) {
        io->boot_level = high ? 1 : 0;
    } else if (pin == 3U) {
        io->reset_level = high ? 1 : 0;
    }
    return true;
}

int8_t read_level(void *context, uint8_t pin)
{
    FakeBootstrapIo *io = static_cast<FakeBootstrapIo *>(context);
    return pin == 14U ? io->boot_level : io->reset_level;
}

bool delay_ms(void *, uint32_t)
{
    return true;
}

bridge::Ra4BootstrapHooks bootstrap_hooks(FakeBootstrapIo &io)
{
    return {
        input_float,
        output,
        read_level,
        delay_ms,
        &io,
    };
}

bool update_baud(void *context, uint32_t baud)
{
    FakeLink *link = static_cast<FakeLink *>(context);
    link->events += "B" + std::to_string(baud) + ";";
    link->baud = baud;
    return true;
}

void clear_rx(void *context)
{
    static_cast<FakeLink *>(context)->events += "C;";
}

size_t write(void *context, const uint8_t *data, size_t size)
{
    FakeLink *link = static_cast<FakeLink *>(context);
    link->writes.insert(link->writes.end(), data, data + size);
    return size;
}

int read(void *context)
{
    FakeLink *link = static_cast<FakeLink *>(context);
    if (link->reads.empty()) {
        return -1;
    }
    const uint8_t value = link->reads.front();
    link->reads.pop_front();
    return value;
}

bool suspend(void *context)
{
    FakeLink *link = static_cast<FakeLink *>(context);
    link->events += "S;";
    link->suspended = true;
    return true;
}

bool resume(void *context)
{
    FakeLink *link = static_cast<FakeLink *>(context);
    link->events += "R;";
    link->resumed = true;
    return true;
}

uint32_t current_baud(void *context)
{
    return static_cast<FakeLink *>(context)->baud;
}

bridge::Ra4SambaLink samba_link(FakeLink &link)
{
    return {
        update_baud,
        clear_rx,
        write,
        read,
        &link,
    };
}

bridge::Ra4SambaUserUartHooks uart_hooks(FakeLink &link)
{
    return {
        suspend,
        resume,
        current_baud,
        &link,
    };
}

void append_read(FakeLink &link, const char *text)
{
    link.reads.insert(link.reads.end(), text, text + strlen(text));
}

void expect_writes(
    const FakeLink &link,
    const std::vector<uint8_t> &expected,
    const char *message)
{
    expect(link.writes == expected, message);
}

void run_success_case()
{
    FakeBootstrapIo bootstrap_io;
    FakeLink link;
    bridge::Ra4BootstrapControl bootstrap(14U, 3U, bootstrap_hooks(bootstrap_io));
    bridge::Ra4SambaService service(bootstrap, samba_link(link), uart_hooks(link));

    expect(service.request_read_only_handshake(), "first handshake request accepted");
    expect(!service.request_read_only_handshake(), "second handshake request rejected");
    service.poll(0U);
    expect(service.active(), "handshake becomes active");
    expect(link.suspended, "user UART bridge suspended");
    expect(
        bootstrap_io.events == "O14H;O3L;O3H;O3L;O3H;",
        "bootstrap entry sequence is preserved");
    expect(link.events == "S;C;B230400;", "service owns UART before SAM-BA");

    service.poll(499U);
    expect(link.writes.empty(), "bootloader settle defers auto-baud");
    service.poll(500U);
    service.poll(510U);
    service.poll(520U);
    expect_writes(
        link,
        {0x80U, 0x80U, '#', 'N', '#'},
        "auto-baud and binary mode command sequence");

    append_read(link, "\n\r");
    service.poll(521U);
    expect_writes(
        link,
        {0x80U, 0x80U, '#', 'N', '#', 'V', '#'},
        "binary reply leads to version command");

    append_read(link, "Arduino Bootloader (SAM-BA extended) 1.0\n\r");
    service.poll(522U);
    expect_writes(
        link,
        {0x80U, 0x80U, '#', 'N', '#', 'V', '#', 'K', '#'},
        "version reply hands back to application");
    service.poll(572U);

    const bridge::Ra4SambaDiagnostic &diagnostic = service.diagnostic();
    expect(!service.active(), "successful handshake completes");
    expect(diagnostic.succeeded, "successful handshake reports success");
    expect(diagnostic.error == bridge::Ra4SambaError::kNone, "success has no error");
    expect(
        std::string(diagnostic.version) ==
            "Arduino Bootloader (SAM-BA extended) 1.0\n\r",
        "version is retained for telemetry");
    expect(link.baud == 115200U, "user UART baud restored");
    expect(link.resumed, "user UART bridge resumed");
    expect(
        bootstrap_io.events == "O14H;O3L;O3H;O3L;O3H;I3;I14;",
        "bootstrap control released after application handoff");
    expect(
        link.events == "S;C;B230400;C;C;B115200;R;",
        "UART recovery is ordered after K#");
}

void run_crlf_version_case()
{
    FakeBootstrapIo bootstrap_io;
    FakeLink link;
    bridge::Ra4BootstrapControl bootstrap(14U, 3U, bootstrap_hooks(bootstrap_io));
    bridge::Ra4SambaService service(bootstrap, samba_link(link), uart_hooks(link));

    expect(service.request_read_only_handshake(), "CRLF handshake request accepted");
    service.poll(0U);
    service.poll(500U);
    service.poll(510U);
    service.poll(520U);
    append_read(link, "\n\r");
    service.poll(521U);
    append_read(link, "Arduino Bootloader (SAM-BA extended) 1.0\r\n");
    service.poll(522U);
    service.poll(572U);

    const bridge::Ra4SambaDiagnostic &diagnostic = service.diagnostic();
    expect(!service.active(), "CRLF handshake completes");
    expect(diagnostic.succeeded, "CRLF handshake reports success");
    expect(
        diagnostic.error == bridge::Ra4SambaError::kNone,
        "CRLF handshake has no error");
}

void run_noise_before_binary_reply_case()
{
    FakeBootstrapIo bootstrap_io;
    FakeLink link;
    bridge::Ra4BootstrapControl bootstrap(14U, 3U, bootstrap_hooks(bootstrap_io));
    bridge::Ra4SambaService service(bootstrap, samba_link(link), uart_hooks(link));

    expect(service.request_read_only_handshake(), "noise-tolerant request accepted");
    service.poll(0U);
    service.poll(500U);
    service.poll(510U);
    service.poll(520U);
    link.reads.push_back('J');
    append_read(link, "\n\r");
    service.poll(521U);
    append_read(link, "Arduino Bootloader (SAM-BA extended) 1.0\n\r");
    service.poll(522U);
    service.poll(572U);

    const bridge::Ra4SambaDiagnostic &diagnostic = service.diagnostic();
    expect(!service.active(), "noise-tolerant handshake completes");
    expect(diagnostic.succeeded, "noise before N# reply is ignored");
    expect(
        diagnostic.binary_reply_bytes[0] == 'J' &&
            diagnostic.binary_reply_bytes[1] == '\n',
        "first binary reply bytes retain the leading noise");
}

void run_timeout_recovery_case()
{
    FakeBootstrapIo bootstrap_io;
    FakeLink link;
    bridge::Ra4BootstrapControl bootstrap(14U, 3U, bootstrap_hooks(bootstrap_io));
    bridge::Ra4SambaService service(bootstrap, samba_link(link), uart_hooks(link));

    expect(service.request_read_only_handshake(), "timeout request accepted");
    service.poll(0U);
    service.poll(500U);
    service.poll(510U);
    service.poll(520U);
    service.poll(1520U);
    service.poll(1521U);

    const bridge::Ra4SambaDiagnostic &diagnostic = service.diagnostic();
    expect(!service.active(), "timeout recovery completes");
    expect(!diagnostic.succeeded, "timeout recovery reports failure");
    expect(
        diagnostic.error == bridge::Ra4SambaError::kBinaryReplyTimeout,
        "binary timeout is diagnosed");
    expect(link.resumed, "timeout resumes user UART bridge");
    expect(link.baud == 115200U, "timeout restores user UART baud");
    expect_writes(
        link,
        {0x80U, 0x80U, '#', 'N', '#', 'K', '#'},
        "timeout makes a best-effort K# recovery request");
}

void run_binary_reply_invalid_case()
{
    FakeBootstrapIo bootstrap_io;
    FakeLink link;
    bridge::Ra4BootstrapControl bootstrap(14U, 3U, bootstrap_hooks(bootstrap_io));
    bridge::Ra4SambaService service(bootstrap, samba_link(link), uart_hooks(link));

    expect(service.request_read_only_handshake(), "invalid reply request accepted");
    service.poll(0U);
    service.poll(500U);
    service.poll(510U);
    service.poll(520U);
    for (uint8_t index = 0U; index <= 32U; ++index) {
        link.reads.push_back('J');
    }
    service.poll(521U);

    const bridge::Ra4SambaDiagnostic &diagnostic = service.diagnostic();
    expect(
        diagnostic.error == bridge::Ra4SambaError::kBinaryReplyInvalid,
        "invalid binary reply is diagnosed");
    expect(
        diagnostic.binary_reply_bytes[0] == 'J',
        "invalid binary reply byte is retained");
    expect(
        diagnostic.binary_reply_bytes[1] == 'J',
        "second invalid binary reply byte is retained");
}

} // namespace

int main()
{
    run_success_case();
    run_crlf_version_case();
    run_noise_before_binary_reply_case();
    run_timeout_recovery_case();
    run_binary_reply_invalid_case();
    return 0;
}
