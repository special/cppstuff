/*
 * Copyright (c) 2017 Crimson AS <info@crimson.no>
 * Author: Robin Burchell <robin.burchell@crimson.no>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
// MAC
#include <unistd.h> // syscall()
#include <sys/syscall.h> // SYS_thread_selfid
// ENDMAC

#include <unordered_map>

#include "CSystrace.h"
#include "CTraceMessages.h"

// Information about SHM chunks
const int ShmChunkSize = 1024 * 10;
static thread_local int shm_fd = -1;
static thread_local char *shmInitialPtr = 0;
static thread_local char *shmPtr = 0;
static thread_local char *currentChunkName = 0;

// How much of the SHM chunk for this thread is left, in bytes?
static thread_local int remainingChunkSize;

// FD to communicate with traced
static int traced_fd = -1;

// Each thread registers unique strings as it comes across them here and sends a
// registration message to traced.
static std::atomic<uint64_t> currentStringId;
static thread_local std::unordered_map<const char *, uint64_t> registeredStrings;

//gettid(); except that mac sucks
static int gettid()
{
    return syscall(SYS_thread_selfid);
}

/*! Update the book keeping for the current position in the chunk.
 */
static void advance_chunk(int len)
{
    shmPtr += len;
    remainingChunkSize -= len;
    assert(remainingChunkSize >= 0);
}

/*!
 * Send the current chunk to traced for processing.
 *
 * ### right now, this will not be called if a thread terminates abruptly.
 * we should somehow monitor old, stale chunks and force-submit them.
 */
static void submit_chunk()
{
    if (shm_fd == -1)
        return;

    munmap(shmInitialPtr, ShmChunkSize);
    close(shm_fd);
    shm_fd = -1;
    shmPtr = 0;

    char buf[1024];
    int blen = sprintf(buf, "%s\n", currentChunkName);
    if (0) // left for debug purposes
        printf("TID %d sending %s", gettid(), buf);
    int ret = write(traced_fd, buf, blen);
    if (ret == -1) {
        // ### we also need to ignore SIGPIPE or clients will die if traced does.
        perror("Can't write to traced! Giving up!");
        shm_unlink(currentChunkName);
        close(traced_fd);
        traced_fd = -1;
    }
}

// When the trace started (when systrace_init was called).
// Do not modify this outside of systrace_init! It is read from multiple
// threads.
static struct timespec originalTp;
static uint64_t getMicroseconds()
{
    struct timespec tp;
    if (clock_gettime(CLOCK_MONOTONIC, &tp) == -1) {
        perror("Can't get time");
        abort();
    }

    return (tp.tv_sec - originalTp.tv_sec) * 1000000 +
           (tp.tv_nsec / 1000) - (originalTp.tv_nsec / 1000);
}


static void systrace_debug()
{
    static thread_local bool debugging = false;
    if (debugging)
        return;

    debugging = true;
    // These vars are to try avoid spurious reporting.
    static thread_local int lastRemainingChunkSize = 0;
    if (remainingChunkSize != lastRemainingChunkSize) {
        lastRemainingChunkSize = remainingChunkSize;
        systrace_record_counter("systrace",  "remainingChunkSize",  remainingChunkSize, gettid());
    }
    static uint64_t lastStringCount = 0;
    if (lastStringCount != currentStringId.load()) {
        lastStringCount = currentStringId.load();
        systrace_record_counter("systrace", "registeredStringCount", lastStringCount); // not thread-specific
    }
    debugging = false;
}

/*!
 * Make sure we have a valid SHM chunk to write events to, or abort if not.
 */
