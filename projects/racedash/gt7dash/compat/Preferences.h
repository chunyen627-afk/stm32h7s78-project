/* ESP32 的 Preferences（NVS）空殼。值存在 racedash 提供的儲存區（重置不會掉）。 */
#pragma once
#include "Arduino.h"

extern "C" int  racedash_pref_get(const char *key, unsigned char *value);   /* 有就回 1 */
extern "C" void racedash_pref_put(const char *key, unsigned char value);
extern "C" void racedash_pref_clear(void);

class Preferences
{
public:
    bool begin(const char *, bool = false) { return true; }
    void end() {}
    bool isKey(const char *key) { unsigned char v; return racedash_pref_get(key, &v) != 0; }
    uint8_t getUChar(const char *key, uint8_t def = 0) { unsigned char v; return racedash_pref_get(key, &v) ? v : def; }
    size_t putUChar(const char *key, uint8_t v) { racedash_pref_put(key, v); return 1; }
    bool clear() { racedash_pref_clear(); return true; }
};
