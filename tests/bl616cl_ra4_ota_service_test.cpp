#include "../UNOR4BL616CLBridge/ra4_ota_service.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using bridge::Ra4OtaCacheBackend;
using bridge::Ra4OtaContainer;
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
    std::string path;
    std::vector<uint8_t> bytes;
    size_t capacity = 1024U;
    bool begin_succeeds = true;
    bool write_succeeds = true;
    bool commit_succeeds = true;
    uint32_t begin_count = 0U;
    uint32_t commit_count = 0U;
    uint32_t discard_count = 0U;
};

bool cache_begin(void *context, const char *path, bool)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    ++cache->begin_count;
    if (!cache->begin_succeeds) {
        return false;
    }
    cache->path = path;
    cache->bytes.clear();
    return true;
}

bool cache_write(void *context, const uint8_t *data, size_t size)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    if (!cache->write_succeeds ||
        cache->bytes.size() + size > cache->capacity) {
        return false;
    }
    cache->bytes.insert(cache->bytes.end(), data, data + size);
    return true;
}

bool cache_commit(void *context)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    ++cache->commit_count;
    return cache->commit_succeeds;
}

void cache_discard(void *context)
{
    FakeCache *cache = static_cast<FakeCache *>(context);
    ++cache->discard_count;
    cache->bytes.clear();
}

size_t cache_capacity(void *context)
{
    return static_cast<FakeCache *>(context)->capacity;
}

Ra4OtaCacheBackend backend(FakeCache &cache)
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

void write_u32_le(std::vector<uint8_t> &data, size_t offset, uint32_t value)
{
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
    data[offset + 2U] = static_cast<uint8_t>(value >> 16U);
    data[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

std::vector<uint8_t> package(
    const std::vector<uint8_t> &compressed,
    uint32_t magic = Ra4OtaContainer::kUnoR4Magic)
{
    std::vector<uint8_t> result(Ra4OtaContainer::kHeaderSize, 0U);
    write_u32_le(result, 0U, static_cast<uint32_t>(12U + compressed.size()));
    write_u32_le(result, 8U, magic);
    uint32_t crc = Ra4OtaContainer::crc32_update(
        0xFFFFFFFFU,
        result.data() + 8U,
        12U);
    crc = Ra4OtaContainer::crc32_update(
        crc,
        compressed.data(),
        compressed.size());
    write_u32_le(result, 4U, crc ^ 0xFFFFFFFFU);
    result.insert(result.end(), compressed.begin(), compressed.end());
    return result;
}

void run_success_test()
{
    FakeCache cache;
    Ra4OtaService service(backend(cache));
    const std::vector<uint8_t> input = package({0xA0U, 0x80U});

    expect(service.begin("/ra4-update.bin", false) == Ra4OtaService::Error::None,
        "start cache");
    expect(service.state() == Ra4OtaService::State::Receiving, "receiving state");
    expect(service.feed(input.data(), 7U) == Ra4OtaService::Error::None,
        "first input chunk");
    expect(service.feed(input.data() + 7U, input.size() - 7U) == Ra4OtaService::Error::None,
        "second input chunk");
    expect(service.finish() == Ra4OtaService::Error::None, "finish success");
    expect(service.state() == Ra4OtaService::State::Ready, "ready state");
    expect(cache.path == "/ra4-update.bin", "cache path");
    expect(cache.bytes == std::vector<uint8_t>({'A'}), "decoded cache bytes");
    expect(cache.commit_count == 1U, "cache committed");
    expect(cache.discard_count == 0U, "successful cache retained");
}

void run_failure_tests()
{
    const std::vector<uint8_t> input = package({0xA0U, 0x80U});

    FakeCache storage_failure;
    storage_failure.begin_succeeds = false;
    Ra4OtaService start_failure(backend(storage_failure));
    expect(start_failure.begin("/bad.bin", false) == Ra4OtaService::Error::StorageInit,
        "storage begin failure");

    FakeCache magic_failure;
    Ra4OtaService magic(backend(magic_failure));
    std::vector<uint8_t> invalid_magic = input;
    invalid_magic[8] ^= 1U;
    expect(magic.begin("/bad.bin", false) == Ra4OtaService::Error::None,
        "magic start");
    expect(magic.feed(invalid_magic.data(), invalid_magic.size()) ==
            Ra4OtaService::Error::HeaderMagic,
        "magic error mapping");
    expect(magic.state() == Ra4OtaService::State::Failed, "magic failed state");
    expect(magic_failure.discard_count == 1U, "magic cache discarded");

    FakeCache output_failure;
    output_failure.write_succeeds = false;
    Ra4OtaService output(backend(output_failure));
    expect(output.begin("/bad.bin", false) == Ra4OtaService::Error::None,
        "output start");
    expect(output.feed(input.data(), input.size()) ==
            Ra4OtaService::Error::StorageEnd,
        "output error mapping");
    expect(output_failure.discard_count == 1U, "output cache discarded");

    FakeCache commit_failure;
    commit_failure.commit_succeeds = false;
    Ra4OtaService commit(backend(commit_failure));
    expect(commit.begin("/bad.bin", false) == Ra4OtaService::Error::None,
        "commit start");
    expect(commit.feed(input.data(), input.size()) == Ra4OtaService::Error::None,
        "commit input");
    expect(commit.finish() == Ra4OtaService::Error::StorageEnd,
        "commit error mapping");
    expect(commit_failure.discard_count == 1U, "commit cache discarded");

    FakeCache length_failure;
    length_failure.capacity = 0U;
    Ra4OtaService length(backend(length_failure));
    expect(length.begin("/bad.bin", false) == Ra4OtaService::Error::StorageInit,
        "zero capacity rejected");
}

} // namespace

int main()
{
    run_success_test();
    run_failure_tests();
    return 0;
}
