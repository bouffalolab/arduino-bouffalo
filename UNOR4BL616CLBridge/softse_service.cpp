#include "softse_service.h"
#include "persistent_record.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "bflb_sec_trng.h"
#include "bflb_sflash.h"
#include "easyflash.h"
#include "mbedtls/asn1.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
}

namespace bridge {

namespace {

constexpr char kSoftSeBegin[] = "+SOFTSEBEGIN";
constexpr char kSoftSeEnd[] = "+SOFTSEEND";
constexpr char kSoftSeSerial[] = "+SOFTSE_SERIAL";
constexpr char kSoftSeRandom[] = "+SOFTSE_RND";
constexpr char kSoftSePrivateKey[] = "+SOFTSE_PRI_KEY";
constexpr char kSoftSePublicKey[] = "+SOFTSE_PUB_KEY";
constexpr char kSoftSeSharedBuffer[] = "+SOFTSE_S_V_BUF_SET";
constexpr char kSoftSeSha256[] = "+SOFTSE_SHA256_GET";
constexpr char kSoftSeSign[] = "+SOFTSE_SIGN_GET";
constexpr char kSoftSeVerify[] = "+SOFTSE_VERIFY_GET";
constexpr char kSoftSeWriteSlot[] = "+SOFTSE_WRITE_SLOT";
constexpr char kSoftSeReadSlot[] = "+SOFTSE_READ_SLOT";
constexpr char kDefaultPartition[] = "nvs";
constexpr char kStoragePrefix[] = "se";
constexpr size_t kFlashUniqueIdSize = 8U;
constexpr size_t kDerSignatureCapacity = 80U;

bool is_name(const at::Command &command, const char *name)
{
    return strcmp(command.name, name) == 0;
}

int trng_random(void *, unsigned char *output, size_t size)
{
    return output != nullptr && size <= UINT32_MAX &&
            bflb_trng_readlen(output, static_cast<uint32_t>(size)) == 0
        ? 0
        : MBEDTLS_ERR_PK_BAD_INPUT_DATA;
}

void write_u32_le(uint8_t destination[4], uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8U);
    destination[2] = static_cast<uint8_t>(value >> 16U);
    destination[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t read_u32_le(const uint8_t source[4])
{
    return static_cast<uint32_t>(source[0]) |
        (static_cast<uint32_t>(source[1]) << 8U) |
        (static_cast<uint32_t>(source[2]) << 16U) |
        (static_cast<uint32_t>(source[3]) << 24U);
}

int extract_signature_integer(
    unsigned char **source,
    const unsigned char *end,
    unsigned char *destination,
    size_t destination_size)
{
    size_t size = 0U;
    int result = mbedtls_asn1_get_tag(
        source,
        end,
        &size,
        MBEDTLS_ASN1_INTEGER);
    if (result != 0) {
        return result;
    }
    while (size != 0U && **source == 0U) {
        ++(*source);
        --size;
    }
    if (size == 0U || size > destination_size ||
        static_cast<size_t>(end - *source) < size) {
        return MBEDTLS_ERR_ASN1_LENGTH_MISMATCH;
    }
    memset(destination, 0, destination_size - size);
    memcpy(destination + destination_size - size, *source, size);
    *source += size;
    return 0;
}

int der_signature_to_raw(
    const uint8_t *der,
    size_t der_size,
    uint8_t raw[64])
{
    if (der == nullptr || raw == nullptr) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }
    unsigned char *source = const_cast<unsigned char *>(der);
    const unsigned char *end = der + der_size;
    size_t sequence_size = 0U;
    int result = mbedtls_asn1_get_tag(
        &source,
        end,
        &sequence_size,
        MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (result != 0 || sequence_size != static_cast<size_t>(end - source)) {
        return result != 0 ? result : MBEDTLS_ERR_ASN1_LENGTH_MISMATCH;
    }
    if ((result = extract_signature_integer(&source, end, raw, 32U)) != 0) {
        return result;
    }
    if ((result = extract_signature_integer(&source, end, raw + 32U, 32U)) != 0) {
        return result;
    }
    return source == end ? 0 : MBEDTLS_ERR_ASN1_LENGTH_MISMATCH;
}

int raw_signature_to_der(
    const uint8_t raw[64],
    uint8_t der[kDerSignatureCapacity],
    size_t *der_size)
{
    if (raw == nullptr || der == nullptr || der_size == nullptr) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int result = mbedtls_mpi_read_binary(&r, raw, 32U);
    if (result == 0) {
        result = mbedtls_mpi_read_binary(&s, raw + 32U, 32U);
    }
    unsigned char *cursor = der + kDerSignatureCapacity;
    size_t size = 0U;
    if (result == 0) {
        result = mbedtls_asn1_write_mpi(&cursor, der, &s);
        if (result >= 0) {
            size += static_cast<size_t>(result);
            result = mbedtls_asn1_write_mpi(&cursor, der, &r);
        }
        if (result >= 0) {
            size += static_cast<size_t>(result);
            result = mbedtls_asn1_write_len(&cursor, der, size);
        }
        if (result >= 0) {
            size += static_cast<size_t>(result);
            result = mbedtls_asn1_write_tag(
                &cursor,
                der,
                MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
        }
        if (result >= 0) {
            size += static_cast<size_t>(result);
            memmove(der, cursor, size);
            *der_size = size;
            result = 0;
        }
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    return result;
}

} // namespace

bool SoftSeService::handles_command(const at::Command &command) const
{
    return is_name(command, kSoftSeBegin) ||
        is_name(command, kSoftSeEnd) ||
        is_name(command, kSoftSeSerial) ||
        is_name(command, kSoftSeRandom) ||
        is_name(command, kSoftSePrivateKey) ||
        is_name(command, kSoftSePublicKey) ||
        is_name(command, kSoftSeSharedBuffer) ||
        is_name(command, kSoftSeSha256) ||
        is_name(command, kSoftSeSign) ||
        is_name(command, kSoftSeVerify) ||
        is_name(command, kSoftSeWriteSlot) ||
        is_name(command, kSoftSeReadSlot);
}

at::CommandResult SoftSeService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kSoftSeBegin)) {
        return handle_begin(command, transport);
    }
    if (is_name(command, kSoftSeEnd)) {
        return handle_end(command, transport);
    }
    if (is_name(command, kSoftSeSerial)) {
        return handle_serial(command, transport);
    }
    if (is_name(command, kSoftSeRandom)) {
        return handle_random(command, transport);
    }
    if (is_name(command, kSoftSePrivateKey)) {
        return handle_private_key(command, transport);
    }
    if (is_name(command, kSoftSePublicKey)) {
        return handle_public_key(command, transport);
    }
    if (is_name(command, kSoftSeSharedBuffer)) {
        return handle_shared_buffer_set(command, transport);
    }
    if (is_name(command, kSoftSeSha256)) {
        return handle_sha256(command, transport);
    }
    if (is_name(command, kSoftSeSign)) {
        return handle_sign(command, transport);
    }
    if (is_name(command, kSoftSeVerify)) {
        return handle_verify(command, transport);
    }
    if (is_name(command, kSoftSeWriteSlot)) {
        return handle_slot_write(command, transport);
    }
    return handle_slot_read(command, transport);
}

at::CommandResult SoftSeService::handle_begin(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 3U ||
        !valid_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    unsigned long read_only = 0U;
    if (!parse_uint(command.arguments[1], 1U, read_only)) {
        return at::CommandResult::Error;
    }
    const char *partition = command.arguments[2][0] == '\0'
        ? kDefaultPartition
        : command.arguments[2];
    if (!valid_name(partition)) {
        return at::CommandResult::Error;
    }
    strncpy(namespace_, command.arguments[0], sizeof(namespace_) - 1U);
    namespace_[sizeof(namespace_) - 1U] = '\0';
    strncpy(partition_, partition, sizeof(partition_) - 1U);
    partition_[sizeof(partition_) - 1U] = '\0';
    read_only_ = read_only != 0U;
    started_ = true;
    (void)transport.write_response(command, "1");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_end(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run) {
        return at::CommandResult::Error;
    }
    started_ = false;
    read_only_ = false;
    namespace_[0] = '\0';
    partition_[0] = '\0';
    (void)transport.write_response(command, "0");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_serial(
    const at::Command &command,
    at::AtTransport &transport) const
{
    if (command.mode != at::CommandMode::Run) {
        return at::CommandResult::Error;
    }
    uint8_t serial[kFlashUniqueIdSize] = {};
    bflb_sflash_get_uniqueid(serial, sizeof(serial));
    write_prefix(transport, command);
    write_unsigned(transport, sizeof(serial));
    (void)transport.write_cstr("|");
    (void)transport.write_data(serial, sizeof(serial));
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_random(
    const at::Command &command,
    at::AtTransport &transport) const
{
    if (command.mode != at::CommandMode::Write || command.argument_count != 1U) {
        return at::CommandResult::Error;
    }
    unsigned long size = 0U;
    if (!parse_uint(command.arguments[0], at::kTransportRawPayloadCapacity, size)) {
        return at::CommandResult::Error;
    }
    const size_t allocation_size = size == 0U ? 1U : size;
    uint8_t *random_bytes = static_cast<uint8_t *>(malloc(allocation_size));
    if (random_bytes == nullptr ||
        (size != 0U &&
         bflb_trng_readlen(random_bytes, static_cast<uint32_t>(size)) != 0)) {
        free(random_bytes);
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_unsigned(transport, size);
    (void)transport.write_cstr("|");
    if (size != 0U) {
        (void)transport.write_data(random_bytes, size);
    }
    (void)transport.write_cstr("\r\n");
    free(random_bytes);
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_private_key(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !session_open() ||
        read_only_ ||
        !valid_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    uint8_t der[kP256PrivateKeyDerCapacity] = {};
    uint8_t public_key[kP256PublicKeySize] = {};
    size_t der_size = 0U;
    if (!generate_p256_private_key(der, der_size, public_key) ||
        !store_slot(command.arguments[0], der, der_size)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_unsigned(transport, sizeof(public_key));
    (void)transport.write_cstr("|");
    (void)transport.write_data(public_key, sizeof(public_key));
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_public_key(
    const at::Command &command,
    at::AtTransport &transport) const
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !session_open() ||
        !valid_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    uint8_t der[kP256PrivateKeyDerCapacity] = {};
    uint8_t public_key[kP256PublicKeySize] = {};
    size_t der_size = 0U;
    if (!load_slot(command.arguments[0], der, sizeof(der), &der_size) ||
        !export_p256_public_key(der, der_size, public_key)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_unsigned(transport, sizeof(public_key));
    (void)transport.write_cstr("|");
    (void)transport.write_data(public_key, sizeof(public_key));
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_shared_buffer_set(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write || command.argument_count != 1U) {
        return at::CommandResult::Error;
    }
    unsigned long size = 0U;
    if (!parse_uint(command.arguments[0], at::kTransportRawPayloadCapacity, size)) {
        return at::CommandResult::Error;
    }
    pending_operation_ = PendingOperation::SharedBufferSet;
    pending_transport_ = &transport;
    if (size == 0U) {
        if (!replace_shared_buffer(nullptr, 0U)) {
            pending_operation_ = PendingOperation::None;
            pending_transport_ = nullptr;
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_unsigned(transport, 0U);
        (void)transport.write_cstr("\r\n");
        pending_operation_ = PendingOperation::None;
        pending_transport_ = nullptr;
        return at::CommandResult::Ok;
    }
    if (!transport.request_raw_payload(
            size,
            &SoftSeService::shared_buffer_payload,
            this)) {
        pending_operation_ = PendingOperation::None;
        pending_transport_ = nullptr;
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult SoftSeService::shared_buffer_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    SoftSeService *service = static_cast<SoftSeService *>(context);
    at::AtTransport *transport = service->pending_transport_;
    const bool stored = service->pending_operation_ == PendingOperation::SharedBufferSet &&
        service->replace_shared_buffer(data, size);
    service->pending_operation_ = PendingOperation::None;
    service->pending_transport_ = nullptr;
    if (!stored || transport == nullptr) {
        return at::CommandResult::Error;
    }
    (void)transport->write_cstr(kSoftSeSharedBuffer);
    (void)transport->write_cstr(": ");
    service->write_unsigned(*transport, size);
    (void)transport->write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_sha256(
    const at::Command &command,
    at::AtTransport &transport) const
{
    if (command.mode != at::CommandMode::Run ||
        (shared_buffer_size_ != 0U && shared_buffer_ == nullptr)) {
        return at::CommandResult::Error;
    }
    uint8_t digest[kSha256Size] = {};
    if (mbedtls_sha256(
            shared_buffer_size_ == 0U
                ? reinterpret_cast<const uint8_t *>("")
                : shared_buffer_,
            shared_buffer_size_,
            digest,
            0) != 0) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_unsigned(transport, sizeof(digest));
    (void)transport.write_cstr("|");
    (void)transport.write_data(digest, sizeof(digest));
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_sign(
    const at::Command &command,
    at::AtTransport &transport) const
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !session_open() ||
        !valid_name(command.arguments[0]) ||
        shared_buffer_size_ != kSha256Size ||
        shared_buffer_ == nullptr) {
        return at::CommandResult::Error;
    }
    uint8_t der[kP256PrivateKeyDerCapacity] = {};
    uint8_t signature[kP256SignatureSize] = {};
    size_t der_size = 0U;
    if (!load_slot(command.arguments[0], der, sizeof(der), &der_size) ||
        !sign_p256(der, der_size, shared_buffer_, signature)) {
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_unsigned(transport, sizeof(signature));
    (void)transport.write_cstr("|");
    (void)transport.write_data(signature, sizeof(signature));
    (void)transport.write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_verify(
    const at::Command &command,
    at::AtTransport &transport) const
{
    if (command.mode != at::CommandMode::Run ||
        shared_buffer_size_ !=
            kSha256Size + kP256SignatureSize + kP256PublicKeySize ||
        shared_buffer_ == nullptr) {
        return at::CommandResult::Error;
    }
    const int result = verify_p256(
        shared_buffer_,
        shared_buffer_ + kSha256Size,
        shared_buffer_ + kSha256Size + kP256SignatureSize);
    write_prefix(transport, command);
    char text[16] = {};
    const int size = snprintf(text, sizeof(text), "%d", result);
    if (size <= 0 || !transport.write_cstr(text) ||
        !transport.write_cstr("\r\n")) {
        return at::CommandResult::Error;
    }
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_slot_write(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 2U ||
        !session_open() ||
        !valid_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    unsigned long size = 0U;
    if (!parse_uint(command.arguments[1], at::kTransportRawPayloadCapacity, size)) {
        return at::CommandResult::Error;
    }
    strncpy(pending_slot_, command.arguments[0], sizeof(pending_slot_) - 1U);
    pending_slot_[sizeof(pending_slot_) - 1U] = '\0';
    pending_operation_ = PendingOperation::SlotWrite;
    pending_transport_ = &transport;
    if (size == 0U) {
        const bool stored = !read_only_ && store_slot(pending_slot_, nullptr, 0U);
        pending_operation_ = PendingOperation::None;
        pending_transport_ = nullptr;
        pending_slot_[0] = '\0';
        if (!stored) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_unsigned(transport, 0U);
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }
    if (!transport.request_raw_payload(
            size,
            &SoftSeService::slot_write_payload,
            this)) {
        pending_operation_ = PendingOperation::None;
        pending_transport_ = nullptr;
        pending_slot_[0] = '\0';
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult SoftSeService::slot_write_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    SoftSeService *service = static_cast<SoftSeService *>(context);
    at::AtTransport *transport = service->pending_transport_;
    const bool stored = service->pending_operation_ == PendingOperation::SlotWrite &&
        !service->read_only_ &&
        service->store_slot(service->pending_slot_, data, size);
    service->pending_operation_ = PendingOperation::None;
    service->pending_transport_ = nullptr;
    service->pending_slot_[0] = '\0';
    if (transport == nullptr || !stored) {
        return at::CommandResult::Error;
    }
    (void)transport->write_cstr(kSoftSeWriteSlot);
    (void)transport->write_cstr(": ");
    service->write_unsigned(*transport, stored ? size : 0U);
    (void)transport->write_cstr("\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult SoftSeService::handle_slot_read(
    const at::Command &command,
    at::AtTransport &transport) const
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 1U ||
        !session_open() ||
        !valid_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    size_t size = 0U;
    if (!load_slot_size(command.arguments[0], size)) {
        write_prefix(transport, command);
        (void)transport.write_cstr("0|\r\n");
        return at::CommandResult::Ok;
    }
    const size_t allocation_size = size == 0U ? 1U : size;
    uint8_t *value = static_cast<uint8_t *>(malloc(allocation_size));
    if (value == nullptr) {
        return at::CommandResult::Error;
    }
    size_t loaded_size = 0U;
    if (!load_slot(command.arguments[0], value, size, &loaded_size) ||
        loaded_size != size) {
        free(value);
        return at::CommandResult::Error;
    }
    write_prefix(transport, command);
    write_unsigned(transport, loaded_size);
    (void)transport.write_cstr("|");
    if (loaded_size != 0U) {
        (void)transport.write_data(value, loaded_size);
    }
    (void)transport.write_cstr("\r\n");
    free(value);
    return at::CommandResult::Ok;
}

bool SoftSeService::copy_private_key(
    const char *slot,
    uint8_t *destination,
    size_t capacity,
    size_t *size) const
{
    if (!session_open() || !valid_name(slot) ||
        destination == nullptr || size == nullptr) {
        return false;
    }
    char metadata_key[kStorageKeyCapacity] = {};
    char value_key[kStorageKeyCapacity] = {};
    if (!storage_key(
            namespace_,
            partition_,
            "m",
            slot,
            metadata_key) ||
        !storage_key(
            namespace_,
            partition_,
            "v",
            slot,
            value_key)) {
        return false;
    }
    uint8_t metadata[kMetadataSize] = {};
    size_t metadata_size = 0U;
    if (ef_get_env_blob(
            metadata_key,
            metadata,
            sizeof(metadata),
            &metadata_size) != sizeof(metadata) ||
        !persistent_record_size_matches(metadata_size, sizeof(metadata), read_u32_le(metadata))) {
        return false;
    }
    const size_t stored_size = read_u32_le(metadata);
    if (stored_size == 0U || stored_size > capacity) {
        return false;
    }
    if (!persistent_record_read(metadata_key, value_key, sizeof(metadata), stored_size,
                                destination, 0, stored_size)) {
        return false;
    }
    *size = stored_size;
    return true;
}

bool SoftSeService::parse_uint(
    const char *text,
    unsigned long maximum,
    unsigned long &value) const
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool SoftSeService::valid_name(const char *name) const
{
    if (name == nullptr || name[0] == '\0' || strlen(name) >= kNameCapacity) {
        return false;
    }
    for (const char *cursor = name; *cursor != '\0'; ++cursor) {
        const unsigned char value = static_cast<unsigned char>(*cursor);
        if (!(isalnum(value) || value == '_' || value == '-' || value == '.')) {
            return false;
        }
    }
    return true;
}

bool SoftSeService::session_open() const
{
    return started_;
}

bool SoftSeService::storage_key(
    const char *namespace_name,
    const char *partition,
    const char *kind,
    const char *slot,
    char destination[kStorageKeyCapacity]) const
{
    if (!valid_name(namespace_name) || !valid_name(partition) ||
        !valid_name(slot) || kind == nullptr ||
        (strcmp(kind, "m") != 0 && strcmp(kind, "v") != 0)) {
        return false;
    }
    const int size = snprintf(
        destination,
        kStorageKeyCapacity,
        "%s/%s/%s/%s/%s",
        kStoragePrefix,
        partition,
        namespace_name,
        kind,
        slot);
    return size > 0 && static_cast<size_t>(size) < kStorageKeyCapacity;
}

bool SoftSeService::session_storage_key(
    const char *kind,
    const char *slot,
    char destination[kStorageKeyCapacity]) const
{
    return session_open() &&
        storage_key(namespace_, partition_, kind, slot, destination);
}

bool SoftSeService::load_slot_size(const char *slot, size_t &size) const
{
    char metadata_key[kStorageKeyCapacity] = {};
    uint8_t metadata[kMetadataSize] = {};
    size_t metadata_size = 0U;
    if (!session_storage_key("m", slot, metadata_key) ||
        ef_get_env_blob(
            metadata_key,
            metadata,
            sizeof(metadata),
            &metadata_size) != sizeof(metadata) ||
        !persistent_record_size_matches(metadata_size, sizeof(metadata), read_u32_le(metadata))) {
        size = 0U;
        return false;
    }
    size = read_u32_le(metadata);
    return size <= at::kTransportRawPayloadCapacity;
}

bool SoftSeService::load_slot(
    const char *slot,
    uint8_t *destination,
    size_t capacity,
    size_t *size) const
{
    if (destination == nullptr || size == nullptr) {
        return false;
    }
    size_t stored_size = 0U;
    if (!load_slot_size(slot, stored_size) || stored_size > capacity) {
        return false;
    }
    if (stored_size == 0U) {
        *size = 0U;
        return true;
    }
    char value_key[kStorageKeyCapacity] = {};
    char metadata_key[kStorageKeyCapacity] = {};
    if (!session_storage_key("v", slot, value_key) ||
        !session_storage_key("m", slot, metadata_key) ||
        !persistent_record_read(metadata_key, value_key, kMetadataSize, stored_size,
                                destination, 0, stored_size)) {
        return false;
    }
    *size = stored_size;
    return true;
}

bool SoftSeService::store_slot(
    const char *slot,
    const uint8_t *data,
    size_t size)
{
    if (!session_open() || read_only_ || !valid_name(slot) ||
        size > at::kTransportRawPayloadCapacity ||
        (size != 0U && data == nullptr)) {
        return false;
    }
    char metadata_key[kStorageKeyCapacity] = {};
    char value_key[kStorageKeyCapacity] = {};
    if (!session_storage_key("m", slot, metadata_key) ||
        !session_storage_key("v", slot, value_key)) {
        return false;
    }
    uint8_t metadata[kMetadataSize] = {};
    write_u32_le(metadata, static_cast<uint32_t>(size));
    if (!persistent_record_write(metadata_key, metadata, sizeof(metadata), data, size)) {
        return false;
    }
    (void)ef_del_env(value_key);
    return true;
}

bool SoftSeService::replace_shared_buffer(const uint8_t *data, size_t size)
{
    if (size > at::kTransportRawPayloadCapacity ||
        (size != 0U && data == nullptr)) {
        return false;
    }
    const size_t allocation_size = size == 0U ? 1U : size;
    uint8_t *replacement = static_cast<uint8_t *>(malloc(allocation_size));
    if (replacement == nullptr) {
        return false;
    }
    if (size != 0U) {
        memcpy(replacement, data, size);
    }
    free(shared_buffer_);
    shared_buffer_ = replacement;
    shared_buffer_size_ = size;
    return true;
}

bool SoftSeService::generate_p256_private_key(
    uint8_t destination[kP256PrivateKeyDerCapacity],
    size_t &size,
    uint8_t public_key[kP256PublicKeySize]) const
{
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int result = mbedtls_pk_setup(
        &key,
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0) {
        result = mbedtls_ecp_gen_key(
            MBEDTLS_ECP_DP_SECP256R1,
            mbedtls_pk_ec(key),
            trng_random,
            nullptr);
    }
    if (result == 0) {
        const int der_size = mbedtls_pk_write_key_der(
            &key,
            destination,
            kP256PrivateKeyDerCapacity);
        if (der_size <= 0 || static_cast<size_t>(der_size) > kP256PrivateKeyDerCapacity) {
            result = der_size == 0 ? MBEDTLS_ERR_PK_BAD_INPUT_DATA : der_size;
        } else {
            memmove(
                destination,
                destination + kP256PrivateKeyDerCapacity - der_size,
                static_cast<size_t>(der_size));
            size = static_cast<size_t>(der_size);
        }
    }
    if (result == 0) {
        const mbedtls_ecp_keypair *ec = mbedtls_pk_ec(key);
        result = mbedtls_mpi_write_binary(
            &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
            public_key,
            kP256CoordinateSize);
        if (result == 0) {
            result = mbedtls_mpi_write_binary(
                &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y),
                public_key + kP256CoordinateSize,
                kP256CoordinateSize);
        }
    }
    mbedtls_pk_free(&key);
    return result == 0;
}

bool SoftSeService::export_p256_public_key(
    const uint8_t *der,
    size_t der_size,
    uint8_t public_key[kP256PublicKeySize]) const
{
    if (der == nullptr || der_size == 0U || public_key == nullptr) {
        return false;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int result = mbedtls_pk_parse_key(
        &key,
        der,
        der_size,
        nullptr,
        0U,
        trng_random,
        nullptr);
    if (result == 0 && !mbedtls_pk_can_do(&key, MBEDTLS_PK_ECKEY)) {
        result = MBEDTLS_ERR_PK_TYPE_MISMATCH;
    }
    if (result == 0) {
        const mbedtls_ecp_keypair *ec = mbedtls_pk_ec(key);
        result = mbedtls_mpi_write_binary(
            &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
            public_key,
            kP256CoordinateSize);
        if (result == 0) {
            result = mbedtls_mpi_write_binary(
                &ec->MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(Y),
                public_key + kP256CoordinateSize,
                kP256CoordinateSize);
        }
    }
    mbedtls_pk_free(&key);
    return result == 0;
}

bool SoftSeService::sign_p256(
    const uint8_t *der,
    size_t der_size,
    const uint8_t digest[kSha256Size],
    uint8_t signature[kP256SignatureSize]) const
{
    if (der == nullptr || der_size == 0U || digest == nullptr || signature == nullptr) {
        return false;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    uint8_t der_signature[kDerSignatureCapacity] = {};
    size_t der_signature_size = 0U;
    int result = mbedtls_pk_parse_key(
        &key,
        der,
        der_size,
        nullptr,
        0U,
        trng_random,
        nullptr);
    if (result == 0) {
        result = mbedtls_pk_sign(
            &key,
            MBEDTLS_MD_SHA256,
            digest,
            kSha256Size,
            der_signature,
            sizeof(der_signature),
            &der_signature_size,
            trng_random,
            nullptr);
    }
    if (result == 0) {
        result = der_signature_to_raw(
            der_signature,
            der_signature_size,
            signature);
    }
    mbedtls_pk_free(&key);
    return result == 0;
}

int SoftSeService::verify_p256(
    const uint8_t digest[kSha256Size],
    const uint8_t signature[kP256SignatureSize],
    const uint8_t public_key[kP256PublicKeySize]) const
{
    if (digest == nullptr || signature == nullptr || public_key == nullptr) {
        return MBEDTLS_ERR_PK_BAD_INPUT_DATA;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    uint8_t encoded_point[kP256PublicKeySize + 1U] = {};
    encoded_point[0] = 0x04U;
    memcpy(encoded_point + 1U, public_key, kP256PublicKeySize);
    uint8_t der_signature[kDerSignatureCapacity] = {};
    size_t der_signature_size = 0U;
    int result = mbedtls_pk_setup(
        &key,
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0) {
        mbedtls_ecp_keypair *ec = mbedtls_pk_ec(key);
        result = mbedtls_ecp_group_load(
            &ec->MBEDTLS_PRIVATE(grp),
            MBEDTLS_ECP_DP_SECP256R1);
        if (result == 0) {
            result = mbedtls_ecp_point_read_binary(
                &ec->MBEDTLS_PRIVATE(grp),
                &ec->MBEDTLS_PRIVATE(Q),
                encoded_point,
                sizeof(encoded_point));
        }
        if (result == 0) {
            result = mbedtls_ecp_check_pubkey(
                &ec->MBEDTLS_PRIVATE(grp),
                &ec->MBEDTLS_PRIVATE(Q));
        }
    }
    if (result == 0) {
        result = raw_signature_to_der(
            signature,
            der_signature,
            &der_signature_size);
    }
    if (result == 0) {
        result = mbedtls_pk_verify(
            &key,
            MBEDTLS_MD_SHA256,
            digest,
            kSha256Size,
            der_signature,
            der_signature_size);
    }
    mbedtls_pk_free(&key);
    return result;
}

void SoftSeService::write_prefix(
    at::AtTransport &transport,
    const at::Command &command) const
{
    (void)transport.write_cstr(command.name);
    (void)transport.write_cstr(": ");
}

void SoftSeService::write_unsigned(at::AtTransport &transport, size_t value) const
{
    char text[24] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%lu",
        static_cast<unsigned long>(value));
    if (size > 0) {
        (void)transport.write_cstr(text);
    }
}

} // namespace bridge
