#pragma once

#include <string>
#include "data/arena_alloc.h"
#include "data/ir_node.h"

class IrPrinter {
public:
    static std::string PrintArena(ArenaAllocator* arena, uint32_t start_pc);

private:
    static std::string OpToString(IrOp op);
    static std::string CondToString(uint8_t cond);
};