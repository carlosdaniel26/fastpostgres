/*
 * peer.c — the ring's two doorbells, measured hop by hop; one binary, two roles.
 *
 * WHAT IS MEASURED. A hop is the product's request/response shape with the
 * payload left out: the client writes a NEXT descriptor (op, seq, batch,
 * data_bytes) into the ring fields, the engine notifies the worker (for regipc
 * the engine copies those four into the module's nine carriers, x0..x8 = 72
 * bytes), the worker answers resp_seq/status, and the client records the round
 * trip. The ring's data segment is never created, mapped or touched: there is
 * no payload in this experiment, only the notification and the wait for it.
 * The record's rows/s is hops/s x --batch-rows, where --batch-rows is what a
 * real batch would have carried; that is a conversion, not rows produced.
 *
 * WHERE THE CODE COMES FROM. Both sides drive the product's own engines
 * (extension/transport/engine_shm.c, engine_regipc.c) through the ops table in
 * extension/transport/transport.h; this file only creates the control segment
 * the product creates (FPRING04, memset, sem_init, the regipc block at offset
 * 4096), spawns the worker role, posts hops and reports. No engine logic is
 * reimplemented here.
 *
 * THE STOP. The client's stop follows the Python client's Ring_stop: op = STOP
 * through the engine's post_request. On regipc that is a real call carrying the
 * STOP, so the worker leaves at once; the extension's other stop shape
 * (fastpostgres_ring_stop: a req_seq bump + sem_post, no call) is noticed by a
 * regipc worker only at its 100 ms wait_request timeout (worker.c's
 * FASTPOSTGRES_RING_WAIT_MS). `wait_timeouts` counts the times that timeout
 * fired during a run, and `teardown_ms` what the stop cost here.
 *
 * EXACTNESS. Both sides carry an FNV-1a chain over every descriptor they post
 * or see (the same fold the module applies to the carriers, module/regipc.c's
 * leaf_fnv), and for regipc the module's own leaf.checksum is the third chain:
 * all three must be equal and leaf.calls must be count + 1 (hops + stop), which
 * is the machine's own record that every hop arrived exactly once, in order. A
 * timed-out or skipped hop fails `verified`.
 *
 * SAFETY. Every wait is bounded: the engines' spins fall back to a semaphore,
 * the client's frames time out at the product's budgets, the worker leaves when
 * the client's pid disappears, and the client reaps the worker within
 * BENCH_EXIT_MS before SIGKILL.
 *
 * Usage:
 *   peer --method shm|regipc --count N --reps R [--batch-rows N] [--json]
 *   peer --selftest                      one hop per method, PASS/FAIL lines
 *   peer --abi                           the module's ABI, through REGIPC_IOC_STATS
 *   peer --role worker --method M --segment NAME   (spawned by the client role)
 *
 * One single-line JSON object per run is printed on stdout; without --json a
 * human summary also goes to stderr. Every run's record carries the counters
 * that prove (or disprove) every hop was seen exactly once.
 */
#include <sys/syscall.h> /* __NR_ioctl, consumed by the vendored handoff asm */

#include "fastpostgres/regipc.h"
#include "fastpostgres/ring.h"

#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------- layout */

/* The worker's report lives in the control segment on its own page, after the
 * ring struct (sizeof(FastpostgresRing) is 4484) and the regipc block
 * (4096..4216), neither of which this may touch. */
#define BENCH_REPORT_OFFSET 8192u
#define BENCH_REPORT_MAGIC  0xbe1175c0ffee5eedull /* the worker wrote the whole report */

/* The budgets, in milliseconds, from the product: worker.c waits 100 ms per
 * wait_request (FASTPOSTGRES_RING_WAIT_MS), and ring.hpp uses RING_POST_MS
 * 1000, RING_WAIT_MS 10000 and RING_READY_MS 10000 on the client. */
#define BENCH_WAIT_MS  100u
#define BENCH_POST_MS  1000u
#define BENCH_RESP_MS  10000u
#define BENCH_READY_MS 10000u

/* The client's bound on the worker's exit, then SIGKILL: a hang detector. */
#define BENCH_EXIT_MS 5000u

/* FNV-1a 64 offset basis: the leaf's starting checksum, and both fold chains'. */
#define BENCH_FNV_SEED 14695981039346656037ull

/* size in bytes of the descriptor frame: the module's nine 64-bit carriers. */
#define BENCH_FRAME_BYTES ((uint64_t)REGIPC_CARRIERS * sizeof(uint64_t))

/* What the worker leaves behind in the segment. Plain fields published by a
 * release store of `magic`; the client reads them only after wait4 has reaped
 * the worker, so nothing here is concurrently accessed. */
