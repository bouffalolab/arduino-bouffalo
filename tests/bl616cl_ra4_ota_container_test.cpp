#include "../UNOR4BL616CLBridge/ra4_ota_container.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using bridge::Ra4OtaContainer;

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

struct Output {
    std::vector<uint8_t> bytes;
    size_t write_limit = static_cast<size_t>(-1);
};

bool write_output(void *context, const uint8_t *data, size_t size)
{
    Output *output = static_cast<Output *>(context);
    if (size > output->write_limit) {
        return false;
    }
    output->bytes.insert(output->bytes.end(), data, data + size);
    return true;
}

void write_u32_le(std::vector<uint8_t> &data, size_t offset, uint32_t value)
{
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1U] = static_cast<uint8_t>(value >> 8U);
    data[offset + 2U] = static_cast<uint8_t>(value >> 16U);
    data[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

std::vector<uint8_t> make_package(
    const std::vector<uint8_t> &compressed,
    uint32_t magic = Ra4OtaContainer::kUnoR4Magic)
{
    std::vector<uint8_t> package(Ra4OtaContainer::kHeaderSize, 0U);
    write_u32_le(package, 8U, magic);
    write_u32_le(
        package,
        0U,
        static_cast<uint32_t>(12U + compressed.size()));
    uint32_t crc = Ra4OtaContainer::crc32_update(
        0xFFFFFFFFU,
        package.data() + 8U,
        12U);
    crc = Ra4OtaContainer::crc32_update(
        crc,
        compressed.data(),
        compressed.size());
    write_u32_le(package, 4U, crc ^ 0xFFFFFFFFU);
    package.insert(package.end(), compressed.begin(), compressed.end());
    return package;
}

void feed_chunked(
    Ra4OtaContainer &container,
    const std::vector<uint8_t> &package,
    size_t chunk_size)
{
    for (size_t offset = 0U; offset < package.size(); offset += chunk_size) {
        const size_t size = package.size() - offset < chunk_size
            ? package.size() - offset
            : chunk_size;
        const Ra4OtaContainer::Error result =
            container.feed(package.data() + offset, size);
        if (result != Ra4OtaContainer::Error::None) {
            std::cerr << "feed error=" << static_cast<int>(result)
                      << " offset=" << offset << " size=" << size << '\n';
            fail("chunk accepted");
        }
    }
}

std::vector<uint8_t> read_file(const char *path)
{
    std::ifstream stream(path, std::ios::binary);
    expect(stream.good(), "fixture opens");
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>(),
    };
}

uint32_t read_u32_le(const std::vector<uint8_t> &data, size_t offset)
{
    return static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1U]) << 8U) |
        (static_cast<uint32_t>(data[offset + 2U]) << 16U) |
        (static_cast<uint32_t>(data[offset + 3U]) << 24U);
}

void run_literal_and_reference_tests()
{
    const std::vector<uint8_t> literal_package = make_package({0xA0U, 0x80U});
    Output literal_output;
    Ra4OtaContainer literal;
    expect(literal.begin({write_output, &literal_output}) == Ra4OtaContainer::Error::None,
        "literal begin");
    feed_chunked(literal, literal_package, 1U);
    expect(literal.finish() == Ra4OtaContainer::Error::None, "literal finish");
    expect(
        literal_output.bytes == std::vector<uint8_t>({'A'}),
        "literal decode");
    expect(literal.complete(), "literal completes");
    expect(
        literal.header().container_length == literal_package.size() - 8U,
        "literal header length");

    const std::vector<uint8_t> reference_package = make_package({0x00U, 0x00U});
    Output reference_output;
    Ra4OtaContainer reference;
    expect(reference.begin({write_output, &reference_output}) == Ra4OtaContainer::Error::None,
        "reference begin");
    feed_chunked(reference, reference_package, 3U);
    expect(reference.finish() == Ra4OtaContainer::Error::None, "reference finish");
    expect(
        reference_output.bytes == std::vector<uint8_t>({' ', ' '}),
        "reference decode");
}

