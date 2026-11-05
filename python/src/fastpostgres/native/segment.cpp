/* segment.cpp — read a result segment (/dev/shm/<seg>) as either zero-copy
 * column views (read) or materialized typed row tuples (read_rows). The worker
 * answers a row request with the row frame (FPROWS) and falls back to the
 * columnar frame (FPCOLUMN) when an attribute is not one the client can walk in
 * place; read_rows takes either. A mapping is kept for the process lifetime:
 * the worker rewrites the same segment in place, so each read only revalidates
 * the header. */
#include "segment.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <x86intrin.h>

#include "codec.hpp"
#include "fastpostgres/fastpostgres_shm.h"
#include "shm_map.h"
#include "types.hpp"

typedef struct {
    std::string name;
    void       *base;
    size_t      size;
    uint64_t    handoff_gen; /* 0: the mapping came from /dev/shm/<name> */
} ReadMap;

/* One mapping per segment name, for the process lifetime. Decoder threads share
 * this cache (the asyncio layer runs one per connection), so the lookup-and-
 * insert is locked: a reallocating push_back under a second reader is UB, even
 * with the GIL held, once the GIL stops being the lock that hides it. */
static std::mutex           g_read_maps_lock;
static std::vector<ReadMap> g_read_maps;

/* ---- fd handoff ----------------------------------------------------------
 *
 * The client listens before it starts the worker (ring.cpp); the worker
 * connects once and sends one record per data segment: the name, the size and
 * the memfd via SCM_RIGHTS. The record travels over SOCK_SEQPACKET, so one
 * recvmsg gets exactly one sendmsg. A record with no descriptor means "this
 * segment is the /dev/shm fallback".
 *
 * The registry is process-wide because read()/read_rows() are module-level
 * functions that only know the segment's name. Each accepted socket is added
 * by Ring_start and removed when its Ring is torn down.
 */
typedef struct {
    std::string name;
    int         fd;  /* -1: use /dev/shm */
    uint64_t    gen; /* identifies this object to the mapping cache */
    int         sock;
} HandoffMsg;

typedef struct {
    uint32_t name_len;
    int32_t  has_fd;
    uint64_t size;
} HandoffRecord;

static std::mutex              g_handoff_lock;
static std::vector<int>        g_handoff_socks;
static std::vector<HandoffMsg> g_handoff_msgs;
static uint64_t                g_handoff_gen;
static pid_t                   g_handoff_pid;

/* Module state is inherited across fork; the parent's sockets are not the
 * child's to read, and a new ring gets its own connection. Closing them in the
 * child cannot affect the parent (separate descriptor tables). */
static void
handoff_check_fork_locked(void)
{
    pid_t pid = getpid();

    if (g_handoff_pid == 0)
    {
        g_handoff_pid = pid;
        return;
    }
    if (g_handoff_pid == pid) return;

    for (int sock : g_handoff_socks) close(sock);
    for (HandoffMsg &m : g_handoff_msgs)
        if (m.fd >= 0) close(m.fd);
    g_handoff_socks.clear();
    g_handoff_msgs.clear();
    g_handoff_pid = pid;
}

void
fp_handoff_register(int sock)
{
    std::lock_guard<std::mutex> held(g_handoff_lock);

    handoff_check_fork_locked();
    g_handoff_socks.push_back(sock);
}

void
fp_handoff_unregister(int sock)
{
    std::lock_guard<std::mutex> held(g_handoff_lock);

    handoff_check_fork_locked();
    for (auto it = g_handoff_socks.begin(); it != g_handoff_socks.end(); ++it)
        if (*it == sock)
        {
            g_handoff_socks.erase(it);
            break;
        }
    for (auto it = g_handoff_msgs.begin(); it != g_handoff_msgs.end();)
        if (it->sock == sock)
        {
            if (it->fd >= 0) close(it->fd);
            it = g_handoff_msgs.erase(it);
        }
        else
            ++it;
}

/* One record; returns 1 when it parsed, 0 when it was malformed and -1 when
 * the socket is gone. */
