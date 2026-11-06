/* pgxx.h — the bridge between PostgreSQL's C API and C++23.
 *
 * PostgreSQL's headers are C: they must be declared with C linkage or every
 * symbol the backend exports (palloc, elog, heap_deform_tuple, ...) would be
 * looked up under a C++-mangled name.  Everything else in the extension is
 * C++23 and free to use the STL; this header keeps the boundary small and in
 * one place.
 *
 * The allocator below is the other half of the bridge.  A std::pmr container
 * given a PgMemoryResource allocates from a PostgreSQL memory context, so an
 * ereport() longjmp frees the storage with its context even though C++
 * destructors do not run.  Use it for per-batch containers; process-lifetime
 * state can keep the default allocator because it is meant to outlive any
 * context anyway.
 */
#pragma once

extern "C" {
#include "postgres.h"

#include "utils/memutils.h"
#include "utils/palloc.h"
}

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fp
{

/* A PostgreSQL-backed std::pmr memory resource.  The context is captured at
 * construction: a container must be destroyed before that context is reset,
 * which is the normal scope for a per-call container.  Over-aligned requests
 * (rare, but the pmr contract allows them) go through palloc_aligned(). */
class PgMemoryResource final : public std::pmr::memory_resource
{
  public:
    explicit PgMemoryResource(MemoryContext context) noexcept : context_(context)
    {
    }

    MemoryContext context() const noexcept
    {
        return context_;
    }

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        if (alignment <= MAXIMUM_ALIGNOF) return palloc(bytes);
        return palloc_aligned(bytes, alignment, 0);
    }

    void do_deallocate(void *p, std::size_t, std::size_t) override
    {
        if (p != nullptr) pfree(p);
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override
    {
        return this == &other;
    }

    MemoryContext context_;
};

/* A resource for the current context, for a container whose lifetime is one
 * function call.  Keep the resource object alive at least as long as the
 * containers that use it. */
inline PgMemoryResource
current_context_resource()
{
    return PgMemoryResource(CurrentMemoryContext);
}

/* A resource for state that must survive transaction aborts (the carry queue). */
inline PgMemoryResource &
top_context_resource()
{
    static PgMemoryResource resource(TopMemoryContext);

    return resource;
}

template <typename T> using PgVector = std::pmr::vector<T>;

template <typename T> using PgString = std::pmr::string;

/* std::string_view of a possibly-NULL C string. */
inline std::string_view
view(const char *s) noexcept
{
    return s != nullptr ? std::string_view(s) : std::string_view();
}

inline std::string_view
view(const void *s, std::size_t len) noexcept
{
    return {static_cast<const char *>(s), len};
}

/* std::string_view of one PostgreSQL identifier (NameData is NUL-terminated). */
inline std::string_view
name_view(const NameData &name) noexcept
{
    return view(NameStr(name));
}

/* palloc'd copy of a view, for APIs that want a C string. */
inline char *
pdup(std::string_view s)
{
    return pnstrdup(s.data(), static_cast<int>(s.size()));
}

/* Little-endian and native-order scalar helpers the wire format uses.  These
 * are memcpy, not casts: the segment is not guaranteed to be aligned for T. */
template <typename T>
inline T
load(const void *p) noexcept
{
    T value;

    std::memcpy(&value, p, sizeof(value));
    return value;
}

template <typename T>
inline void
store(void *p, T value) noexcept
{
    std::memcpy(p, &value, sizeof(value));
}

inline void
store_u32(char *p, std::uint32_t v) noexcept
{
    store(p, v);
}
inline void
store_u64(char *p, std::uint64_t v) noexcept
{
    store(p, v);
}
inline std::uint32_t
load_u32(const void *p) noexcept
{
    return load<std::uint32_t>(p);
}
inline std::uint64_t
load_u64(const void *p) noexcept
{
    return load<std::uint64_t>(p);
}

/* The bits of a double as the u64 the reduce frame carries, and back, without
 * a strict-aliasing violation. */
inline std::uint64_t
double_bits(double v) noexcept
{
    return std::bit_cast<std::uint64_t>(v);
}

/* std::size_t clamped to the int most PostgreSQL APIs take. */
inline int
to_int(std::size_t n) noexcept
{
    return static_cast<int>(std::min(n, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

inline std::size_t
align8(std::size_t n) noexcept
{
    return (n + 7u) & ~std::size_t{7u};
}

/* Bounded copy into a fixed shared-memory field (the ring's error[]). */
inline void
copy_bounded(char *dst, std::size_t cap, std::string_view src) noexcept
{
    if (cap == 0) return;
    const std::size_t n = std::min(src.size(), cap - 1);

    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // namespace fp
