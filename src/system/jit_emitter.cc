#include "system/jit_emitter.h"
#include "util/logger.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

CodeEmitter::CodeEmitter(size_t buffer_size) 
    : buffer_capacity_(buffer_size), buffer_cursor_(0) {
    code_buffer_ = static_cast<uint8_t*>(AllocateExecutableBuffer(buffer_capacity_));
}

CodeEmitter::~CodeEmitter() {
    FreeExecutableBuffer(code_buffer_, buffer_capacity_);
}

void* CodeEmitter::EmitBlock(ArenaAllocator* /*ir_arena*/, uint32_t /*gba_pc*/) {
    void* block_entry = &code_buffer_[buffer_cursor_];
    size_t dummy_size = 64;  // Stub: reserve space so each block has a unique pointer.

    if (buffer_cursor_ + dummy_size > buffer_capacity_) {
        Logger::log("CodeEmitter: Executable buffer exhausted!", LogLevel::ERR);
        return nullptr;
    }

    buffer_cursor_ += dummy_size;
    return block_entry;
}

void* CodeEmitter::AllocateExecutableBuffer(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    return mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, 
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
}

void CodeEmitter::FreeExecutableBuffer(void* ptr, size_t size) {
#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}