static int
handoff_recv(int sock, std::string *name, int *fd, uint64_t *gen)
{
    HandoffRecord hdr;
    char          buf[4096 + 64]; /* a full ring argument plus the record header */
    struct iovec  iov;
    char          control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg;
    ssize_t       n;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base           = buf;
    iov.iov_len            = sizeof(buf);
    msg.msg_iov            = &iov;
    msg.msg_iovlen         = 1;
    msg.msg_control        = control;
    msg.msg_controllen     = sizeof(control);

    do
        n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;
    if ((size_t)n < sizeof(hdr)) return 0;

    memcpy(&hdr, buf, sizeof(hdr));
    if (sizeof(hdr) + (size_t)hdr.name_len > (size_t)n) return 0;
    name->assign(buf + sizeof(hdr), hdr.name_len);

    *fd  = -1;
    *gen = 0;
    if (hdr.has_fd)
    {
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);

        if (c == nullptr || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(sizeof(int)))
            return 0;
        memcpy(fd, CMSG_DATA(c), sizeof(int));
        if (*fd < 0) return 0;
    }
    *gen = ++g_handoff_gen;
    return 1;
}

/* Take the next handoff record for `name`, waiting up to `timeout_ms`. Returns
 * the memfd (>= 0), -1 when the worker said "use /dev/shm", or -2 when there is
 * no handoff channel at all (the original protocol, or a worker that predates
 * it). *gen is 0 in the last two cases. */
static int
handoff_take(const std::string &name, int timeout_ms, uint64_t *gen)
{
    std::unique_lock<std::mutex> held(g_handoff_lock);
    int                          fd;

    handoff_check_fork_locked();

    auto take_queued = [&](void) -> bool {
        for (auto it = g_handoff_msgs.begin(); it != g_handoff_msgs.end(); ++it)
            if (it->name == name)
            {
                fd   = it->fd;
                *gen = it->gen;
                g_handoff_msgs.erase(it);
                return true;
            }
        return false;
    };

    if (take_queued()) return fd;
    if (g_handoff_socks.empty())
    {
        *gen = 0;
        return -2;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    for (;;)
    {
        std::vector<struct pollfd> pfds;
        int                        wait_ms;

        for (int sock : g_handoff_socks)
        {
            struct pollfd p;

            p.fd     = sock;
            p.events = POLLIN;
            p.revents = 0;
            pfds.push_back(p);
        }
        wait_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                      deadline - std::chrono::steady_clock::now())
                      .count();
        if (wait_ms < 0) wait_ms = 0;

        /* Held across the poll: a socket must not be closed and its number
         * reused while it is in this set. Only a first mapping waits, and only
         * that one blocks for up to the remaining budget. */
        int rc = poll(pfds.data(), (nfds_t)pfds.size(), wait_ms);
        int e  = errno;

        if (rc < 0 && e == EINTR) continue;
        if (rc <= 0)
        {
            if (take_queued()) return fd;
            *gen = 0;
            return -2;
        }

        for (size_t i = 0; i < pfds.size(); i++)
        {
            std::string n;
            int         nfd;
            uint64_t    g;

            if (pfds[i].revents == 0) continue;
            int r = handoff_recv(pfds[i].fd, &n, &nfd, &g);

            if (r < 0)
            {
                /* the peer is gone: drop the socket from the registry (the
                 * owning Ring still closes its descriptor) */
                for (auto it = g_handoff_socks.begin(); it != g_handoff_socks.end(); ++it)
                    if (*it == pfds[i].fd)
                    {
                        g_handoff_socks.erase(it);
                        break;
                    }
                continue;
            }
            if (r > 0) g_handoff_msgs.push_back({n, nfd, g, pfds[i].fd});
        }
        if (take_queued()) return fd;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            *gen = 0;
            return -2;
        }
    }
}

