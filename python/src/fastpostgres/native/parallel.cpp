/* parallel.cpp — drive N rings' reduce streams at once, one worker thread each.
 *
 * `Cursor.aggregate()` splits a query into ctid-range partitions (the Python
 * side creates one ring per partition and writes the range SQL), and this is
 * the half that runs them: each pooled thread drives OPEN, REDUCE ... REDUCE,
 * CLOSE on its ring's own mapped header, with the GIL released for the whole
 * run.
 *
 * Work stealing. The caller hands over every range, not one per ring: a thread
 * takes the next range from a shared atomic index and runs it, so a partition
 * that is slower than its neighbours (dead tuples, a hot page, a cold cache)
 * does not set the wall time — the thread that finishes early takes another
 * range. A range is one OPEN/CLOSE pair, which is a parse and a plan against a
 * page-range scan; that is noise at the sizes this is used for.
 *
 * Threads are pooled. Creating six std::threads per call costs 150-250 us,
 * which is 2% of a 100k-row aggregate and 20% of a 10k-row one; the pool makes
 * the cost of a fan-out a couple of condition-variable wakeups instead. The
 * pool is per process and rebuilt when the process forks (a forked child has
 * no threads, and must not touch a mutex a dead thread held).
 *
 * Nothing here decodes a column. The worker's FPREDUCE frame is a header plus
 * one 32-byte slot per op, so a thread only copies the payload out (little-
 * endian u32 length + bytes) and Python turns the frames into scalars with
 * _client.read_reduce_bytes() and combines them with the same per-batch combiner
 * the serial path uses. That is why the split can be this thin: the frames are
 * tiny, and the type knowledge stays on the Python side.
 *
 * Why not the Ring methods: they take the GIL (PyErr_*, PyThread_get_thread_ident)
 * and are the single-driver guard for Python callers. These threads hold no
 * Python state at all — the mapped FastpostgresRing, its transport ops table and
 * a byte buffer — which is what makes the fan-out safe both on an interpreter
 * with the GIL and on a free-threaded one.
 *
 *     _client.aggregate_frames(rings, segs, sqls, plan, batch, snapshot)
 *         -> [bytes per ring, in input order] | None
 *
 * Any ring that cannot be driven — stopped, driven from another process, a
 * partition whose SQL does not fit the ring's 4096-byte argument, a worker error
 * or timeout, a frame over the 64 MiB budget — makes the whole call return None
 * with no exception, so the caller falls back to the serial path rather than
 * guessing which partitions ran.
 */
#include <Python.h>

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "aggregate.hpp"
#include "fastpostgres/fastpostgres_shm.h"
#include "ring.hpp"
#include "segment.hpp"
#include "transport.h"

/* The handoff budgets ring.cpp uses (RING_POST_MS / RING_WAIT_MS there). Those
 * are file-local constants there, so the values are repeated here rather than
 * shared; a fan-out must neither wait longer nor give up sooner than the serial
 * path would. */
#define PAR_POST_MS 1000
#define PAR_WAIT_MS 10000

/* Per-ring ceiling on what one thread buffers. Every frame is a header plus one
 * slot per op (a few hundred bytes), so this bounds batches-in-flight at a very
 * large number while keeping a runaway query from eating the process's memory.
 * Overflow fails the ring, and a failed ring makes the call return None. */
#define PAR_FRAMES_MAX (64ull * 1024ull * 1024ull)

/* One ring's worth of fan-out state. The strings are copies made under the GIL,
 * so the thread needs no Python object: it reads a FastpostgresRing header and
 * its own buffer, nothing else. `sqls`/`next` belong to the call and outlive
 * every thread: a thread takes one range at a time, so rings and ranges are not
 * one-to-one. */
