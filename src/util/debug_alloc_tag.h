#pragma once

// Debug-only tagging for new/delete to track down free(): invalid size and heap corruption.
// In Debug builds (B_DEBUG), logs to stderr before each alloc/dealloc so you can see which
// site is active when the crash occurs. Uses fprintf(stderr, ...) only to avoid any
// allocation in the logging path.
//
// Relevant allocation sites that use raw new/delete (candidates for invalid size):
//   1. CodeEmitter::Impl  - jit_emitter.cc: new Impl(buffer_size), delete impl_
//   2. BlockMap::BlockPage - block_map.h: new BlockPage(), delete p (in dtor), delete new_page (lost race)
//
// Not tagged here (different allocator): ArenaAllocator uses aligned_alloc/free in arena_alloc.h.
// STL (vector, string, etc.) use the default allocator; if heap is corrupted, any delete can fail.
//
// Set AXOLOTL_ALLOC_DEBUG to 1 to enable the wrapper (logging in B_DEBUG); 0 to disable.

#include <cstdio>
#include <cstdint>

#ifndef AXOLOTL_ALLOC_DEBUG
#define AXOLOTL_ALLOC_DEBUG 0
#endif

#if defined(B_DEBUG) && (AXOLOTL_ALLOC_DEBUG)
#define AXOLOTL_ALLOC_TAG_NEW(tag, expr) ([&]() -> decltype(expr) { \
    std::fprintf(stderr, "AXOLOTL_alloc: new [%s] ...\n", (tag)); \
    auto* _p = (expr); \
    std::fprintf(stderr, "AXOLOTL_alloc: new [%s] ptr=%p\n", (tag), static_cast<const void*>(_p)); \
    return _p; \
}())
#define AXOLOTL_ALLOC_TAG_DELETE(tag, ptr) do { \
    std::fprintf(stderr, "AXOLOTL_alloc: delete [%s] ptr=%p\n", (tag), static_cast<const void*>(ptr)); \
    delete (ptr); \
} while(0)
#else
#define AXOLOTL_ALLOC_TAG_NEW(tag, expr) (expr)
#define AXOLOTL_ALLOC_TAG_DELETE(tag, ptr) do { delete (ptr); } while(0)
#endif