static void *
read_map_get(const char *path, const std::string &name, size_t *size_out)
{
    std::lock_guard<std::mutex> held(g_read_maps_lock);

    /* A hit usually costs one non-blocking poll: if the worker replaced this
     * segment's object, the record is already queued and the mapping below is
     * replaced with the new descriptor. */
    uint64_t gen = 0;
    int      fd  = handoff_take(name, 0, &gen);

    for (ReadMap &m : g_read_maps)
    {
        if (m.name != name) continue;

        if (gen != 0 && gen != m.handoff_gen)
        {
            munmap(m.base, m.size);
            m.base = nullptr;
            if (fd >= 0)
            {
                m.base = fp_map_fd_ro(fd, &m.size);
                close(fd);
            }
            else
                m.base = fp_map_ro(path, &m.size);
            if (m.base == nullptr) return nullptr;
            m.handoff_gen = gen;
        }
        *size_out = m.size;
        return m.base;
    }

    /* Not mapped yet: wait for the worker's one handoff for this segment. The
     * non-blocking probe above may already have taken it; only wait when there
     * was no record and no handoff facility was consulted. With no handoff
     * socket this returns immediately and the named file is used. */
    if (fd == -2) fd = handoff_take(name, 1000, &gen);

    size_t size = 0;
    void  *base;

    if (fd >= 0)
    {
        base = fp_map_fd_ro(fd, &size);
        close(fd);
    }
    else
        base = fp_map_ro(path, &size);
    if (base == nullptr) return nullptr;
    g_read_maps.push_back({name, base, size, gen});
    *size_out = size;
    return base;
}