typedef struct {
    RingObject                   *self; /* borrowed by the job, held by its own INCREF */
    FastpostgresRing             *ring;
    const FastpostgresTransport  *ops;
    std::string                   seg;      /* the ring's stream, and its data segment */
    const std::vector<std::string> *sqls;   /* every partition, shared by the call */
    std::atomic<size_t>          *next;     /* the range a thread takes next */
    const char                   *plan;     /* the reduce plan (REDUCE's a2) */
    const char                   *snapshot; /* exported snapshot id, or "" */
    std::vector<uint8_t>          frames;
    uint32_t                      batch;
    int                           ok;     /* every range opened, reduced and closed */
    int                           opened; /* the worker has a stream for this ring */
} ParJob;

/* One request, driven straight on the ring: exactly what ring_send() +
 * ring_collect() do, minus the GIL, the per-thread ownership checks and the
 * Python exceptions. Returns 0 on success, -1 on any failure (post, wait or the
 * worker's own status), so the thread can fail its ring without raising. */
static int
par_request(ParJob *job, uint32_t op, const char *a1, const char *a2, uint32_t batch, int64_t *nrows)
{
    FastpostgresRing *r = job->ring;

    /* The two NUL-terminated arguments share the ring's fixed arg buffer; a
     * request that does not fit is refused by the helper, not truncated. */
    if (fastpostgres_ring_arg_set(r->arg, a1, a2) != 0) return -1;

    r->op       = op;
    r->batch    = batch;
    r->status   = 0;
    r->nrows    = 0;
    r->error[0] = 0;

    /* The sequence this request carries: one more than the ring's counter, which
     * post_request settles; the store below is the backstop. */
    uint64_t seq = __atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) + 1;
    int      rc;

    errno = 0;
    rc    = job->ops->post_request(r, seq, PAR_POST_MS);
    if (rc <= 0) return -1;
    if (__atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) < seq)
        __atomic_store_n(&r->req_seq, seq, __ATOMIC_RELEASE);

    errno = 0;
    rc    = job->ops->wait_response(r, seq, PAR_WAIT_MS);
    if (rc <= 0) return -1;

    /* Everything the worker wrote before its release post is now visible. */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (r->status != 0) return -1;
    if (nrows != nullptr) *nrows = r->nrows;
    return 0;
}

/* Append one frame as little-endian u32 length + payload, the encoding the
 * Python side reads back. 0 on success, -1 when the frame (or the buffer) is
 * over the budget. */
static int
par_append_frame(ParJob *job, const uint8_t *payload, uint64_t payload_len)
{
    if (payload_len > 0xFFFFFFFFull) return -1;
    if ((uint64_t)job->frames.size() + 4 + payload_len > PAR_FRAMES_MAX) return -1;

    uint32_t n  = (uint32_t)payload_len;
    size_t   at = job->frames.size();

    job->frames.resize(at + 4 + (size_t)payload_len);
    job->frames[at + 0] = (uint8_t)(n & 0xFFu);
    job->frames[at + 1] = (uint8_t)((n >> 8) & 0xFFu);
    job->frames[at + 2] = (uint8_t)((n >> 16) & 0xFFu);
    job->frames[at + 3] = (uint8_t)((n >> 24) & 0xFFu);
    if (payload_len != 0) memcpy(job->frames.data() + at + 4, payload, (size_t)payload_len);
    return 0;
}

/* The whole ring's protocol. No Python API in here and no GIL: it reports
 * failure only through job->ok. */
