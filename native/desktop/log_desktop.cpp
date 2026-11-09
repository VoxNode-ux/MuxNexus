#include "../core/remux_core.h"

#include <cstdarg>
#include <cstdio>

// Desktop logging sinks for muxnexus_core. Android provides its own in
// android/jni_bridge.cpp. Kept in a separate file so the test binaries link
// them too, not only the CLI executable.
namespace muxnexus {
void LogError(const char* tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s] ERROR: ", tag); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
    va_end(ap);
}
void LogInfo(const char* tag, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stdout, "[%s] ", tag); vfprintf(stdout, fmt, ap); fprintf(stdout, "\n");
    va_end(ap);
}
} // namespace muxnexus