typedef struct BenchReport {
    uint64_t magic;             /* BENCH_REPORT_MAGIC */
    uint64_t hops_seen;         /* NEXT descriptors answered */
    uint64_t stop_seen;         /* the STOP was observed */
    uint64_t wait_timeouts;     /* wait_request returned 0 (the 100 ms budget fired) */
    uint64_t anomalies;         /* wait_request/post_response failures */
    uint64_t seq_first;         /* first request sequence handled */
    uint64_t seq_last;          /* last request sequence handled */
    uint64_t gaps;              /* a sequence arrived ahead of the next expected */
    uint64_t duplicates;        /* a sequence arrived again */
    uint64_t checksum;          /* FNV-1a over the descriptors the worker saw */
    uint64_t leaf_calls_end;    /* regipc: leaf.calls when the worker left */
    uint64_t leaf_checksum_end; /* regipc: leaf.checksum when the worker left */
    uint64_t cpu_user_us;       /* the worker's own loop CPU, in microseconds */
    uint64_t cpu_sys_us;
} BenchReport;

/* The per-process engine state, exactly worker.c's union: each engine reads
 * only the shape it belongs to. */
typedef union BenchCtx
{
    FastpostgresShmCtx    shm;
    FastpostgresRegipcCtx regipc;
} BenchCtx;

_Static_assert(BENCH_REPORT_OFFSET >= sizeof(FastpostgresRing),
               "the report must not land in the ring struct");
_Static_assert(BENCH_REPORT_OFFSET >= FASTPOSTGRES_RING_REGIPC_OFFSET + sizeof(FastpostgresRingRegipc),
               "the report must not land in the regipc block");
_Static_assert(BENCH_REPORT_OFFSET % 64u == 0u, "the report should start a cache line");
_Static_assert(BENCH_REPORT_OFFSET + sizeof(BenchReport) <= FASTPOSTGRES_RING_BYTES,
               "the report must fit the control segment");
_Static_assert(REGIPC_CARRIERS == 9, "the carrier set is x0..x8");

/* ------------------------------------------------------------------- basics */

typedef enum BenchRole
{
    BENCH_ROLE_CLIENT,
    BENCH_ROLE_WORKER
} BenchRole;

typedef struct BenchOptions {
    BenchRole   role;
    const char *method;
    const char *segment;
    uint64_t    count;
    unsigned    reps;
    uint64_t    batch_rows;
    bool        json;
    bool        selftest;
    bool        abi;
} BenchOptions;

typedef struct BenchResult {
    uint64_t count;
    uint64_t batch_rows;
    uint64_t hops_completed; /* client: answered hops recorded */
    uint64_t hops_seen;      /* worker: NEXT descriptors answered */
    uint64_t gaps;
    uint64_t duplicates;
    uint64_t seq_first;
    uint64_t seq_last;
    uint64_t client_timeouts;
    uint64_t client_errors;
    uint64_t wait_timeouts;
    uint64_t anomalies;
    uint64_t checksum;        /* the client's chain over what it posted */
    uint64_t worker_checksum; /* the worker's chain over what it saw */
    uint64_t leaf_checksum;   /* regipc: the module's own chain */
    uint64_t leaf_calls;      /* regipc: the module's calls counter */
    uint64_t req_seq_final;   /* the ring's counters after the stop */
    uint64_t resp_seq_final;
    double   seconds;
    double   hops_per_second;
    double   rows_per_second;
    double   mb_per_second;
    double   us_p50;
    double   us_p99;
    double   producer_cpu_s;
    double   consumer_cpu_s;
    double   worker_loop_cpu_s;
    double   teardown_ms;
    bool     stop_seen;
    bool     checksum_match;
    bool     leaf_checksum_match;
    bool     child_exit_ok;
    bool     verified;
} BenchResult;

