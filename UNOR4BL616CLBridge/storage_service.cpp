#include "storage_service.h"
#include "persistent_record.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "bflb_flash.h"
#include "bflb_mtd.h"
#include "easyflash.h"
}

namespace bridge {

StorageService::StorageService(const char *partition)
    : partition_(partition), ota_image_source_(
          {
              ota_image_open,
              ota_image_read,
              ota_image_close,
              this,
          })
{
}

namespace {

constexpr char kMountFs[] = "+MOUNTFS";
constexpr char kFileSystem[] = "+FS";
constexpr char kPreferenceBegin[] = "+PREFBEGIN";
constexpr char kPreferenceEnd[] = "+PREFEND";
constexpr char kPreferenceClear[] = "+PREFCLEAR";
constexpr char kPreferenceRemove[] = "+PREFREMOVE";
constexpr char kPreferencePut[] = "+PREFPUT";
constexpr char kPreferenceType[] = "+PREFTYPE";
constexpr char kPreferenceGet[] = "+PREFGET";
constexpr char kPreferenceLength[] = "+PREFLEN";
constexpr char kPreferenceStat[] = "+PREFSTAT";
constexpr char kPreferenceDefaultPartition[] = "nvs";
constexpr char kPreferenceKeyPrefix[] = "p";
// A private single-image partition must not accumulate orphaned files when
// applications choose a different logical cache path after a bridge reset.
constexpr char kRa4OtaCacheFile[] = "/ra4-cache.bin";

enum FileOperation : unsigned long {
    FileDelete = 0U,
    FileWrite = 1U,
    FileRead = 2U,
    FileAppend = 3U,
};

enum PreferenceType : uint8_t {
    PreferenceI8 = 0U,
    PreferenceU8 = 1U,
    PreferenceI16 = 2U,
    PreferenceU16 = 3U,
    PreferenceI32 = 4U,
    PreferenceU32 = 5U,
    PreferenceI64 = 6U,
    PreferenceU64 = 7U,
    PreferenceString = 8U,
    PreferenceBlob = 9U,
    PreferenceInvalid = 10U,
};

bool is_name(const at::Command &command, const char *name)
{
    return strcmp(command.name, name) == 0;
}

size_t scalar_size(uint8_t type)
{
    switch (type) {
    case PreferenceI8:
    case PreferenceU8:
        return 1U;
    case PreferenceI16:
    case PreferenceU16:
        return 2U;
    case PreferenceI32:
    case PreferenceU32:
        return 4U;
    case PreferenceI64:
    case PreferenceU64:
        return 8U;
    default:
        return 0U;
    }
}

uint32_t read_u32_le(const uint8_t data[4])
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8U) |
        (static_cast<uint32_t>(data[2]) << 16U) |
        (static_cast<uint32_t>(data[3]) << 24U);
}

void write_u32_le(uint8_t data[4], uint32_t value)
{
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8U);
    data[2] = static_cast<uint8_t>(value >> 16U);
    data[3] = static_cast<uint8_t>(value >> 24U);
}

struct PreferenceCount {
    const char *prefix = nullptr;
    size_t prefix_size = 0U;
    size_t count = 0U;
};

EfErrCode count_preference_metadata(const char *key, void *context)
{
    PreferenceCount *count = static_cast<PreferenceCount *>(context);
    if (strncmp(key, count->prefix, count->prefix_size) == 0 &&
        key[count->prefix_size] != '\0') {
        ++count->count;
    }
    return EF_NO_ERR;
}

} // namespace

bool StorageService::handles_command(const at::Command &command) const
{
    return is_name(command, kMountFs) ||
        is_name(command, kFileSystem) ||
        is_name(command, kPreferenceBegin) ||
        is_name(command, kPreferenceEnd) ||
        is_name(command, kPreferenceClear) ||
        is_name(command, kPreferenceRemove) ||
        is_name(command, kPreferencePut) ||
        is_name(command, kPreferenceType) ||
        is_name(command, kPreferenceGet) ||
        is_name(command, kPreferenceLength) ||
        is_name(command, kPreferenceStat);
}

at::CommandResult StorageService::handle_command(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kMountFs)) {
        return handle_mount(command, transport);
    }
    if (is_name(command, kFileSystem)) {
        return handle_file(command, transport);
    }
    return handle_preference(command, transport);
}

Ra4OtaCacheBackend StorageService::ra4_ota_cache_backend()
{
    return {
        ota_cache_begin,
        ota_cache_write,
        ota_cache_commit,
        ota_cache_discard,
        ota_cache_maximum_output_size,
        this,
    };
}

bool StorageService::ready_ra4_ota_cache(const char *path) const
{
    return ota_image_source_.ready(path);
}

Ra4SambaImageSource StorageService::ra4_ota_image_source()
{
    return ota_image_source_.image_source();
}