static void
par_run_inner(ParJob *job)
{
    /* The snapshot, when the caller exported one, is part of the ring header the
     * worker reads before it opens the transaction — set once for the ring. */
    if (job->snapshot != nullptr && job->snapshot[0] != '\0')
    {
        size_t n = strlen(job->snapshot);

        if (n >= FASTPOSTGRES_SNAPSHOT_LEN) return;
        memcpy(job->ring->snapshot, job->snapshot, n + 1);
    }
    else
        job->ring->snapshot[0] = '\0';

    for (;;)
    {
        size_t at = job->next->fetch_add(1, std::memory_order_relaxed);

        if (at >= job->sqls->size()) break;

        const std::string &sql = (*job->sqls)[at];

        if (par_request(job, FASTPOSTGRES_OP_OPEN, job->seg.c_str(), sql.c_str(), 0, nullptr) < 0)
            return;
        job->opened = 1;

        for (;;)
        {
            int64_t nrows = 0;

            if (par_request(job, FASTPOSTGRES_OP_REDUCE, job->seg.c_str(), job->plan, job->batch, &nrows) < 0)
                return;

            /* The worker published this batch's frame into the ring's data
             * segment. Re-mapped per batch, as the serial reader does: a memfd
             * handoff gives the worker a new object for the next batch. */
            uint64_t       payload_len = 0;
            const uint8_t *payload     = fp_segment_payload_raw(job->seg.c_str(), &payload_len);
            if (payload == nullptr) return;

            /* Validating the frame here costs a header check and means a
             * malformed or stale segment fails the ring instead of reaching
             * Python. */
            FastpostgresReduceReader rd;

            if (!fastpostgres_reduce_init(&rd, payload, payload_len)) return;
            if (par_append_frame(job, payload, payload_len) < 0) return;

            /* nrows == 0 is the last batch of this range: its frame is the
             * identity/empty result the serial path also combines. */
            if (nrows == 0) break;
        }

        if (par_request(job, FASTPOSTGRES_OP_CLOSE, job->seg.c_str(), nullptr, 0, nullptr) < 0)
            return;
        job->opened = 0;
    }

    job->ok = 1;
}

/* The thread entry point. std::thread treats an escaping exception as terminate,
 * so a bad_alloc from the frame buffer becomes the ring's failure instead. */
static void
par_run(ParJob *job)
{
    try
    {
        par_run_inner(job);
    }
    catch (...)
    {
        job->ok = 0;
    }
}

/* CLOSE for a ring whose thread failed part-way, without waiting for the answer:
 * the same best-effort teardown Ring_stop() does, so an abandoned partition does
 * not leave the worker's portal open. Runs on the calling thread after the
 * join, with no Python API. */
static void
par_close_quietly(ParJob *job)
{
    FastpostgresRing *r = job->ring;

    if (fastpostgres_ring_arg_set(r->arg, job->seg.c_str(), nullptr) != 0) return;

    r->op       = FASTPOSTGRES_OP_CLOSE;
    r->batch    = 0;
    r->status   = 0;
    r->nrows    = 0;
    r->error[0] = 0;

    uint64_t seq = __atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) + 1;

    if (job->ops->post_request(r, seq, PAR_POST_MS) > 0 &&
        __atomic_load_n(&r->req_seq, __ATOMIC_RELAXED) < seq)
        __atomic_store_n(&r->req_seq, seq, __ATOMIC_RELEASE);
}

/* ---- the thread pool -----------------------------------------------------
 *
 * Jobs are pointers to ParJob; a worker pops one, runs it, and decrements the
 * call's pending count. The caller blocks until that count reaches zero. The
 * pool is created on first use and never destroyed: shutdown at process exit
 * would race a thread that is still waiting on the mutex.
 *
 * Fork: a child inherits the memory but not the threads. A pool whose pid does
 * not match the caller's is abandoned (leaked) and a fresh one is installed, so
 * the child never takes a mutex a dead thread held.
 */

struct ParPool
{
    std::mutex               mu;
    std::condition_variable  work;  /* workers wait here */
    std::condition_variable  done;  /* the caller waits here */
    std::vector<ParJob *>    queue;
    size_t                   pending = 0;
    bool                     stop    = false;
    uint32_t                 pid     = 0;
    std::vector<std::thread> threads;
};

static std::atomic<ParPool *> g_par_pool{nullptr};

static void
par_worker(ParPool *p)
{
    for (;;)
    {
        ParJob *job = nullptr;

        {
            std::unique_lock<std::mutex> held(p->mu);

            p->work.wait(held, [p] { return p->stop || !p->queue.empty(); });
            if (p->queue.empty())
            {
                if (p->stop) return;
                continue;
            }
            job = p->queue.back();
            p->queue.pop_back();
        }

        par_run(job);

        {
            std::lock_guard<std::mutex> held(p->mu);

            if (--p->pending == 0) p->done.notify_all();
        }
    }
}