static void
bench_err(char *err, size_t errsz, const char *fmt, ...)
{
    va_list ap;

    if (errsz == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static uint64_t
bench_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* getrusage's timevals are microseconds; everything here is seconds. */
static double
bench_rusage_s(const struct rusage *ru)
{
    return (double)ru->ru_utime.tv_sec + (double)ru->ru_utime.tv_usec / 1e6 + (double)ru->ru_stime.tv_sec +
           (double)ru->ru_stime.tv_usec / 1e6;
}

static uint64_t
bench_tv_us(const struct timeval *tv)
{
    return (uint64_t)tv->tv_sec * 1000000ull + (uint64_t)tv->tv_usec;
}

/* The module's leaf_fnv (extension/transport/regipc/module/regipc.c): FNV-1a 64
 * over the low 32 bits of every carrier, four little-endian bytes each, in
 * carrier order. Both sides fold their descriptors with this, so a chain can be
 * compared with the module's own leaf.checksum for regipc. */
static uint64_t
bench_fold(uint64_t hash, const uint64_t *carriers)
{
    int i;
    int b;

    for (i = 0; i < REGIPC_CARRIERS; i++)
        for (b = 0; b < 4; b++)
        {
            hash ^= (uint8_t)((uint32_t)carriers[i] >> (b * 8));
            hash *= 1099511628211ull;
        }
    return hash;
}

/* Nearest-rank, the same convention as benchmarks/lib/harness.py's
 * percentile(): index int(q * (n - 1)) on the sorted samples. */
static double
bench_percentile_us(const uint64_t *sorted, uint64_t n, double q)
{
    if (n == 0) return 0.0;
    return (double)sorted[(size_t)(q * (double)(n - 1))] / 1000.0;
}

static int
bench_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static const char *
bench_bool(bool value)
{
    return value ? "true" : "false";
}

/* --------------------------------------------------------------------- role */

/* The worker role: map the segment the client made, initialise the engine,
 * publish ready, answer until the STOP (or the client is gone), leave a report.
 * This is worker.c's loop with ring_handle() replaced by "an empty answer" —
 * the hop is the doorbell, there is no SQL to run. */
static int
bench_worker_main(const BenchOptions *o)
{
    const FastpostgresTransport *t;
    FastpostgresRing            *r;
    FastpostgresRingRegipc      *ip;
    struct stat                  st;
    union BenchCtx               ctx;
    BenchReport                 *rep;
    struct rusage                ru0;
    struct rusage                ru1;
    char                         path[128];
    uint64_t                     expected = 1;
    uint64_t                     checksum = BENCH_FNV_SEED;
    size_t                       rsize;
    int                          fd;
    int                          rc;
    int                          i;
    int                          anomalies = 0;

    t = fastpostgres_transport_by_name(o->method);
    if (t == NULL)
    {
        fprintf(stderr, "peer: unknown transport \"%s\"\n", o->method ? o->method : "");
        return 2;
    }
    if (o->segment == NULL)
    {
        fprintf(stderr, "peer: the worker role needs --segment\n");
        return 2;
    }

    snprintf(path, sizeof path, "/%s", o->segment);
    fd = shm_open(path, O_RDWR, 0);
    if (fd < 0)
    {
        fprintf(stderr, "peer: worker shm_open(%s): %s\n", path, strerror(errno));
        return 1;
    }
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(FastpostgresRing))
    {
        fprintf(stderr, "peer: worker fstat(%s): %s\n", path,
                (size_t)st.st_size < sizeof(FastpostgresRing) ? "segment too small" : strerror(errno));
        close(fd);
        return 1;
    }
    r = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (r == MAP_FAILED)
    {
        fprintf(stderr, "peer: worker mmap(%s): %s\n", path, strerror(errno));
        return 1;
    }
    rsize = (size_t)st.st_size;

    /* worker.c's provenance check: a segment another layout or engine made is
     * refused, never driven. */
    if (memcmp(r->magic, FASTPOSTGRES_RING_MAGIC, 8) != 0 || fastpostgres_transport_check(r, t) != 0)
    {
        fprintf(stderr, "peer: worker: %s is not a %s ring\n", path, t->name);
        munmap(r, rsize);
        return 1;
    }

    memset(&ctx, 0, sizeof ctx);
    if (t->worker_init(r, &ctx) < 0)
    {
        fprintf(stderr, "peer: %s worker_init: %s\n", t->name, strerror(errno));
        munmap(r, rsize);
        return 1;
    }
    ip = fastpostgres_ring_regipc(r);

    /* worker.c's order: init, then ready. For regipc worker_init already
     * published the pid and the block's ready; this is the ring's flag. */
    __atomic_store_n(&r->worker_ready, 1u, __ATOMIC_RELEASE);

    rep = (BenchReport *)((char *)r + BENCH_REPORT_OFFSET);
    getrusage(RUSAGE_SELF, &ru0);

    for (;;)
    {
        uint64_t carriers[REGIPC_CARRIERS];
        uint64_t seq;

        rc = t->wait_request(r, &ctx, &seq, BENCH_WAIT_MS);
        if (rc == 0)
        {
            /* worker.c's timeout path: re-check the stop, then the client's
             * liveness, then keep waiting. The 100 ms budget is what makes the
             * semaphore-only stop (and SIGTERM) visible at all. */
            rep->wait_timeouts += 1;
            if (r->op == FASTPOSTGRES_OP_STOP)
            {
                rep->stop_seen = 1;
                break;
            }
            if (r->client_pid != 0 && kill((pid_t)r->client_pid, 0) != 0 && errno == ESRCH) break;
            continue;
        }
        if (rc < 0)
        {
            fprintf(stderr, "peer: %s wait_request: %s\n", t->name, strerror(errno));
            rep->anomalies += 1;
            break;
        }

        /* The descriptor as this side can see it: regipc reads the module's
         * ordered copy (leaf.values, x0..x8); shm reads the ring's fields and
         * the sequence its counter carried. */
        if (t->id == FASTPOSTGRES_TRANSPORT_REGIPC)
        {
            for (i = 0; i < REGIPC_CARRIERS; i++)
                carriers[i] = ip->leaf.values[i];
        }
        else
        {
            memset(carriers, 0, sizeof carriers);
            carriers[0] = r->op;
            carriers[1] = seq;
            carriers[2] = r->batch;
            carriers[3] = r->data_bytes;
        }
        checksum = bench_fold(checksum, carriers);

        if (r->op == FASTPOSTGRES_OP_STOP)
        {
            rep->stop_seen = 1;
            break;
        }

        /* The exactly-once accounting: sequence 1, 2, 3, ... with no repeat and
         * no skip. Anything else is counted here and fails the client's
         * verification. */
        if (seq < expected)
            rep->duplicates += 1;
        else if (seq > expected)
            rep->gaps += seq - expected;
        expected = seq + 1;
        if (rep->hops_seen == 0) rep->seq_first = seq;
        rep->seq_last = seq;
        rep->hops_seen += 1;

        /* worker.c answers a request with status 0 and the rows it wrote; this
         * hop carries no payload, so nrows is 0 and the error slot is clear. */
        r->status   = 0;
        r->nrows    = 0;
        r->error[0] = 0;
        if (t->post_response(r, &ctx, seq) < 0)
        {
            fprintf(stderr, "peer: %s post_response: %s\n", t->name, strerror(errno));
            rep->anomalies += 1;
            break;
        }
    }

    getrusage(RUSAGE_SELF, &ru1);
    rep->checksum    = checksum;
    rep->cpu_user_us = bench_tv_us(&ru1.ru_utime) - bench_tv_us(&ru0.ru_utime);
    rep->cpu_sys_us  = bench_tv_us(&ru1.ru_stime) - bench_tv_us(&ru0.ru_stime);
    if (t->id == FASTPOSTGRES_TRANSPORT_REGIPC)
    {
        rep->leaf_calls_end    = ip->leaf.calls;
        rep->leaf_checksum_end = ip->leaf.checksum;
    }

    /* Publish the report last, then the ring's ready flag, then leave. */
    __atomic_store_n(&rep->magic, BENCH_REPORT_MAGIC, __ATOMIC_RELEASE);
    __atomic_store_n(&r->worker_ready, 0u, __ATOMIC_RELEASE);
    anomalies = rep->anomalies;
    t->worker_fini(r, &ctx);
    munmap(r, rsize);
    return anomalies != 0 ? 1 : 0;
}

/* ------------------------------------------------------------------- client */

/* One run: create the segment, start the engine, spawn the worker role, post
 * `count` hops and time them, stop, reap, verify, fill `out`.
 * Returns 0 when a run happened (verified or not) and -1 on a setup failure. */
static int
bench_run_one(const FastpostgresTransport *t, const BenchOptions *o, unsigned rep, BenchResult *out,
              char *err, size_t errsz)
{
    union BenchCtx    ctx;
    struct rusage     ru0;
    struct rusage     ru1;
    struct rusage     child_rusage;
    FastpostgresRing *r     = NULL;
    pid_t             child = -1;
    char              seg[64];
    char              path[80];
    uint64_t         *lat = NULL;
    uint64_t          checksum;
    uint64_t          t_first = 0;
    uint64_t          t_last  = 0;
    uint64_t          t_stop;
    size_t            rsize         = 0;
    int               child_status  = 0;
    bool              child_reaped  = false;
    bool              client_inited = false;
    int               fd            = -1;
    int               rc            = -1;

    memset(out, 0, sizeof *out);
    memset(&ctx, 0, sizeof ctx);
    memset(&child_rusage, 0, sizeof child_rusage);
    out->count      = o->count;
    out->batch_rows = o->batch_rows;
    err[0]          = 0;

    snprintf(seg, sizeof seg, "fptransport_%d_%s_r%u", (int)getpid(), t->name, rep);
    snprintf(path, sizeof path, "/%s", seg);

    fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        bench_err(err, errsz, "shm_open(%s): %s", path, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, FASTPOSTGRES_RING_BYTES) != 0)
    {
        bench_err(err, errsz, "ftruncate(%s): %s", path, strerror(errno));
        goto out;
    }
    r = mmap(NULL, FASTPOSTGRES_RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    fd = -1;
    if (r == MAP_FAILED)
    {
        r = NULL;
        bench_err(err, errsz, "mmap(%s): %s", path, strerror(errno));
        goto out;
    }
    rsize = FASTPOSTGRES_RING_BYTES;

    /* The product's creation order (python Ring_init): zero the segment,
     * initialise the doorbells while there is no worker to race, stamp the
     * layout and the engine, set the capacity ask and the owner, and only then
     * let the engine open what it needs. */
    memset(r, 0, FASTPOSTGRES_RING_BYTES);
    if (sem_init(&r->req_ready, 1, 0) != 0 || sem_init(&r->resp_ready, 1, 0) != 0)
    {
        bench_err(err, errsz, "sem_init: %s", strerror(errno));
        goto out;
    }
    memcpy(r->magic, FASTPOSTGRES_RING_MAGIC, 8);
    r->version    = FASTPOSTGRES_RING_VERSION;
    r->transport  = t->id;
    r->data_bytes = 0; /* the product's default capacity ask (0 = the worker's default) */
    r->client_pid = (uint32_t)getpid();
    {
        FastpostgresRingRegipc *ip = fastpostgres_ring_regipc(r);

        ip->leaf.magic    = REGIPC_LEAF_MAGIC;
        ip->leaf.checksum = BENCH_FNV_SEED;
    }

    if (t->client_init(r, &ctx) < 0)
    {
        bench_err(err, errsz, "%s client_init: %s", t->name, strerror(errno));
        goto out;
    }
    client_inited = true;

    /* The worker is a real second process, started with this same binary in the
     * other role; its startup is outside the timed region. */
    child = fork();
    if (child < 0)
    {
        bench_err(err, errsz, "fork: %s", strerror(errno));
        goto out;
    }
    if (child == 0)
    {
        execl("/proc/self/exe", "peer", "--role", "worker", "--method", t->name, "--segment", seg,
              (char *)NULL);
        fprintf(stderr, "peer: worker exec: %s\n", strerror(errno));
        _exit(127);
    }

    /* The engine's readiness: for regipc this also pairs the caller with the
     * worker's leaf, which is why it cannot be a bare poll of worker_ready. */
    if (t->wait_ready(r, &ctx, BENCH_READY_MS) != 1)
    {
        bench_err(err, errsz, "worker did not become ready (%s)", errno ? strerror(errno) : "timeout");
        goto out;
    }

    lat = malloc((size_t)o->count * sizeof *lat);
    if (lat == NULL)
    {
        bench_err(err, errsz, "malloc(%llu latencies): %s", (unsigned long long)o->count, strerror(errno));
        goto out;
    }

    checksum = BENCH_FNV_SEED;
    getrusage(RUSAGE_SELF, &ru0);
    for (uint64_t seq = 1; seq <= o->count; seq++)
    {
        uint64_t carriers[REGIPC_CARRIERS];
        uint64_t t0;
        uint64_t t1;
        int      prc;
        int      wrc;

        /* ring_send's request shape: op, batch, status/nrows/error cleared. */
        r->op       = FASTPOSTGRES_OP_NEXT;
        r->batch    = (uint32_t)o->batch_rows;
        r->status   = 0;
        r->nrows    = 0;
        r->error[0] = 0;

        t0  = bench_now_ns();
        prc = t->post_request(r, &ctx, seq, BENCH_POST_MS);
        if (prc != 1)
        {
            out->client_errors += 1;
            bench_err(err, errsz, "post_request hop %llu: %s", (unsigned long long)seq,
                      prc == 0 ? "timeout" : strerror(errno));
            break;
        }
        /* ring_send keeps the ring's counter at least at the sequence it sent:
         * the shm engine bumped it, the regipc engine carried it in the call. */
        if (__atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) < seq)
            __atomic_store_n(&r->req_seq, seq, __ATOMIC_RELEASE);

        wrc = t->wait_response(r, &ctx, seq, BENCH_RESP_MS);
        t1  = bench_now_ns();
        if (wrc != 1)
        {
            out->client_timeouts += 1;
            bench_err(err, errsz, "wait_response hop %llu: %s", (unsigned long long)seq,
                      wrc == 0 ? "timeout" : strerror(errno));
            break;
        }
        /* ring_collect: acquire after the wait, then the worker's status. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (r->status != 0)
        {
            out->client_errors += 1;
            bench_err(err, errsz, "hop %llu answered status %u: %s", (unsigned long long)seq, r->status,
                      r->error[0] ? r->error : "no message");
            break;
        }
        if (__atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE) < seq)
        {
            out->client_errors += 1;
            bench_err(err, errsz, "hop %llu answered without advancing resp_seq", (unsigned long long)seq);
            break;
        }

        if (out->hops_completed == 0) t_first = t0;
        t_last                   = t1;
        lat[out->hops_completed] = t1 - t0;
        out->hops_completed += 1;

        memset(carriers, 0, sizeof carriers);
        carriers[0] = FASTPOSTGRES_OP_NEXT;
        carriers[1] = seq;
        carriers[2] = o->batch_rows;
        carriers[3] = r->data_bytes;
        checksum    = bench_fold(checksum, carriers);
    }
    getrusage(RUSAGE_SELF, &ru1);
    out->producer_cpu_s = bench_rusage_s(&ru1) - bench_rusage_s(&ru0);

    /* The stop, in the Python client's shape: op = STOP through the engine. On
     * regipc this is a call carrying the STOP, so the worker leaves at once;
     * the semaphore-only stop would cost it the 100 ms wait_request timeout. */
    {
        uint64_t carriers[REGIPC_CARRIERS];
        uint64_t stop_seq = o->count + 1;
        int      prc;

        r->op       = FASTPOSTGRES_OP_STOP;
        r->batch    = (uint32_t)o->batch_rows;
        r->status   = 0;
        r->nrows    = 0;
        r->error[0] = 0;

        t_stop = bench_now_ns();
        prc    = t->post_request(r, &ctx, stop_seq, BENCH_POST_MS);
        if (prc != 1)
        {
            out->client_errors += 1;
            if (err[0] == 0)
                bench_err(err, errsz, "stop post_request: %s", prc == 0 ? "timeout" : strerror(errno));
        }
        else
        {
            if (__atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) < stop_seq)
                __atomic_store_n(&r->req_seq, stop_seq, __ATOMIC_RELEASE);
            memset(carriers, 0, sizeof carriers);
            carriers[0] = FASTPOSTGRES_OP_STOP;
            carriers[1] = stop_seq;
            carriers[2] = o->batch_rows;
            carriers[3] = r->data_bytes;
            checksum    = bench_fold(checksum, carriers);
        }
    }
    out->checksum = checksum;

    /* Reap within a bound; the worker has the STOP (or the ring's op) and
     * should already be on its way out. */
    for (;;)
    {
        pid_t got = wait4(child, &child_status, WNOHANG, &child_rusage);

        if (got == child)
        {
            child_reaped = true;
            break;
        }
        if (got < 0 && errno != EINTR) break;
        if (bench_now_ns() - t_stop >= (uint64_t)BENCH_EXIT_MS * 1000000ull)
        {
            kill(child, SIGKILL);
            wait4(child, &child_status, 0, &child_rusage);
            child_reaped = true;
            break;
        }
        {
            struct timespec pause = {0, 1000000L}; /* 1 ms: a bound, not a measurement */

            nanosleep(&pause, NULL);
        }
    }
    out->teardown_ms = (double)(bench_now_ns() - t_stop) / 1e6;
    if (child_reaped)
    {
        out->child_exit_ok  = WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
        out->consumer_cpu_s = bench_rusage_s(&child_rusage);
    }

    /* The worker's report; it is stable now that the worker has exited. */
    {
        BenchReport *rep = (BenchReport *)((char *)r + BENCH_REPORT_OFFSET);

        if (__atomic_load_n(&rep->magic, __ATOMIC_ACQUIRE) == BENCH_REPORT_MAGIC)
        {
            out->hops_seen         = rep->hops_seen;
            out->stop_seen         = rep->stop_seen != 0;
            out->wait_timeouts     = rep->wait_timeouts;
            out->anomalies         = rep->anomalies;
            out->seq_first         = rep->seq_first;
            out->seq_last          = rep->seq_last;
            out->gaps              = rep->gaps;
            out->duplicates        = rep->duplicates;
            out->worker_checksum   = rep->checksum;
            out->leaf_calls        = rep->leaf_calls_end;
            out->leaf_checksum     = rep->leaf_checksum_end;
            out->worker_loop_cpu_s = ((double)rep->cpu_user_us + (double)rep->cpu_sys_us) / 1e6;
        }
        else if (err[0] == 0)
        {
            bench_err(err, errsz, "the worker left no report");
        }
    }

    out->req_seq_final  = __atomic_load_n(&r->req_seq, __ATOMIC_ACQUIRE);
    out->resp_seq_final = __atomic_load_n(&r->resp_seq, __ATOMIC_ACQUIRE);

    /* Everything the run reports, and the gates behind `verified`. */
    if (out->hops_completed > 0)
    {
        out->seconds         = (double)(t_last - t_first) / 1e9;
        out->hops_per_second = (double)out->hops_completed / out->seconds;
    }
    out->rows_per_second = out->hops_per_second * (double)o->batch_rows;
    out->mb_per_second   = out->hops_per_second * (double)BENCH_FRAME_BYTES / 1e6;
    if (out->hops_completed > 0)
    {
        qsort(lat, (size_t)out->hops_completed, sizeof *lat, bench_cmp_u64);
        out->us_p50 = bench_percentile_us(lat, out->hops_completed, 0.50);
        out->us_p99 = bench_percentile_us(lat, out->hops_completed, 0.99);
    }

    out->checksum_match = out->checksum == out->worker_checksum;
    if (t->id == FASTPOSTGRES_TRANSPORT_REGIPC)
        out->leaf_checksum_match = out->checksum == out->leaf_checksum;

    out->verified = out->child_exit_ok && out->stop_seen && out->checksum_match &&
                    out->hops_completed == o->count && out->hops_seen == o->count && out->gaps == 0 &&
                    out->duplicates == 0 && out->client_timeouts == 0 && out->client_errors == 0 &&
                    out->anomalies == 0 && out->seq_first == 1 && out->seq_last == o->count &&
                    out->req_seq_final == o->count + 1 && out->resp_seq_final == o->count;
    if (t->id == FASTPOSTGRES_TRANSPORT_REGIPC)
        out->verified = out->verified && out->leaf_calls == o->count + 1 && out->leaf_checksum_match;

    rc = 0;

out:
    if (child > 0 && !child_reaped)
    {
        kill(child, SIGKILL);
        while (wait4(child, &child_status, 0, &child_rusage) < 0 && errno == EINTR)
        {
        }
    }
    if (r != NULL)
    {
        if (client_inited) t->client_fini(r, &ctx);
        munmap(r, rsize);
    }
    if (fd >= 0) close(fd);
    shm_unlink(path);
    free(lat);
    return rc;
}