/* Map the segment and hand back its payload. */
const uint8_t *
fp_segment_payload(const char *name, uint64_t *payload_len)
{
    std::string path = "/" + std::string(name);
    size_t      size = 0;
    void       *map  = read_map_get(path.c_str(), name, &size);

    if (map == nullptr)
    {
        PyErr_SetString(PyExc_OSError, "cannot map data segment");
        return nullptr;
    }
    if (!fastpostgres_shm_validate(map, size, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres segment");
        return nullptr;
    }
    return (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
}

/* The field names and type OIDs every Result carries. */
static bool
result_meta(const std::vector<FastpostgresColumn> &cols, PyObject **fields, PyObject **types)
{
    *fields = PyList_New((Py_ssize_t)cols.size());
    *types  = PyList_New((Py_ssize_t)cols.size());
    if (*fields == nullptr || *types == nullptr)
    {
        Py_XDECREF(*fields);
        Py_XDECREF(*types);
        return false;
    }
    for (size_t c = 0; c < cols.size(); c++)
    {
        PyObject *name = PyUnicode_FromStringAndSize(cols[c].name, cols[c].name_len);
        PyObject *oid  = PyLong_FromLong((long)cols[c].pgtype);

        if (name == nullptr || oid == nullptr)
        {
            Py_XDECREF(name);
            Py_XDECREF(oid);
            Py_DECREF(*fields);
            Py_DECREF(*types);
            return false;
        }
        PyList_SET_ITEM(*fields, (Py_ssize_t)c, name);
        PyList_SET_ITEM(*types, (Py_ssize_t)c, oid);
    }
    return true;
}

static PyObject *
result_from(const std::vector<FastpostgresColumn> &cols, uint64_t nrows, PyObject *rows)
{
    PyObject *fields, *types;

    if (!result_meta(cols, &fields, &types))
    {
        Py_DECREF(rows);
        return nullptr;
    }
    return fp_result(fields, types, rows, (long)nrows);
}

/* read(seg): the columnar frame as zero-copy column views. */
static PyObject *
segment_read(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    FastpostgresReader rd;
    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    PyObject *columns = PyList_New(rd.ncols);
    if (!columns) return nullptr;

    for (uint32_t c = 0; c < rd.ncols; c++)
    {
        FastpostgresColumn col;
        if (!fastpostgres_reader_next(&rd, &col)) break;

        ColumnObject *column = PyObject_New(ColumnObject, &ColumnType);
        if (!column)
        {
            Py_DECREF(columns);
            return nullptr;
        }
        column->name         = PyUnicode_FromStringAndSize(col.name, col.name_len);
        column->kind         = col.kind;
        column->pgtype       = (long)col.pgtype;
        column->nrows        = (long)rd.nrows;
        column->data         = fp_memory_view(col.data, col.data_len); /* the mapping outlives the chunk */
        size_t offsets_bytes = (size_t)(col.nrows + 1) * 4;
        column->bytes        = (fastpostgres_kind_is_varlena(col.kind) && col.bytes)
                                   ? fp_memory_view(col.bytes, (size_t)col.data_len - offsets_bytes)
                                   : Py_NewRef(Py_None);
        column->nulls        = fp_memory_view(col.nulls, (size_t)((rd.nrows + 7) / 8));
        PyList_SET_ITEM(columns, c, (PyObject *)column);
    }

    SegmentObject *segment = PyObject_New(SegmentObject, &SegmentType);
    if (!segment)
    {
        Py_DECREF(columns);
        return nullptr;
    }
    segment->columns   = columns;
    segment->row_count = PyLong_FromLongLong((long long)rd.nrows);
    return (PyObject *)segment;
}

/* Materialize the chunk as typed row tuples (the DB-API fast path). */
static PyObject *
segment_read_rows(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t           payload_len = 0;
    unsigned long long t0          = __rdtsc();
    const uint8_t     *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    FastpostgresRowsReader rrd;
    if (fastpostgres_rows_init(&rrd, payload, payload_len))
    {
        /* The row frame: PostgreSQL's own tuple bytes, decoded where they lie.
         * The tuples are built by the decoder as it walks each one. */
        FastpostgresRowsDesc            desc;
        std::vector<FastpostgresColumn> cols(rrd.ncols);

        fastpostgres_rows_desc_begin(&desc, payload, (uint64_t)(rrd.p - rrd.base));
        for (uint32_t c = 0; c < rrd.ncols; c++)
            if (!fastpostgres_rows_desc_next(&desc, &cols[c]))
            {
                PyErr_SetString(PyExc_ValueError, "truncated row payload");
                return nullptr;
            }

        PyObject *rows = PyList_New((Py_ssize_t)rrd.left);
        if (!rows) return nullptr;

        if (!fp_decode_tuples(cols.data(), rrd.ncols, &rrd, rows))
        {
            Py_DECREF(rows);
            return nullptr;
        }
        fp_stats_add(__rdtsc() - t0, (unsigned long long)PyList_GET_SIZE(rows));
        return result_from(cols, (uint64_t)PyList_GET_SIZE(rows), rows);
    }

    /* The columnar frame, from the fallback for attributes that have to go
     * through their output function. */
    FastpostgresReader              rd;
    std::vector<FastpostgresColumn> cols;

    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres payload");
        return nullptr;
    }
    cols.resize(rd.ncols);
    uint32_t got = 0;
    for (uint32_t c = 0; c < rd.ncols; c++)
    {
        if (!fastpostgres_reader_next(&rd, &cols[c])) break;
        got++;
    }
    if (got != rd.ncols)
    {
        PyErr_SetString(PyExc_ValueError, "truncated columnar payload");
        return nullptr;
    }

    PyObject *rows = PyList_New((Py_ssize_t)rd.nrows);
    if (!rows) return nullptr;

    /* Every tuple up front, then one specialized pass per column, writing into
     * the row slots as each column is swept. A tuple with NULL slots is only
     * reachable through the list, and both the GC and the deallocator walk
     * tuple items null-safely. */
    for (uint64_t r = 0; r < rd.nrows; r++)
    {
        PyObject *row = PyTuple_New(rd.ncols);
        if (!row)
        {
            Py_DECREF(rows);
            return nullptr;
        }
        PyList_SET_ITEM(rows, (Py_ssize_t)r, row);
    }

    if (!fp_decode_columns(cols.data(), rd.ncols, rd.nrows, rows))
    {
        Py_DECREF(rows);
        return nullptr;
    }
    return result_from(cols, rd.nrows, rows);
}

static PyMethodDef segment_methods[] = {
    {"read", segment_read, METH_VARARGS, "read(segName) -> Segment (zero-copy column views)"},
    {"read_rows", segment_read_rows, METH_VARARGS, "read_rows(segName) -> Result (typed tuple rows)"},
    {nullptr},
};

bool
fp_register_segment(PyObject *module)
{
    for (PyMethodDef *m = segment_methods; m->ml_name != nullptr; m++)
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
