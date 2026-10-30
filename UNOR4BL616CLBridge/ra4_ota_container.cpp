#include "ra4_ota_container.h"

#include <string.h>

namespace bridge {

namespace {

constexpr uint32_t kCrcPolynomial = 0xEDB88320U;

uint32_t read_u32_le(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8U) |
        (static_cast<uint32_t>(data[2]) << 16U) |
        (static_cast<uint32_t>(data[3]) << 24U);
}

} // namespace

Ra4OtaContainer::Ra4OtaContainer(
    uint32_t expected_magic,
    size_t maximum_decoded_length)
    : expected_magic_(expected_magic),
      maximum_decoded_length_(maximum_decoded_length)
{
}

Ra4OtaContainer::Error Ra4OtaContainer::begin(Sink sink)
{
    if (sink.write == nullptr) {
        return fail(Error::InvalidArgument);
    }

    sink_ = sink;
    memset(&header_, 0, sizeof(header_));
    memset(header_bytes_, 0, sizeof(header_bytes_));
    memset(history_, ' ', sizeof(history_));
    header_size_ = 0U;
    input_size_ = 0U;
    decoded_size_ = 0U;
    calculated_crc_ = 0xFFFFFFFFU;
    history_write_ = sizeof(history_) - ((1U << 4U) + 1U);
    bits_ = 0U;
    bit_count_ = 0U;
    reference_offset_ = 0U;
    decode_state_ = DecodeState::Flag;
    output_size_ = 0U;
    started_ = true;
    header_ready_ = false;
    complete_ = false;
    error_ = Error::None;
    return error_;
}

Ra4OtaContainer::Error Ra4OtaContainer::feed(
    const uint8_t *data,
    size_t size)
{
    if (!started_) {
        return fail(Error::NotStarted);
    }
    if ((data == nullptr && size != 0U) || error_ != Error::None) {
        return data == nullptr && size != 0U
            ? fail(Error::InvalidArgument)
            : error_;
    }

    for (size_t index = 0U; index < size; ++index) {
        ++input_size_;
        if (!header_ready_) {
            if (!consume_header_byte(data[index])) {
                return error_;
            }
        } else if (!consume_payload_byte(data[index])) {
            return error_;
        }
    }

    return flush_output() ? Error::None : error_;
}

Ra4OtaContainer::Error Ra4OtaContainer::finish()
{
    if (!started_) {
        return fail(Error::NotStarted);
    }
    if (error_ != Error::None) {
        return error_;
    }
    if (!header_ready_) {
        return fail(Error::Incomplete);
    }
    const uint64_t expected_input_size =
        static_cast<uint64_t>(header_.container_length) + 8U;
    if (input_size_ < expected_input_size) {
        return fail(Error::Incomplete);
    }
    if (input_size_ > expected_input_size) {
        return fail(Error::PayloadExcess);
    }
    if (!flush_output()) {
        return error_;
    }
    if ((calculated_crc_ ^ 0xFFFFFFFFU) != header_.crc32) {
        return fail(Error::CrcMismatch);
    }
    complete_ = true;
    return Error::None;
}

bool Ra4OtaContainer::started() const
{
    return started_;
}

bool Ra4OtaContainer::header_ready() const
{
    return header_ready_;
}

bool Ra4OtaContainer::complete() const
{
    return complete_;
}

Ra4OtaContainer::Error Ra4OtaContainer::error() const
{
    return error_;
}

const Ra4OtaHeader &Ra4OtaContainer::header() const
{
    return header_;
}

size_t Ra4OtaContainer::input_size() const
{
    return input_size_;
}

size_t Ra4OtaContainer::decoded_size() const
{
    return decoded_size_;
}

uint32_t Ra4OtaContainer::crc32_update(
    uint32_t crc,
    const uint8_t *data,
    size_t size)
{
    if (data == nullptr) {
        return crc;
    }

    while (size-- != 0U) {
        crc ^= *data++;
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                ? (crc >> 1U) ^ kCrcPolynomial
                : crc >> 1U;
        }
    }
    return crc;
}