/* ------------------------------------------------------------------- report */

static void
bench_print_json(const FastpostgresTransport *t, const BenchOptions *o, unsigned rep, const BenchResult *res)
{
    printf("{\"method\":\"%s\",\"mechanism\":\"%s\",\"rep\":%u,\"reps\":%u,\"count\":%llu,"
           "\"batch_rows\":%llu,\"payload_bytes\":%llu,\"values_per_hop\":%d,"
           "\"seconds\":%.9f,\"hops_per_second\":%.1f,\"rows_per_second\":%.1f,\"mb_per_second\":%.3f,"
           "\"us_per_hop_p50\":%.3f,\"us_per_hop_p99\":%.3f,"
           "\"producer_cpu_s\":%.6f,\"consumer_cpu_s\":%.6f,\"worker_loop_cpu_s\":%.6f,"
           "\"checksum\":%llu,\"worker_checksum\":%llu,\"verified\":%s,"
           "\"hops_completed\":%llu,\"hops_seen\":%llu,\"gaps\":%llu,\"duplicates\":%llu,"
           "\"client_timeouts\":%llu,\"client_errors\":%llu,\"wait_timeouts\":%llu,\"anomalies\":%llu,"
           "\"stop_seen\":%s,\"checksum_match\":%s,\"child_exit_ok\":%s,"
           "\"req_seq_final\":%llu,\"resp_seq_final\":%llu,\"teardown_ms\":%.3f",
           t->name, t->mechanism, rep, o->reps, (unsigned long long)res->count,
           (unsigned long long)res->batch_rows, (unsigned long long)BENCH_FRAME_BYTES, REGIPC_CARRIERS,
           res->seconds, res->hops_per_second, res->rows_per_second, res->mb_per_second, res->us_p50,
           res->us_p99, res->producer_cpu_s, res->consumer_cpu_s, res->worker_loop_cpu_s,
           (unsigned long long)res->checksum, (unsigned long long)res->worker_checksum,
           bench_bool(res->verified), (unsigned long long)res->hops_completed,
           (unsigned long long)res->hops_seen, (unsigned long long)res->gaps,
           (unsigned long long)res->duplicates, (unsigned long long)res->client_timeouts,
           (unsigned long long)res->client_errors, (unsigned long long)res->wait_timeouts,
           (unsigned long long)res->anomalies, bench_bool(res->stop_seen), bench_bool(res->checksum_match),
           bench_bool(res->child_exit_ok), (unsigned long long)res->req_seq_final,
           (unsigned long long)res->resp_seq_final, res->teardown_ms);
    if (t->id == FASTPOSTGRES_TRANSPORT_REGIPC)
        printf(",\"leaf_calls\":%llu,\"leaf_checksum\":%llu,\"leaf_checksum_match\":%s",
               (unsigned long long)res->leaf_calls, (unsigned long long)res->leaf_checksum,
               bench_bool(res->leaf_checksum_match));
    printf("}\n");
    fflush(stdout);
}

