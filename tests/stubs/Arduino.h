#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstdarg>
#define OUTPUT 1
#define HIGH 1
#define LOW 0
static uint32_t clockMs = 0;
inline uint32_t millis() { return clockMs; }
inline void delay(unsigned n) { clockMs += n; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline bool psramFound() { return true; }
struct SerialStub {
    std::string input, output;
    void begin(unsigned) {}
    int available() { return static_cast<int>(input.size()); }
    int read() { auto ch = input[0]; input.erase(0, 1); return ch; }
    void print(char ch) { output += ch; }
    void println(const char* s) { output += s; output += '\n'; }
    void write(const uint8_t* p, size_t n) { output.append(reinterpret_cast<const char*>(p), n); }
    void printf(const char* format, ...) {
        char buffer[512]; va_list args; va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args); va_end(args); output += buffer;
    }
};
static SerialStub Serial;
