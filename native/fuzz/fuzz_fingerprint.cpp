#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "remux_checks.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    char path[] = "/tmp/muxnexus_fuzz_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    if (write(fd, data, size) != (ssize_t)size) { close(fd); unlink(path); return 0; }
    lseek(fd, 0, SEEK_SET);
    muxnexus::FingerprintFd(fd);
    close(fd);
    unlink(path);
    return 0;
}