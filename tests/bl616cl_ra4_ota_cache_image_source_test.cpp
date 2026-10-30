#include "../UNOR4BL616CLBridge/ra4_ota_cache_image_source.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct FakeFile {
    std::string path;
    std::vector<uint8_t> bytes;
    bool open = false;
    unsigned int open_count = 0U;
    unsigned int close_count = 0U;
};

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

bool open_file(void *context, const char *path, size_t *size)
{
    FakeFile *file = static_cast<FakeFile *>(context);
    if (file->open || path == nullptr || file->path != path) {
        return false;
    }
    file->open = true;
    ++file->open_count;
    *size = file->bytes.size();
    return true;
}

bool read_file(
    void *context,
    size_t offset,
    uint8_t *destination,
    size_t size)
{
    FakeFile *file = static_cast<FakeFile *>(context);
    if (!file->open || destination == nullptr || offset > file->bytes.size() ||
        size > file->bytes.size() - offset) {
        return false;
    }
    memcpy(destination, file->bytes.data() + offset, size);
    return true;
}

void close_file(void *context)
{
    FakeFile *file = static_cast<FakeFile *>(context);
    if (file->open) {
        file->open = false;
        ++file->close_count;
    }
}

bridge::Ra4OtaCacheImageReader reader(FakeFile &file)
{
    return {open_file, read_file, close_file, &file};
}

void run_lifecycle_case()
{
    FakeFile file = {
        "/ra4-update.bin",
        {0x10U, 0x20U, 0x30U, 0x40U},
    };
    bridge::Ra4OtaCacheImageSource source(reader(file));

    expect(!source.ready(), "empty source is not ready");
    expect(source.mark_committed(file.path.c_str(), file.bytes.size()),
        "committed cache accepted");
    expect(source.ready(), "committed source ready");
    expect(source.ready(file.path.c_str()), "matching path ready");
    expect(!source.ready("/different.bin"), "different path rejected");
    expect(source.size() == file.bytes.size(), "committed size retained");

    bridge::Ra4SambaImageSource image = source.image_source();
    size_t size = 0U;
    expect(image.open(image.context, &size), "image source opens");
    expect(size == file.bytes.size(), "image source reports exact size");
    expect(!source.ready(), "source cannot be selected concurrently");
    uint8_t read[2] = {};
    expect(image.read(image.context, 1U, read, sizeof(read)),
        "bounded image read works");
    expect(read[0] == 0x20U && read[1] == 0x30U, "image read data exact");
    expect(!image.read(image.context, 3U, read, sizeof(read)),
        "image read cannot pass committed size");
    image.close(image.context);
    expect(file.close_count == 1U, "image source closes file");
    expect(source.ready(), "source becomes selectable after close");

    file.bytes.push_back(0x50U);
    expect(!image.open(image.context, &size),
        "changed cache file size is rejected");
    expect(!file.open, "size mismatch closes backing file");
    source.invalidate();
    expect(!source.ready(), "invalidated cache is not selectable");
}

void run_invalid_input_case()
{
    FakeFile file = {"/ra4.bin", {0x01U}};
    bridge::Ra4OtaCacheImageSource source(reader(file));
    expect(!source.mark_committed(nullptr, 1U), "null path rejected");
    expect(!source.mark_committed("", 1U), "empty path rejected");
    expect(!source.mark_committed(file.path.c_str(), 0U), "empty image rejected");
}

} // namespace

int main()
{
    run_lifecycle_case();
    run_invalid_input_case();
    return 0;
}
