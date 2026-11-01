/*
 * transport_bench.cpp — the transport cells of the codec/transport isolation
 * experiment (benchmark/codec_transport/SPEC.md, "Experiment 2: transport").
 *
 *   transport_bench --which shm|uds --payload FILE [--chunk BYTES] [--in-flight N]
 *
 * One binary, two transports, ONE producer/consumer code path. The payload file
 * is mmap()ed read-only and each `--chunk` slice is published out of that
 * mapping straight away (nothing is copied into the producer). The consumer is
 * a fork()ed child, so the two sides are separate processes and each one
 * reports its own CPU time and its own peak RSS.
 *
 * --in-flight N (default 1) is the DEPTH OF THE SHM WINDOW: the number of chunks
 * the producer may publish ahead of the consumer's ack. The segment carries N
 * page-aligned chunk windows and a counting sem_t with N permits; a publish
 * takes a permit and a release returns it, so chunk seq reuses its window only
 * after chunk seq-N has been consumed. N = 1 is the original handshake
 * (publish, wait for the ack, touch the window again) with the wait moved to
 * just before the window is reused. The uds cell accepts the flag and reports
 * it in the JSON, but keeps one message in flight; the comment on uds_publish
 * says why.
 *
 * Producer and consumer are the SAME two loops for both transports; everything
 * that differs between the cells is one of the function pointers in struct
 * TpXport (open/publish/receive/release/close per side). That is what makes the
 * cells comparable: the difference between them is the transport, not the
 * harness.
 *
 * Copy accounting is part of the result (SPEC.md, "Copy accounting"):
 *
 *   shm: file page cache -> producer mapping   0  (the file is mmap()ed, so the
 *                                                 producer's view of the bytes
 *                                                 aliases the page cache)
 *        producer mapping -> shm segment       1  (memcpy into the window, the
 *                                                 producer's only copy)
 *        shm segment -> consumer               0  (the consumer gets a pointer
 *                                                 into the MAP_SHARED mapping
 *                                                 and folds the bytes in place)
 *   uds: file page cache -> producer mapping   0  (same mmap)
 *        producer mapping -> kernel socket buf 1  (send() copies the bytes)
 *        kernel socket buf -> consumer buffer  1  (recv() copies them out)
 *
 * The counters are incremented where the copies happen (x->copied_bytes on the
 * producer side, sh->recv_bytes on the consumer side) and the totals are
 * checked against the payload size, so the reported numbers are measured, not
 * asserted.
 *
 * The JSON is one line on stdout; extra keys are allowed by the SPEC and the
 * extras here say how each number was produced. `rows`/`rows_per_s` are null
 * and `checksum` is null: Experiment 2 moves opaque bytes, there are no rows to
 * count and no codec in the loop.
 */
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ tuning */

static const size_t TP_DEFAULT_CHUNK = 1u << 20; /* 1 MiB */
static const int    TP_WAIT_MS       = 10000;    /* every blocking wait has a deadline, like the ring's */
static const int    TP_SOCK_TIMEOUT  = 10;       /* seconds, SO_RCVTIMEO/SO_SNDTIMEO on the socketpair */

/* The deepest shm window --in-flight accepts. The control block carries one
 * 8-byte length per window, and 256 of them still fit in the page before the
 * data (see TP_SHM_DATA_OFF), so the limit is the layout, not the protocol. */
static const int TP_MAX_IN_FLIGHT = 256;
static const int TP_DEFAULT_IN_FLIGHT = 1; /* one chunk in flight: the original handshake */

/* The shm segment: control block at offset 0, then in_flight chunk windows,
 * each page-aligned (the producer's stores to the control must not share a
 * cache line with the payload it is copying, and window i+1 must not share one
 * with window i). */
static const size_t   TP_SHM_DATA_OFF = 4096;
/* 8 bytes, NUL-padded — the same shape as FASTPOSTGRES_RING_MAGIC ("FPRING02"). */
static const char     TP_SHM_MAGIC[8]  = "FPTP01";
static const uint32_t TP_SHM_VERSION   = 1;

/* Seed of the stream fold (the FNV-1a 64 offset basis). */
static const uint64_t TP_FOLD_SEED = 0xcbf29ce484222325ull;

/* ------------------------------------------------------------------- timing */

/* CLOCK_MONOTONIC is a system-wide clock: it is per boot, not per process, so a
 * timestamp taken in the consumer can be subtracted from one taken in the
 * producer. That is exactly what the SPEC's per-chunk latency is: the
 * consumer's "received" reading minus the producer's "published" reading. */
static inline uint64_t
tp_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline double
tp_ms(uint64_t ns)
{
    return (double)ns / 1e6;
}

static inline void
tp_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

