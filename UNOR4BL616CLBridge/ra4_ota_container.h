#ifndef UNOR4_BL616CL_RA4_OTA_CONTAINER_H
#define UNOR4_BL616CL_RA4_OTA_CONTAINER_H

#include <stddef.h>
#include <stdint.h>

namespace bridge {

/*
 * This is the Arduino UNO R4 package format consumed by the ESP32-S3 bridge:
 * a 20-byte little-endian header followed by an LZSS stream. It deliberately
 * has no ESP32, LittleFS, network, or SAM-BA dependency.
 */
struct Ra4OtaHeader {
    uint32_t container_length;
    uint32_t crc32;
    uint32_t magic;
    uint8_t version[8];
};

static_assert(sizeof(Ra4OtaHeader) == 20U, "RA4 OTA header must be 20 bytes");

class Ra4OtaContainer {
public:
    static constexpr uint32_t kUnoR4Magic = 0x23411002U;
    static constexpr size_t kHeaderSize = sizeof(Ra4OtaHeader);

    enum class Error : int {
        None = 0,
        NotStarted,
        InvalidArgument,
        HeaderMagic,
        HeaderLength,
        OutputOverflow,
        OutputSink,
        PayloadExcess,
        Incomplete,
        CrcMismatch,
    };

    struct Sink {
        bool (*write)(void *context, const uint8_t *data, size_t size);
        void *context;
    };

    explicit Ra4OtaContainer(
        uint32_t expected_magic = kUnoR4Magic,
        size_t maximum_decoded_length = static_cast<size_t>(-1));

    Error begin(Sink sink);
    Error feed(const uint8_t *data, size_t size);
    Error finish();

    bool started() const;
    bool header_ready() const;
    bool complete() const;
    Error error() const;
    const Ra4OtaHeader &header() const;
    size_t input_size() const;
    size_t decoded_size() const;

    static uint32_t crc32_update(
        uint32_t crc,
        const uint8_t *data,
        size_t size);

private:
    enum class DecodeState : uint8_t {
        Flag,
        Literal,
        ReferenceOffset,
        ReferenceLength,
    };

    bool consume_header_byte(uint8_t byte);
    bool initialize_header();
    bool consume_payload_byte(uint8_t byte);
    bool decode_available_bits();
    bool take_bits(uint8_t count, uint32_t &value);
    bool emit(uint8_t byte);
    bool flush_output();
    Error fail(Error error);

    uint32_t expected_magic_;
    size_t maximum_decoded_length_;
    Sink sink_ = {};
    Ra4OtaHeader header_ = {};
    uint8_t header_bytes_[kHeaderSize] = {};
    size_t header_size_ = 0U;
    size_t input_size_ = 0U;
    size_t decoded_size_ = 0U;
    uint32_t calculated_crc_ = 0xFFFFFFFFU;
    uint8_t history_[1U << 11U] = {};
    size_t history_write_ = (1U << 11U) - ((1U << 4U) + 1U);
    uint64_t bits_ = 0U;
    uint8_t bit_count_ = 0U;
    uint16_t reference_offset_ = 0U;
    DecodeState decode_state_ = DecodeState::Flag;
    uint8_t output_[64] = {};
    size_t output_size_ = 0U;
    bool started_ = false;
    bool header_ready_ = false;
    bool complete_ = false;
    Error error_ = Error::NotStarted;
};

} // namespace bridge

#endif
