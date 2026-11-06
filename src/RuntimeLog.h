#pragma once
#include <cstdarg>
#include <string_view>

namespace runtime_log {
// Only the normal application starts the worker. Producers do no file I/O.
void initialize();
void shutdown(); // Drain the queue; used by standalone verification tools.
void write(const char* format, ...);
void writeV(const char* format, va_list arguments);
void writeWide(std::wstring_view text);
}
