#include "../UNOR4BL616CLBridge/at_transport.h"
#include "../UNOR4BL616CLBridge/ra4_ota_at_service.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using bridge::Ra4OtaAtService;
using bridge::Ra4OtaCacheBackend;
using bridge::Ra4OtaContainer;
using bridge::Ra4OtaService;
using bridge::at::AtTransport;
using bridge::at::Command;
using bridge::at::CommandResult;

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

struct FakeCache {
    std::vector<uint8_t> bytes;
    bool open = false;
    uint32_t discard_count = 0U;
};

bool cache_begin(void *context, const char *, bool)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    cache->bytes.clear();
    cache->open = true;
    return true;
}

bool cache_write(void *context, const uint8_t *data, size_t size)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    if (!cache->open) {
        return false;
    }
    cache->bytes.insert(cache->bytes.end(), data, data + size);
    return true;
}

bool cache_commit(void *context)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    cache->open = false;
    return true;
}

void cache_discard(void *context)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    ++cache->discard_count;
    cache->open = false;
    cache->bytes.clear();
}

size_t cache_capacity(void *)
{
    return 1024U;
}

struct FakeDownload {
    bool opened = false;
    bool secure = false;
    size_t ca_size = 0U;
    std::vector<uint8_t> ca;
    std::string hostname;
    std::string request;
    std::vector<std::string> chunks;
    size_t next_chunk = 0U;
};

bool download_open(
    void *context,
    bool secure,
    const char *hostname,
    uint16_t,
    const uint8_t *ca,
    size_t ca_size,
    uint32_t,
    int *handle)
{
    FakeDownload *download = static_cast<FakeDownload *>(context);
    download->opened = true;
    download->secure = secure;
    download->ca_size = ca_size;
    download->ca.assign(ca, ca + ca_size);
    download->hostname = hostname;
    *handle = 5;
    return true;
}

bool download_write(
    void *context,
    int,
    const uint8_t *data,
    size_t size)
{
    static_cast<FakeDownload *>(context)->request.append(
        reinterpret_cast<const char *>(data),
        size);
    return true;
}

bridge::Ra4OtaDownloadReadResult download_read(
    void *context,
    int,
    uint8_t *data,
    size_t capacity,
    size_t *size)
{
    FakeDownload *download = static_cast<FakeDownload *>(context);
    if (download->next_chunk == download->chunks.size()) {
        *size = 0U;
        return bridge::Ra4OtaDownloadReadResult::Pending;
    }
    const std::string &chunk = download->chunks[download->next_chunk++];
    if (chunk.size() > capacity) {
        return bridge::Ra4OtaDownloadReadResult::Error;
    }
    memcpy(data, chunk.data(), chunk.size());
    *size = chunk.size();
    return bridge::Ra4OtaDownloadReadResult::Data;
}

void download_close(void *context, int)
{
    static_cast<FakeDownload *>(context)->opened = false;
}

size_t collect_output(void *context, const uint8_t *data, size_t size)
{
    static_cast<std::string *>(context)->append(
        reinterpret_cast<const char *>(data),
        size);
    return size;
}

CommandResult dispatch(void *context, const Command &command, AtTransport &transport)
{
    return static_cast<Ra4OtaAtService *>(context)->handle_command(command, transport);
}

void flush(AtTransport &transport)
{
    for (size_t count = 0U; transport.output_pending() && count < 128U; ++count) {
        transport.flush_output();
    }
    expect(!transport.output_pending(), "transport output flushed");
}

void feed(AtTransport &transport, const char *text, uint32_t time_ms)
{
    const size_t size = strlen(text);
    expect(
        transport.feed(
            reinterpret_cast<const uint8_t *>(text),
            size,
            time_ms) == size,
        "complete AT input consumption");
}

void feed_bytes(
    AtTransport &transport,
    const uint8_t *data,
    size_t size,
    uint32_t time_ms)
{
    expect(
        transport.feed(data, size, time_ms) == size,
        "complete binary AT input consumption");
}

