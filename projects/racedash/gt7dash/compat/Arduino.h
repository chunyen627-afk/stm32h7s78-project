/* Arduino 相容層：讓原作（caa1211/esp32-gt7-dashboard）的儀表程式碼不改一行
 * 就能在 STM32（與電腦端參考渲染器）上編譯。只實作原作用到的部分。 */
#pragma once
#define RACEDASH_ARDUINO_COMPAT 1   /* LovyanGFX 看到這個才開 String 版本的 drawString */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <string>
#include <map>
#include <algorithm>
#include <type_traits>

typedef bool    boolean;
typedef uint8_t byte;

#define TWO_PI     6.283185307179586476925286766559
#define HALF_PI    1.5707963267948966192313216916398
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define PROGMEM
#define F(s) (s)
#define LOW  0
#define HIGH 1

unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
static inline void yield(void) {}

/* Arduino 的 min/max/constrain 是不挑型別的（int 跟 long 混用也行）。 */
/* 回傳「值」：寫成 decltype(a < b ? a : b) 的話兩個同型別參數會推成參照，
 * 回傳的是已經消失的參數 —— 實測讓 GT3 的檔位算出亂數座標、整個不見。 */
template <class A, class B> static inline typename std::common_type<A, B>::type min(A a, B b) { return a < b ? a : b; }
template <class A, class B> static inline typename std::common_type<A, B>::type max(A a, B b) { return a > b ? a : b; }
template <class T, class L, class H> static inline T constrain(T x, L lo, H hi) { return x < (T)lo ? (T)lo : (x > (T)hi ? (T)hi : x); }
static inline long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/* ---- String：行為照 Arduino（含 ESP32 core）的 WString ---- */
class String
{
public:
    String() {}
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    String(const String &o) = default;
    String &operator=(const String &o) = default;
    explicit String(char c) : s_(1, c) {}
    String(unsigned char v, unsigned char base = 10) { fromULong(v, base); }
    String(int v, unsigned char base = 10) { fromLong(v, base); }
    String(unsigned int v, unsigned char base = 10) { fromULong(v, base); }
    String(long v, unsigned char base = 10) { fromLong(v, base); }
    String(unsigned long v, unsigned char base = 10) { fromULong(v, base); }
    String(long long v, unsigned char base = 10) { fromLong((long)v, base); }
    String(unsigned long long v, unsigned char base = 10) { fromULong((unsigned long)v, base); }
    String(float v, unsigned int decimals = 2) { fromDouble(v, decimals); }
    String(double v, unsigned int decimals = 2) { fromDouble(v, decimals); }