static void
bench_print_human(const FastpostgresTransport *t, const BenchOptions *o, unsigned rep, const BenchResult *res)
{
    fprintf(stderr,
            "method=%s rep=%u/%u count=%llu hops/s=%.1f p50=%.3fus p99=%.3fus rows/s=%.0f verified=%s\n",
            t->name, rep, o->reps, (unsigned long long)res->count, res->hops_per_second, res->us_p50,
            res->us_p99, res->rows_per_second, res->verified ? "yes" : "NO");
}

/* ---------------------------------------------------------------------- abi */

/* The module's own ABI through REGIPC_IOC_STATS: register as a caller (some ABI
 * 2 modules insist on a leaf page even from the caller, so retry the way
 * engine_regipc.c does), read the version, quit. Prints one JSON line. */
static int
bench_abi(void)
{
    struct regipc_context_desc desc;
    struct regipc_stats        st;
    const char                *shape = "identity";
    void                      *leaf  = MAP_FAILED;
    int                        fd;
    int                        rc;

    fd = regipc_open();
    if (fd < 0)
    {
        fprintf(stderr, "peer: %s: %s\n", REGIPC_DEVICE, strerror(-fd));
        return 1;
    }

    memset(&desc, 0, sizeof desc);
    rc = regipc_register(fd, &desc);
    if (rc == -EINVAL)
    {
        struct regipc_leaf_result *l;

        leaf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (leaf == MAP_FAILED)
        {
            fprintf(stderr, "peer: mmap(leaf): %s\n", strerror(errno));
            close(fd);
            return 1;
        }
        l             = leaf;
        l->magic      = REGIPC_LEAF_MAGIC;
        l->checksum   = BENCH_FNV_SEED;
        desc.data     = (uint64_t)(uintptr_t)l;
        desc.data_len = sizeof *l;
        desc.flags    = REGIPC_F_CALLEE;
        rc            = regipc_register(fd, &desc);
        shape         = "leaf";
    }
    if (rc < 0)
    {
        fprintf(stderr, "peer: REGIPC_IOC_REGISTER: %s\n", strerror(-rc));
        close(fd);
        return 1;
    }

    memset(&st, 0, sizeof st);
    rc = regipc_stats(fd, &st);
    if (rc < 0)
    {
        fprintf(stderr, "peer: REGIPC_IOC_STATS: %s\n", strerror(-rc));
        regipc_quit(fd);
        close(fd);
        return 1;
    }

    printf("{\"device\":\"%s\",\"registered\":\"%s\",\"abi\":%u,\"expected_abi\":%u}\n", REGIPC_DEVICE, shape,
           st.abi_version, REGIPC_ABI_VERSION);
    fflush(stdout);
    regipc_quit(fd);
    close(fd);
    return st.abi_version == REGIPC_ABI_VERSION ? 0 : 1;
}