/* The pool for this process, creating one when there is none or the one there
 * belongs to the parent process. */
static ParPool *
par_pool(void)
{
    uint32_t pid = (uint32_t)getpid();
    ParPool *p   = g_par_pool.load(std::memory_order_acquire);

    for (;;)
    {
        if (p != nullptr && p->pid == pid) return p;

        ParPool *fresh = new ParPool();

        fresh->pid = pid;
        if (g_par_pool.compare_exchange_strong(p, fresh, std::memory_order_acq_rel, std::memory_order_acquire))
            return fresh;
        delete fresh; /* nothing has been started on it: no threads, no jobs */
    }
}

/* Grow the pool to at least `want` threads. Returns how many it has; a failed
 * spawn is not fatal (the caller runs the surplus jobs itself). */
static size_t
par_pool_grow(ParPool *p, size_t want)
{
    std::lock_guard<std::mutex> held(p->mu);

    while (p->threads.size() < want)
    {
        try
        {
            p->threads.emplace_back(par_worker, p);
        }
        catch (...)
        {
            break;
        }
    }
    return p->threads.size();
}

/* Hand every job to the pool and wait for all of them. The caller must have
 * released the GIL: this blocks on a native condition variable. */
static void
par_run_jobs(ParPool *p, std::vector<ParJob *> &jobs)
{
    {
        std::lock_guard<std::mutex> held(p->mu);

        p->pending = jobs.size();
        for (ParJob *job : jobs) p->queue.push_back(job);
        p->work.notify_all();
    }

    std::unique_lock<std::mutex> held(p->mu);

    p->done.wait(held, [p] { return p->pending == 0; });
}

