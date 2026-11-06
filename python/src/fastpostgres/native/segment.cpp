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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
    int         fd;          /* memfd kept for diagnostics, or -1 for /dev/shm */
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
/* Bulk-loaded columnar tables: (pid, fd) names an immutable sealed memfd, or a
 * handle names a descriptor the client received over SCM_RIGHTS (pid == -1).
 * The mapping is kept for the process lifetime and never rewritten, so unlike
 * the streaming segment above there is no next-batch invalidation.  The
 * received-fd path deliberately does not key on the fd number: the client
 * closes it once mapped, and the number could be reused by an unrelated
 * descriptor while the old mapping is still cached. */
typedef struct {
    int      handle; /* 0: a /proc entry; otherwise the client-side capability */
    int      pid;
    int      fd;
    uint64_t size; /* the size the caller asked for (0 = fstat) */
    void    *base;
    uint64_t len;
} ColumnarMap;

static std::mutex                 g_columnar_lock;
static std::vector<ColumnarMap>   g_columnar_maps;
static int                        g_columnar_next_handle = 1;

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
            if (m.fd >= 0)
            {
                close(m.fd);
                m.fd = -1;
            }
            if (fd >= 0)
            {
                m.base = fp_map_fd_ro(fd, &m.size);
                if (m.base != nullptr)
                    m.fd = fd;
                else
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

    size_t size   = 0;
    void  *base   = nullptr;
    int    keepfd = -1;

    if (fd >= 0)
    {
        base = fp_map_fd_ro(fd, &size);
        if (base != nullptr)
            keepfd = fd; /* the descriptor is how segment_fd() inspects the seal */
        else
            close(fd);
    }
    else
        base = fp_map_ro(path, &size);
    if (base == nullptr) return nullptr;
    g_read_maps.push_back({name, base, size, gen, keepfd});
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

/* The silent twin of fp_segment_payload(): same cache lookup, same header
 * validation, no Python API. The parallel aggregate's worker threads run with
 * the GIL released, so they cannot raise; a nullptr is their only failure
 * signal and the caller turns it into "this ring failed", not an exception. */
const uint8_t *
fp_segment_payload_raw(const char *name, uint64_t *payload_len)
{
    std::string path = "/" + std::string(name);
    size_t      size = 0;
    void       *map  = read_map_get(path.c_str(), name, &size);

    if (map == nullptr) return nullptr;
    if (!fastpostgres_shm_validate(map, size, payload_len)) return nullptr;
    return (const uint8_t *)map + FASTPOSTGRES_SHM_HEADER_SIZE;
}

/* segment_fd(segName) -> int | None
 *
 * A duplicate of the memfd the segment is mapped from, for diagnostics: the
 * regression test reads F_GET_SEALS and proves both shrink and grow are
 * refused. None means this segment came from /dev/shm (the fallback), which
 * has no descriptor to inspect. */
static PyObject *
segment_fd(PyObject *, PyObject *args)
{
    const char *name;

    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    std::string path = "/" + std::string(name);
    size_t      size = 0;

    if (read_map_get(path.c_str(), name, &size) == nullptr) return nullptr;

    std::lock_guard<std::mutex> held(g_read_maps_lock);
    for (ReadMap &m : g_read_maps)
    {
        int fd;

        if (m.name != name) continue;
        if (m.fd < 0) Py_RETURN_NONE;
        fd = dup(m.fd);
        if (fd < 0)
        {
            PyErr_SetFromErrno(PyExc_OSError);
            return nullptr;
        }
        return PyLong_FromLong(fd);
    }
    Py_RETURN_NONE;
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

/* Turn one FPCOLUMN payload into the Segment of zero-copy column views. */
static PyObject *
segment_from_payload(const uint8_t *payload, uint64_t payload_len)
{
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

/* read(seg): the columnar frame as zero-copy column views. */
static PyObject *
segment_read(PyObject *, PyObject *args)
{
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_segment_payload(name, &payload_len);
    if (payload == nullptr) return nullptr;

    return segment_from_payload(payload, payload_len);
}

/* Map an already-open columnar descriptor.  The caller owns `fd` and may close
 * it on return: the mapping keeps its own reference to the sealed object.
 * Caller holds g_columnar_lock. */
static const uint8_t *
columnar_map_fd(int fd, int pid, int handle, uint64_t size, uint64_t *payload_len)
{
    struct stat st;

    if (fstat(fd, &st) != 0)
    {
        PyErr_SetFromErrno(PyExc_OSError);
        return nullptr;
    }

    uint64_t len = size ? size : (uint64_t)st.st_size;
    if ((uint64_t)st.st_size < len)
    {
        PyErr_Format(PyExc_ValueError,
                     "columnar memfd is %lld bytes, but %llu were asked for",
                     (long long)st.st_size, (unsigned long long)len);
        return nullptr;
    }

    void *base = mmap(nullptr, (size_t)len, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED)
    {
        PyErr_SetFromErrno(PyExc_OSError);
        return nullptr;
    }

    FastpostgresReader probe;
    if (!fastpostgres_reader_init(&probe, base, len))
    {
        munmap(base, (size_t)len);
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    g_columnar_maps.push_back({handle, pid, fd, size, base, len});
    *payload_len = len;
    return (const uint8_t *)base;
}

/* Map a bulk-loaded columnar memfd, via the backend that owns it (/proc path). */
const uint8_t *
fp_columnar_map(int pid, int fd, uint64_t size, uint64_t *payload_len)
{
    std::lock_guard<std::mutex> held(g_columnar_lock);

    for (ColumnarMap &m : g_columnar_maps)
    {
        if (m.pid == pid && m.fd == fd && m.size == size)
        {
            *payload_len = m.len;
            return (const uint8_t *)m.base;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd/%d", pid, fd);

    int h = open(path, O_RDONLY | O_CLOEXEC);
    if (h < 0)
    {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path);
        return nullptr;
    }

    const uint8_t *payload = columnar_map_fd(h, pid, 0, size, payload_len);

    close(h);
    return payload;
}

/* Map a descriptor the client received over SCM_RIGHTS.  A handle names the
 * mapping afterwards, so the fd can be closed and its number reused without
 * ever aliasing a cached object. */
const uint8_t *
fp_columnar_map_handle(int handle, uint64_t size, uint64_t *payload_len)
{
    std::lock_guard<std::mutex> held(g_columnar_lock);

    for (ColumnarMap &m : g_columnar_maps)
    {
        if (m.handle != 0 && m.handle == handle)
        {
            if (size != 0 && m.size != 0 && m.size != size)
            {
                PyErr_SetString(PyExc_ValueError, "columnar handle used with a different size");
                return nullptr;
            }
            *payload_len = m.len;
            return (const uint8_t *)m.base;
        }
    }

    PyErr_SetString(PyExc_ValueError, "columnar handle is not attached");
    return nullptr;
}

static PyObject *
columnar_attach(PyObject *, PyObject *args)
{
    int                pid, fd;
    unsigned long long size;

    if (!PyArg_ParseTuple(args, "iiK", &pid, &fd, &size)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_columnar_map(pid, fd, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    return segment_from_payload(payload, payload_len);
}

/* columnar_attach_fd(fd, size) -> (Segment, handle)
 *
 * The SCM_RIGHTS half of the columnar path: `fd` is a descriptor received on
 * the control socket.  Unlike columnar_attach() there is no pid and no
 * /proc lookup; the descriptor itself is the capability.  The mapping holds the
 * object, so the caller may close `fd` once this returns.
 */
static PyObject *
columnar_attach_fd(PyObject *, PyObject *args)
{
    int                fd;
    unsigned long long size;

    if (!PyArg_ParseTuple(args, "iK", &fd, &size)) return nullptr;
    if (fd < 0)
    {
        PyErr_SetString(PyExc_ValueError, "columnar_attach_fd: invalid descriptor");
        return nullptr;
    }

    std::lock_guard<std::mutex> held(g_columnar_lock);

    int            handle      = g_columnar_next_handle++;
    uint64_t       payload_len = 0;
    const uint8_t *payload     = columnar_map_fd(fd, -1, handle, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    PyObject *segment = segment_from_payload(payload, payload_len);
    if (segment == nullptr)
    {
        /* The mapping stays in the cache: it is valid for the process lifetime
         * and another attach of the same handle would find it. */
        return nullptr;
    }

    PyObject *number = PyLong_FromLong(handle);
    PyObject *out    = number ? PyTuple_New(2) : nullptr;

    if (out == nullptr)
    {
        Py_XDECREF(number);
        Py_DECREF(segment);
        return nullptr;
    }
    PyTuple_SET_ITEM(out, 0, segment);
    PyTuple_SET_ITEM(out, 1, number);
    return out;
}

/* FNV-1a over 8-byte words (and a byte tail), chosen only so the full-column
 * walk has a data dependency on every byte it reads: the point is to touch
 * every page, not to be cryptographic.  Hashing a word at a time keeps the
 * chain from hiding the memory traffic it is supposed to measure. */
static inline uint64_t
hash_bytes(const uint8_t *p, uint64_t n, uint64_t h)
{
    uint64_t i = 0;

    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;

        memcpy(&w, p + i, 8);
        h ^= w;
        h *= 1099511628211ULL;
        h ^= h >> 29;
    }
    for (; i < n; i++)
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* columnar_scan(pid, fd, size) -> (rows, data_bytes, checksum, ncols)
 *
 * A real scan of the whole table: every fixed-width byte and every varlena
 * payload byte is read where it lies, with no per-row object and no copy.  The
 * checksum is a deterministic function of the payload, and `data_bytes` is the
 * column bytes touched (null slots for fixed kinds included, matching data_len),
 * so a caller can report bytes/s and page faults for the whole SELECT *. */
static PyObject *
scan_payload(const uint8_t *payload, uint64_t payload_len)
{
    FastpostgresReader rd;
    if (!fastpostgres_reader_init(&rd, payload, payload_len))
    {
        PyErr_SetString(PyExc_ValueError, "not a fastpostgres columnar payload");
        return nullptr;
    }

    const uint64_t rows     = rd.nrows;
    const uint32_t ncols    = rd.ncols;
    uint64_t       bytes    = 0;
    uint64_t       checksum = 1469598103934665603ULL;

    Py_BEGIN_ALLOW_THREADS;
    for (uint32_t c = 0; c < ncols; c++)
    {
        FastpostgresColumn col;

        if (!fastpostgres_reader_next(&rd, &col)) break;

        if (fastpostgres_kind_is_varlena(col.kind))
        {
            const uint32_t *off   = (const uint32_t *)col.data;
            uint64_t        total = rows ? (uint64_t)off[rows] : 0;

            bytes += total;
            checksum = hash_bytes(col.bytes, total, checksum);
        }
        else
        {
            int      width = fastpostgres_kind_width(col.kind);
            uint64_t n     = width > 0 ? rows * (uint64_t)width : 0;

            bytes += n;
            checksum = hash_bytes(col.data, n, checksum);
        }
    }
    Py_END_ALLOW_THREADS;

    return Py_BuildValue("(KKKn)", (unsigned long long)rows, (unsigned long long)bytes,
                         (unsigned long long)checksum, (Py_ssize_t)ncols);
}

static PyObject *
columnar_scan(PyObject *, PyObject *args)
{
    int                pid, fd;
    unsigned long long size;

    if (!PyArg_ParseTuple(args, "iiK", &pid, &fd, &size)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_columnar_map(pid, fd, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    return scan_payload(payload, payload_len);
}

/* The same full walk over a descriptor received with SCM_RIGHTS. */
static PyObject *
columnar_scan_handle(PyObject *, PyObject *args)
{
    int                handle;
    unsigned long long size;

    if (!PyArg_ParseTuple(args, "iK", &handle, &size)) return nullptr;

    uint64_t       payload_len = 0;
    const uint8_t *payload     = fp_columnar_map_handle(handle, (uint64_t)size, &payload_len);
    if (payload == nullptr) return nullptr;

    return scan_payload(payload, payload_len);
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

/* A memoryview over a buffer the caller owns, for a Column that must outlive
 * the segment the values arrived in. */
static PyObject *
column_view(PyObject *obj)
{
    if (obj == Py_None) return Py_NewRef(Py_None);
    if (PyMemoryView_Check(obj)) return Py_NewRef(obj);
    return PyMemoryView_FromObject(obj);
}

/* column(name, kind, pgtype, nrows, data, bytes, nulls) -> Column
 *
 * The lazy read copies one batch's column regions out of /dev/shm and keeps
 * them as Column objects, so a row can be decoded long after the ring went
 * back to the pool. The buffers are held by the Column exactly as read()'s are
 * held by its mapping: same accessors, same value_at(), different lifetime. */
static PyObject *
column_from_buffers(PyObject *, PyObject *args)
{
    PyObject *name, *data_obj, *bytes_obj, *nulls_obj;
    long      kind, pgtype, nrows;

    if (!PyArg_ParseTuple(args, "OlllOOO", &name, &kind, &pgtype, &nrows, &data_obj, &bytes_obj, &nulls_obj))
        return nullptr;
    if (!PyUnicode_Check(name))
    {
        PyErr_SetString(PyExc_TypeError, "column(): name must be str");
        return nullptr;
    }
    if (nrows < 0)
    {
        PyErr_SetString(PyExc_ValueError, "column(): negative row count");
        return nullptr;
    }

    PyObject *data  = column_view(data_obj);
    PyObject *bytes = column_view(bytes_obj);
    PyObject *nulls = column_view(nulls_obj);
    if (data == nullptr || bytes == nullptr || nulls == nullptr)
    {
        Py_XDECREF(data);
        Py_XDECREF(bytes);
        Py_XDECREF(nulls);
        return nullptr;
    }

    ColumnObject *column = PyObject_New(ColumnObject, &ColumnType);
    if (column == nullptr)
    {
        Py_DECREF(data);
        Py_DECREF(bytes);
        Py_DECREF(nulls);
        return nullptr;
    }
    column->name    = Py_NewRef(name);
    column->kind    = kind;
    column->pgtype  = pgtype;
    column->nrows   = nrows;
    column->data    = data;
    column->bytes   = bytes;
    column->nulls   = nulls;
    return (PyObject *)column;
}


/* decode_rows(columns, start, count) -> list of tuples
 *
 * A contiguous window of a copied columnar batch, decoded in C. The lazy
 * result's iteration and unit-step slices use this: the kind switch runs once
 * per column per window instead of once per value, while a consumer that stops
 * early still pays for only the window it asked for. */
static PyObject *
decode_rows(PyObject *, PyObject *args)
{
    PyObject  *columns;
    Py_ssize_t start, count;

    if (!PyArg_ParseTuple(args, "Onn", &columns, &start, &count)) return nullptr;
    if (!PyList_Check(columns))
    {
        PyErr_SetString(PyExc_TypeError, "decode_rows(): columns must be a list");
        return nullptr;
    }
    if (start < 0 || count < 0)
    {
        PyErr_SetString(PyExc_ValueError, "decode_rows(): negative start or count");
        return nullptr;
    }

    Py_ssize_t                      ncols = PyList_GET_SIZE(columns);
    std::vector<FastpostgresColumn> cols((size_t)ncols);

    for (Py_ssize_t c = 0; c < ncols; c++)
    {
        PyObject *item = PyList_GET_ITEM(columns, c);
        if (!PyObject_TypeCheck(item, &ColumnType))
        {
            PyErr_SetString(PyExc_TypeError, "decode_rows(): expected Column objects");
            return nullptr;
        }
        ColumnObject *column = (ColumnObject *)item;
        FastpostgresColumn &fc = cols[(size_t)c];

        if (start + count > column->nrows)
        {
            PyErr_SetString(PyExc_IndexError, "row range out of range for this chunk");
            return nullptr;
        }
        memset(&fc, 0, sizeof(fc));
        fc.kind   = (uint8_t)column->kind;
        fc.pgtype = (uint32_t)column->pgtype;
        fc.nulls  = fp_col_bytes(column->nulls);
        fc.data   = fp_col_bytes(column->data);
        fc.bytes  = fp_col_bytes(column->bytes);
        fc.nrows  = (uint64_t)column->nrows;

        if (count > 0 && fc.nulls == nullptr)
        {
            PyErr_SetString(PyExc_ValueError, "decode_rows(): column has no null bitmap");
            return nullptr;
        }
        if (count > 0 && fc.data == nullptr)
        {
            PyErr_SetString(PyExc_ValueError, "decode_rows(): column has no data");
            return nullptr;
        }
    }

    PyObject *rows = PyList_New(count);
    if (rows == nullptr) return nullptr;
    for (Py_ssize_t r = 0; r < count; r++)
    {
        PyObject *tuple = PyTuple_New(ncols);
        if (tuple == nullptr)
        {
            Py_DECREF(rows);
            return nullptr;
        }
        PyList_SET_ITEM(rows, r, tuple);
    }
    if (!fp_decode_columns_range(cols.data(), (uint32_t)ncols, (uint64_t)start, (uint64_t)count, rows))
    {
        Py_DECREF(rows);
        return nullptr;
    }
    return rows;
}

/* decode_row(columns, row) -> tuple
 *
 * One row, decoded in one call: the lazy result's __getitem__ and iteration use
 * this instead of a Python-level loop over value_at(), so a consumer that reads
 * rows one by one pays one C call per row rather than one per value. */
static PyObject *
decode_row(PyObject *, PyObject *args)
{
    PyObject  *columns;
    Py_ssize_t row;

    if (!PyArg_ParseTuple(args, "On", &columns, &row)) return nullptr;
    if (!PyList_Check(columns))
    {
        PyErr_SetString(PyExc_TypeError, "decode_row(): columns must be a list");
        return nullptr;
    }

    Py_ssize_t ncols = PyList_GET_SIZE(columns);
    PyObject  *tuple = PyTuple_New(ncols);
    if (tuple == nullptr) return nullptr;

    for (Py_ssize_t c = 0; c < ncols; c++)
    {
        PyObject *item = PyList_GET_ITEM(columns, c);
        if (!PyObject_TypeCheck(item, &ColumnType))
        {
            PyErr_SetString(PyExc_TypeError, "decode_row(): expected Column objects");
            Py_DECREF(tuple);
            return nullptr;
        }
        ColumnObject *column = (ColumnObject *)item;
        if (row < 0 || row >= column->nrows)
        {
            PyErr_SetString(PyExc_IndexError, "row out of range for this chunk");
            Py_DECREF(tuple);
            return nullptr;
        }

        FastpostgresColumn fc;
        memset(&fc, 0, sizeof(fc));
        fc.kind   = (uint8_t)column->kind;
        fc.pgtype = (uint32_t)column->pgtype;
        fc.nulls  = fp_col_bytes(column->nulls);
        fc.data   = fp_col_bytes(column->data);
        fc.bytes  = fp_col_bytes(column->bytes);
        fc.nrows  = (uint64_t)column->nrows;

        PyObject *value = fp_decode_column(&fc, (uint64_t)row);
        if (value == nullptr)
        {
            Py_DECREF(tuple);
            return nullptr;
        }
        PyTuple_SET_ITEM(tuple, c, value);
    }
    return tuple;
}

static PyMethodDef segment_methods[] = {
    {"read", segment_read, METH_VARARGS, "read(segName) -> Segment (zero-copy column views)"},
    {"read_rows", segment_read_rows, METH_VARARGS, "read_rows(segName) -> Result (typed tuple rows)"},
    {"segment_fd", segment_fd, METH_VARARGS,
     "segment_fd(segName) -> duplicated memfd for inspection, or None for /dev/shm"},
    {"column", column_from_buffers, METH_VARARGS,
     "column(name, kind, pgtype, nrows, data, bytes, nulls) -> Column over caller-owned buffers"},
    {"decode_rows", decode_rows, METH_VARARGS,
     "decode_rows(columns, start, count) -> list of tuples for a row window"},
    {"decode_row", decode_row, METH_VARARGS, "decode_row(columns, row) -> one tuple"},
    {"columnar_attach", columnar_attach, METH_VARARGS,
     "columnar_attach(pid, fd, size) -> Segment (read-only memfd, zero-copy column views)"},
    {"columnar_attach_fd", columnar_attach_fd, METH_VARARGS,
     "columnar_attach_fd(fd, size) -> (Segment, handle) (descriptor received over SCM_RIGHTS)"},
    {"columnar_scan", columnar_scan, METH_VARARGS,
     "columnar_scan(pid, fd, size) -> (rows, data_bytes, checksum, ncols) (in-place full walk)"},
    {"columnar_scan_handle", columnar_scan_handle, METH_VARARGS,
     "columnar_scan_handle(handle, size) -> (rows, data_bytes, checksum, ncols)"},
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
