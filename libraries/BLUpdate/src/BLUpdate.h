#pragma once

#include <stddef.h>
#include <stdint.h>

struct ota_handle;

/* A single sequential update session. The caller owns transport and timeouts. */
class BLUpdateClass {
public:
    enum class State { Idle, Receiving, Verified, Committed, Failed };
    enum class Error { None, State, Header, Layout, Size, Offset, Image, Backend, Hash };

    /** @brief Start an update using the 512-byte header from a .bin.blfota file. */
    bool begin(const uint8_t *header, size_t size);
    /** @brief Write sequential raw image bytes; the first call must contain 256 bytes. */
    bool write(uint32_t offset, const uint8_t *data, size_t size);
    /** @brief Verify the entire staged image against its SHA-256 without selecting it. */
    bool finish();
    /** @brief Select a verified image for the next boot; the caller controls reboot. */
    bool commit();
    /** @brief Cancel an uncommitted session and release its resources. */
    void abort();
    /** @brief Return the current session state. */
    State state() const { return state_; }
    /** @brief Return the last session error. */
    Error error() const { return error_; }
    /** @brief Return the raw application image size. */
    uint32_t size() const { return size_; }
    /** @brief Return the number of accepted raw application bytes. */
    uint32_t written() const { return written_; }

private:
    bool fail(Error error);
    ota_handle *handle_ = nullptr;
    State state_ = State::Idle;
    Error error_ = Error::None;
    uint32_t size_ = 0;
    uint32_t written_ = 0;
    uint8_t header_[512] = {};
};

extern BLUpdateClass BLUpdate;
