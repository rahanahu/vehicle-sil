// SPDX-License-Identifier: MIT
//
// Counts the allocations of the test binary: replaces the global operator new (with
// ThreadSanitizer, whose runtime defines operator new, it counts through the sanitizer's
// allocator hooks instead). Include it in exactly one source of a test executable, which then
// counts every allocation of the process: give such tests a binary of their own.
//
// g_allocations counts while g_counting is true.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VEHICLE_SIL_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define VEHICLE_SIL_TSAN 1
#endif

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void count_allocation()
{
    if (g_counting.load(std::memory_order_relaxed))
    {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

#if defined(VEHICLE_SIL_TSAN)
// The ThreadSanitizer runtime defines operator new itself; count through its allocator hooks.
#include <sanitizer/allocator_interface.h>

namespace {

void malloc_hook(const volatile void*, std::size_t) { count_allocation(); }
void free_hook(const volatile void*) {}
[[maybe_unused]] const int g_hooks_installed = __sanitizer_install_malloc_and_free_hooks(malloc_hook, free_hook);

} // namespace
#else
#if defined(_WIN32)
#include <malloc.h>
#endif
namespace {

void* allocate(std::size_t size)
{
    count_allocation();
    if (void* p = std::malloc(size == 0 ? 1 : size))
    {
        return p;
    }
    throw std::bad_alloc();
}

/// Aligned allocations: MinGW's runtime (UCRT) has no std::aligned_alloc, and its aligned
/// blocks must be freed with _aligned_free.
void* allocate_aligned(std::size_t size, std::size_t alignment)
{
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
#endif
}

void free_aligned(void* p) noexcept
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return operator new(size, tag); }
void* operator new(std::size_t size, std::align_val_t align)
{
    count_allocation();
    const auto a = static_cast<std::size_t>(align);
    if (void* p = allocate_aligned(size == 0 ? 1 : size, a))
    {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t align) { return operator new(size, align); }
void operator delete(void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif
