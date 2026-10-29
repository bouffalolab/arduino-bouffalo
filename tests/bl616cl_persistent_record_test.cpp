#include "persistent_record.h"
#include <assert.h>

extern "C" int record_write(int mode, const void *value, size_t size)
{
    uint8_t header[5] = {};
    const unsigned offset = mode == 3 ? 1 : 0;
    if (offset) { header[0] = size == 6000 ? 8 : 9; }
    for (unsigned i = 0; i < 4; ++i) { header[offset+i] = size >> (8*i); }
    return bridge::persistent_record_write(mode == 3 ? "p/nvs/fault/m/value" : "s/nvs/fault/m/key",
                                           header, offset+4, value, size);
}

extern "C" size_t record_read(int mode, void *value, size_t capacity)
{
    const char *key = mode == 3 ? "p/nvs/fault/m/value" : "s/nvs/fault/m/key";
    uint8_t header[5] = {};
    const unsigned offset = mode == 3 ? 1 : 0;
    size_t total = 0;
    assert(ef_get_env_blob(key,header,offset+4,&total) == offset+4);
    size_t size = 0;
    for (unsigned i = 0; i < 4; ++i) { size |= size_t(header[offset+i]) << (8*i); }
    assert(size <= capacity && total == size+offset+4);
    if (offset) { assert(header[0] == (size == 6000 ? 8 : 9)); }
    assert(bridge::persistent_record_read(key,"missing-legacy",offset+4,size,value,0,size));
    return size;
}

extern "C" void record_legacy_tests()
{
    const char *m="p/nvs/legacy/m/scalar", *v="p/nvs/legacy/v/scalar";
    uint8_t header[5]={5,4,0,0,0};
    uint8_t old[4]={1,2,3,4}, replacement[8]={8,7,6,5,4,3,2,1},output[8]={};
    assert(ef_set_env_blob(m,header,sizeof header)==EF_NO_ERR);
    assert(ef_set_env_blob(v,old,sizeof old)==EF_NO_ERR);
    assert(bridge::persistent_record_read(m,v,5,4,output,0,4));
    assert(!memcmp(output,old,4));
    header[0]=7;header[1]=8;
    assert(bridge::persistent_record_write(m,header,sizeof header,replacement,8));
    assert(bridge::persistent_record_read(m,v,5,8,output,0,8));
    assert(!memcmp(output,replacement,8)); // The old v record must be ignored.
    assert(!bridge::persistent_record_read(m,v,5,7,output,0,7));
    header[0]=9;header[1]=0;
    assert(bridge::persistent_record_write(m,header,sizeof header,nullptr,0));
    assert(bridge::persistent_record_read(m,v,5,0,nullptr,0,0));
}
