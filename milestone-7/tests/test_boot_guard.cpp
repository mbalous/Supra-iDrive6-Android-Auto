// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
#include "../rhmiapp/boot_guard.hpp"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>

using namespace nbt::bootguard;

static int syncCalls = 0;
static int failSyncAt = 0;
static int testSync(int fd) {
    if (++syncCalls == failSyncAt) { errno = EIO; return -1; }
    return fsync(fd);
}
static ssize_t failWrite(int, const void*, size_t) { errno = ENOSPC; return -1; }

static void put(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

int main() {
    char dir[] = "/tmp/nbt-boot-guard-XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    assert(snprintf(path, sizeof(path), "%s/bootguard", dir) > 0);
    Io io = { &write, &testSync };
    int error = 0;
    const long now = kClockValidFrom + 1000;

    timespec start = { 10, 500000000 }, before = { 25, 499999999 }, ready = { 25, 500000000 };
    assert(delay(true, start, before, 15) == kWait);
    assert(delay(true, start, ready, 15) == kReady);
    assert(delay(false, start, ready, 15) == kClockError);
    assert(delay(true, ready, start, 15) == kClockError);

    AutostartGate gate;
    gate.init(true, start);
    assert(gate.poll(true, before, 15) == kWait);
    assert(gate.poll(true, ready, 15) == kReady);
    assert(record(path, kClockValidFrom - 1, 900, 3, io, &error) == kInvalidClock);
    assert(gate.poll(true, ready, 15) == kFinished); // clock correction does not retry the skipped launch
    gate.init(false, start);
    assert(gate.poll(true, ready, 15) == kClockError);
    assert(gate.poll(true, ready, 15) == kFinished);
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    assert(record(path, kClockValidFrom, 900, 3, io, &error) == kAllowed);
    assert(unlink(path) == 0);
    for (int i = 0; i < 3; ++i) assert(record(path, now + i, 900, 3, io, &error) == kAllowed);
    assert(record(path, now + 3, 900, 3, io, &error) == kLimit);
    assert(record(path, now + 902, 900, 3, io, &error) == kAllowed);
    assert(unlink(path) == 0);

    put(path, "1577836900\n1577836901\n1577836902\n");
    assert(record(path, now, 900, 3, io, &error) == kAllowed); // exactly 900 seconds old is excluded
    put(path, "1577837801\n"); // future timestamp is discarded after clock correction
    assert(record(path, now, 900, 3, io, &error) == kAllowed);
    put(path, "1577837800\nbroken\n");
    assert(record(path, now, 900, 3, io, &error) == kStorageError && error == EINVAL);
    put(path, "1577837800");
    assert(record(path, now, 900, 3, io, &error) == kStorageError && error == EINVAL);
    put(path, "");
    assert(record(path, now, 900, 3, io, &error) == kStorageError && error == EINVAL);
    assert(unlink(path) == 0);
    assert(mkdir(path, 0755) == 0);
    assert(record(path, now, 900, 3, io, &error) == kStorageError);
    assert(rmdir(path) == 0);

    put(path, "1577837799\n");
    Io badWrite = { &failWrite, &testSync };
    assert(record(path, now, 900, 3, badWrite, &error) == kStorageError && error == ENOSPC);
    assert(record(path, now + 1, 900, 2, io, &error) == kAllowed); // previous history survived
    assert(unlink(path) == 0);
    syncCalls = 0; failSyncAt = 1;
    assert(record(path, now, 900, 3, io, &error) == kStorageError && error == EIO);
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    syncCalls = 0; failSyncAt = 2;
    assert(record(path, now, 900, 3, io, &error) == kStorageError && error == EIO);
    failSyncAt = 0;
    assert(record(path, now + 1, 900, 1, io, &error) == kLimit);
    assert(unlink(path) == 0); // manual reset
    assert(record(path, now + 1, 900, 1, io, &error) == kAllowed);
    assert(unlink(path) == 0);
    assert(rmdir(dir) == 0);
    puts("test_boot_guard OK");
    return 0;
}
