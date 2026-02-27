/**
 * ir_dump: For each ARM instruction (hex on stdin or argv), build IR and print micro-ops.
 * Usage: ir_dump [hex]     → dump IR for one instruction
 *        ir_dump < file    → dump IR for each hex line in file
 */
#include <iostream>
#include <sstream>
#include <string>
#include "system/jit_ir.h"
#include "system/memory_bus.h"
#include "data/arena_alloc.h"
#include "util/ir_printer.h"

static constexpr uint32_t kBasePc = 0x08000000;

// Branch-to-self (B .) so the block ends after the instruction under test
static constexpr uint32_t kSentinelInstr = 0xEAFFFFFE;

static bool RunOne(uint32_t instr, std::string& out_ir) {
    ArenaAllocator arena(4096);
    MemoryBus bus;
    bus.Write32(kBasePc, instr);
    bus.Write32(kBasePc + 4, kSentinelInstr);
    IrBuilder builder(&bus, &arena);
    builder.BuildBlock<false>(kBasePc);
    out_ir = IrPrinter::PrintArena(&arena, kBasePc);
    return true;
}

int main(int argc, char** argv) {
    if (argc >= 2) {
        uint32_t instr = 0;
        std::stringstream ss(argv[1]);
        if (!(ss >> std::hex >> instr)) {
            std::cerr << "invalid hex: " << argv[1] << "\n";
            return 1;
        }
        std::string ir;
        if (!RunOne(instr, ir)) return 1;
        std::cout << ir;
        return 0;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        uint32_t instr = 0;
        std::stringstream ss(line);
        if (!(ss >> std::hex >> instr)) continue;
        std::string ir;
        if (!RunOne(instr, ir)) continue;
        std::cout << ir;
    }
    return 0;
}
