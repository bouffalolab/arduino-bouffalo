#ifndef BL616CL_ESP32_COMPAT_SPIFFS_H_
#define BL616CL_ESP32_COMPAT_SPIFFS_H_

#include "FS.h"

class SPIFFSClass {
public:
    bool begin(bool formatOnFail = false) { (void)formatOnFail; return false; }
    void end() {}
    bool format() { return false; }
    bool exists(const char *path) { (void)path; return false; }
    bool remove(const char *path) { (void)path; return false; }
    File open(const char *path, const char *mode = FILE_READ) { (void)path; (void)mode; return File(); }
};

extern SPIFFSClass SPIFFS;

#endif