/* Blocking sem_timedwait with a deadline, mirroring worker.c's ring wait loop. */
static int
tp_sem_wait(sem_t *s, const char *what, char *err, size_t errsz)
{
    struct timespec ts;
    int             rc;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += TP_WAIT_MS / 1000;
    ts.tv_nsec += (long)(TP_WAIT_MS % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L)
    {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    do
    {
        rc = sem_timedwait(s, &ts);
    } while (rc != 0 && errno == EINTR);

    if (rc != 0)
    {
        snprintf(err, errsz, "%s: gave up after %d ms (%s)", what, TP_WAIT_MS, strerror(errno));
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ cpu and rss */

struct TpCpu
{
    uint64_t user_ns;
    uint64_t sys_ns;
};

static inline uint64_t
tp_tv_ns(const struct timeval &tv)
{
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

static inline TpCpu
tp_cpu_delta(const struct rusage &a, const struct rusage &b)
{
    TpCpu c;
    c.user_ns = tp_tv_ns(b.ru_utime) - tp_tv_ns(a.ru_utime);
    c.sys_ns  = tp_tv_ns(b.ru_stime) - tp_tv_ns(a.ru_stime);
    return c;
}

/* Peak resident set (VmHWM, in KiB): the SPEC asks for the peak of both sides,
 * so each process reports its own and the caller takes the max. */
static uint64_t
tp_vmhwm_kb(void)
{
    FILE    *f = fopen("/proc/self/status", "r");
    char     line[256];
    uint64_t kb = 0;

    if (!f) return 0;
    while (fgets(line, sizeof line, f))
    {
        if (strncmp(line, "VmHWM:", 6) == 0)
        {
            kb = strtoull(line + 6, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

/* --------------------------------------------------------- stream fold ---- */

/* A cheap, chunk-boundary-independent fold of the byte stream: every 8-byte
 * word is mixed with its absolute word index, so a lost word, a reordering or a
 * truncation changes the value, and the same payload yields the same value for
 * any slice size (non-final slices are 8-byte aligned, see main()).
 *
 * It has two jobs: (1) it makes the consumer actually touch the bytes it
 * received — for shm it reads them straight out of the mapping; (2) the
 * producer folds the whole file once BEFORE the timed region, so "the bytes
 * that arrived are the bytes in the file" is a check and not a claim.
 *
 * It is not a cryptographic checksum: ~1 load + 1 add + 1 xor per 8 bytes,
 * deliberately far cheaper than the transports so it does not decide the
 * comparison.
 */
static inline uint64_t
tp_fold(const void *p, size_t n, uint64_t h, uint64_t base_word)
{
    const uint8_t *b  = (const uint8_t *)p;
    size_t         nw = n >> 3;

    for (size_t i = 0; i < nw; i++)
    {
        uint64_t w;
        memcpy(&w, b + 8 * i, 8); /* unaligned-safe */
        h ^= w + (base_word + i) * 0x9e3779b97f4a7c15ull;
    }

    if (n & 7) /* only the file's last slice has a tail */
    {
        uint64_t w = 0;
        memcpy(&w, b + 8 * nw, n & 7);
        h ^= w + (base_word + nw) * 0x9e3779b97f4a7c15ull;
    }
    return h;
}

/* ------------------------------------------------- shared memory: control -- */

/* Mirrors FastpostgresRing (pg-extension/src/ring.h):
 *
 *   magic/version      — a stale or foreign segment is refused, like FPRING02
 *   seq                — the sequence counter: bumped once per published chunk
 *                        with a RELEASE store, read with an ACQUIRE load, in
 *                        the same place req_seq/resp_seq sit in the ring
 *   len[]              — the per-window chunk length, len[seq % in_flight],
 *                        stored before the seq bump
 *   pub                — sem_t doorbell (sem_post / sem_timedwait), the same
 *                        mechanism the ring uses for req_ready/resp_ready
 *   free_sem           — counting sem_t with `in_flight` permits: the producer
 *                        takes one before it touches a window and the consumer
 *                        returns one with its ack. That is the window: at most
 *                        in_flight chunks are unacked, and window seq % N is
 *                        reused only after chunk seq-N has been released. With
 *                        in_flight = 1 this is the original publish/ack doorbell
 *                        (there is exactly one permit, so the producer waits for
 *                        the consumer's ack before the next publish).
 *
 * Why a sem_t and not a futex or an atomic spin: the ring's doorbell is a
 * semaphore, and a sleeping waiter is the right shape here — a spinning waiter
 * burns a core (visible as cpu_sys_ms) and, under the SPEC's "one measurement
 * at a time" rule, makes noise for whatever the neighbours are measuring. The
 * counter still carries the publication (length + sequence); the semaphore only
 * carries "look now".
 *
 * The ring keeps the batch in a separate segment; one segment is enough here
 * because this cell has exactly one reader and one writer.
 */
struct TpShmCtl
{
    char     magic[8];  /* "FPTP01" */
    uint32_t version;
    uint32_t reserved;
    uint64_t seq;      /* producer -> consumer: chunks published (RELEASE) */
    uint64_t ack;      /* consumer -> producer: chunks consumed (RELEASE) */
    uint64_t len[TP_MAX_IN_FLIGHT]; /* bytes in window seq % in_flight */
    sem_t    pub;      /* producer -> consumer doorbell */
    sem_t    free_sem; /* in_flight permits: -1 per publish, +1 per ack */
};

/* -------------------------------------------- shared memory: bookkeeping --- */

/* Per-chunk publish timestamp: written by the producer, read by the consumer.
 *
 * PAIRING. The array is indexed by the chunk ordinal. Both sides count chunks
 * in order, so the consumer's chunk number i is paired with slot i no matter
 * how many chunks are in flight (the slots are bookkeeping, not the transport
 * window: one slot per chunk, all of them alive for the whole run). `seq` is
 * what validates that pairing: the consumer waits until it reads slot.seq == i,
 * which cannot happen for a stale or future slot. The store is RELEASE and the
 * load is ACQUIRE, so a consumer that sees the sequence also sees the timestamp
 * it belongs to.
 *
 * The chunk can reach the consumer before the producer's slot store lands (the
 * publish happens first, the slot is written after the publish call returns),
 * so the consumer may spin briefly here — the spin is counted and reported, and
 * it happens after the receive timestamp is recorded, so it cannot pollute the
 * latency sample. */
struct TpTsSlot
{
    uint64_t seq;
    uint64_t t_pub_ns;
};

/* The consumer's report. Anonymous MAP_SHARED memory allocated before the fork
 * — this is bookkeeping, not transport: neither cell moves payload or control
 * through it. It carries what only the child can know (its own CPU, its own
 * peak RSS, the latencies it computed) since the SPEC asks each side to report
 * its own CPU. */
struct TpShared
{
    uint32_t consumer_ready;  /* consumer's end is set up (RELEASE) */
    uint32_t consumer_failed; /* 1 = see err */
    char     err[256];
    uint64_t cpu_user_ns;  /* getrusage(RUSAGE_SELF) delta measured IN the child */
    uint64_t cpu_sys_ns;
    uint64_t peak_rss_kb; /* VmHWM in the child */
    uint64_t recv_bytes;  /* bytes the consumer actually took out of the transport */
    uint64_t recv_chunks;
    uint64_t integrity_ok; /* 0 = a length or edge check failed */
    uint64_t fold;         /* in-place fold over everything received */
    uint64_t first_recv_ns;
    uint64_t last_recv_ns;
    uint64_t ts_slot_spins;
    uint64_t lat_inverted; /* samples where t_recv < t_pub (see consumer_loop) */
    uint64_t lat_min_ns;
    uint64_t lat_p50_ns;
    uint64_t lat_p99_ns;
    uint64_t lat_max_ns;
    uint64_t lat_sum_ns;
};

/* ------------------------------------------------------------- transport -- */

struct TpXport;

struct TpXport
{
    const char *name; /* "shm" | "uds" */

    /* both sides */
    size_t chunk;         /* slice size actually used (8-byte aligned) */
    size_t payload_bytes; /* the whole file */
    int    in_flight;     /* window depth: shm --in-flight N, uds always 1 */

    /* uds */
    int      fd;       /* this side's end of the socketpair (-1 if unused) */
    int      other_fd; /* the consumer's end while the producer holds it */
    uint8_t *rbuf;     /* consumer only: preallocated receive buffer */
    int      sock_buf; /* granted SO_SNDBUF, for the JSON */

    /* shm */
    char      seg_name[64]; /* "/fastpostgres_tp_<pid>" */
    size_t    seg_bytes;
    size_t    window_stride; /* bytes between window i and i+1 (page-rounded chunk) */
    void     *map; /* this side's MAP_SHARED mapping of the segment */
    TpShmCtl *ctl; /* == map (control block at offset 0) */
    uint8_t  *data; /* == map + TP_SHM_DATA_OFF (window 0) */

    /* the pairing slot the producer is publishing into right now (see
     * tp_pair_publish); the consumer loop owns the array itself */
    TpTsSlot *pair;

    /* the only per-transport code: the two loops below never change */
    int  (*open_producer)(TpXport *x);
    int  (*open_consumer)(TpXport *x, TpShared *sh);
    int  (*publish)(TpXport *x, const void *src, size_t len, uint64_t seq, uint64_t *t_pub);
    int  (*receive)(TpXport *x, uint64_t seq, size_t expected, const void **view, size_t *len, uint64_t *t_recv);
    /* "I am done with chunk seq": the consumer's ack. It MUST come after the
     * consumer has finished reading the bytes, not when they arrive. */
    int  (*release)(TpXport *x, uint64_t seq);
    void (*close_producer)(TpXport *x);
    void (*close_consumer)(TpXport *x);

    uint64_t copied_bytes; /* producer-side copy counter, incremented where the copy is */

    char err[256];
};

static void
tp_seterr(TpXport *x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->err, sizeof x->err, fmt, ap);
    va_end(ap);
}

static inline uint8_t *
tp_shm_data(void *map)
{
    return (uint8_t *)map + TP_SHM_DATA_OFF;
}

/* Publish the pairing slot for chunk `seq`, from inside the transport's publish
 * path and IMMEDIATELY after the publish timestamp is taken: the timestamp
 * first, then seq with a RELEASE store. Doing it here (rather than after the
 * publish call returns) is what keeps the consumer's pairing spin at zero
 * iterations — the consumer may only look for the slot once the chunk has
 * arrived, and by then the slot is already there. */
static inline void
tp_pair_publish(TpXport *x, uint64_t seq, uint64_t t_pub)
{
    x->pair->t_pub_ns = t_pub;
    __atomic_store_n(&x->pair->seq, seq, __ATOMIC_RELEASE);
}

/* ============================================================ shm ========= */

static int
shm_open_producer(TpXport *x)
{
    char           name[64];
    size_t         stride, want;
    struct statvfs vfs;
    int            fd;
    void          *map;
    TpShmCtl      *c;

    snprintf(x->seg_name, sizeof x->seg_name, "/fastpostgres_tp_%d", (int)getpid());
    snprintf(name, sizeof name, "%s", x->seg_name);

    if (x->in_flight < 1 || x->in_flight > TP_MAX_IN_FLIGHT)
    {
        tp_seterr(x, "shm: in-flight window is %d, must be 1..%d", x->in_flight, TP_MAX_IN_FLIGHT);
        return -1;
    }
    if (sizeof(TpShmCtl) > TP_SHM_DATA_OFF)
    {
        tp_seterr(x, "shm: control block does not fit before the windows");
        return -1;
    }
    /* N page-aligned windows: the producer's stores to a window's control (the
     * length) and to the next window's payload never share a cache line. */
    if (x->chunk > SIZE_MAX - 4095)
    {
        tp_seterr(x, "shm: chunk %zu is not a size", x->chunk);
        return -1;
    }
    stride = (x->chunk + 4095) & ~(size_t)4095;
    if ((size_t)x->in_flight > (SIZE_MAX - TP_SHM_DATA_OFF) / stride)
    {
        tp_seterr(x, "shm: %d windows of %zu bytes overflow the segment size", x->in_flight, stride);
        return -1;
    }
    want = TP_SHM_DATA_OFF + stride * (size_t)x->in_flight;
    x->window_stride = stride;
    /* /dev/shm is a tmpfs; in the dev image it is 64 MiB. Fail with a clear
     * message instead of at the first touch. */
    if (statvfs("/dev/shm", &vfs) == 0)
    {
        unsigned long long avail = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
        if ((unsigned long long)want > avail)
        {
            tp_seterr(x, "shm: need %zu bytes for %d x %zu-byte windows, /dev/shm has %llu free", want, x->in_flight,
                      x->chunk, avail);
            return -1;
        }
    }

    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        tp_seterr(x, "shm: shm_open(%s): %s", name, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)want) != 0)
    {
        tp_seterr(x, "shm: ftruncate(%s, %zu): %s", name, want, strerror(errno));
        close(fd);
        shm_unlink(name);
        return -1;
    }
    map = mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
    {
        tp_seterr(x, "shm: mmap(%s): %s", name, strerror(errno));
        shm_unlink(name);
        return -1;
    }

    /* Fresh segment (O_EXCL): zero it and initialise the semaphores BEFORE the
     * consumer exists, which is the ring's no-init-race rule (Ring_init creates
     * /dev/shm/<seg>, zeroes it and sem_init()s before the worker is booted).
     * free_sem starts with one permit per window: the producer may fill the
     * whole window before it has anything to wait for. */
    memset(map, 0, want);
    c = (TpShmCtl *)map;
    if (sem_init(&c->pub, 1, 0) != 0 || sem_init(&c->free_sem, 1, (unsigned)x->in_flight) != 0)
    {
        tp_seterr(x, "shm: sem_init: %s", strerror(errno));
        munmap(map, want);
        shm_unlink(name);
        return -1;
    }
    c->version = TP_SHM_VERSION;
    memcpy(c->magic, TP_SHM_MAGIC, 8);

    x->map       = map;
    x->seg_bytes = want;
    x->ctl       = c;
    x->data      = tp_shm_data(map);
    return 0;
}

static int
shm_open_consumer(TpXport *x, TpShared *sh)
{
    int       fd;
    void     *map;
    TpShmCtl *c;

    /* Drop the mapping the fork inherited and open the segment again: this is
     * what the repo's worker does (ring_map_rw) and it keeps the cell honest —
     * the consumer really talks to the segment, it does not secretly reuse the
     * producer's mapping. Same physical pages, by construction. */
    if (x->map)
    {
        munmap(x->map, x->seg_bytes);
        x->map = nullptr;
        x->ctl = nullptr;
        x->data = nullptr;
    }

    fd = shm_open(x->seg_name, O_RDWR, 0600);
    if (fd < 0)
    {
        tp_seterr(x, "shm: consumer shm_open(%s): %s", x->seg_name, strerror(errno));
        return -1;
    }
    map = mmap(nullptr, x->seg_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
    {
        tp_seterr(x, "shm: consumer mmap(%s): %s", x->seg_name, strerror(errno));
        return -1;
    }
    c = (TpShmCtl *)map;
    if (memcmp(c->magic, TP_SHM_MAGIC, 8) != 0 || c->version != TP_SHM_VERSION)
    {
        tp_seterr(x, "shm: consumer found no FPTP%02u segment in %s", TP_SHM_VERSION, x->seg_name);
        munmap(map, x->seg_bytes);
        return -1;
    }
    x->map  = map;
    x->ctl  = c;
    x->data = tp_shm_data(map);

    /* The ring's worker_ready: the producer waits for this before publishing. */
    __atomic_store_n(&sh->consumer_ready, 1, __ATOMIC_RELEASE);
    return 0;
}

static int
shm_publish(TpXport *x, const void *src, size_t len, uint64_t seq, uint64_t *t_pub)
{
    TpShmCtl *c    = x->ctl;
    size_t    slot = (size_t)(seq % (uint64_t)x->in_flight);
    uint8_t  *win;

    /* The window: exactly x->in_flight permits exist, one is taken per publish
     * and one is returned by shm_release(). Waiting BEFORE the window is
     * touched is what bounds the unacked chunks. At in_flight = 1 there is one
     * permit, so this is the original "publish, wait for the consumer's ack
     * before reusing the window", at the point where the window is about to be
     * reused (the last ack is still drained below, so the default cell waits
     * exactly where the original did). At in_flight > 1 the producer may fill
     * the whole window before its first wait, which is the overlap the flag
     * exists to measure. */
    if (tp_sem_wait(&c->free_sem, "shm: waiting for a free chunk window", x->err, sizeof x->err) != 0) return -1;

    /* Taking permit number seq+1 (the first in_flight are the initial count)
     * guarantees the consumer has released at least seq + 1 - in_flight chunks
     * — otherwise the permit would not have been posted. Check it anyway, so a
     * protocol drift fails the run instead of overwriting a chunk the consumer
     * is still reading. */
    if (__atomic_load_n(&c->ack, __ATOMIC_ACQUIRE) + (uint64_t)x->in_flight <= seq)
    {
        tp_seterr(x, "shm: ack is %llu, need at least %llu before chunk %llu can reuse its window",
                  (unsigned long long)__atomic_load_n(&c->ack, __ATOMIC_ACQUIRE),
                  (unsigned long long)(seq + 1 - (uint64_t)x->in_flight), (unsigned long long)seq);
        return -1;
    }

    win = x->data + slot * x->window_stride;

    /* COPY #1 of 1: the payload mapping -> the segment window. The source is the
     * read-only mapping of the file, so the producer has no staging buffer and
     * nothing is copied into it. */
    memcpy(win, src, len);
    x->copied_bytes += len;

    /* Publish in the ring's order: payload -> length -> sequence counter
     * (RELEASE) -> doorbell. A consumer that sees the counter with an ACQUIRE
     * load has seen the bytes and the length of THIS window, because the length
     * store is released by the same bump. */
    __atomic_store_n(&c->len[slot], (uint64_t)len, __ATOMIC_RELAXED);
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    *t_pub = tp_now_ns(); /* the SPEC's "right after publishing" */
    tp_pair_publish(x, seq, *t_pub);
    sem_post(&c->pub);

    /* The original cell waited for the ack of the FINAL chunk too, and the run
     * should not end with a window still in flight, so the last publish drains
     * its ack explicitly: at in_flight = 1 this is exactly the end-of-run wait
     * the old code did. With a wider window some permits from earlier acks are
     * still around, so the loop waits until the ack COUNTER reaches seq+1, not
     * merely until one permit is available. The consumer acks in order and is
     * always draining, so each iteration either sees ack == seq+1 or blocks
     * until the next release posts a permit. */
    if (seq + 1 == ((uint64_t)x->payload_bytes + (uint64_t)x->chunk - 1) / (uint64_t)x->chunk)
    {
        while (__atomic_load_n(&c->ack, __ATOMIC_ACQUIRE) != seq + 1)
        {
            if (tp_sem_wait(&c->free_sem, "shm: waiting for the final ack", x->err, sizeof x->err) != 0) return -1;
        }
    }
    return 0;
}

static int
shm_receive(TpXport *x, uint64_t seq, size_t expected, const void **view, size_t *len, uint64_t *t_recv)
{
    TpShmCtl *c = x->ctl;
    uint64_t  got;
    size_t    n, slot;

    if (tp_sem_wait(&c->pub, "shm: waiting for a chunk", x->err, sizeof x->err) != 0) return -1;

    /* The doorbell is posted once per publish, in order, so this wait means at
     * least seq+1 chunks have been published; with a window they may be ahead
     * of us by up to in_flight. Either bound being violated means the two sides
     * have drifted apart (the counter is not being consumed one chunk at a time
     * any more). */
    got = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    if (got < seq + 1)
    {
        tp_seterr(x, "shm: chunk %llu has not been published (seq is %llu)", (unsigned long long)seq,
                  (unsigned long long)got);
        return -1;
    }
    if (got > seq + (uint64_t)x->in_flight)
    {
        tp_seterr(x, "shm: seq is %llu, more than %d chunks ahead of chunk %llu", (unsigned long long)got, x->in_flight,
                  (unsigned long long)seq);
        return -1;
    }

    /* The length of this chunk, released by the seq bump that doorbell'd us. */
    slot = (size_t)(seq % (uint64_t)x->in_flight);
    n    = (size_t)__atomic_load_n(&c->len[slot], __ATOMIC_RELAXED);
    if (n != expected)
    {
        tp_seterr(x, "shm: chunk %llu is %zu bytes, expected %zu", (unsigned long long)seq, n, expected);
        return -1;
    }

    /* ZERO COPY on the consumer side: the chunk is read where the producer left
     * it. This pointer aliases the MAP_SHARED segment; the consumer folds the
     * bytes through it and nothing is ever memcpy()d out.
     *
     * The window belongs to the producer until x->release() is called: the ack
     * moved there because handing the chunk back at arrival would let the
     * producer overwrite it while the consumer is still reading it. With a
     * window of N, the producer can only reach this window again at chunk
     * seq+N, and free_sem holds it back until this release. */
    *view = x->data + slot * x->window_stride;
    *len  = n;
    *t_recv = tp_now_ns(); /* the SPEC's "right after receiving" */
    return 0;
}

/* The consumer is done with chunk `seq`: release the window so the producer may
 * reuse it (at chunk seq + in_flight). This is the consumer's ack, the mirror
 * of the ring's resp_seq + sem_post(resp_ready), and it returns one of the
 * window's permits. */
static int
shm_release(TpXport *x, uint64_t seq)
{
    TpShmCtl *c = x->ctl;

    (void)seq;
    __atomic_add_fetch(&c->ack, 1, __ATOMIC_RELEASE);
    sem_post(&c->free_sem);
    return 0;
}

static void
shm_close_producer(TpXport *x)
{
    if (x->map)
    {
        TpShmCtl *c = x->ctl;
        if (c)
        {
            sem_destroy(&c->pub);
            sem_destroy(&c->free_sem);
        }
        munmap(x->map, x->seg_bytes);
        x->map = nullptr;
    }
    if (x->seg_name[0]) shm_unlink(x->seg_name);
}

static void
shm_close_consumer(TpXport *x)
{
    if (x->map)
    {
        munmap(x->map, x->seg_bytes);
        x->map = nullptr;
    }
}

/* ============================================================ uds ========= */

static int
uds_open_producer(TpXport *x)
{
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
    {
        tp_seterr(x, "uds: socketpair: %s", strerror(errno));
        return -1;
    }

    /* AF_UNIX SEQPACKET refuses a message bigger than the sending socket's
     * SO_SNDBUF (the default 212992-byte buffer answers a 1 MiB send with
     * EMSGSIZE), and a receiver whose SO_RCVBUF is smaller than the message
     * would see it truncated. So both ends are sized for one chunk plus a
     * little. Both are clamped by net.core.wmem_max / net.core.rmem_max. */
    int want = (int)std::min<size_t>(x->chunk + 65536, (size_t)(1 << 30));
    for (int i = 0; i < 2; i++)
    {
        struct timeval tv;
        tv.tv_sec  = TP_SOCK_TIMEOUT;
        tv.tv_usec = 0;
        if (setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &want, sizeof want) != 0 ||
            setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &want, sizeof want) != 0)
        {
            tp_seterr(x, "uds: setsockopt(SO_SNDBUF/SO_RCVBUF = %d): %s", want, strerror(errno));
            return -1;
        }
        /* Blocking with a deadline, so a broken peer cannot wedge the run. */
        setsockopt(sv[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(sv[i], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    /* The message has to fit on both sides: the sender's SO_SNDBUF decides
     * whether the message can be sent at all, the receiver's SO_RCVBUF decides
     * whether it arrives whole. Both are clamped by net.core.wmem_max /
     * net.core.rmem_max, so an oversized --chunk is reported here instead of
     * failing later with EMSGSIZE or a silently truncated message. */
    int       eff_snd = 0, eff_rcv = 0;
    socklen_t el      = sizeof eff_snd;
    getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &eff_snd, &el);
    el = sizeof eff_rcv;
    getsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &eff_rcv, &el);
    x->sock_buf = eff_rcv;
    if ((size_t)eff_snd < x->chunk + 4096)
    {
        tp_seterr(x, "uds: SO_SNDBUF is only %d bytes for a %zu-byte chunk: raise net.core.wmem_max or lower --chunk",
                  eff_snd, x->chunk);
        return -1;
    }
    if ((size_t)eff_rcv < x->chunk + 4096)
    {
        tp_seterr(x, "uds: SO_RCVBUF is only %d bytes for a %zu-byte chunk: raise net.core.rmem_max or lower --chunk",
                  eff_rcv, x->chunk);
        return -1;
    }

    x->fd       = sv[0];
    x->other_fd = sv[1];
    return 0;
}

static int
uds_open_consumer(TpXport *x, TpShared *sh)
{
    close(x->fd); /* the producer's end is not ours */
    x->fd       = x->other_fd;
    x->other_fd = -1;

    /* Preallocated once: recv() writes the message straight into it. */
    x->rbuf = (uint8_t *)malloc(x->chunk);
    if (!x->rbuf)
    {
        tp_seterr(x, "uds: malloc(%zu) for the receive buffer failed", x->chunk);
        return -1;
    }
    __atomic_store_n(&sh->consumer_ready, 1, __ATOMIC_RELEASE);
    return 0;
}

static int
uds_publish(TpXport *x, const void *src, size_t len, uint64_t seq, uint64_t *t_pub)
{
    ssize_t n;
    uint8_t ack;

    (void)seq; /* the ordinal is not needed on the wire: one message is in flight */

    /* --in-flight is accepted for both cells but only the shm cell uses it to
     * widen its window. Here the ack below keeps exactly one message in flight
     * and the socketpair is sized for one chunk, so a wider window cannot be
     * expressed by this cell without changing it: the kernel buffer would queue
     * a second message only if it were sent, and the producer never sends it
     * before the ack. The JSON therefore reports in_flight = 1 for uds (plus
     * in_flight_requested, the flag as given), which records what actually ran
     * instead of what was asked for. */

    /* COPY #1 of 2: user -> kernel socket buffer. send() reads straight out of
     * the payload mapping; the producer never materialises the chunk. */
    n = send(x->fd, src, len, MSG_NOSIGNAL);
    if (n != (ssize_t)len)
    {
        tp_seterr(x, "uds: send of %zu bytes returned %zd: %s", len, n, n < 0 ? strerror(errno) : "short write");
        return -1;
    }
    *t_pub = tp_now_ns(); /* the SPEC's "right after publishing" */
    tp_pair_publish(x, seq, *t_pub);
    x->copied_bytes += len;

    /* One message in flight, the same shape as the shm cell at --in-flight 1:
     * the consumer sends a single byte back once it is done with the chunk (see
     * uds_release) and the producer waits for it before sending the next one.
     * Unlike shm, this cell does not widen with --in-flight; see the comment at
     * the top of this function. */
    n = recv(x->fd, &ack, 1, 0);
    if (n != 1)
    {
        tp_seterr(x, "uds: waiting for the consumer's ack: %s", n < 0 ? strerror(errno) : "peer closed");
        return -1;
    }
    return 0;
}

static int
uds_receive(TpXport *x, uint64_t seq, size_t expected, const void **view, size_t *len, uint64_t *t_recv)
{
    ssize_t n;

    /* COPY #2 of 2: kernel socket buffer -> the preallocated receive buffer.
     * SOCK_SEQPACKET keeps message boundaries, so one recv() returns exactly
     * the one send() — no length prefix and no reassembly loop are needed. */
    n = recv(x->fd, x->rbuf, x->chunk, 0);
    if (n < 0)
    {
        tp_seterr(x, "uds: recv of chunk %llu: %s", (unsigned long long)seq, strerror(errno));
        return -1;
    }
    if ((size_t)n != expected)
    {
        tp_seterr(x, "uds: recv of chunk %llu returned %zd bytes, expected %zu", (unsigned long long)seq, n, expected);
        return -1;
    }
    *t_recv = tp_now_ns(); /* the SPEC's "right after receiving" */
    *view   = x->rbuf;
    *len    = (size_t)n;
    return 0;
}

/* The consumer is done with chunk `seq`. For uds the buffer cannot be
 * overwritten by the producer (it is the consumer's own memory), so this is
 * pacing rather than safety — but it is the one-in-flight contract the uds cell
 * keeps whatever --in-flight says (see uds_publish), so the default cell is the
 * same shape as the shm cell at --in-flight 1. */
static int
uds_release(TpXport *x, uint64_t seq)
{
    uint8_t ack = 1;

    (void)seq;
    if (send(x->fd, &ack, 1, MSG_NOSIGNAL) != 1)
    {
        tp_seterr(x, "uds: ack send failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void
uds_close_producer(TpXport *x)
{
    if (x->fd >= 0) close(x->fd);
    if (x->other_fd >= 0) close(x->other_fd);
    x->fd = x->other_fd = -1;
}

static void
uds_close_consumer(TpXport *x)
{
    if (x->fd >= 0) close(x->fd);
    free(x->rbuf);
    x->fd   = -1;
    x->rbuf = nullptr;
}

/* ================================================== the two shared loops === */

/* THE PRODUCER. Identical for shm and uds: the only transport-specific step is
 * x->publish(). Everything else — slicing the mapping, pairing the timestamps,
 * counting copies — is the harness. */
static int
producer_loop(TpXport *x, const uint8_t *payload, size_t bytes, size_t chunk, TpTsSlot *slots,
              uint64_t *wall_ns, TpCpu *cpu, uint64_t *first_pub_ns)
{
    struct rusage ru0, ru1;
    uint64_t      t0, t1, seq = 0, off = 0;

    getrusage(RUSAGE_SELF, &ru0);
    t0 = tp_now_ns();

    while (off < bytes)
    {
        size_t   len = std::min(chunk, bytes - off);
        uint64_t t_pub;

        /* The slot this chunk's publish timestamp goes into: the transport's
         * publish path fills it in right after it has the timestamp, so the
         * consumer can pair the chunk as soon as it arrives. */
        x->pair = &slots[seq];
        if (x->publish(x, payload + off, len, seq, &t_pub) != 0) return -1;
        if (seq == 0) *first_pub_ns = t_pub;

        off += len;
        seq += 1;
    }

    t1 = tp_now_ns();
    getrusage(RUSAGE_SELF, &ru1);
    *wall_ns = t1 - t0;
    *cpu     = tp_cpu_delta(ru0, ru1);
    return 0;
}

/* THE CONSUMER. Also identical for both transports: x->receive() hands back a
 * view of the chunk — a pointer into the shm mapping for shm, the receive
 * buffer for uds — and the rest of this function is the same work either way. */
static int
consumer_loop(TpXport *x, TpShared *sh, const uint8_t *payload, size_t bytes, size_t chunk, TpTsSlot *slots)
{
    struct rusage        ru0, ru1;
    std::vector<uint64_t> lat;
    uint64_t             nchunks;
    uint64_t             fold  = TP_FOLD_SEED;
    uint64_t             got   = 0;
    uint64_t             spins = 0;
    int                  ok    = 1;

    getrusage(RUSAGE_SELF, &ru0);
    if (x->open_consumer(x, sh) != 0) return -1;

    nchunks = (bytes + chunk - 1) / chunk;
    lat.reserve(nchunks);

    for (uint64_t seq = 0; seq < nchunks; seq++)
    {
        size_t      off    = (size_t)seq * chunk;
        size_t      expect = std::min(chunk, bytes - off);
        const void *view   = nullptr;
        size_t      len    = 0;
        uint64_t    t_recv = 0;
        uint64_t    t_pub;

        if (x->receive(x, seq, expect, &view, &len, &t_recv) != 0) return -1;

        /* Pair this chunk with the producer's publish timestamp. The chunk can
         * arrive before the producer's slot store lands, so spin (bounded) until
         * the slot names this chunk; a stale slot would mean the pairing has
         * drifted, which the same check catches. */
        while (__atomic_load_n(&slots[seq].seq, __ATOMIC_ACQUIRE) != seq)
        {
            if (++spins > 1000000000ull)
            {
                tp_seterr(x, "consumer: publish timestamp for chunk %llu never arrived", (unsigned long long)seq);
                return -1;
            }
        }
        t_pub = slots[seq].t_pub_ns;

        /* t_recv - t_pub, valid across processes because CLOCK_MONOTONIC is
         * system-wide. On uds the consumer can win the race against the
         * producer's own clock read (send() has already handed the message over
         * when the producer reads the clock), which would underflow an unsigned
         * difference: those samples are recorded as the 0 floor and counted. */
        if (t_recv >= t_pub)
            lat.push_back(t_recv - t_pub);
        else
        {
            lat.push_back(0);
            sh->lat_inverted += 1;
        }
        got += len;
        if (!sh->first_recv_ns) sh->first_recv_ns = t_recv;
        sh->last_recv_ns = t_recv;

        /* Integrity, at both edges of the slice, against the payload mapping the
         * fork inherited: the bytes that came through the transport must be the
         * bytes in the file. Two 8-byte compares per chunk, same on both cells. */
        if (len >= 8 &&
            (memcmp(view, payload + off, 8) != 0 ||
             memcmp((const uint8_t *)view + len - 8, payload + off + len - 8, 8) != 0))
            ok = 0;

        /* The consumer's work: fold the chunk where it lies. For shm the reads
         * hit the segment mapping directly (no copy out); for uds they hit the
         * receive buffer that recv() filled. Latency was recorded above, so the
         * fold never lands inside a latency sample. */
        fold = tp_fold(view, len, fold, off / 8);

        /* Done with the bytes: only now may the producer reuse the window. This
         * is a correctness requirement for shm — acking when the chunk arrived
         * instead of when the consumer is finished lets the producer overwrite
         * the window mid-fold. */
        if (x->release(x, seq) != 0) return -1;
    }

    getrusage(RUSAGE_SELF, &ru1);

    {
        TpCpu c = tp_cpu_delta(ru0, ru1);
        sh->cpu_user_ns = c.user_ns;
        sh->cpu_sys_ns  = c.sys_ns;
    }
    sh->peak_rss_kb  = tp_vmhwm_kb();
    sh->recv_bytes   = got;
    sh->recv_chunks  = nchunks;
    sh->integrity_ok = (uint64_t)ok;
    sh->fold         = fold;
    sh->ts_slot_spins = spins;

    if (!lat.empty())
    {
        std::vector<uint64_t> sorted = lat;
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();
        /* nearest-rank percentiles on the sorted sample */
        size_t i50 = (size_t)(0.50 * (double)n);
        size_t i99 = (size_t)(0.99 * (double)n);
        if (i50 >= n) i50 = n - 1;
        if (i99 >= n) i99 = n - 1;
        uint64_t sum = 0;
        for (size_t i = 0; i < n; i++) sum += sorted[i];
        sh->lat_min_ns = sorted[0];
        sh->lat_p50_ns = sorted[i50];
        sh->lat_p99_ns = sorted[i99];
        sh->lat_max_ns = sorted[n - 1];
        sh->lat_sum_ns = sum;
    }

    x->close_consumer(x);
    return 0;
}

/* --------------------------------------------------------------- json ----- */

static std::string
tp_json_escape(const char *s)
{
    std::string out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            out += '\\';
            out += (char)*p;
        }
        else if (*p < 0x20)
        {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", *p);
            out += b;
        }
        else
            out += (char)*p;
    }
    return out;
}

static void
tp_appendf(std::string &s, const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s += buf;
}

static void
tp_usage(void)
{
    fprintf(stderr,
            "usage: transport_bench --which shm|uds --payload FILE [--chunk BYTES] [--in-flight N]\n"
            "  --which    shm: POSIX shared memory segment + seq counter + sem_t doorbells\n"
            "             uds: AF_UNIX SOCK_SEQPACKET, one send/recv per chunk\n"
            "  --payload  file mapped read-only and moved in --chunk slices (default 1 MiB)\n"
            "  --chunk    slice size in bytes (default %zu)\n"
            "  --in-flight  chunks the shm producer may publish ahead of the consumer's ack\n"
            "             (default %d, max %d); uds accepts and reports it but keeps one\n"
            "             message in flight (see the comment on uds_publish)\n",
            TP_DEFAULT_CHUNK, TP_DEFAULT_IN_FLIGHT, TP_MAX_IN_FLIGHT);
}

/* One error line, still valid JSON: the runner reads stdout, so a failure must
 * be visible there too, and the exit code is non-zero. The cell name matches the
 * success line ("transport_shm" / "transport_uds"). */
static void
tp_fail_json(const char *name, const std::string &msg, int code)
{
    printf("{\"cell\":\"transport_%s\",\"which\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n", name, name,
           tp_json_escape(msg.c_str()).c_str());
    fflush(stdout);
    exit(code);
}

int
main(int argc, char **argv)
{
    const char *which = nullptr;
    const char *path  = nullptr;
    size_t      req_chunk = TP_DEFAULT_CHUNK;
    int         req_in_flight = TP_DEFAULT_IN_FLIGHT;
    int         i;

    signal(SIGPIPE, SIG_IGN); /* a dead consumer must return EPIPE, not kill the producer */

    for (i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto        next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };

        if (a == "--which") which = next();
        else if (a == "--payload") path = next();
        else if (a == "--chunk")
        {
            const char *v = next();
            char       *end = nullptr;
            if (!v) { tp_usage(); return 2; }
            unsigned long long n = strtoull(v, &end, 10);
            if (!end || *end != 0 || n == 0) { tp_usage(); return 2; }
            req_chunk = (size_t)n;
        }
        else if (a == "--in-flight")
        {
            const char *v = next();
            char       *end = nullptr;
            if (!v) { tp_usage(); return 2; }
            unsigned long long n = strtoull(v, &end, 10);
            if (!end || *end != 0 || n == 0 || n > (unsigned long long)TP_MAX_IN_FLIGHT) { tp_usage(); return 2; }
            req_in_flight = (int)n;
        }
        else if (a == "-h" || a == "--help") { tp_usage(); return 0; }
        else
        {
            tp_usage();
            return 2;
        }
    }
    if (!which || !path || (strcasecmp(which, "shm") != 0 && strcasecmp(which, "uds") != 0))
    {
        tp_usage();
        return 2;
    }

    TpXport x;
    memset(&x, 0, sizeof x);
    x.fd       = -1;
    x.other_fd = -1;
    x.name     = strcasecmp(which, "shm") == 0 ? "shm" : "uds";
    /* The flag is accepted for both transports; only the shm cell widens its
     * window (uds_publish says why), so the effective depth is recorded here
     * and in the JSON. */
    x.in_flight      = x.name[0] == 's' ? req_in_flight : 1;
    x.open_producer  = x.name[0] == 's' ? shm_open_producer : uds_open_producer;
    x.open_consumer  = x.name[0] == 's' ? shm_open_consumer : uds_open_consumer;
    x.publish        = x.name[0] == 's' ? shm_publish : uds_publish;
    x.receive        = x.name[0] == 's' ? shm_receive : uds_receive;
    x.release        = x.name[0] == 's' ? shm_release : uds_release;
    x.close_producer = x.name[0] == 's' ? shm_close_producer : uds_close_producer;
    x.close_consumer = x.name[0] == 's' ? shm_close_consumer : uds_close_consumer;

    /* --- the payload: mapped read-only, never copied into the producer ---- */
    int         pfd = open(path, O_RDONLY);
    struct stat st;
    size_t      bytes;
    const uint8_t *payload = nullptr;

    if (pfd < 0) tp_fail_json(x.name, std::string("open(") + path + "): " + strerror(errno), 1);
    if (fstat(pfd, &st) != 0) tp_fail_json(x.name, std::string("fstat(") + path + "): " + strerror(errno), 1);
    bytes = (size_t)st.st_size;
    if (bytes)
    {
        void *m = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, pfd, 0);
        if (m == MAP_FAILED) tp_fail_json(x.name, std::string("mmap(") + path + "): " + strerror(errno), 1);
        payload = (const uint8_t *)m;
    }
    close(pfd);

    /* Slice size: 8-byte aligned, so the fold is a function of the bytes and not
     * of the chunking (see tp_fold); a slice is never longer than the file, so a
     * payload smaller than --chunk is a single slice. */
    size_t chunk = req_chunk;
    if (chunk > 8) chunk &= ~(size_t)7;
    if (bytes && chunk > bytes) chunk = bytes;
    if (chunk == 0) chunk = 1;
    x.chunk         = chunk;
    x.payload_bytes = bytes;

    size_t nchunks = (bytes + chunk - 1) / chunk;

    /* --- the producer's reference fold, BEFORE the timed region ------------ */
    uint64_t ref_fold = TP_FOLD_SEED;
    for (size_t off = 0; off < bytes; off += chunk)
        ref_fold = tp_fold(payload + off, std::min(chunk, bytes - off), ref_fold, off / 8);

    /* --- bookkeeping shared with the child --------------------------------- */
    size_t    sh_bytes = sizeof(TpShared) + nchunks * sizeof(TpTsSlot);
    TpShared *sh       = (TpShared *)mmap(nullptr, sh_bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sh == MAP_FAILED) tp_fail_json(x.name, std::string("mmap(shared bookkeeping): ") + strerror(errno), 1);
    memset(sh, 0, sh_bytes);
    TpTsSlot *slots = (TpTsSlot *)(void *)((char *)sh + sizeof(TpShared));
    /* Sentinel: the producer stores 0,1,2,... so an untouched slot must not look
     * like "chunk 0 has been published" (seq starts at 0). */
    for (size_t k = 0; k < nchunks; k++) slots[k].seq = ~(uint64_t)0;

    /* --- transport setup in the parent, before the fork -------------------- */
    if (x.open_producer(&x) != 0) tp_fail_json(x.name, x.err, 1);

    pid_t child = fork();
    if (child < 0)
    {
        x.close_producer(&x);
        tp_fail_json(x.name, std::string("fork: ") + strerror(errno), 1);
    }

    if (child == 0)
    {
        /* ---- the consumer, in its own process: its CPU is its own ---- */
        int rc = consumer_loop(&x, sh, payload, bytes, chunk, slots);
        if (rc != 0)
        {
            __atomic_store_n(&sh->consumer_failed, 1, __ATOMIC_RELEASE);
            snprintf(sh->err, sizeof sh->err, "%s", x.err);
        }
        _exit(rc == 0 ? 0 : 1); /* _exit: never flush the parent's stdio */
    }

    /* ---- the producer, in the parent ---- */
    if (x.other_fd >= 0) /* uds: the consumer's end is not ours */
    {
        close(x.other_fd);
        x.other_fd = -1;
    }

    /* Wait for the consumer to be ready before publishing anything: the ring
     * does the same thing with worker_ready (Ring_start's 1 ms poll loop). */
    {
        int waited = 0;
        while (__atomic_load_n(&sh->consumer_ready, __ATOMIC_ACQUIRE) == 0)
        {
            if (__atomic_load_n(&sh->consumer_failed, __ATOMIC_ACQUIRE))
                tp_fail_json(x.name, std::string("consumer setup failed: ") + sh->err, 1);
            tp_sleep_ms(1);
            if (++waited > TP_WAIT_MS)
                tp_fail_json(x.name, "consumer did not become ready", 1);
        }
    }

    uint64_t wall_ns = 0, first_pub_ns = 0;
    TpCpu    prod_cpu = {0, 0};
    int      prod_rc  = producer_loop(&x, payload, bytes, chunk, slots, &wall_ns, &prod_cpu, &first_pub_ns);
    uint64_t prod_rss = tp_vmhwm_kb();

    /* If the producer broke, do not sit out the consumer's deadline: kill it. */
    if (prod_rc != 0) kill(child, SIGKILL);

    int           status = 0;
    struct rusage cru;
    memset(&cru, 0, sizeof cru);
    while (wait4(child, &status, 0, &cru) < 0 && errno == EINTR)
    {
    }

    /* The consumer is gone, so the segment can be destroyed. The anonymous
     * bookkeeping block is deliberately NOT unmapped: it is read just below and
     * this process exits immediately afterwards. */
    x.close_producer(&x);

    if (prod_rc != 0) tp_fail_json(x.name, std::string("producer: ") + x.err, 1);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || sh->consumer_failed)
        tp_fail_json(x.name, std::string("consumer: ") + (sh->err[0] ? sh->err : "died"), 1);

    /* ------------------------------------------------------------------ out */
    const bool    lengths_ok = sh->integrity_ok != 0;
    const bool    counts_ok  = sh->recv_bytes == (uint64_t)bytes && x.copied_bytes == (uint64_t)bytes &&
                               sh->recv_chunks == nchunks;
    const bool    fold_ok    = sh->fold == ref_fold;
    const bool    verified   = lengths_ok && counts_ok && fold_ok;
    const double  transfer_ms = nchunks ? tp_ms(sh->last_recv_ns - first_pub_ns) : 0.0;
    const double  gb_per_s    = transfer_ms > 0 ? ((double)bytes / (transfer_ms / 1000.0)) / 1e9 : 0.0;
    const double  lat_mean_ms = nchunks ? tp_ms(sh->lat_sum_ns / nchunks) : 0.0;
    const uint64_t rss_kb     = std::max(prod_rss, sh->peak_rss_kb);
    const bool    is_shm      = strcmp(x.name, "shm") == 0;

    std::string j = "{";
    tp_appendf(j, "\"cell\":\"transport_%s\"", x.name);
    tp_appendf(j, ",\"transport\":\"%s\",\"which\":\"%s\"", x.name, x.name);

    /* SPEC keys. rows/rows_per_s are null: the payload is opaque, there are no
     * rows to count; throughput lives in gb_per_s. encode_ms/decode_ms are 0.0
     * because no codec is in the loop in this cell. checksum is null for the
     * same reason (the fold below is the integrity evidence instead). */
    j += ",\"rows\":null,\"rows_per_s\":null";
    tp_appendf(j, ",\"payload_bytes\":%zu,\"chunks\":%zu,\"chunk_bytes\":%zu,\"chunk_requested_bytes\":%zu", bytes,
               nchunks, chunk, req_chunk);
    /* in_flight is what ran (uds keeps one message in flight whatever the flag
     * says); in_flight_requested is the flag as given on the command line. */
    tp_appendf(j, ",\"in_flight\":%d,\"in_flight_requested\":%d", x.in_flight, req_in_flight);
    j += ",\"encode_ms\":0.0,\"decode_ms\":0.0";
    tp_appendf(j, ",\"gb_per_s\":%.4f,\"p50_ms\":%.4f,\"p99_ms\":%.4f", gb_per_s, tp_ms(sh->lat_p50_ns),
               tp_ms(sh->lat_p99_ns));
    tp_appendf(j, ",\"latency_min_ms\":%.4f,\"latency_mean_ms\":%.4f,\"latency_max_ms\":%.4f", tp_ms(sh->lat_min_ns),
               lat_mean_ms, tp_ms(sh->lat_max_ns));
    /* Samples whose t_recv was earlier than t_pub: the transport handed the
     * chunk over in less time than the producer's own clock read took, so the
     * difference is recorded as 0 (see the comment in consumer_loop). */
    tp_appendf(j, ",\"latency_inverted_chunks\":%llu", (unsigned long long)sh->lat_inverted);

    /* Copies: what each stage copies, per payload byte stream. */
    if (is_shm)
    {
        j += ",\"copies\":1";
        j += ",\"copy_stages\":{\"file page cache -> producer mapping\":0,\"producer mmap -> shm segment\":1,"
             "\"shm segment -> consumer (in place)\":0}";
    }
    else
    {
        j += ",\"copies\":2";
        j += ",\"copy_stages\":{\"file page cache -> producer mapping\":0,\"producer mmap -> kernel socket buffer\":1,"
             "\"kernel socket buffer -> consumer buffer\":1}";
    }
    tp_appendf(j, ",\"copy_bytes\":{\"producer_to_transport\":%llu,\"consumer_received\":%llu}",
               (unsigned long long)x.copied_bytes, (unsigned long long)sh->recv_bytes);

    /* CPU: the sum of both sides plus each side on its own. */
    tp_appendf(j, ",\"cpu_user_ms\":%.3f,\"cpu_sys_ms\":%.3f",
               tp_ms(prod_cpu.user_ns + sh->cpu_user_ns), tp_ms(prod_cpu.sys_ns + sh->cpu_sys_ns));
    tp_appendf(j, ",\"producer_cpu_ms\":%.3f,\"consumer_cpu_ms\":%.3f",
               tp_ms(prod_cpu.user_ns + prod_cpu.sys_ns), tp_ms(sh->cpu_user_ns + sh->cpu_sys_ns));
    tp_appendf(j, ",\"producer_user_ms\":%.3f,\"producer_sys_ms\":%.3f,\"consumer_user_ms\":%.3f,"
                  "\"consumer_sys_ms\":%.3f",
               tp_ms(prod_cpu.user_ns), tp_ms(prod_cpu.sys_ns), tp_ms(sh->cpu_user_ns), tp_ms(sh->cpu_sys_ns));
    /* The kernel's own accounting for the child, next to the child's own: they
     * should agree, which shows the fork resets the child's cpu counters. */
    tp_appendf(j, ",\"consumer_cpu_wait4_ms\":%.3f",
               tp_ms(tp_tv_ns(cru.ru_utime) + tp_tv_ns(cru.ru_stime)));

    tp_appendf(j, ",\"rss_kb\":%llu,\"producer_rss_kb\":%llu,\"consumer_rss_kb\":%llu",
               (unsigned long long)rss_kb, (unsigned long long)prod_rss, (unsigned long long)sh->peak_rss_kb);
    tp_appendf(j, ",\"wall_ms\":%.3f,\"transfer_ms\":%.3f", tp_ms(wall_ns), transfer_ms);

    tp_appendf(j, ",\"payload_fold\":\"%llu\",\"payload_fold_reference\":\"%llu\"",
               (unsigned long long)sh->fold, (unsigned long long)ref_fold);
    tp_appendf(j, ",\"verified\":%s", verified ? "true" : "false");
    tp_appendf(j,
               ",\"verify\":{\"chunks_ok\":%s,\"bytes_ok\":%s,\"lengths_ok\":%s,\"edges_ok\":%s,\"fold_match\":%s,"
               "\"consumer_exit_ok\":%s,\"ts_slot_spins\":%llu}",
               sh->recv_chunks == nchunks ? "true" : "false", sh->recv_bytes == (uint64_t)bytes ? "true" : "false",
               counts_ok ? "true" : "false", lengths_ok ? "true" : "false", fold_ok ? "true" : "false",
               (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? "true" : "false",
               (unsigned long long)sh->ts_slot_spins);

    j += ",\"checksum\":null";

    /* How the cell was run, in the result itself. */
    j += ",\"method\":{";
    if (is_shm)
    {
        tp_appendf(j,
                   "\"handshake\":\"shm_open/ftruncate/mmap segment; ctl{magic FPTP01, seq bumped with a RELEASE "
                   "store per chunk, per-window len[]}; sem_t doorbell pub plus a counting sem_t with in_flight "
                   "permits (sem_post/sem_timedwait; the ring's mechanism: req_seq/resp_seq + req_ready/resp_ready); "
                   "each publish takes one permit and each consumer release returns one, so at most in_flight chunks "
                   "are unacked and window seq %% in_flight is rewritten only after chunk seq - in_flight has been "
                   "released\"");
        tp_appendf(j, ",\"segment\":\"%s\",\"segment_bytes\":%zu,\"windows\":%d,\"window_stride_bytes\":%zu,"
                      "\"window_offset\":%zu",
                   x.seg_name, x.seg_bytes, x.in_flight, x.window_stride, TP_SHM_DATA_OFF);
    }
    else
    {
        tp_appendf(j,
                   "\"handshake\":\"socketpair(AF_UNIX, SOCK_SEQPACKET); one send/recv per chunk and message "
                   "boundaries come free with SEQPACKET, so no framing/length prefix is needed; one 1-byte ack per "
                   "chunk keeps exactly one message in flight, the same shape as shm at in_flight=1; "
                   "SO_SNDBUF/SO_RCVBUF sized for one chunk; --in-flight is accepted and reported "
                   "(in_flight_requested) but the uds cell does not widen its window\"");
        tp_appendf(j, ",\"sock_buf_bytes\":%d", x.sock_buf);
    }
    j += ",\"latency_pairing\":\"publish timestamps live in an anonymous MAP_SHARED array indexed by chunk ordinal, "
         "written by the producer right after each publish and validated with slot.seq (RELEASE/ACQUIRE); the consumer "
         "computes t_recv - t_pub, valid across processes because CLOCK_MONOTONIC is system-wide; a sample where the "
         "consumer's clock read beat the producer's is reported as 0 and counted in latency_inverted_chunks\"";
    j += ",\"consumer_work\":\"an in-place fold over every received byte (shm: read straight out of the mapping; uds: "
         "read out of the preallocated receive buffer), recorded after the receive timestamp and before the chunk is "
         "released back to the producer\"";
    j += "}";

    tp_appendf(j, ",\"ok\":true}");

    printf("%s\n", j.c_str());
    fflush(stdout);
    return 0;
}
