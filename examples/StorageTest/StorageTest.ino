#include <Arduino.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok)
{
    Serial.print(ok ? "[PASS] " : "[FAIL] ");
    Serial.println(name);
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void test_fs()
{
    Serial.println("== FS ==");
    check("begin(false) after format", SPIFFS.format() && SPIFFS.begin(false));

    File f = SPIFFS.open("/hello.txt", FILE_WRITE);
    check("open write", (bool)f);
    if (f) {
        check("write", f.write((const uint8_t *)"hello lfs", 9) == 9);
        f.close();
    }
    check("exists", SPIFFS.exists("/hello.txt"));

    f = SPIFFS.open("/hello.txt", FILE_READ);
    check("open read", (bool)f);
    if (f) {
        check("size", f.size() == 9);
        char buf[16] = {0};
        check("read", f.read((uint8_t *)buf, 9) == 9 && strcmp(buf, "hello lfs") == 0);
        check("seek 0", f.seek(0));
        check("read after seek", f.read() == 'h');
        check("position", f.position() == 1);
        check("available", f.available() == 8);
        f.close();
    }

    check("rename", SPIFFS.rename("/hello.txt", "/renamed.txt"));
    check("old gone", !SPIFFS.exists("/hello.txt"));
    check("new exists", SPIFFS.exists("/renamed.txt"));
    check("remove", SPIFFS.remove("/renamed.txt"));
    check("removed gone", !SPIFFS.exists("/renamed.txt"));

    check("mkdir", SPIFFS.mkdir("/dir"));
    /* LittleFS directory handles are snapshots at open time: create the entry
     * before opening the directory for iteration. */
    File e = SPIFFS.open("/dir/item.txt", FILE_WRITE);
    if (e) {
        e.write((const uint8_t *)"x", 1);
        e.close();
    }
    File d = SPIFFS.open("/dir", FILE_READ);
    check("open dir", (bool)d && d.isDirectory());
    if (d) {
        File child = d.openNextFile();
        check("openNextFile", (bool)child && child.name() == "/dir/item.txt");
        if (child) {
            child.close();
        }
        d.close();
    }
    check("rmdir file", SPIFFS.rmdir("/dir/item.txt"));
    check("rmdir dir", SPIFFS.rmdir("/dir"));
}

static void test_fopen()
{
    Serial.println("== fopen ==");
    FILE *fp = fopen("/spiffs/fp.txt", "w");
    check("fopen w", fp != nullptr);
    if (fp) {
        check("fwrite", fwrite("via fopen", 1, 9, fp) == 9);
        fclose(fp);
    }
    fp = fopen("/spiffs/fp.txt", "r");
    check("fopen r", fp != nullptr);
    if (fp) {
        char buf[16] = {0};
        check("fread", fread(buf, 1, 9, fp) == 9 && strcmp(buf, "via fopen") == 0);
        fclose(fp);
    }
    check("fopen remove", remove("/spiffs/fp.txt") == 0);
}

static void test_prefs()
{
    Serial.println("== Preferences ==");
    Preferences prefs;
    check("begin", prefs.begin("tns"));
    prefs.clear();

    check("putChar", prefs.putChar("c", -5) == 1);
    check("getChar", prefs.getChar("c", 0) == -5);
    check("putInt", prefs.putInt("i", 123456) == 4);
    check("getInt", prefs.getInt("i", 0) == 123456);
    check("putUInt", prefs.putUInt("u", 4000000000UL) == 4);
    check("getUInt", prefs.getUInt("u", 0) == 4000000000UL);
    check("putLong64", prefs.putLong64("l", -9000000000LL) == 8);
    check("getLong64", prefs.getLong64("l", 0) == -9000000000LL);
    check("putString", prefs.putString("s", "hello world") == 11);
    check("getString", prefs.getString("s", "") == "hello world");
    check("getType", prefs.getType("s") == PT_STR);
    check("getType invalid", prefs.getType("nope") == PT_INVALID);

    uint8_t blob[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    check("putBytes", prefs.putBytes("b", blob, 4) == 4);
    check("getBytesLength", prefs.getBytesLength("b") == 4);
    uint8_t out[8] = {0};
    check("getBytes", prefs.getBytes("b", out, 8) == 4 &&
                      memcmp(out, blob, 4) == 0);

    check("remove", prefs.remove("c"));
    check("removed", prefs.getType("c") == PT_INVALID);
    /* 6 writes (c,i,u,l,s,b) - 1 remove = 5 */
    check("freeEntries", prefs.freeEntries() == 5);
    check("clear", prefs.clear());
    check("cleared", prefs.freeEntries() == 0);
    prefs.end();
}

static void test_persist()
{
    Serial.println("== Persistence ==");
    Preferences prefs;
    check("begin p", prefs.begin("persist"));
    int32_t boot_count = prefs.getInt("boots", 0);
    Serial.print("boot count: ");
    Serial.println(boot_count);
    check("increment", prefs.putInt("boots", boot_count + 1) == 4);
    check("readback", prefs.getInt("boots", 0) == boot_count + 1);
    prefs.end();
}

void setup()
{
    Serial.begin(2000000);
    delay(200);
    Serial.println("== StorageTest ==");

    test_fs();
    test_fopen();
    test_prefs();
    test_persist();

    Serial.print("RESULT: ");
    Serial.print(g_pass);
    Serial.print(" pass, ");
    Serial.print(g_fail);
    Serial.println(" fail");
}

void loop()
{
    delay(1000);
}