bool Ra4OtaContainer::consume_header_byte(uint8_t byte)
{
    header_bytes_[header_size_++] = byte;
    if (header_size_ != sizeof(header_bytes_)) {
        return true;
    }
    return initialize_header();
}

bool Ra4OtaContainer::initialize_header()
{
    header_.container_length = read_u32_le(&header_bytes_[0]);
    header_.crc32 = read_u32_le(&header_bytes_[4]);
    header_.magic = read_u32_le(&header_bytes_[8]);
    memcpy(header_.version, &header_bytes_[12], sizeof(header_.version));

    if (header_.magic != expected_magic_) {
        fail(Error::HeaderMagic);
        return false;
    }
    if (header_.container_length < 12U) {
        fail(Error::HeaderLength);
        return false;
    }

    calculated_crc_ = crc32_update(
        calculated_crc_,
        &header_bytes_[8],
        sizeof(header_bytes_) - 8U);
    header_ready_ = true;
    return true;
}

bool Ra4OtaContainer::consume_payload_byte(uint8_t byte)
{
    calculated_crc_ = crc32_update(calculated_crc_, &byte, 1U);
    bits_ = (bits_ << 8U) | byte;
    bit_count_ = static_cast<uint8_t>(bit_count_ + 8U);
    return decode_available_bits();
}

bool Ra4OtaContainer::decode_available_bits()
{
    uint32_t value = 0U;
    while (true) {
        switch (decode_state_) {
        case DecodeState::Flag:
            if (!take_bits(1U, value)) {
                return true;
            }
            decode_state_ = value != 0U
                ? DecodeState::Literal
                : DecodeState::ReferenceOffset;
            break;
        case DecodeState::Literal:
            if (!take_bits(8U, value)) {
                return true;
            }
            if (!emit(static_cast<uint8_t>(value))) {
                return false;
            }
            decode_state_ = DecodeState::Flag;
            break;
        case DecodeState::ReferenceOffset:
            if (!take_bits(11U, value)) {
                return true;
            }
            reference_offset_ = static_cast<uint16_t>(value);
            decode_state_ = DecodeState::ReferenceLength;
            break;
        case DecodeState::ReferenceLength:
            if (!take_bits(4U, value)) {
                return true;
            }
            for (uint8_t count = 0U; count <= value + 1U; ++count) {
                if (!emit(history_[(reference_offset_ + count) & (sizeof(history_) - 1U)])) {
                    return false;
                }
            }
            decode_state_ = DecodeState::Flag;
            break;
        }
    }
    return true;
}

bool Ra4OtaContainer::take_bits(uint8_t count, uint32_t &value)
{
    if (bit_count_ < count) {
        return false;
    }
    const uint8_t remaining = static_cast<uint8_t>(bit_count_ - count);
    value = static_cast<uint32_t>((bits_ >> remaining) & ((1ULL << count) - 1ULL));
    bits_ &= remaining == 0U ? 0U : ((1ULL << remaining) - 1ULL);
    bit_count_ = remaining;
    return true;
}

bool Ra4OtaContainer::emit(uint8_t byte)
{
    if (decoded_size_ >= maximum_decoded_length_) {
        fail(Error::OutputOverflow);
        return false;
    }
    history_[history_write_++] = byte;
    history_write_ &= sizeof(history_) - 1U;
    output_[output_size_++] = byte;
    ++decoded_size_;
    return output_size_ != sizeof(output_) || flush_output();
}

bool Ra4OtaContainer::flush_output()
{
    if (output_size_ == 0U) {
        return true;
    }
    if (sink_.write == nullptr ||
        !sink_.write(sink_.context, output_, output_size_)) {
        fail(Error::OutputSink);
        return false;
    }
    output_size_ = 0U;
    return true;
}

Ra4OtaContainer::Error Ra4OtaContainer::fail(Error error)
{
    error_ = error;
    return error_;
}

} // namespace bridge