/* ----------------------------------------------------------------- selftest */

static int
bench_selftest(void)
{
    static const char *methods[] = {"shm", "regipc"};
    int                failed    = 0;
    size_t             i;

    for (i = 0; i < sizeof methods / sizeof methods[0]; i++)
    {
        const FastpostgresTransport *t = fastpostgres_transport_by_name(methods[i]);
        BenchOptions                 o;
        BenchResult                  res;
        char                         err[256] = "";

        memset(&o, 0, sizeof o);
        o.role       = BENCH_ROLE_CLIENT;
        o.method     = methods[i];
        o.count      = 1;
        o.reps       = 1;
        o.batch_rows = 100000;

        if (t == NULL || bench_run_one(t, &o, 1, &res, err, sizeof err) != 0 || !res.verified)
        {
            printf("FAIL %s: %s\n", methods[i], err[0] ? err : "unverified");
            failed = 1;
            continue;
        }
        printf("PASS %s: 1 hop, %.3f us, checksum %llu\n", methods[i], res.us_p50,
               (unsigned long long)res.checksum);
    }
    fflush(stdout);
    return failed ? 1 : 0;
}

/* ---------------------------------------------------------------------- cli */

static void
bench_usage(void)
{
    fprintf(stderr,
            "usage: peer --method shm|regipc --count N --reps R [--batch-rows N] [--json]\n"
            "       peer --selftest                        one hop per method, PASS/FAIL lines\n"
            "       peer --abi                             the module's ABI, through REGIPC_IOC_STATS\n"
            "       peer --role worker --method M --segment NAME   (spawned by the client role)\n"
            "\n"
            "  --method     shm or regipc; extension/transport/transport.c resolves the name\n"
            "  --count      hops per run (default 100000)\n"
            "  --reps       runs, one JSON record each (default 1)\n"
            "  --batch-rows rows a real batch would have carried; rows/s = hops/s x this (default 100000)\n"
            "  --json       suppress the human summary on stderr (the JSON record is always on stdout)\n"
            "  --selftest   one hop per method, exit non-zero on failure\n");
}

