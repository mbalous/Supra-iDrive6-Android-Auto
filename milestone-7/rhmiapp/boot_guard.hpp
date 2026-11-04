// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
//
// "Start with the car" bootloop guard: pure decision logic (host-tested in tests/test_boot_guard.cpp).
#ifndef NBT_AAD_BOOT_GUARD_HPP
#define NBT_AAD_BOOT_GUARD_HPP

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace nbt {
namespace bootguard {

const long kClockValidFrom = 1577836800L;   // 2020-01-01: earlier = RTC not set

// Keeps the recorded autostart times that lie within `window` seconds before `now` (future stamps dropped, they come
// from a clock that was later corrected). Writes up to maxOut of them to out, oldest first; returns how many.
inline int recent(const long* in, int n, long now, long window, long* out, int maxOut) {
    int k = 0;
    for (int i = 0; i < n && k < maxOut; ++i)
        if (in[i] <= now && now - in[i] < window) out[k++] = in[i];
    return k;
}

// true = this boot may autostart.
inline bool allows(int recentCount, int maxBoots) { return recentCount < maxBoots; }

enum DelayDecision { kWait, kReady, kClockError, kFinished };

inline DelayDecision delay(bool clockOk, const timespec& started, const timespec& now, long delaySeconds) {
    if (!clockOk || now.tv_sec < started.tv_sec ||
        (now.tv_sec == started.tv_sec && now.tv_nsec < started.tv_nsec)) return kClockError;
    long seconds = now.tv_sec - started.tv_sec;
    if (now.tv_nsec < started.tv_nsec) --seconds;
    return seconds < delaySeconds ? kWait : kReady;
}

class AutostartGate {
public:
    AutostartGate() : clockOk_(false), done_(false) { started_.tv_sec = 0; started_.tv_nsec = 0; }
    void init(bool clockOk, const timespec& started) { clockOk_ = clockOk; started_ = started; done_ = false; }
    bool done() const { return done_; }
    DelayDecision poll(bool clockOk, const timespec& now, long delaySeconds) {
        if (done_) return kFinished;
        DelayDecision result = delay(clockOk_ && clockOk, started_, now, delaySeconds);
        if (result != kWait) done_ = true;
        return result;
    }
private:
    timespec started_;
    bool clockOk_;
    bool done_;
};

enum Result { kAllowed, kInvalidClock, kLimit, kStorageError };

struct Io {
    ssize_t (*writeFile)(int, const void*, size_t);
    int (*syncFile)(int);
};

inline Result record(const char* path, long now, long window, int maxBoots, const Io& io, int* error) {
    *error = 0;
    if (now < kClockValidFrom) return kInvalidClock;
    long raw[8], kept[8];
    int count = 0;
    FILE* f = fopen(path, "r");
    if (!f && errno != ENOENT) { *error = errno; return kStorageError; }
    if (f) {
        char line[64];
        bool bad = false;
        while (fgets(line, sizeof(line), f)) {
            if (count == 8 || !strchr(line, '\n')) { bad = true; break; }
            char* end;
            errno = 0;
            long value = strtol(line, &end, 10);
            if (errno == ERANGE || end == line || value <= 0 || strcmp(end, "\n") != 0) { bad = true; break; }
            raw[count++] = value;
        }
        if (ferror(f)) { *error = errno ? errno : EIO; bad = true; }
        if (fclose(f) != 0) { *error = errno; bad = true; }
        if (!count) bad = true;
        if (bad) { if (!*error) *error = EINVAL; return kStorageError; }
    }
    int n = recent(raw, count, now, window, kept, 8);
    if (!allows(n, maxBoots)) return kLimit;

    char tmp[512], dir[512], contents[256];
    int len = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (len < 0 || len >= (int)sizeof(tmp)) { *error = ENAMETOOLONG; return kStorageError; }
    const char* slash = strrchr(path, '/');
    if (!slash || slash == path || slash - path >= (int)sizeof(dir)) { *error = EINVAL; return kStorageError; }
    memcpy(dir, path, slash - path);
    dir[slash - path] = '\0';
    size_t used = 0;
    for (int i = 0; i <= n; ++i) {
        len = snprintf(contents + used, sizeof(contents) - used, "%ld\n", i == n ? now : kept[i]);
        if (len < 0 || (size_t)len >= sizeof(contents) - used) { *error = EOVERFLOW; return kStorageError; }
        used += (size_t)len;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { *error = errno; return kStorageError; }
    size_t written = 0;
    while (written < used) {
        ssize_t w = io.writeFile(fd, contents + written, used - written);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { *error = w < 0 ? errno : EIO; break; }
        written += (size_t)w;
    }
    if (!*error && io.syncFile(fd) != 0) *error = errno;
    if (close(fd) != 0 && !*error) *error = errno;
    if (!*error && rename(tmp, path) != 0) *error = errno;
    if (*error) { unlink(tmp); return kStorageError; }

    // QNX 6.5 fsync requires a writable descriptor. On QNX6 it commits a filesystem snapshot,
    // including the preceding rename; elsewhere sync the containing directory.
#ifdef __QNX__
    int dfd = open(path, O_WRONLY);
#else
    int dfd = open(dir, O_RDONLY);
#endif
    if (dfd < 0) { *error = errno; return kStorageError; }
    if (io.syncFile(dfd) != 0) *error = errno;
    if (close(dfd) != 0 && !*error) *error = errno;
    return *error ? kStorageError : kAllowed;
}

}  // namespace bootguard
}  // namespace nbt

#endif