void run_error_tests()
{
    const std::vector<uint8_t> package = make_package({0xA0U, 0x80U});

    Output output;
    Ra4OtaContainer magic;
    expect(magic.begin({write_output, &output}) == Ra4OtaContainer::Error::None,
        "magic begin");
    std::vector<uint8_t> invalid_magic = package;
    invalid_magic[8] ^= 1U;
    expect(
        magic.feed(invalid_magic.data(), invalid_magic.size()) ==
            Ra4OtaContainer::Error::HeaderMagic,
        "magic mismatch rejected");

    Ra4OtaContainer bounded(Ra4OtaContainer::kUnoR4Magic, 0U);
    expect(bounded.begin({write_output, &output}) == Ra4OtaContainer::Error::None,
        "bound begin");
    expect(
        bounded.feed(package.data(), package.size()) ==
            Ra4OtaContainer::Error::OutputOverflow,
        "decoded output bound rejected");

    std::vector<uint8_t> invalid_crc = package;
    invalid_crc.back() ^= 1U;
    Ra4OtaContainer crc;
    expect(crc.begin({write_output, &output}) == Ra4OtaContainer::Error::None,
        "crc begin");
    feed_chunked(crc, invalid_crc, 2U);
    expect(crc.finish() == Ra4OtaContainer::Error::CrcMismatch,
        "crc mismatch rejected");

    Ra4OtaContainer incomplete;
    expect(incomplete.begin({write_output, &output}) == Ra4OtaContainer::Error::None,
        "incomplete begin");
    expect(
        incomplete.feed(package.data(), package.size() - 1U) ==
            Ra4OtaContainer::Error::None,
        "partial payload accepted pending finish");
    expect(incomplete.finish() == Ra4OtaContainer::Error::Incomplete,
        "truncated package rejected");

    std::vector<uint8_t> excess = package;
    excess.push_back(0U);
    Ra4OtaContainer excess_parser;
    expect(excess_parser.begin({write_output, &output}) == Ra4OtaContainer::Error::None,
        "excess begin");
    expect(
        excess_parser.feed(excess.data(), excess.size()) ==
            Ra4OtaContainer::Error::None,
        "payload excess remains streamable");
    expect(excess_parser.finish() == Ra4OtaContainer::Error::PayloadExcess,
        "payload excess rejected");

    Output sink_failure;
    sink_failure.write_limit = 0U;
    Ra4OtaContainer sink;
    expect(sink.begin({write_output, &sink_failure}) == Ra4OtaContainer::Error::None,
        "sink begin");
    expect(
        sink.feed(package.data(), package.size()) ==
            Ra4OtaContainer::Error::OutputSink,
        "sink failure propagated");
}

void run_repository_fixture_test(
    const char *path,
    uint32_t expected_magic,
    size_t chunk_size)
{
    const std::vector<uint8_t> package = read_file(path);
    expect(package.size() > Ra4OtaContainer::kHeaderSize, "fixture has payload");
    expect(read_u32_le(package, 8U) == expected_magic, "fixture magic matches");

    Output output;
    Ra4OtaContainer parser(expected_magic);
    expect(parser.begin({write_output, &output}) == Ra4OtaContainer::Error::None,
        "fixture begin");
    feed_chunked(parser, package, chunk_size);
    expect(parser.finish() == Ra4OtaContainer::Error::None, "fixture finish");
    expect(parser.decoded_size() != 0U, "fixture decoded count");
    expect(output.bytes.size() == parser.decoded_size(), "fixture sink count");
}

} // namespace

int main()
{
    run_literal_and_reference_tests();
    run_error_tests();
    run_repository_fixture_test(
        "libraries/Arduino_ESP32_OTA/examples/LOLIN_32_Blink/LOLIN_32_Blink.ino.ota",
        0x45535033U,
        37U);
    run_repository_fixture_test(
        "libraries/Arduino_ESP32_OTA/examples/NANO_ESP32_Blink/NANO_ESP32_Blink.ino.ota",
        0x23410070U,
        251U);
    return 0;
}
