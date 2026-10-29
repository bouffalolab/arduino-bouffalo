#include "../UNOR4BL616CLBridge/ra4_ota_downloader.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using bridge::Ra4OtaCacheBackend;
using bridge::Ra4OtaContainer;
using bridge::Ra4OtaDownloadBackend;
using bridge::Ra4OtaDownloadReadResult;
using bridge::Ra4OtaDownloader;
using bridge::Ra4OtaService;

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
    static_cast<FakeCache *>(context)->open = false;
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

Ra4OtaCacheBackend cache_backend(FakeCache &cache)
{
    return {
        cache_begin,
        cache_write,
        cache_commit,
        cache_discard,
        cache_capacity,
        &cache,
    };
}

struct FakeStream {
    bool opened = false;
    bool secure = false;
    std::string hostname;
    uint16_t port = 0U;
    std::string request;
    std::vector<std::string> chunks;
    size_t next_chunk = 0U;
};

bool stream_open(
    void *context,
    bool secure,
    const char *hostname,
    uint16_t port,
    const uint8_t *,
    size_t,
    uint32_t,
    int *handle)
{
    FakeStream *stream = static_cast<FakeStream *>(context);
    stream->opened = true;
    stream->secure = secure;
    stream->hostname = hostname;
    stream->port = port;
    *handle = 7;
    return true;
}

bool stream_write(void *context, int, const uint8_t *data, size_t size)
{
    static_cast<FakeStream *>(context)->request.append(
        reinterpret_cast<const char *>(data),
        size);
    return true;
}

Ra4OtaDownloadReadResult stream_read(
    void *context,
    int,
    uint8_t *data,
    size_t capacity,
    size_t *size)
{
    FakeStream *stream = static_cast<FakeStream *>(context);
    if (stream->next_chunk == stream->chunks.size()) {
        *size = 0U;
        return Ra4OtaDownloadReadResult::Pending;
    }
    const std::string &chunk = stream->chunks[stream->next_chunk++];
    if (chunk.size() > capacity) {
        return Ra4OtaDownloadReadResult::Error;
    }
    memcpy(data, chunk.data(), chunk.size());
    *size = chunk.size();
    return Ra4OtaDownloadReadResult::Data;
}

void stream_close(void *context, int)
{
    static_cast<FakeStream *>(context)->opened = false;
}

Ra4OtaDownloadBackend stream_backend(FakeStream &stream)
{
    return {
        &stream,
        stream_open,
        stream_write,
        stream_read,
        stream_close,
    };
}

