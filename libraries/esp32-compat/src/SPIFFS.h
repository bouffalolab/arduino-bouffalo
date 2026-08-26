#ifndef BL616CL_ESP32_COMPAT_SPIFFS_H_
#define BL616CL_ESP32_COMPAT_SPIFFS_H_

#include "FS.h"

class SPIFFSClass : public FS {
public:
    bool begin(bool formatOnFail = false);
    void end();
    bool format();
    bool exists(const char *path) override;
    bool remove(const char *path) override;
    bool rename(const char *from, const char *to) override;
    File open(const char *path, const char *mode = FILE_READ) override;
    bool mkdir(const char *path) override;
    bool rmdir(const char *path) override;
};

extern SPIFFSClass SPIFFS;

#endif