static int
bench_parse_u64(const char *text, uint64_t *out)
{
    char              *end = NULL;
    unsigned long long value;

    if (text == NULL || *text == 0) return -1;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != 0) return -1;
    *out = (uint64_t)value;
    return 0;
}

int
main(int argc, char **argv)
{
    BenchOptions                 o;
    const FastpostgresTransport *t;
    int                          i;
    int                          failed = 0;

    memset(&o, 0, sizeof o);
    o.role       = BENCH_ROLE_CLIENT;
    o.count      = 100000;
    o.reps       = 1;
    o.batch_rows = 100000;

    for (i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(arg, "--method") == 0 && val != NULL)
        {
            o.method = val;
            i++;
        }
        else if (strcmp(arg, "--segment") == 0 && val != NULL)
        {
            o.segment = val;
            i++;
        }
        else if (strcmp(arg, "--role") == 0 && val != NULL)
        {
            if (strcmp(val, "worker") == 0)
                o.role = BENCH_ROLE_WORKER;
            else if (strcmp(val, "client") == 0)
                o.role = BENCH_ROLE_CLIENT;
            else
            {
                bench_usage();
                return 2;
            }
            i++;
        }
        else if (strcmp(arg, "--count") == 0 && val != NULL)
        {
            if (bench_parse_u64(val, &o.count) != 0 || o.count == 0)
            {
                bench_usage();
                return 2;
            }
            i++;
        }
        else if (strcmp(arg, "--reps") == 0 && val != NULL)
        {
            uint64_t reps;

            if (bench_parse_u64(val, &reps) != 0 || reps == 0 || reps > 1000)
            {
                bench_usage();
                return 2;
            }
            o.reps = (unsigned)reps;
            i++;
        }
        else if (strcmp(arg, "--batch-rows") == 0 && val != NULL)
        {
            if (bench_parse_u64(val, &o.batch_rows) != 0 || o.batch_rows == 0 || o.batch_rows > 0xffffffffull)
            {
                bench_usage();
                return 2;
            }
            i++;
        }
        else if (strcmp(arg, "--json") == 0)
            o.json = true;
        else if (strcmp(arg, "--selftest") == 0)
            o.selftest = true;
        else if (strcmp(arg, "--abi") == 0)
            o.abi = true;
        else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            bench_usage();
            return 0;
        }
        else
        {
            bench_usage();
            return 2;
        }
    }

    if (o.abi) return bench_abi();
    if (o.selftest) return bench_selftest();
    if (o.role == BENCH_ROLE_WORKER) return bench_worker_main(&o);

    if (o.method == NULL)
    {
        bench_usage();
        return 2;
    }
    t = fastpostgres_transport_by_name(o.method);
    if (t == NULL)
    {
        fprintf(stderr, "peer: unknown transport \"%s\" (expected \"shm\" or \"regipc\")\n", o.method);
        return 2;
    }

    for (unsigned rep = 1; rep <= o.reps; rep++)
    {
        BenchResult res;
        char        err[256] = "";

        if (bench_run_one(t, &o, rep, &res, err, sizeof err) != 0)
        {
            fprintf(stderr, "peer: %s rep %u: %s\n", t->name, rep, err[0] ? err : "run failed");
            failed = 1;
            break;
        }
        bench_print_json(t, &o, rep, &res);
        if (!o.json) bench_print_human(t, &o, rep, &res);
        if (!res.verified) failed = 1;
    }
    return failed ? 1 : 0;
}