void write_u32_le(std::vector<uint8_t> &data, size_t offset, uint32_t value)
{
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
    data[offset + 2U] = static_cast<uint8_t>(value >> 16U);
    data[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

std::string package()
{
    const std::vector<uint8_t> compressed = {0xA0U, 0x80U};
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
    return std::string(
        reinterpret_cast<const char *>(result.data()),
        result.size());
}

void run_success_test()
{
    FakeCache cache;
    FakeStream stream;
    Ra4OtaService ota(cache_backend(cache));
    Ra4OtaDownloader downloader(ota, stream_backend(stream));
    const std::string body = package();
    stream.chunks = {
        "HTTP/1.1 200 OK\r\nContent-L",
        "ength: " + std::to_string(body.size()) + "\r\nX-Test: yes\r\n\r\n" +
            body.substr(0U, 7U),
        body.substr(7U),
    };

    expect(ota.begin("/ra4.bin", false) == Ra4OtaService::Error::None,
        "cache begin");
    expect(downloader.start("http://updates.example:8080/update.ota", nullptr, 0U, 1U) ==
            Ra4OtaService::Error::None,
        "start http download");
    expect(stream.hostname == "updates.example" && stream.port == 8080U,
        "parsed host and port");
    expect(stream.request ==
            "GET /update.ota HTTP/1.1\r\nHost: updates.example:8080\r\n"
            "Accept: application/octet-stream\r\nConnection: close\r\n\r\n",
        "request framing");
    downloader.poll(2U);
    const bridge::Ra4OtaDownloadDiagnostic first_diagnostic =
        downloader.diagnostic();
    expect(first_diagnostic.secure == false, "HTTP diagnostic security");
    expect(first_diagnostic.poll_count == 1U, "first diagnostic poll count");
    expect(first_diagnostic.data_count == 1U, "first diagnostic data count");
    expect(
        first_diagnostic.last_read_result == Ra4OtaDownloadReadResult::Data &&
            first_diagnostic.last_read_valid,
        "first diagnostic read result");
    downloader.poll(3U);
    downloader.poll(4U);
    expect(downloader.state() == Ra4OtaDownloader::State::Ready, "download ready");
    expect(downloader.content_length() == body.size(), "content length");
    expect(downloader.downloaded_size() == body.size(), "download size");
    expect(ota.state() == Ra4OtaService::State::Ready, "ota cache ready");
    expect(cache.bytes == std::vector<uint8_t>({'A'}), "decoded cache");
    const bridge::Ra4OtaDownloadDiagnostic completed_diagnostic =
        downloader.diagnostic();
    expect(completed_diagnostic.poll_count == 3U, "completed diagnostic polls");
    expect(
        completed_diagnostic.content_length == body.size() &&
            completed_diagnostic.downloaded_size == body.size(),
        "completed diagnostic sizes");
}

void run_failure_tests()
{
    FakeCache cache;
    FakeStream stream;
    Ra4OtaService ota(cache_backend(cache));
    Ra4OtaDownloader downloader(ota, stream_backend(stream));

    expect(ota.begin("/ra4.bin", false) == Ra4OtaService::Error::None,
        "invalid url cache begin");
    expect(downloader.start("ftp://updates.example/a", nullptr, 0U, 0U) ==
            Ra4OtaService::Error::UrlParse,
        "invalid url rejected");
    expect(cache.discard_count == 1U, "invalid url discards cache");

    expect(ota.begin("/ra4.bin", false) == Ra4OtaService::Error::None,
        "https cache begin");
    expect(downloader.start("https://updates.example/a", nullptr, 0U, 0U) ==
            Ra4OtaService::Error::ServerConnect,
        "https requires CA");

    const std::string body = package();
    expect(ota.begin("/ra4.bin", false) == Ra4OtaService::Error::None,
        "body timeout cache begin");
    stream.chunks = {
        "HTTP/1.1 200 OK\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\n\r\n",
    };
    stream.next_chunk = 0U;
    expect(downloader.start("http://updates.example/slow.ota", nullptr, 0U, 100U) ==
            Ra4OtaService::Error::None,
        "body timeout download start");
    downloader.poll(101U);
    const bridge::Ra4OtaDownloadDiagnostic slow_diagnostic =
        downloader.diagnostic();
    expect(slow_diagnostic.data_count == 1U, "slow diagnostic header data count");
    expect(
        slow_diagnostic.timeout_reason == bridge::Ra4OtaDownloadTimeoutReason::None,
        "slow diagnostic does not time out early");
    expect(downloader.state() == Ra4OtaDownloader::State::ReceivingBody,
        "body timeout enters body state");
    downloader.poll(30099U);
    expect(downloader.state() == Ra4OtaDownloader::State::ReceivingBody,
        "body timeout permits a 30 second idle interval");
    const bridge::Ra4OtaDownloadDiagnostic pending_diagnostic =
        downloader.diagnostic();
    expect(
        pending_diagnostic.pending_count == 1U &&
            pending_diagnostic.timeout_reason ==
                bridge::Ra4OtaDownloadTimeoutReason::None,
        "body idle diagnostic");
    downloader.poll(30101U);
    expect(downloader.state() == Ra4OtaDownloader::State::Failed &&
            downloader.error() == Ra4OtaService::Error::Download,
        "body timeout rejects an interval beyond 30 seconds");
    expect(
        downloader.diagnostic().timeout_reason ==
            bridge::Ra4OtaDownloadTimeoutReason::Body,
        "body timeout diagnostic");

    expect(ota.begin("/ra4.bin", false) == Ra4OtaService::Error::None,
        "status cache begin");
    stream.chunks = {"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"};
    stream.next_chunk = 0U;
    expect(downloader.start("http://updates.example/a", nullptr, 0U, 0U) ==
            Ra4OtaService::Error::None,
        "start status failure");
    downloader.poll(1U);
    expect(downloader.error() == Ra4OtaService::Error::HttpResponse,
        "status failure is rejected");

    expect(ota.begin("/ra4.bin", false) == Ra4OtaService::Error::None,
        "timeout cache begin");
    stream.chunks.clear();
    stream.next_chunk = 0U;
    expect(downloader.start("http://updates.example/a", nullptr, 0U, 100U) ==
            Ra4OtaService::Error::None,
        "start timeout");
    downloader.poll(10100U);
    expect(downloader.error() == Ra4OtaService::Error::HeaderTimeout,
        "header timeout");
}

} // namespace

int main()
{
    run_success_test();
    run_failure_tests();
    return 0;
}