    const char *c_str() const { return s_.c_str(); }
    unsigned int length() const { return (unsigned int)s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    char charAt(unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
    char operator[](unsigned int i) const { return charAt(i); }

    bool equals(const String &o) const { return s_ == o.s_; }
    bool equalsIgnoreCase(const String &o) const
    {
        if (s_.size() != o.s_.size()) { return false; }
        for (size_t i = 0; i < s_.size(); i++) { if (tolower((unsigned char)s_[i]) != tolower((unsigned char)o.s_[i])) { return false; } }
        return true;
    }
    bool startsWith(const String &p) const { return s_.compare(0, p.s_.size(), p.s_) == 0 && p.s_.size() <= s_.size(); }
    bool startsWith(const String &p, unsigned int off) const { return off <= s_.size() && s_.compare(off, p.s_.size(), p.s_) == 0; }
    bool endsWith(const String &p) const { return p.s_.size() <= s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0; }

    int indexOf(char c, unsigned int from = 0) const { size_t r = s_.find(c, from); return r == std::string::npos ? -1 : (int)r; }
    int indexOf(const String &t, unsigned int from = 0) const { size_t r = s_.find(t.s_, from); return r == std::string::npos ? -1 : (int)r; }
    int lastIndexOf(char c) const { size_t r = s_.rfind(c); return r == std::string::npos ? -1 : (int)r; }
    int lastIndexOf(const String &t) const { size_t r = s_.rfind(t.s_); return r == std::string::npos ? -1 : (int)r; }

    String substring(unsigned int b) const { return b >= s_.size() ? String() : String(s_.substr(b)); }
    String substring(unsigned int b, unsigned int e) const
    {
        if (b > e) { unsigned int t = b; b = e; e = t; }
        if (b >= s_.size()) { return String(); }
        if (e > s_.size()) { e = (unsigned int)s_.size(); }
        return String(s_.substr(b, e - b));
    }

    long toInt() const { return atol(s_.c_str()); }
    float toFloat() const { return (float)atof(s_.c_str()); }
    double toDouble() const { return atof(s_.c_str()); }

    void trim()
    {
        size_t b = 0, e = s_.size();
        while (b < e && isspace((unsigned char)s_[b])) { b++; }
        while (e > b && isspace((unsigned char)s_[e - 1])) { e--; }
        s_ = s_.substr(b, e - b);
    }
    void toUpperCase() { for (auto &c : s_) { c = (char)toupper((unsigned char)c); } }
    void toLowerCase() { for (auto &c : s_) { c = (char)tolower((unsigned char)c); } }
    void replace(const String &f, const String &r)
    {
        if (f.s_.empty()) { return; }
        size_t p = 0;
        while ((p = s_.find(f.s_, p)) != std::string::npos) { s_.replace(p, f.s_.size(), r.s_); p += r.s_.size(); }
    }
    void replace(char f, char r) { for (auto &c : s_) { if (c == f) { c = r; } } }
    void remove(unsigned int i) { if (i < s_.size()) { s_.erase(i); } }
    void remove(unsigned int i, unsigned int n) { if (i < s_.size()) { s_.erase(i, n); } }
    void reserve(unsigned int n) { s_.reserve(n); }
    void toCharArray(char *buf, unsigned int n) const { if (n) { strncpy(buf, s_.c_str(), n - 1); buf[n - 1] = 0; } }

    String &operator+=(const String &o) { s_ += o.s_; return *this; }
    String &operator+=(const char *o) { s_ += (o ? o : ""); return *this; }
    String &operator+=(char c) { s_ += c; return *this; }
    String &operator+=(int v) { return *this += String(v); }
    String &operator+=(unsigned int v) { return *this += String(v); }
    String &operator+=(long v) { return *this += String(v); }
    String &operator+=(unsigned long v) { return *this += String(v); }
    String &operator+=(float v) { return *this += String(v); }
    String &operator+=(double v) { return *this += String(v); }
    bool concat(const String &o) { s_ += o.s_; return true; }

    friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
    friend String operator+(const String &a, const char *b) { return String(a.s_ + (b ? b : "")); }
    friend String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.s_); }
    friend String operator+(const String &a, char c) { return String(a.s_ + c); }
    friend String operator+(const String &a, int v) { return a + String(v); }
    friend String operator+(const String &a, unsigned int v) { return a + String(v); }
    friend String operator+(const String &a, long v) { return a + String(v); }
    friend String operator+(const String &a, unsigned long v) { return a + String(v); }
    friend String operator+(const String &a, float v) { return a + String(v); }
    friend String operator+(const String &a, double v) { return a + String(v); }

    friend bool operator==(const String &a, const String &b) { return a.s_ == b.s_; }
    friend bool operator==(const String &a, const char *b) { return a.s_ == (b ? b : ""); }
    friend bool operator==(const char *a, const String &b) { return b == a; }
    friend bool operator!=(const String &a, const String &b) { return !(a == b); }
    friend bool operator!=(const String &a, const char *b) { return !(a == b); }
    friend bool operator!=(const char *a, const String &b) { return !(b == a); }
    friend bool operator<(const String &a, const String &b) { return a.s_ < b.s_; }
    friend bool operator>(const String &a, const String &b) { return a.s_ > b.s_; }

private:
    std::string s_;

    void fromLong(long v, unsigned char base)
    {
        if (base == 10) { char b[24]; snprintf(b, sizeof(b), "%ld", v); s_ = b; }
        else if (v < 0) { fromULong((unsigned long)(-v), base); s_ = "-" + s_; }
        else { fromULong((unsigned long)v, base); }
    }
    void fromULong(unsigned long v, unsigned char base)
    {
        char b[40]; int i = 39; b[i] = 0;
        if (v == 0) { b[--i] = '0'; }
        while (v) { int d = (int)(v % base); b[--i] = (char)(d < 10 ? '0' + d : 'A' + d - 10); v /= base; }
        s_ = &b[i];
    }
    /* Arduino 的 dtostrf 行為：四捨五入到指定位數。 */
    void fromDouble(double v, unsigned int decimals)
    {
        char b[48];
        if (isnan(v)) { s_ = "nan"; return; }
        if (isinf(v)) { s_ = "inf"; return; }
        snprintf(b, sizeof(b), "%.*f", (int)decimals, v);
        s_ = b;
    }
};

/* ---- Serial：沒有地方印，全部吃掉 ---- */
class HardwareSerialStub
{
public:
    void begin(unsigned long) {}
    template <class T> size_t print(const T &) { return 0; }
    template <class T> size_t println(const T &) { return 0; }
    size_t println() { return 0; }
    int printf(const char *, ...) { return 0; }
    operator bool() const { return true; }
};
extern HardwareSerialStub Serial;
