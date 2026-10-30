#include "../UNOR4BL616CLBridge/ra4_samba_programmer.h"

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

struct FakeImage {
    std::vector<uint8_t> bytes;
    bool opened = false;
    uint32_t close_count = 0U;
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
    return {input_float, output, read_level, delay_ms, &io};
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
    return {update_baud, clear_rx, write, read, &link};
}

bridge::Ra4SambaUserUartHooks uart_hooks(FakeLink &link)
{
    return {suspend, resume, current_baud, &link};
}

bool image_open(void *context, size_t *size)
{
    FakeImage *image = static_cast<FakeImage *>(context);
    image->opened = true;
    *size = image->bytes.size();
    return true;
}

bool image_read(
    void *context,
    size_t offset,
    uint8_t *destination,
    size_t size)
{
    FakeImage *image = static_cast<FakeImage *>(context);
    if (!image->opened || offset + size > image->bytes.size()) {
        return false;
    }
    memcpy(destination, image->bytes.data() + offset, size);
    return true;
}

void image_close(void *context)
{
    FakeImage *image = static_cast<FakeImage *>(context);
    image->opened = false;
    ++image->close_count;
}

bridge::Ra4SambaImageSource image_source(FakeImage &image)
{
    return {image_open, image_read, image_close, &image};
}

void append_read(FakeLink &link, const char *text)
{
    link.reads.insert(link.reads.end(), text, text + strlen(text));
}

std::string writes_text(const FakeLink &link)
{
    return std::string(link.writes.begin(), link.writes.end());
}

void advance_to_version(
    bridge::Ra4SambaProgrammer &programmer,
    FakeLink &link)
{
    programmer.poll(0U);
    programmer.poll(500U);
    programmer.poll(510U);
    programmer.poll(520U);
    append_read(link, "\n\r");
    programmer.poll(521U);
    append_read(
        link,
        "Arduino Bootloader (SAM-BA extended) 2.0 [Arduino:IKXYZ]\n\r");
    programmer.poll(522U);
}

void run_success_case()
{
    FakeBootstrapIo bootstrap_io;
    FakeLink link;
    FakeImage image;
    image.bytes.resize(4200U);
    for (size_t index = 0U; index < image.bytes.size(); ++index) {
        image.bytes[index] = static_cast<uint8_t>(index);
    }

    bridge::Ra4BootstrapControl bootstrap(14U, 3U, bootstrap_hooks(bootstrap_io));
    bridge::Ra4SambaProgrammer programmer(
        bootstrap, samba_link(link), uart_hooks(link));
    expect(programmer.request_program(image_source(image)), "program request accepted");
    expect(programmer.busy(), "queued program request is busy");
    expect(!programmer.request_program(image_source(image)), "duplicate request rejected");

    advance_to_version(programmer, link);
    expect(
        writes_text(link).find("S00000000,00000034#") != std::string::npos,
        "word-copy applet is staged");

    programmer.poll(524U);
    programmer.poll(532U);
    programmer.poll(542U);
    programmer.poll(552U);
    programmer.poll(562U);
    expect(
        writes_text(link).find("X00000000#") != std::string::npos,
        "application erase request emitted");
    append_read(link, "X\n\r");
    programmer.poll(563U);
    expect(
        writes_text(link).find("S00000034,00001000#") != std::string::npos,
        "first 4KiB image chunk staged");
    const size_t first_chunk_command_bytes = link.writes.size();
    programmer.poll(564U);
    expect(
        link.writes.size() == first_chunk_command_bytes,
        "chunk payload waits after staging command");
    programmer.poll(565U);
    expect(
        link.writes.size() == first_chunk_command_bytes + 64U,
        "first chunk payload packet is bounded to 64 bytes");
    for (uint32_t now = 570U; now <= 880U; now += 5U) {
        programmer.poll(now);
    }
    expect(
        link.writes.size() == first_chunk_command_bytes + 4096U,
        "first 4KiB payload is fully staged in packets");
    programmer.poll(890U);
    append_read(link, "Y\n\r");
    programmer.poll(891U);
    append_read(link, "Y\n\r");
    programmer.poll(892U);

    expect(
        writes_text(link).find("S00000034,00000100#") != std::string::npos,
        "final chunk is page aligned before staging");
    programmer.poll(895U);
    programmer.poll(900U);
    programmer.poll(905U);
    programmer.poll(910U);
    programmer.poll(920U);
    append_read(link, "Y\n\r");
    programmer.poll(921U);
    append_read(link, "Y\n\r");
    programmer.poll(922U);
    programmer.poll(973U);

    const bridge::Ra4SambaProgrammerDiagnostic &diagnostic =
        programmer.diagnostic();
    expect(!programmer.active(), "programmer completes");
    expect(!programmer.busy(), "programmer clears busy state");
    expect(diagnostic.succeeded, "programmer reports success");
    expect(
        diagnostic.error == bridge::Ra4SambaProgrammerError::kNone,
        "success has no error");
    expect(diagnostic.image_bytes == 4200U, "image size recorded");
    expect(diagnostic.verified_bytes == 4200U, "all payload bytes verified");
    expect(diagnostic.chunk_index == 2U, "two chunks recorded");
    expect(image.close_count == 1U, "image source is closed");
    expect(link.baud == 115200U, "user UART baud restored");
    expect(link.resumed, "CDC UART bridge resumed");
    expect(
        writes_text(link).find("K#") != std::string::npos,
        "application handoff requested");
}

void run_write_failure_case()
{
    FakeBootstrapIo bootstrap_io;
    FakeLink link;
    FakeImage image;
    image.bytes = {0x12U, 0x34U};
    bridge::Ra4BootstrapControl bootstrap(14U, 3U, bootstrap_hooks(bootstrap_io));
    bridge::Ra4SambaProgrammer programmer(
        bootstrap, samba_link(link), uart_hooks(link));
    expect(programmer.request_program(image_source(image)), "failure request accepted");
    advance_to_version(programmer, link);
    programmer.poll(524U);
    programmer.poll(534U);
    programmer.poll(544U);
    programmer.poll(554U);
    append_read(link, "X\n\r");
    programmer.poll(555U);
    programmer.poll(557U);
    programmer.poll(562U);
    programmer.poll(567U);
    programmer.poll(572U);
    programmer.poll(582U);
    append_read(link, "Y\n\r");
    programmer.poll(583U);
    append_read(link, "Q\n\r");
    programmer.poll(584U);
    programmer.poll(585U);

    const bridge::Ra4SambaProgrammerDiagnostic &diagnostic =
        programmer.diagnostic();
    expect(!programmer.active(), "write failure recovers");
    expect(!diagnostic.succeeded, "write failure is not successful");
    expect(
        diagnostic.error == bridge::Ra4SambaProgrammerError::kWriteReplyInvalid,
        "write reply is diagnosed");
    expect(image.close_count == 1U, "write failure closes image source");
    expect(link.resumed, "write failure resumes user UART bridge");
    expect(writes_text(link).find("K#") != std::string::npos, "write failure requests K#");
}

} // namespace

int main()
{
    run_success_case();
    run_write_failure_case();
    return 0;
}