void write_u32_le(std::vector<uint8_t> &data, size_t offset, uint32_t value)
{
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
    data[offset + 2U] = static_cast<uint8_t>(value >> 16U);
    data[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

std::vector<uint8_t> package()
{
    const std::vector<uint8_t> compressed = { 0xA0U, 0x80U };
    std::vector<uint8_t> result(Ra4OtaContainer::kHeaderSize, 0U);
    write_u32_le(result, 0U, static_cast<uint32_t>(12U + compressed.size()));
    write_u32_le(result, 8U, Ra4OtaContainer::kUnoR4Magic);
    uint32_t crc = Ra4OtaContainer::crc32_update(
        0xFFFFFFFFU,
        result.data() + 8U,
        12U);
    crc = Ra4OtaContainer::crc32_update(crc, compressed.data(), compressed.size());
    write_u32_le(result, 4U, crc ^ 0xFFFFFFFFU);
    result.insert(result.end(), compressed.begin(), compressed.end());
    return result;
}

void run_test()
{
    FakeCache cache;
    FakeDownload download;
    const Ra4OtaCacheBackend backend = {
        cache_begin,
        cache_write,
        cache_commit,
        cache_discard,
        cache_capacity,
        &cache,
    };
    const bridge::Ra4OtaDownloadBackend download_backend = {
        &download,
        download_open,
        download_write,
        download_read,
        download_close,
    };
    Ra4OtaService ota(backend);
    Ra4OtaAtService service(ota, download_backend);
    std::string output;
    AtTransport transport(
        {
            1024U,
            16384U,
            17424U,
            5000U,
        },
        collect_output,
        &output);
    transport.set_command_handler(dispatch, &service);

    feed(transport, "AT+OTABEGIN\r\n", 1U);
    flush(transport);
    expect(output == "ERROR\r\n", "self OTA run form is rejected");
    output.clear();

    feed(transport, "AT+OTABEGIN=/ra4.bin,2\r\n", 2U);
    flush(transport);
    expect(output == "ERROR\r\n", "invalid format flag rejected");
    output.clear();

    feed(transport, "AT+OTABEGIN=/ra4.bin,0\r\n", 3U);
    flush(transport);
    expect(output == "+OTABEGIN: 0\r\nOK\r\n", "cache begin transcript");
    output.clear();
    expect(ota.state() == Ra4OtaService::State::Receiving, "cache receiving");

    feed(transport, "AT+OTAVERIFY\r\n", 4U);
    flush(transport);
    expect(output == "+OTAVERIFY: -12\r\nOK\r\n", "verify rejects incomplete download");
    output.clear();

    const std::vector<uint8_t> input = package();
    expect(service.feed_download(input.data(), 7U) == Ra4OtaService::Error::None,
        "first downloaded chunk");
    expect(service.feed_download(input.data() + 7U, input.size() - 7U) ==
            Ra4OtaService::Error::None,
        "second downloaded chunk");
    expect(service.finish_download() == Ra4OtaService::Error::None, "download finish");
    expect(cache.bytes == std::vector<uint8_t>({ 'A' }), "decoded cache preserved");

    feed(transport, "AT+OTAVERIFY\r\n", 5U);
    flush(transport);
    expect(output == "+OTAVERIFY: 0\r\nOK\r\n", "verify transcript");
    output.clear();

    feed(transport, "AT+OTARESET\r\n", 6U);
    flush(transport);
    expect(output == "+OTARESET: 0\r\nOK\r\n", "reset transcript");
    expect(cache.bytes == std::vector<uint8_t>({ 'A' }), "verified cache retained");
    expect(ota.state() == Ra4OtaService::State::Idle, "reset returns idle");
    output.clear();

    const char ca[] = "ca\n";
    feed(transport, "AT+OTASETCAROOT=3\r\n", 7U);
    feed_bytes(
        transport,
        reinterpret_cast<const uint8_t *>(ca),
        sizeof(ca) - 1U,
        7U);
    flush(transport);
    expect(output == "OK\r\n", "CA raw payload transcript");
    output.clear();

    const std::vector<uint8_t> downloaded_package = package();
    const std::string package_text(
        reinterpret_cast<const char *>(downloaded_package.data()),
        downloaded_package.size());
    download.chunks = {
        "HTTP/1.1 200 OK\r\nContent-Length: " +
            std::to_string(package_text.size()) + "\r\n\r\n" +
            package_text.substr(0U, 5U),
        package_text.substr(5U),
    };
    download.next_chunk = 0U;
    feed(
        transport,
        "AT+OTADOWNLOAD=https://updates.example/ra4.ota,/ra4.bin\r\n",
        8U);
    expect(transport.response_deferred(), "synchronous download response deferred");
    expect(output.empty(), "synchronous download waits for backend");
    service.poll(9U);
    service.poll(10U);
    flush(transport);
    expect(
        output == "+OTADOWNLOAD: 1\r\nOK\r\n",
        "synchronous download transcript");
    output.clear();
    expect(download.secure, "synchronous HTTPS selected");
    expect(ota.state() == Ra4OtaService::State::Ready, "synchronous cache ready");
    expect(cache.bytes == std::vector<uint8_t>({ 'A' }), "synchronous decoded cache");

    download.chunks = {
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n",
    };
    download.next_chunk = 0U;
    feed(
        transport,
        "AT+OTADOWNLOAD=https://updates.example/missing.ota,/ra4.bin\r\n",
        11U);
    expect(transport.response_deferred(), "synchronous failure response deferred");
    service.poll(12U);
    flush(transport);
    expect(
        output == "+OTADOWNLOAD: -14\r\nOK\r\n",
        "synchronous backend failure transcript");
    output.clear();
    expect(ota.state() == Ra4OtaService::State::Failed, "synchronous failure state");

    feed(transport, "AT+OTADOWNLOAD\r\n", 13U);
    flush(transport);
    expect(output == "ERROR\r\n", "synchronous invalid mode rejected");
    output.clear();

    download.chunks = {
        "HTTP/1.1 200 OK\r\nContent-Length: " +
            std::to_string(package_text.size()) + "\r\n\r\n" +
            package_text.substr(0U, 5U),
        package_text.substr(5U),
    };
    download.next_chunk = 0U;
    feed(
        transport,
        "AT+OTADOWNLOADSTART=https://updates.example/ra4.ota,/ra4.bin\r\n",
        14U);
    flush(transport);
    expect(
        output == "+OTADOWNLOADSTART: 0\r\nOK\r\n",
        "async download start transcript");
    output.clear();
    expect(download.secure, "HTTPS selected");
    expect(download.hostname == "updates.example", "HTTPS hostname");
    expect(download.ca_size == sizeof(ca), "NUL-terminated CA forwarded");
    expect(
        download.ca == std::vector<uint8_t>({ 'c', 'a', '\n', '\0' }),
        "CA NUL terminator forwarded");

    feed(transport, "AT+OTADOWNLOADPROGRESS\r\n", 15U);
    flush(transport);
    expect(
        output == "+OTADOWNLOADPROGRESS: 0\r\nOK\r\n",
        "initial download progress");
    output.clear();

    service.poll(16U);
    service.poll(17U);
    expect(ota.state() == Ra4OtaService::State::Ready, "downloaded cache ready");
    expect(cache.bytes == std::vector<uint8_t>({ 'A' }), "async decoded cache");

    feed(transport, "AT+OTADOWNLOADPROGRESS\r\n", 18U);
    flush(transport);
    expect(
        output == "+OTADOWNLOADPROGRESS: 22\r\nOK\r\n",
        "completed download progress");
    output.clear();

    feed(transport, "AT+OTAVERIFY\r\n", 19U);
    flush(transport);
    expect(output == "+OTAVERIFY: 0\r\nOK\r\n", "download verify transcript");
}

} // namespace

int main()
{
    run_test();
    return 0;
}