static void ensure_chunk(int mlen)
{
    if (shm_fd != -1 && remainingChunkSize >= mlen)
        return;

    if (shm_fd != -1) {
        submit_chunk();
    }

    // ### linux via /dev/shm or memfd_create?
    int nextShmId = 0;
    while (shm_fd == -1 && nextShmId < TRACED_MAX_SHM_CHUNKS) {
        if (!currentChunkName)
            asprintf(&currentChunkName, "tracechunk-%d", nextShmId++);

        shm_fd = shm_open(currentChunkName, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
        if (shm_fd != -1) {
            break; // we won!
        } else {
            // try again. either something is using that chunk name, or traced
            // hasn't released it for us to reuse yet.
            free(currentChunkName);
            currentChunkName = 0;
        }
    }

    if (shm_fd == -1) {
        fprintf(stderr, "Something is seriously screwed. Can't find any free SHM chunk, tried all %d\n", TRACED_MAX_SHM_CHUNKS);
        abort();
    }

    if (ftruncate(shm_fd, ShmChunkSize) == -1) {
        perror("Can't ftruncate SHM!");
        abort();
    }
    shmPtr = (char*)mmap(0, ShmChunkSize, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shmPtr == MAP_FAILED) {
        perror("Can't map SHM!");
        abort();
    }
    remainingChunkSize = ShmChunkSize;

    ChunkHeader *h = (ChunkHeader*)shmPtr;
    h->magic = TRACED_PROTOCOL_MAGIC;
    h->version = TRACED_PROTOCOL_VERSION;
    h->pid = getpid();
    h->tid = gettid();
    h->epoch = (originalTp.tv_sec * 1000000) +
               (originalTp.tv_nsec / 1000);
    advance_chunk(sizeof(ChunkHeader));
}

__attribute__((constructor)) void systrace_init()
{
    if (clock_gettime(CLOCK_MONOTONIC, &originalTp) == -1) {
        perror("Can't get time");
        abort();
    }

    if (getenv("TRACED") == NULL) {
        traced_fd = open("/tmp/traced", O_WRONLY);

        if ((traced_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
            perror("Can't create socket for traced!");
        }

        struct sockaddr_un remote;
        remote.sun_family = AF_UNIX;
        strcpy(remote.sun_path, "/tmp/traced");
        int len = strlen(remote.sun_path) + sizeof(remote.sun_family) + 1;
        if (connect(traced_fd, (struct sockaddr *)&remote, len) == -1) {
            perror("Can't connect to traced!");
        }
    } else {
        fprintf(stderr, "Running trace daemon. Not tracing.\n");
    }
}

__attribute__((destructor)) void systrace_deinit()
{
    if (traced_fd == -1)
        return;
    submit_chunk();
    close(traced_fd);
    traced_fd = -1;
}

int systrace_should_trace(const char *module)
{
    if (traced_fd == -1)
        return 0;
    // hack this if you want to temporarily omit some traces.
    return 1;
}

// ### should make use of process id, thread id, and maybe some uniqueness too
// just in case we repeat either PIDs or TIDs.
static uint64_t getStringId(const char *string)
{
    auto it = registeredStrings.find(string);
    if (it == registeredStrings.end()) {
        uint64_t nid = currentStringId.fetch_add(1);
        registeredStrings[string] = nid;

        int slen = strlen(string);
        assert(slen < ShmChunkSize / 100); // 102 characters, assuming 10kb
        ensure_chunk(sizeof(RegisterStringMessage) + slen);
        RegisterStringMessage *m = (RegisterStringMessage*)shmPtr;
        m->messageType = MessageType::RegisterStringMessage;
        m->id = nid;
        m->length = slen;
        strncpy(&m->stringData, string, slen);
        advance_chunk(sizeof(RegisterStringMessage) + slen);
        systrace_debug();
        return nid;
    }

    return it->second;
}

// ### use "X" events?
void systrace_duration_begin(const char *module, const char *tracepoint)
{
    if (!systrace_should_trace(module))
        return;

    uint64_t modid = getStringId(module);
    uint64_t tpid = getStringId(tracepoint);

    ensure_chunk(sizeof(BeginMessage));
    BeginMessage *m = (BeginMessage*)shmPtr;
    m->messageType = MessageType::BeginMessage;
    m->microseconds = getMicroseconds();
    m->categoryId = modid;
    m->tracepointId = tpid;
    advance_chunk(sizeof(BeginMessage));

    systrace_debug();
}

void systrace_duration_end(const char *module, const char *tracepoint)
{
    if (!systrace_should_trace(module))
        return;

    uint64_t modid = getStringId(module);
    uint64_t tpid = getStringId(tracepoint);

    ensure_chunk(sizeof(EndMessage));
    EndMessage *m = (EndMessage*)shmPtr;
    m->messageType = MessageType::EndMessage;
    m->microseconds = getMicroseconds();
    m->categoryId = modid;
    m->tracepointId = tpid;
    advance_chunk(sizeof(EndMessage));

    systrace_debug();
}

void systrace_record_counter(const char *module, const char *tracepoint, int value, int id)
{
    if (!systrace_should_trace(module))
        return;

    uint64_t modid = getStringId(module);
    uint64_t tpid = getStringId(tracepoint);

    if (id == -1) {
        ensure_chunk(sizeof(CounterMessage));
        CounterMessage *m = (CounterMessage*)shmPtr;
        m->messageType = MessageType::CounterMessage;
        m->microseconds = getMicroseconds();
        m->categoryId = modid;
        m->tracepointId = tpid;
        m->value = value;
        advance_chunk(sizeof(CounterMessage));
    } else {
        ensure_chunk(sizeof(CounterMessageWithId));
        CounterMessageWithId *m = (CounterMessageWithId*)shmPtr;
        m->messageType = MessageType::CounterMessageWithId;
        m->microseconds = getMicroseconds();
        m->categoryId = modid;
        m->tracepointId = tpid;
        m->value = value;
        m->id = id;
        advance_chunk(sizeof(CounterMessageWithId));
    }

    systrace_debug();
}

void systrace_async_begin(const char *module, const char *tracepoint, const void *cookie)
{
    if (!systrace_should_trace(module))
        return;

    uint64_t modid = getStringId(module);
    uint64_t tpid = getStringId(tracepoint);

    ensure_chunk(sizeof(AsyncBeginMessage));
    AsyncBeginMessage *m = (AsyncBeginMessage*)shmPtr;
    m->messageType = MessageType::AsyncBeginMessage;
    m->microseconds = getMicroseconds();
    m->categoryId = modid;
    m->tracepointId = tpid;
    m->cookie = (intptr_t)cookie;
    advance_chunk(sizeof(AsyncBeginMessage));

    systrace_debug();
}

void systrace_async_end(const char *module, const char *tracepoint, const void *cookie)
{
    if (!systrace_should_trace(module))
        return;

    uint64_t modid = getStringId(module);
    uint64_t tpid = getStringId(tracepoint);

    ensure_chunk(sizeof(AsyncEndMessage));
    AsyncEndMessage *m = (AsyncEndMessage*)shmPtr;
    m->messageType = MessageType::AsyncEndMessage;
    m->microseconds = getMicroseconds();
    m->categoryId = modid;
    m->tracepointId = tpid;
    m->cookie = (intptr_t)cookie;
    advance_chunk(sizeof(AsyncEndMessage));

    systrace_debug();
}


