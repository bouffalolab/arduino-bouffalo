#ifndef UNOR4_BL616CL_STORAGE_SERVICE_H
#define UNOR4_BL616CL_STORAGE_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "at_transport.h"
#include "ra4_ota_cache_image_source.h"
#include "ra4_ota_service.h"

extern "C" {
#include <FreeRTOS.h>
#include <semphr.h>
#include "lfs.h"
}

namespace bridge {

class StorageService {
public:
    explicit StorageService(const char *partition = "media");

    bool handles_command(const at::Command &command) const;
    at::CommandResult handle_command(
        const at::Command &command,
        at::AtTransport &transport);
    Ra4OtaCacheBackend ra4_ota_cache_backend();
    bool ready_ra4_ota_cache(const char *path = nullptr) const;
    Ra4SambaImageSource ra4_ota_image_source();

private:
    enum class PendingOperation : uint8_t {
        None,
        FileWrite,
        FileAppend,
        PreferencePut,
    };

    struct FileSystemContext {
        uint32_t flash_address = 0U;
        SemaphoreHandle_t mutex = nullptr;
#if configUSE_RECURSIVE_MUTEXES
        bool recursive_mutex = true;
#else
        bool recursive_mutex = false;
#endif
    };

    static constexpr size_t kPreferenceNameCapacity = 16U;
    static constexpr size_t kPreferenceKeyCapacity = 64U;
    static constexpr size_t kPreferenceMetadataSize = 5U;
    static constexpr unsigned long kPreferenceVirtualEntryCount = 126U;
    static constexpr size_t kRa4OtaCacheMaximumBytes = 0x40000U - 0x4000U;

    static at::CommandResult write_payload(
        void *context,
        const uint8_t *data,
        size_t size);
    static at::CommandResult append_payload(
        void *context,
        const uint8_t *data,
        size_t size);
    static at::CommandResult preference_payload(
        void *context,
        const uint8_t *data,
        size_t size);

    static int file_read(
        const lfs_config *config,
        lfs_block_t block,
        lfs_off_t offset,
        void *buffer,
        lfs_size_t size);
    static int file_program(
        const lfs_config *config,
        lfs_block_t block,
        lfs_off_t offset,
        const void *buffer,
        lfs_size_t size);
    static int file_erase(
        const lfs_config *config,
        lfs_block_t block);
    static int file_sync(const lfs_config *config);
    static int file_lock(const lfs_config *config);
    static int file_unlock(const lfs_config *config);
    static bool ota_cache_begin(
        void *context,
        const char *path,
        bool format_on_fail);
    static bool ota_cache_write(
        void *context,
        const uint8_t *data,
        size_t size);
    static bool ota_cache_commit(void *context);
    static void ota_cache_discard(void *context);
    static size_t ota_cache_maximum_output_size(void *context);
    static bool ota_image_open(
        void *context,
        const char *path,
        size_t *size);
    static bool ota_image_read(
        void *context,
        size_t offset,
        uint8_t *destination,
        size_t size);
    static void ota_image_close(void *context);

    at::CommandResult handle_mount(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_file(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference(
        const at::Command &command,
        at::AtTransport &transport);

    at::CommandResult handle_file_write(
        const at::Command &command,
        at::AtTransport &transport,
        bool append);
    at::CommandResult complete_file_write(
        const uint8_t *data,
        size_t size,
        bool append);
    at::CommandResult handle_file_read(
        const at::Command &command,
        at::AtTransport &transport);

    at::CommandResult handle_preference_begin(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference_put(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference_get(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference_type(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference_length(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference_remove(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference_clear(
        const at::Command &command,
        at::AtTransport &transport);
    at::CommandResult handle_preference_stat(
        const at::Command &command,
        at::AtTransport &transport);

    bool mount_file_system(bool format_on_fail);
    bool open_ota_cache(const char *path, bool format_on_fail);
    bool write_ota_cache(const uint8_t *data, size_t size);
    bool commit_ota_cache();
    void discard_ota_cache();
    void invalidate_committed_ota_cache();
    bool open_committed_ota_cache(const char *path, size_t *size);
    bool read_committed_ota_cache(
        size_t offset,
        uint8_t *destination,
        size_t size);
    void close_committed_ota_cache();
    bool write_preference_result(
        at::AtTransport &transport,
        const at::Command &command,
        uint64_t value) const;
    bool write_preference_result(
        at::AtTransport &transport,
        const char *command_name,
        uint64_t value) const;
    bool write_preference_signed_result(
        at::AtTransport &transport,
        const at::Command &command,
        int64_t value) const;
    bool parse_uint(
        const char *text,
        unsigned long maximum,
        unsigned long &value) const;
    bool parse_i64(const char *text, int64_t &value) const;
    bool parse_u64(const char *text, uint64_t &value) const;
    bool valid_path(const char *path) const;
    bool valid_preference_name(const char *name) const;
    bool preference_open() const;
    bool preference_key(
        const char *kind,
        const char *key,
        char destination[kPreferenceKeyCapacity]) const;
    bool load_preference_metadata(
        const char *key,
        uint8_t &type,
        size_t &value_size) const;
    bool store_preference(
        const char *key,
        uint8_t type,
        const uint8_t *value,
        size_t value_size);
    bool remove_preference(const char *key);
    bool preference_type_matches(uint8_t stored, uint8_t requested) const;
    bool write_preference_value(
        const uint8_t *data,
        size_t size);
    void write_prefix(at::AtTransport &transport, const at::Command &command) const;
    void write_u64(at::AtTransport &transport, uint64_t value) const;
    void write_i64(at::AtTransport &transport, int64_t value) const;
    void close_preferences();

    const char *partition_;
    lfs_t file_system_ = {};
    lfs_config file_config_ = {};
    FileSystemContext file_context_ = {};
    uint8_t file_read_cache_[256] = {};
    uint8_t file_program_cache_[256] = {};
    uint8_t file_lookahead_[64] = {};
    bool file_system_mounted_ = false;
    lfs_file_t ota_cache_file_ = {};
    bool ota_cache_open_ = false;
    char ota_cache_path_[at::kCommandArgumentBytes] = {};
    lfs_file_t ota_image_file_ = {};
    bool ota_image_open_ = false;
    Ra4OtaCacheImageSource ota_image_source_ = {};

    PendingOperation pending_operation_ = PendingOperation::None;
    char pending_path_[at::kCommandArgumentBytes] = {};
    char pending_preference_key_[kPreferenceNameCapacity] = {};
    uint8_t pending_preference_type_ = 0U;
    at::AtTransport *pending_preference_transport_ = nullptr;

    bool preference_started_ = false;
    bool preference_read_only_ = false;
    char preference_namespace_[kPreferenceNameCapacity] = {};
    char preference_partition_[kPreferenceNameCapacity] = {};
};

} // namespace bridge

#endif