static PyObject *
aggregate_frames_impl(PyObject *args)
{
    PyObject                *rings_obj, *segs_obj, *sqls_obj;
    const char              *plan     = nullptr;
    const char              *snapshot = nullptr;
    long long                batch    = 100000;
    PyObject                *rings = nullptr, *segs = nullptr, *sqls = nullptr, *out = nullptr;
    Py_ssize_t               nrings = 0, nranges = 0, increfed = 0;
    std::vector<std::string> ranges;
    std::atomic<size_t>      next{0};
    std::vector<ParJob>      jobs;
    std::vector<ParJob *>    todo;
    bool                     failed = false;
    bool                     ran    = false;
    size_t                   threads = 0;
    ParPool                 *pool   = nullptr;

    if (!PyArg_ParseTuple(args, "OOOs|Ls", &rings_obj, &segs_obj, &sqls_obj, &plan, &batch, &snapshot))
        return nullptr;

    rings = PySequence_Fast(rings_obj, "aggregate_frames: rings must be a sequence of Ring");
    if (rings == nullptr) goto done;
    segs = PySequence_Fast(segs_obj, "aggregate_frames: segs must be a sequence of str");
    if (segs == nullptr) goto done;
    sqls = PySequence_Fast(sqls_obj, "aggregate_frames: sqls must be a sequence of str");
    if (sqls == nullptr) goto done;

    nrings  = PySequence_Fast_GET_SIZE(rings);
    nranges = PySequence_Fast_GET_SIZE(sqls);
    if (PySequence_Fast_GET_SIZE(segs) != nrings)
    {
        PyErr_SetString(PyExc_ValueError, "aggregate_frames: rings and segs must have the same length");
        goto done;
    }
    if (nrings == 0 || nranges == 0)
    {
        out = PyList_New(0); /* nothing to drive: no ring failed */
        goto done;
    }

    /* The row batch, capped where ring.cpp's requests land: a positive int that
     * fits the ring's uint32 batch field (the default is the pull path's). */
    if (batch <= 0) batch = 100000;
    if (batch > (long long)0xFFFFFFFFll) batch = (long long)0xFFFFFFFFll;

    if (snapshot != nullptr && strlen(snapshot) >= FASTPOSTGRES_SNAPSHOT_LEN) snapshot = nullptr;

    /* The ranges are copied under the GIL: the threads read them with no Python
     * object in sight. */
    try
    {
        ranges.reserve((size_t)nranges);
        for (Py_ssize_t i = 0; i < nranges; i++)
        {
            Py_ssize_t  len = 0;
            const char *sql = PyUnicode_AsUTF8AndSize(PySequence_Fast_GET_ITEM(sqls, i), &len);

            if (sql == nullptr) goto done;
            ranges.emplace_back(sql, (size_t)len);
        }
        jobs.resize((size_t)nrings);
        todo.reserve((size_t)nrings);
    }
    catch (...)
    {
        PyErr_NoMemory();
        goto done;
    }

    for (Py_ssize_t i = 0; i < nrings; i++)
    {
        ParJob   *job  = &jobs[(size_t)i];
        PyObject *item = PySequence_Fast_GET_ITEM(rings, i);
        PyObject *sego = PySequence_Fast_GET_ITEM(segs, i);

        if (!PyObject_TypeCheck(item, &RingType))
        {
            PyErr_SetString(PyExc_TypeError, "aggregate_frames: rings must be Ring objects");
            goto done;
        }
        if (!PyUnicode_Check(sego))
        {
            PyErr_SetString(PyExc_TypeError, "aggregate_frames: segs must be str");
            goto done;
        }

        Py_ssize_t  seg_len = 0;
        const char *seg     = PyUnicode_AsUTF8AndSize(sego, &seg_len);

        if (seg == nullptr) goto done;

        RingObject *ro = (RingObject *)item;

        /* A stopped ring, one created by another process, or a half-built object
         * cannot be driven — and driving one is silent corruption (ring.cpp
         * refuses it for the same reason). Fail to None: the caller falls back
         * to a serial path it can still explain. */
        if (ro->ring == nullptr || ro->ops == nullptr || ro->done || ro->pid != (uint32_t)getpid() ||
            seg_len == 0)
        {
            failed = true;
            break;
        }

        /* OPEN carries (seg, sql) and REDUCE (seg, plan); both pairs share the
         * ring's fixed 4096-byte argument. A partition SQL that does not fit is
         * a limit of this interface, not a crash. */
        if ((size_t)seg_len + 1 + strlen(plan) + 1 > FASTPOSTGRES_RING_ARG)
        {
            failed = true;
            break;
        }
        for (const std::string &sql : ranges)
            if ((size_t)seg_len + 1 + sql.size() + 1 > FASTPOSTGRES_RING_ARG)
            {
                failed = true;
                break;
            }
        if (failed) break;

        job->self     = ro;
        job->ring     = ro->ring;
        job->ops      = ro->ops;
        job->seg.assign(seg, (size_t)seg_len);
        job->sqls     = &ranges;
        job->next     = &next;
        job->plan     = plan;
        job->snapshot = snapshot != nullptr ? snapshot : "";
        job->batch    = (uint32_t)batch;
        job->ok       = 0;
        job->opened   = 0;
        todo.push_back(job);
    }

    /* Two threads on one data segment would race on its mapping and on the
     * worker's publish; the interface gives each ring its own, so a repeat is a
     * caller error this can only decline. */
    if (!failed)
        for (Py_ssize_t i = 0; i < nrings && !failed; i++)
            for (Py_ssize_t k = 0; k < i; k++)
                if (jobs[(size_t)i].seg == jobs[(size_t)k].seg)
                {
                    failed = true;
                    break;
                }

    if (failed) goto done;

    for (Py_ssize_t i = 0; i < nrings; i++)
    {
        RingObject *ro = jobs[(size_t)i].self;

        /* Keep every ring alive across the fan-out: the objects may only be held
         * by the sequence above, and the threads run while this frame waits. */
        Py_INCREF((PyObject *)ro);
        increfed++;

        /* Hold each ring's single request slot for the whole fan-out. A Python
         * thread that drives or stops one of these rings meanwhile then gets the
         * ring's own "already in flight" refusal instead of interleaving with a
         * native thread that cannot see it. */
        ro->busy    = 1;
        ro->busy_by = 0;
    }

    /* The fan-out runs here: no Python API, the GIL is held by nobody, and the
     * interpreter can run its own threads while the workers reduce. */
    pool    = par_pool();
    threads = par_pool_grow(pool, todo.size());

    Py_BEGIN_ALLOW_THREADS;
    if (threads >= todo.size())
        par_run_jobs(pool, todo);
    else
        for (ParJob *job : todo) par_run(job); /* no threads to be had: run in line */
    Py_END_ALLOW_THREADS;

    ran = true;

    for (ParJob &job : jobs)
        if (!job.ok) failed = true;

    /* An abandoned ring keeps its worker's stream (and the partition's portal)
     * open until somebody closes it; do it here, best effort, and only when the
     * threads are really done with the rings. */
    if (failed && ran)
    {
        Py_BEGIN_ALLOW_THREADS;
        for (ParJob &job : jobs)
            if (job.opened) par_close_quietly(&job);
        Py_END_ALLOW_THREADS;
    }

    if (failed) goto done;

    out = PyList_New(nrings);
    if (out == nullptr) goto done;

    for (Py_ssize_t i = 0; i < nrings; i++)
    {
        const std::vector<uint8_t> &frames = jobs[(size_t)i].frames;
        const char                 *bytes  = frames.empty() ? "" : (const char *)frames.data();
        PyObject                   *item   = PyBytes_FromStringAndSize(bytes, (Py_ssize_t)frames.size());

        if (item == nullptr)
        {
            Py_CLEAR(out);
            goto done;
        }
        PyList_SET_ITEM(out, i, item);
    }

done:
    /* The rings were checked out, never disowned: release the slot and the
     * reference, whatever happened above. */
    for (Py_ssize_t i = 0; i < increfed; i++)
    {
        RingObject *ro = jobs[(size_t)i].self;

        ro->busy    = 0;
        ro->busy_by = 0;
        Py_DECREF((PyObject *)ro);
    }
    Py_XDECREF(rings);
    Py_XDECREF(segs);
    Py_XDECREF(sqls);

    /* An argument error propagates; a failed ring is None, never an exception. */
    if (out == nullptr)
    {
        if (PyErr_Occurred()) return nullptr;
        Py_RETURN_NONE;
    }
    return out;
}

