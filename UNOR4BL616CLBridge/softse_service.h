#ifndef UNOR4_BL616CL_SOFTSE_SERVICE_H
#define UNOR4_BL616CL_SOFTSE_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"

namespace bridge {

class SoftSeService {
public:
    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);

    bool copy_private_key(
        const char *slot,
        uint8_t *destination,
        size_t capacity,
        size_t *size) const;

private:
    enum class PendingOperation : uint8_t {
        None,
        SharedBufferSet,
        SlotWrite,
    };

    static constexpr size_t kNameCapacity = 16U;
    static constexpr size_t kStorageKeyCapacity = 64U;
    static constexpr size_t kMetadataSize = 4U;
    static constexpr size_t kSha256Size = 32U;
    static constexpr size_t kP256CoordinateSize = 32U;
    static constexpr size_t kP256PublicKeySize = kP256CoordinateSize * 2U;
    static constexpr size_t kP256SignatureSize = kP256CoordinateSize * 2U;
    static constexpr size_t kP256PrivateKeyDerCapacity = 160U;

    static at::CommandResult shared_buffer_payload(
        void *context,
        const uint8_t *data,
        size_t size);
    static at::CommandResult slot_write_payload(
        void *context,
        const uint8_t *data,
        size_t size);

    at::CommandResult handle_begin(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_end(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_serial(
        const at::Command &command,
        at::AtTransport &transport) const;
    at::CommandResult handle_random(
        const at::Command &command,
        at::AtTransport &transport) const;
    at::CommandResult handle_private_key(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_public_key(
        const at::Command &command,
        at::AtTransport &transport) const;
    at::CommandResult handle_shared_buffer_set(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_sha256(
        const at::Command &command,
        at::AtTransport &transport) const;
    at::CommandResult handle_sign(
        const at::Command &command,
        at::AtTransport &transport) const;
    at::CommandResult handle_verify(
        const at::Command &command,
        at::AtTransport &transport) const;
    at::CommandResult handle_slot_write(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_slot_read(
        const at::Command &command,
        at::AtTransport &transport) const;

    bool parse_uint(
        const char *text,
        unsigned long maximum,
        unsigned long &value) const;
    bool valid_name(const char *name) const;
    bool session_open() const;
    bool storage_key(
        const char *namespace_name,
        const char *partition,
        const char *kind,
        const char *slot,
        char destination[kStorageKeyCapacity]) const;
    bool session_storage_key(
        const char *kind,
        const char *slot,
        char destination[kStorageKeyCapacity]) const;
    bool load_slot_size(const char *slot, size_t &size) const;
    bool load_slot(
        const char *slot,
        uint8_t *destination,
        size_t capacity,
        size_t *size) const;
    bool store_slot(const char *slot, const uint8_t *data, size_t size);
    bool replace_shared_buffer(const uint8_t *data, size_t size);
    bool generate_p256_private_key(
        uint8_t destination[kP256PrivateKeyDerCapacity],
        size_t &size,
        uint8_t public_key[kP256PublicKeySize]) const;
    bool export_p256_public_key(
        const uint8_t *der,
        size_t der_size,
        uint8_t public_key[kP256PublicKeySize]) const;
    bool sign_p256(
        const uint8_t *der,
        size_t der_size,
        const uint8_t digest[kSha256Size],
        uint8_t signature[kP256SignatureSize]) const;
    int verify_p256(
        const uint8_t digest[kSha256Size],
        const uint8_t signature[kP256SignatureSize],
        const uint8_t public_key[kP256PublicKeySize]) const;
    void write_prefix(
        at::AtTransport &transport,
        const at::Command &command) const;
    void write_unsigned(at::AtTransport &transport, size_t value) const;

    PendingOperation pending_operation_ = PendingOperation::None;
    at::AtTransport *pending_transport_ = nullptr;
    char pending_slot_[kNameCapacity] = {};

    bool started_ = false;
    bool read_only_ = false;
    char namespace_[kNameCapacity] = {};
    char partition_[kNameCapacity] = {};

    uint8_t *shared_buffer_ = nullptr;
    size_t shared_buffer_size_ = 0U;
};

} // namespace bridge

#endif