at::CommandResult StorageService::handle_mount(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write || command.argument_count != 1U) {
        return at::CommandResult::Error;
    }
    unsigned long format_on_fail = 0U;
    if (!parse_uint(command.arguments[0], 1U, format_on_fail) ||
        !mount_file_system(format_on_fail != 0U)) {
        return at::CommandResult::Error;
    }
    (void)transport.write_response(command, "FS Mounted\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult StorageService::handle_file(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count < 3U ||
        !file_system_mounted_) {
        return at::CommandResult::Error;
    }
    unsigned long storage_type = 0U;
    unsigned long operation = 0U;
    if (!parse_uint(command.arguments[0], 0U, storage_type) ||
        !parse_uint(command.arguments[1], FileAppend, operation) ||
        !valid_path(command.arguments[2])) {
        return at::CommandResult::Error;
    }

    if (operation == FileDelete && command.argument_count == 3U) {
        return lfs_remove(&file_system_, command.arguments[2]) == LFS_ERR_OK
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }
    if (operation == FileWrite) {
        return handle_file_write(command, transport, false);
    }
    if (operation == FileAppend) {
        return handle_file_write(command, transport, true);
    }
    if (operation == FileRead) {
        return handle_file_read(command, transport);
    }
    return at::CommandResult::Error;
}

at::CommandResult StorageService::handle_file_write(
    const at::Command &command,
    at::AtTransport &transport,
    bool append)
{
    if (command.argument_count != 4U) {
        return at::CommandResult::Error;
    }
    unsigned long size = 0U;
    if (!parse_uint(command.arguments[3], at::kTransportRawPayloadCapacity, size)) {
        return at::CommandResult::Error;
    }
    strncpy(pending_path_, command.arguments[2], sizeof(pending_path_) - 1U);
    pending_path_[sizeof(pending_path_) - 1U] = '\0';
    pending_operation_ = append ? PendingOperation::FileAppend : PendingOperation::FileWrite;
    if (size == 0U) {
        const at::CommandResult result = complete_file_write(nullptr, 0U, append);
        pending_operation_ = PendingOperation::None;
        return result;
    }
    if (!transport.request_raw_payload(
            static_cast<size_t>(size),
            append ? &StorageService::append_payload : &StorageService::write_payload,
            this)) {
        pending_operation_ = PendingOperation::None;
        return at::CommandResult::Error;
    }
    return at::CommandResult::Deferred;
}

at::CommandResult StorageService::write_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    StorageService *service = static_cast<StorageService *>(context);
    const at::CommandResult result = service->complete_file_write(data, size, false);
    service->pending_operation_ = PendingOperation::None;
    return result;
}

at::CommandResult StorageService::append_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    StorageService *service = static_cast<StorageService *>(context);
    const at::CommandResult result = service->complete_file_write(data, size, true);
    service->pending_operation_ = PendingOperation::None;
    return result;
}