/* aggregate_frames(rings, segs, sqls, plan, batch, snapshot) -> [bytes, ...] | None.
 * A C++ exception must not unwind into the interpreter, so the body is fenced. */
static PyObject *
aggregate_frames(PyObject *, PyObject *args)
{
    try
    {
        return aggregate_frames_impl(args);
    }
    catch (const std::bad_alloc &)
    {
        PyErr_NoMemory();
        return nullptr;
    }
    catch (...)
    {
        PyErr_SetString(PyExc_RuntimeError, "aggregate_frames: native failure");
        return nullptr;
    }
}

static PyMethodDef parallel_methods[] = {
    {"aggregate_frames", aggregate_frames, METH_VARARGS,
     "aggregate_frames(rings, segs, sqls, plan, batch, snapshot='') -> list of bytes, or None when a ring failed"},
    {nullptr},
};

bool
fp_register_parallel(PyObject *module)
{
    for (PyMethodDef *m = parallel_methods; m->ml_name != nullptr; m++)
    {
        PyObject *fn = PyCFunction_NewEx(m, nullptr, nullptr);
        if (fn == nullptr) return false;
        if (PyModule_AddObject(module, m->ml_name, fn) != 0)
        {
            Py_DECREF(fn);
            return false;
        }
    }
    return true;
}
