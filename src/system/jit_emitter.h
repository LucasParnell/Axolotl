#pragma once

#include <cstdint>
#include <vector>
#include "data/arena_alloc.h"

// IR -> x86-64 in executable buffer.
class CodeEmitter {
 public:
    explicit CodeEmitter(size_t buffer_size = 32 * 1024 * 1024);
    ~CodeEmitter();

    void* EmitBlock(ArenaAllocator* ir_arena, uint32_t gba_pc);

    CodeEmitter(const CodeEmitter&) = delete;
    CodeEmitter& operator=(const CodeEmitter&) = delete;

 private:
    uint8_t* code_buffer_;
    size_t buffer_capacity_;
    size_t buffer_cursor_;

    void* AllocateExecutableBuffer(size_t size);
    void FreeExecutableBuffer(void* ptr, size_t size);
};