at::CommandResult StorageService::complete_file_write(
    const uint8_t *data,
    size_t size,
    bool append)
{
    if ((size != 0U && data == nullptr) ||
        pending_path_[0] == '\0') {
        return at::CommandResult::Error;
    }
    lfs_file_t file = {};
    const int flags = LFS_O_WRONLY | LFS_O_CREAT |
        (append ? LFS_O_APPEND : LFS_O_TRUNC);
    if (lfs_file_open(&file_system_, &file, pending_path_, flags) != LFS_ERR_OK) {
        return at::CommandResult::Error;
    }
    const lfs_ssize_t written = size == 0U
        ? 0
        : lfs_file_write(&file_system_, &file, data, size);
    const int sync_status = lfs_file_sync(&file_system_, &file);
    const int close_status = lfs_file_close(&file_system_, &file);
    return written == static_cast<lfs_ssize_t>(size) &&
            sync_status == LFS_ERR_OK &&
            close_status == LFS_ERR_OK
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult StorageService::handle_file_read(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.argument_count != 5U) {
        return at::CommandResult::Error;
    }
    unsigned long offset = 0U;
    unsigned long length = 0U;
    if (!parse_uint(command.arguments[3], ULONG_MAX, offset) ||
        !parse_uint(command.arguments[4], at::kTransportRawPayloadCapacity, length)) {
        return at::CommandResult::Error;
    }
    lfs_file_t file = {};
    if (lfs_file_open(&file_system_, &file, command.arguments[2], LFS_O_RDONLY) != LFS_ERR_OK) {
        return at::CommandResult::Error;
    }
    const lfs_soff_t position = lfs_file_seek(
        &file_system_,
        &file,
        static_cast<lfs_soff_t>(offset),
        LFS_SEEK_SET);
    const lfs_soff_t file_size = lfs_file_size(&file_system_, &file);
    if (position < 0 || file_size < 0) {
        (void)lfs_file_close(&file_system_, &file);
        return at::CommandResult::Error;
    }
    size_t remaining = 0U;
    if (static_cast<uint64_t>(position) < static_cast<uint64_t>(file_size)) {
        const uint64_t available = static_cast<uint64_t>(file_size) -
            static_cast<uint64_t>(position);
        remaining = available < length ? static_cast<size_t>(available) :
            static_cast<size_t>(length);
    }

    write_prefix(transport, command);
    write_u64(transport, remaining);
    (void)transport.write_cstr("|");
    uint8_t *buffer = remaining != 0U ? transport.temporary_workspace(remaining) : nullptr;
    if (remaining != 0U && buffer == nullptr) {
        (void)lfs_file_close(&file_system_, &file);
        return at::CommandResult::Error;
    }
    const size_t payload_size = remaining;
    size_t payload_offset = 0U;
    while (remaining != 0U) {
        const size_t chunk = remaining < 256U ? remaining : 256U;
        const lfs_ssize_t received = lfs_file_read(&file_system_, &file, buffer + payload_offset, chunk);
        if (received != static_cast<lfs_ssize_t>(chunk)) {
            if (payload_offset != 0U) {
                (void)transport.write_borrowed_data(buffer, payload_offset);
            }
            (void)lfs_file_close(&file_system_, &file);
            return at::CommandResult::Error;
        }
        remaining -= chunk;
        payload_offset += chunk;
    }
    if (payload_size != 0U && !transport.write_borrowed_data(buffer, payload_size)) {
        (void)lfs_file_close(&file_system_, &file);
        return at::CommandResult::Error;
    }
    return lfs_file_close(&file_system_, &file) == LFS_ERR_OK &&
            transport.write_cstr("\r\n")
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult StorageService::handle_preference(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (is_name(command, kPreferenceBegin)) {
        return handle_preference_begin(command, transport);
    }
    if (is_name(command, kPreferenceEnd)) {
        if (command.mode != at::CommandMode::Run) {
            return at::CommandResult::Error;
        }
        close_preferences();
        (void)transport.write_response(command, "0\r\n");
        return at::CommandResult::Ok;
    }
    if (!preference_open()) {
        return at::CommandResult::Error;
    }
    if (is_name(command, kPreferenceClear)) {
        return handle_preference_clear(command, transport);
    }
    if (is_name(command, kPreferenceRemove)) {
        return handle_preference_remove(command, transport);
    }
    if (is_name(command, kPreferencePut)) {
        return handle_preference_put(command, transport);
    }
    if (is_name(command, kPreferenceType)) {
        return handle_preference_type(command, transport);
    }
    if (is_name(command, kPreferenceGet)) {
        return handle_preference_get(command, transport);
    }
    if (is_name(command, kPreferenceLength)) {
        return handle_preference_length(command, transport);
    }
    if (is_name(command, kPreferenceStat)) {
        return handle_preference_stat(command, transport);
    }
    return at::CommandResult::Error;
}

at::CommandResult StorageService::handle_preference_begin(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write || command.argument_count != 3U ||
        !valid_preference_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    unsigned long read_only = 0U;
    if (!parse_uint(command.arguments[1], 1U, read_only)) {
        return at::CommandResult::Error;
    }
    close_preferences();
    strncpy(preference_namespace_, command.arguments[0], sizeof(preference_namespace_) - 1U);
    preference_namespace_[sizeof(preference_namespace_) - 1U] = '\0';
    const char *partition = command.arguments[2][0] == '\0'
        ? kPreferenceDefaultPartition
        : command.arguments[2];
    if (!valid_preference_name(partition)) {
        return at::CommandResult::Error;
    }
    strncpy(preference_partition_, partition, sizeof(preference_partition_) - 1U);
    preference_partition_[sizeof(preference_partition_) - 1U] = '\0';
    preference_read_only_ = read_only != 0U;
    preference_started_ = true;
    (void)transport.write_response(command, "1\r\n");
    return at::CommandResult::Ok;
}

at::CommandResult StorageService::handle_preference_put(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count != 3U ||
        !valid_preference_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    unsigned long type = 0U;
    if (!parse_uint(command.arguments[1], PreferenceBlob, type)) {
        return at::CommandResult::Error;
    }
    const uint8_t preference_type = static_cast<uint8_t>(type);
    if (preference_type == PreferenceString || preference_type == PreferenceBlob) {
        unsigned long size = 0U;
        if (!parse_uint(
                command.arguments[2],
                at::kTransportRawPayloadCapacity,
                size)) {
            return at::CommandResult::Error;
        }
        strncpy(
            pending_preference_key_,
            command.arguments[0],
            sizeof(pending_preference_key_) - 1U);
        pending_preference_key_[sizeof(pending_preference_key_) - 1U] = '\0';
        pending_preference_type_ = preference_type;
        pending_operation_ = PendingOperation::PreferencePut;
        if (size == 0U) {
            const bool stored = write_preference_value(nullptr, 0U);
            const at::CommandResult result =
                write_preference_result(transport, command, stored ? 0U : 0U)
                    ? at::CommandResult::Ok
                    : at::CommandResult::Error;
            pending_operation_ = PendingOperation::None;
            return result;
        }
        pending_preference_transport_ = &transport;
        if (!transport.request_raw_payload(
                static_cast<size_t>(size),
                &StorageService::preference_payload,
                this)) {
            pending_preference_transport_ = nullptr;
            pending_operation_ = PendingOperation::None;
            return at::CommandResult::Error;
        }
        return at::CommandResult::Deferred;
    }

    const size_t value_size = scalar_size(preference_type);
    if (value_size == 0U) {
        return at::CommandResult::Error;
    }
    uint8_t value[sizeof(uint64_t)] = {};
    if (preference_type == PreferenceI8 ||
        preference_type == PreferenceI16 ||
        preference_type == PreferenceI32 ||
        preference_type == PreferenceI64) {
        int64_t parsed = 0;
        if (!parse_i64(command.arguments[2], parsed)) {
            return at::CommandResult::Error;
        }
        memcpy(value, &parsed, value_size);
    } else {
        uint64_t parsed = 0U;
        if (!parse_u64(command.arguments[2], parsed)) {
            return at::CommandResult::Error;
        }
        memcpy(value, &parsed, value_size);
    }
    return write_preference_result(
               transport,
               command,
               store_preference(
                   command.arguments[0],
                   preference_type,
                   value,
                   value_size)
                   ? value_size
                   : 0U)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult StorageService::preference_payload(
    void *context,
    const uint8_t *data,
    size_t size)
{
    StorageService *service = static_cast<StorageService *>(context);
    const bool stored = service->write_preference_value(data, size);
    if (service->pending_preference_transport_ != nullptr) {
        service->write_preference_result(
            *service->pending_preference_transport_,
            kPreferencePut,
            stored ? size : 0U);
    }
    service->pending_preference_transport_ = nullptr;
    service->pending_operation_ = PendingOperation::None;
    return at::CommandResult::Ok;
}

bool StorageService::write_preference_value(const uint8_t *data, size_t size)
{
    const bool stored = preference_open() &&
        pending_preference_key_[0] != '\0' &&
        store_preference(
            pending_preference_key_,
            pending_preference_type_,
            data,
            size);
    pending_preference_key_[0] = '\0';
    pending_preference_type_ = PreferenceInvalid;
    return stored;
}

at::CommandResult StorageService::handle_preference_type(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write || command.argument_count != 1U ||
        !valid_preference_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    uint8_t type = PreferenceInvalid;
    size_t value_size = 0U;
    (void)load_preference_metadata(command.arguments[0], type, value_size);
    return write_preference_result(transport, command, type)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult StorageService::handle_preference_get(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write ||
        command.argument_count < 2U ||
        command.argument_count > 3U ||
        !valid_preference_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    unsigned long requested = 0U;
    if (!parse_uint(command.arguments[1], PreferenceBlob, requested)) {
        return at::CommandResult::Error;
    }
    const uint8_t requested_type = static_cast<uint8_t>(requested);
    uint8_t stored_type = PreferenceInvalid;
    size_t stored_size = 0U;
    const bool found = load_preference_metadata(
        command.arguments[0],
        stored_type,
        stored_size) &&
        preference_type_matches(stored_type, requested_type);

    if (requested_type == PreferenceString || requested_type == PreferenceBlob) {
        if (requested_type == PreferenceBlob && !found) {
            write_prefix(transport, command);
            (void)transport.write_cstr("0|\r\n");
            return at::CommandResult::Ok;
        }
        if (requested_type == PreferenceString && !found) {
            const char *default_value = command.argument_count == 3U
                ? command.arguments[2]
                : "";
            const size_t default_size = strlen(default_value);
            write_prefix(transport, command);
            write_u64(transport, default_size);
            (void)transport.write_cstr("|");
            (void)transport.write_data(default_value, default_size);
            (void)transport.write_cstr("\r\n");
            return at::CommandResult::Ok;
        }

        char value_key[kPreferenceKeyCapacity] = {};
        char metadata_key[kPreferenceKeyCapacity] = {};
        if (!preference_key("v", command.arguments[0], value_key) ||
            !preference_key("m", command.arguments[0], metadata_key)) {
            return at::CommandResult::Error;
        }
        write_prefix(transport, command);
        write_u64(transport, stored_size);
        (void)transport.write_cstr("|");
        uint8_t *buffer = stored_size != 0U ? transport.temporary_workspace(stored_size) : nullptr;
        if (stored_size != 0U && buffer == nullptr) {
            return at::CommandResult::Error;
        }
        size_t offset = 0U;
        while (offset < stored_size) {
            const size_t chunk = stored_size - offset < 256U ? stored_size - offset : 256U;
            if (!persistent_record_read(metadata_key, value_key, kPreferenceMetadataSize,
                                        stored_size, buffer + offset, offset, chunk)) {
                if (offset != 0U) {
                    (void)transport.write_borrowed_data(buffer, offset);
                }
                return at::CommandResult::Error;
            }
            offset += chunk;
        }
        if (stored_size != 0U && !transport.write_borrowed_data(buffer, stored_size)) {
            return at::CommandResult::Error;
        }
        (void)transport.write_cstr("\r\n");
        return at::CommandResult::Ok;
    }

    const size_t value_size = scalar_size(requested_type);
    if (value_size == 0U || command.argument_count != 3U) {
        return at::CommandResult::Error;
    }
    uint8_t value[sizeof(uint64_t)] = {};
    if (found && stored_size == value_size) {
        char value_key[kPreferenceKeyCapacity] = {};
        char metadata_key[kPreferenceKeyCapacity] = {};
        if (!preference_key("v", command.arguments[0], value_key) ||
            !preference_key("m", command.arguments[0], metadata_key) ||
            !persistent_record_read(metadata_key, value_key, kPreferenceMetadataSize,
                                    stored_size, value, 0, value_size)) {
            return at::CommandResult::Error;
        }
    } else if (requested_type == PreferenceI8 ||
        requested_type == PreferenceI16 ||
        requested_type == PreferenceI32 ||
        requested_type == PreferenceI64) {
        int64_t parsed = 0;
        if (!parse_i64(command.arguments[2], parsed)) {
            return at::CommandResult::Error;
        }
        memcpy(value, &parsed, value_size);
    } else {
        uint64_t parsed = 0U;
        if (!parse_u64(command.arguments[2], parsed)) {
            return at::CommandResult::Error;
        }
        memcpy(value, &parsed, value_size);
    }

    if (requested_type == PreferenceI8 ||
        requested_type == PreferenceI16 ||
        requested_type == PreferenceI32 ||
        requested_type == PreferenceI64) {
        int64_t signed_value = 0;
        switch (requested_type) {
        case PreferenceI8: {
            int8_t decoded = 0;
            memcpy(&decoded, value, sizeof(decoded));
            signed_value = decoded;
            break;
        }
        case PreferenceI16: {
            int16_t decoded = 0;
            memcpy(&decoded, value, sizeof(decoded));
            signed_value = decoded;
            break;
        }
        case PreferenceI32: {
            int32_t decoded = 0;
            memcpy(&decoded, value, sizeof(decoded));
            signed_value = decoded;
            break;
        }
        case PreferenceI64:
            memcpy(&signed_value, value, sizeof(signed_value));
            break;
        default:
            return at::CommandResult::Error;
        }
        return write_preference_signed_result(transport, command, signed_value)
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    } else {
        uint64_t unsigned_value = 0U;
        memcpy(&unsigned_value, value, value_size);
        return write_preference_result(transport, command, unsigned_value)
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }
}

at::CommandResult StorageService::handle_preference_length(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write || command.argument_count != 1U ||
        !valid_preference_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    uint8_t type = PreferenceInvalid;
    size_t value_size = 0U;
    if (!load_preference_metadata(command.arguments[0], type, value_size)) {
        value_size = 0U;
    }
    return write_preference_result(transport, command, value_size)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult StorageService::handle_preference_remove(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Write || command.argument_count != 1U ||
        !valid_preference_name(command.arguments[0])) {
        return at::CommandResult::Error;
    }
    return write_preference_result(
               transport,
               command,
               remove_preference(command.arguments[0]) ? 1U : 0U)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult StorageService::handle_preference_clear(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run) {
        return at::CommandResult::Error;
    }
    if (preference_read_only_) {
        return write_preference_result(transport, command, 0U)
            ? at::CommandResult::Ok
            : at::CommandResult::Error;
    }
    char prefix[kPreferenceKeyCapacity] = {};
    const int length = snprintf(
        prefix,
        sizeof(prefix),
        "%s/%s/%s/m/",
        kPreferenceKeyPrefix,
        preference_partition_,
        preference_namespace_);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(prefix)) {
        return at::CommandResult::Error;
    }
    PreferenceCount count = {
        prefix,
        static_cast<size_t>(length),
        0U,
    };
    if (ef_foreach_env(count_preference_metadata, &count) != EF_NO_ERR) {
        return at::CommandResult::Error;
    }
    for (;;) {
        char matched_key[kPreferenceKeyCapacity] = {};
        struct FindFirst {
            const char *prefix;
            size_t prefix_size;
            char *result;
            bool found;
        } find = {
            prefix,
            static_cast<size_t>(length),
            matched_key,
            false,
        };
        const auto find_first = [](const char *key, void *context) -> EfErrCode {
            FindFirst *state = static_cast<FindFirst *>(context);
            if (!state->found &&
                strncmp(key, state->prefix, state->prefix_size) == 0) {
                strncpy(state->result, key + state->prefix_size, kPreferenceNameCapacity - 1U);
                state->result[kPreferenceNameCapacity - 1U] = '\0';
                state->found = state->result[0] != '\0';
            }
            return EF_NO_ERR;
        };
        if (ef_foreach_env(find_first, &find) != EF_NO_ERR) {
            return at::CommandResult::Error;
        }
        if (!find.found) {
            break;
        }
        if (!remove_preference(matched_key)) {
            return at::CommandResult::Error;
        }
    }
    (void)count;
    return write_preference_result(transport, command, 1U)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

at::CommandResult StorageService::handle_preference_stat(
    const at::Command &command,
    at::AtTransport &transport)
{
    if (command.mode != at::CommandMode::Run) {
        return at::CommandResult::Error;
    }
    char prefix[kPreferenceKeyCapacity] = {};
    const int length = snprintf(
        prefix,
        sizeof(prefix),
        "%s/%s/%s/m/",
        kPreferenceKeyPrefix,
        preference_partition_,
        preference_namespace_);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(prefix)) {
        return at::CommandResult::Error;
    }
    PreferenceCount count = {
        prefix,
        static_cast<size_t>(length),
        0U,
    };
    if (ef_foreach_env(count_preference_metadata, &count) != EF_NO_ERR) {
        return at::CommandResult::Error;
    }
    const unsigned long free_entries = count.count >= kPreferenceVirtualEntryCount
        ? 0U
        : kPreferenceVirtualEntryCount - count.count;
    return write_preference_result(transport, command, free_entries)
        ? at::CommandResult::Ok
        : at::CommandResult::Error;
}

bool StorageService::mount_file_system(bool format_on_fail)
{
    if (file_system_mounted_) {
        return true;
    }
    bflb_mtd_handle_t handle = nullptr;
    bflb_mtd_info_t info = {};
    // These callbacks use raw Flash offsets, never an XIP address. The SDK
    // BUSADDR conversion rejects partitions below the second FW slot even
    // when FW A is active, which includes the independent RA4 cache.
    if (bflb_mtd_open(partition_, &handle, 0U) < 0 ||
        bflb_mtd_info(handle, &info) < 0) {
        if (handle != nullptr) {
            bflb_mtd_close(handle);
        }
        return false;
    }
    bflb_mtd_close(handle);
    if (info.size == 0U || info.size % 4096U != 0U) {
        return false;
    }

    memset(&file_system_, 0, sizeof(file_system_));
    memset(&file_config_, 0, sizeof(file_config_));
    file_context_.flash_address = info.offset;
    file_config_.context = &file_context_;
    file_config_.read = &StorageService::file_read;
    file_config_.prog = &StorageService::file_program;
    file_config_.erase = &StorageService::file_erase;
    file_config_.sync = &StorageService::file_sync;
    if (file_context_.mutex == nullptr) {
        file_context_.mutex = file_context_.recursive_mutex
            ? xSemaphoreCreateRecursiveMutex()
            : xSemaphoreCreateMutex();
        if (file_context_.mutex == nullptr) {
            return false;
        }
    }
    file_config_.lock = &StorageService::file_lock;
    file_config_.unlock = &StorageService::file_unlock;
    file_config_.read_size = 256U;
    file_config_.prog_size = 256U;
    file_config_.block_size = 4096U;
    file_config_.block_count = info.size / file_config_.block_size;
    file_config_.block_cycles = 500;
    file_config_.cache_size = sizeof(file_read_cache_);
    file_config_.lookahead_size = sizeof(file_lookahead_);
    file_config_.read_buffer = file_read_cache_;
    file_config_.prog_buffer = file_program_cache_;
    file_config_.lookahead_buffer = file_lookahead_;

    int status = lfs_mount(&file_system_, &file_config_);
    bool initialize_blank_cache = false;
    if (status != LFS_ERR_OK && strcmp(partition_, "RA4_OTA") == 0) {
        initialize_blank_cache = true;
        uint8_t probe[256];
        for (uint32_t offset = 0U; offset < info.size && initialize_blank_cache;
             offset += sizeof(probe)) {
            if (bflb_flash_read(info.offset + offset, probe, sizeof(probe)) != 0) {
                initialize_blank_cache = false;
                break;
            }
            for (size_t i = 0; i < sizeof(probe); ++i) {
                if (probe[i] != 0xffU) {
                    initialize_blank_cache = false;
                    break;
                }
            }
        }
    }
    if (status != LFS_ERR_OK && (format_on_fail || initialize_blank_cache)) {
        status = lfs_format(&file_system_, &file_config_);
        if (status == LFS_ERR_OK) {
            status = lfs_mount(&file_system_, &file_config_);
        }
    }
    file_system_mounted_ = status == LFS_ERR_OK;
    return file_system_mounted_;
}

bool StorageService::open_ota_cache(const char *path, bool format_on_fail)
{
    discard_ota_cache();
    invalidate_committed_ota_cache();
    if (!valid_path(path) || !mount_file_system(format_on_fail)) {
        return false;
    }
    if (lfs_file_open(
            &file_system_,
            &ota_cache_file_,
            kRa4OtaCacheFile,
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != LFS_ERR_OK) {
        return false;
    }
    strncpy(ota_cache_path_, path, sizeof(ota_cache_path_) - 1U);
    ota_cache_path_[sizeof(ota_cache_path_) - 1U] = '\0';
    ota_cache_open_ = true;
    return true;
}

bool StorageService::write_ota_cache(const uint8_t *data, size_t size)
{
    if (!ota_cache_open_ || (data == nullptr && size != 0U)) {
        return false;
    }
    return size == 0U ||
        lfs_file_write(&file_system_, &ota_cache_file_, data, size) ==
            static_cast<lfs_ssize_t>(size);
}

bool StorageService::commit_ota_cache()
{
    if (!ota_cache_open_) {
        return false;
    }
    const lfs_soff_t size = lfs_file_size(&file_system_, &ota_cache_file_);
    const int sync_status = lfs_file_sync(&file_system_, &ota_cache_file_);
    const int close_status = lfs_file_close(&file_system_, &ota_cache_file_);
    ota_cache_open_ = false;
    const bool committed = size > 0 &&
        static_cast<size_t>(size) <= kRa4OtaCacheMaximumBytes &&
        sync_status == LFS_ERR_OK && close_status == LFS_ERR_OK &&
        ota_image_source_.mark_committed(
            ota_cache_path_,
            static_cast<size_t>(size));
    ota_cache_path_[0] = '\0';
    return committed;
}

void StorageService::discard_ota_cache()
{
    if (ota_cache_open_) {
        (void)lfs_file_close(&file_system_, &ota_cache_file_);
        ota_cache_open_ = false;
    }
    if (file_system_mounted_ && ota_cache_path_[0] != '\0') {
        (void)lfs_remove(&file_system_, kRa4OtaCacheFile);
    }
    ota_cache_path_[0] = '\0';
}

bool StorageService::ota_cache_begin(
    void *context,
    const char *path,
    bool format_on_fail)
{
    return static_cast<StorageService *>(context)->open_ota_cache(
        path,
        format_on_fail);
}

bool StorageService::ota_cache_write(
    void *context,
    const uint8_t *data,
    size_t size)
{
    return static_cast<StorageService *>(context)->write_ota_cache(data, size);
}

bool StorageService::ota_cache_commit(void *context)
{
    return static_cast<StorageService *>(context)->commit_ota_cache();
}

void StorageService::ota_cache_discard(void *context)
{
    static_cast<StorageService *>(context)->discard_ota_cache();
}

size_t StorageService::ota_cache_maximum_output_size(void *)
{
    return kRa4OtaCacheMaximumBytes;
}

bool StorageService::ota_image_open(
    void *context,
    const char *path,
    size_t *size)
{
    return static_cast<StorageService *>(context)->open_committed_ota_cache(
        path,
        size);
}

bool StorageService::ota_image_read(
    void *context,
    size_t offset,
    uint8_t *destination,
    size_t size)
{
    return static_cast<StorageService *>(context)->read_committed_ota_cache(
        offset,
        destination,
        size);
}

void StorageService::ota_image_close(void *context)
{
    static_cast<StorageService *>(context)->close_committed_ota_cache();
}

void StorageService::invalidate_committed_ota_cache()
{
    const char *path = ota_image_source_.path();
    close_committed_ota_cache();
    if (file_system_mounted_ && path != nullptr) {
        (void)lfs_remove(&file_system_, kRa4OtaCacheFile);
    }
    ota_image_source_.invalidate();
}

bool StorageService::open_committed_ota_cache(
    const char *path,
    size_t *size)
{
    if (!ready_ra4_ota_cache(path) || size == nullptr ||
        !mount_file_system(false) || ota_image_open_) {
        return false;
    }
    if (lfs_file_open(
            &file_system_,
            &ota_image_file_,
            kRa4OtaCacheFile,
            LFS_O_RDONLY) != LFS_ERR_OK) {
        return false;
    }
    const lfs_soff_t file_size =
        lfs_file_size(&file_system_, &ota_image_file_);
    if (file_size < 0 ||
        static_cast<size_t>(file_size) != ota_image_source_.size()) {
        (void)lfs_file_close(&file_system_, &ota_image_file_);
        return false;
    }
    ota_image_open_ = true;
    *size = static_cast<size_t>(file_size);
    return true;
}

bool StorageService::read_committed_ota_cache(
    size_t offset,
    uint8_t *destination,
    size_t size)
{
    if (!ota_image_open_ || destination == nullptr ||
        offset > ota_image_source_.size() ||
        size > ota_image_source_.size() - offset ||
        lfs_file_seek(
            &file_system_,
            &ota_image_file_,
            static_cast<lfs_soff_t>(offset),
            LFS_SEEK_SET) < 0) {
        return false;
    }
    return lfs_file_read(
        &file_system_,
        &ota_image_file_,
        destination,
        size) == static_cast<lfs_ssize_t>(size);
}

void StorageService::close_committed_ota_cache()
{
    if (ota_image_open_) {
        (void)lfs_file_close(&file_system_, &ota_image_file_);
        ota_image_open_ = false;
    }
}

bool StorageService::parse_uint(
    const char *text,
    unsigned long maximum,
    unsigned long &value) const
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool StorageService::write_preference_result(
    at::AtTransport &transport,
    const at::Command &command,
    uint64_t value) const
{
    return write_preference_result(transport, command.name, value);
}

bool StorageService::write_preference_result(
    at::AtTransport &transport,
    const char *command_name,
    uint64_t value) const
{
    (void)transport.write_cstr(command_name);
    (void)transport.write_cstr(": ");
    write_u64(transport, value);
    return transport.write_cstr("\r\n");
}

bool StorageService::write_preference_signed_result(
    at::AtTransport &transport,
    const at::Command &command,
    int64_t value) const
{
    write_prefix(transport, command);
    write_i64(transport, value);
    return transport.write_cstr("\r\n");
}

bool StorageService::parse_i64(const char *text, int64_t &value) const
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    value = static_cast<int64_t>(parsed);
    return true;
}

bool StorageService::parse_u64(const char *text, uint64_t &value) const
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool StorageService::valid_path(const char *path) const
{
    return path != nullptr && path[0] != '\0' &&
        strlen(path) < at::kCommandArgumentBytes;
}

bool StorageService::valid_preference_name(const char *name) const
{
    return name != nullptr && name[0] != '\0' &&
        strlen(name) < kPreferenceNameCapacity;
}

bool StorageService::preference_open() const
{
    return preference_started_;
}

bool StorageService::preference_key(
    const char *kind,
    const char *key,
    char destination[kPreferenceKeyCapacity]) const
{
    if (!preference_open() || kind == nullptr ||
        !valid_preference_name(key)) {
        return false;
    }
    const int length = snprintf(
        destination,
        kPreferenceKeyCapacity,
        "%s/%s/%s/%s/%s",
        kPreferenceKeyPrefix,
        preference_partition_,
        preference_namespace_,
        kind,
        key);
    return length > 0 && static_cast<size_t>(length) < kPreferenceKeyCapacity;
}

bool StorageService::load_preference_metadata(
    const char *key,
    uint8_t &type,
    size_t &value_size) const
{
    char metadata_key[kPreferenceKeyCapacity] = {};
    uint8_t metadata[kPreferenceMetadataSize] = {};
    size_t saved_size = 0U;
    if (!preference_key("m", key, metadata_key) ||
        ef_get_env_blob(
            metadata_key,
            metadata,
            sizeof(metadata),
            &saved_size) != sizeof(metadata) ||
        metadata[0] > PreferenceBlob) {
        type = PreferenceInvalid;
        value_size = 0U;
        return false;
    }
    type = metadata[0];
    value_size = read_u32_le(metadata + 1U);
    return value_size <= at::kTransportRawPayloadCapacity &&
        persistent_record_size_matches(saved_size, sizeof(metadata), value_size);
}

bool StorageService::store_preference(
    const char *key,
    uint8_t type,
    const uint8_t *value,
    size_t value_size)
{
    if (preference_read_only_ || type > PreferenceBlob ||
        value_size > at::kTransportRawPayloadCapacity ||
        (value_size != 0U && value == nullptr)) {
        return false;
    }
    char metadata_key[kPreferenceKeyCapacity] = {};
    char value_key[kPreferenceKeyCapacity] = {};
    uint8_t metadata[kPreferenceMetadataSize] = {};
    if (!preference_key("m", key, metadata_key) ||
        !preference_key("v", key, value_key)) {
        return false;
    }
    metadata[0] = type;
    write_u32_le(metadata + 1U, static_cast<uint32_t>(value_size));
    if (!persistent_record_write(metadata_key, metadata, sizeof(metadata), value, value_size)) {
        return false;
    }
    (void)ef_del_env(value_key); // Reclaim obsolete legacy payload after commit.
    return true;
}

bool StorageService::remove_preference(const char *key)
{
    if (preference_read_only_) {
        return false;
    }
    uint8_t type = PreferenceInvalid;
    size_t value_size = 0U;
    if (!load_preference_metadata(key, type, value_size)) {
        return false;
    }
    char metadata_key[kPreferenceKeyCapacity] = {};
    char value_key[kPreferenceKeyCapacity] = {};
    if (!preference_key("m", key, metadata_key) ||
        !preference_key("v", key, value_key) ||
        ef_del_env(metadata_key) != EF_NO_ERR) {
        return false;
    }
    (void)ef_del_env(value_key);
    return true;
}

bool StorageService::preference_type_matches(uint8_t stored, uint8_t requested) const
{
    return stored == requested;
}

void StorageService::write_prefix(
    at::AtTransport &transport,
    const at::Command &command) const
{
    (void)transport.write_cstr(command.name);
    (void)transport.write_cstr(": ");
}

void StorageService::write_u64(at::AtTransport &transport, uint64_t value) const
{
    char text[24] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%llu",
        static_cast<unsigned long long>(value));
    if (size > 0) {
        (void)transport.write_cstr(text);
    }
}

void StorageService::write_i64(at::AtTransport &transport, int64_t value) const
{
    char text[24] = {};
    const int size = snprintf(
        text,
        sizeof(text),
        "%lld",
        static_cast<long long>(value));
    if (size > 0) {
        (void)transport.write_cstr(text);
    }
}

void StorageService::close_preferences()
{
    preference_started_ = false;
    preference_read_only_ = false;
    preference_namespace_[0] = '\0';
    preference_partition_[0] = '\0';
}

int StorageService::file_read(
    const lfs_config *config,
    lfs_block_t block,
    lfs_off_t offset,
    void *buffer,
    lfs_size_t size)
{
    const FileSystemContext *context =
        static_cast<const FileSystemContext *>(config->context);
    return bflb_flash_read(
        context->flash_address + block * config->block_size + offset,
        static_cast<uint8_t *>(buffer),
        size);
}

int StorageService::file_program(
    const lfs_config *config,
    lfs_block_t block,
    lfs_off_t offset,
    const void *buffer,
    lfs_size_t size)
{
    const FileSystemContext *context =
        static_cast<const FileSystemContext *>(config->context);
    return bflb_flash_write(
        context->flash_address + block * config->block_size + offset,
        const_cast<uint8_t *>(static_cast<const uint8_t *>(buffer)),
        size);
}

int StorageService::file_erase(
    const lfs_config *config,
    lfs_block_t block)
{
    const FileSystemContext *context =
        static_cast<const FileSystemContext *>(config->context);
    return bflb_flash_erase(
        context->flash_address + block * config->block_size,
        config->block_size);
}

int StorageService::file_sync(const lfs_config *)
{
    return 0;
}

int StorageService::file_lock(const lfs_config *config)
{
    const FileSystemContext *context =
        static_cast<const FileSystemContext *>(config->context);
    if (context == nullptr || context->mutex == nullptr) {
        return LFS_ERR_IO;
    }
    const BaseType_t status = context->recursive_mutex
        ? xSemaphoreTakeRecursive(context->mutex, portMAX_DELAY)
        : xSemaphoreTake(context->mutex, portMAX_DELAY);
    return status == pdTRUE
        ? 0
        : LFS_ERR_IO;
}

int StorageService::file_unlock(const lfs_config *config)
{
    const FileSystemContext *context =
        static_cast<const FileSystemContext *>(config->context);
    if (context == nullptr || context->mutex == nullptr) {
        return LFS_ERR_IO;
    }
    const BaseType_t status = context->recursive_mutex
        ? xSemaphoreGiveRecursive(context->mutex)
        : xSemaphoreGive(context->mutex);
    return status == pdTRUE
        ? 0
        : LFS_ERR_IO;
}

} // namespace bridge
