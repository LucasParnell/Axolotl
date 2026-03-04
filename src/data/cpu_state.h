#pragma once
#include <cstdint>
#include <cstring>  // memset

enum LazyOp : uint32_t {
    kLazyNone  = 0,
    kLazyAdd   = 1,
    kLazySub   = 2,
    kLazyLogic = 3
};

// ARM processor mode bits [4:0] (CPSR/SPSR encoding):
//   0x10 = USR   0x11 = FIQ   0x12 = IRQ   0x13 = SVC
//   0x17 = ABT   0x1B = UND   0x1F = SYS
// USR and SYS share bank index 0.
// Bank index map: 0=USR/SYS, 1=FIQ, 2=IRQ, 3=SVC, 4=ABT, 5=UND
//
// Memory layout (verified by static_assert below, alignas(64)):
//   0x000  registers[16]          64 bytes
//   0x040  cpsr                    4 bytes
//   0x044  spsr                    4 bytes
//   0x048  bank_r13[6]            24 bytes
//   0x060  bank_r14[6]            24 bytes
//   0x078  bank_r8_r12[2][5]      40 bytes
//   0x0A0  bank_spsr[6]           24 bytes
//   0x0B8  cycle_counter           4 bytes
//   0x0BC  halted                  4 bytes  ← uint32_t: no padding hole
//   0x0C0  flag_op                 4 bytes
//   0x0C4  flag_res                4 bytes
//   0x0C8  flag_op1                4 bytes
//   0x0CC  flag_op2                4 bytes
//   0x0D0  page_table_ptr          8 bytes
//   0x0D8  memory_bus              8 bytes
//   0x0E0  read32_helper           8 bytes
//   0x0E8  write32_helper          8 bytes
//   0x0F0  read16_helper           8 bytes
//   0x0F8  write16_helper          8 bytes
//   0x100  read8_helper            8 bytes
//   0x108  write8_helper           8 bytes
//   0x110  (end, raw 272 bytes → padded to 320 for alignas(64))

struct alignas(64) CpuState {
    // ── Active register file ──────────────────────────────────────────────
    // R13 and R14 (and R8–R12 in FIQ mode) hold the current banked values.
    // All 16 entries are always valid for the executing mode.
    uint32_t registers[16] = {};

    // ── Program Status Registers ──────────────────────────────────────────
    // cpsr: mode bits, T (Thumb), I (IRQ mask), F (FIQ mask).
    //       NZCV are NOT stored here while executing — see flag_op/flag_res
    //       below (lazy evaluation).  MaterializeCPSR() merges them on demand.
    // spsr: saved CPSR of the previous mode on exception entry.
    //       USR and SYS modes have no architectural SPSR; bank_spsr[0] is
    //       unused and set to 0 by convention.
    uint32_t cpsr = 0x1F;  // boot in SYS mode (0x1F), interrupts unmasked
    uint32_t spsr = 0;

    // ── Banked registers ─────────────────────────────────────────────────
    // bank_r13 / bank_r14:
    //   Index  Mode
    //     0    USR / SYS  (share one physical bank per ARM DDI 0100E §2.6)
    //     1    FIQ
    //     2    IRQ
    //     3    SVC
    //     4    ABT
    //     5    UND
    uint32_t bank_r13[6] = {};
    uint32_t bank_r14[6] = {};

    // bank_r8_r12: only FIQ has its own R8–R12 bank.
    //   [0] = USR/SYS/IRQ/SVC/ABT/UND shared bank (all non-FIQ modes)
    //   [1] = FIQ bank
    // All non-FIQ modes share bank_r8_r12[0]; FIQ uses bank_r8_r12[1].
    uint32_t bank_r8_r12[2][5] = {};

    // bank_spsr: holds the SPSR for each exception mode.
    //   Index 0 is unused (USR/SYS have no SPSR).
    //   1=FIQ, 2=IRQ, 3=SVC, 4=ABT, 5=UND
    uint32_t bank_spsr[6] = {};

    // ── Timing ───────────────────────────────────────────────────────────
    // Counts GBA master cycles within the current frame.  The dispatcher
    // subtracts kCyclesPerFrame at each VBlank boundary so this never
    // accumulates unboundedly.
    int32_t cycle_counter = 0;

    // ── Halt state ───────────────────────────────────────────────────────
    // Keep halted as uint32_t so layout stays explicit and naturally aligned.
    // Value 0 means running; non-zero means halted.
    uint32_t halted = 0;

    // ── Lazy NZCV flag state ──────────────────────────────────────────────
    // Initialize lazy flag state so first flag materialization is deterministic.
    uint32_t flag_op  = kLazyNone;  // which operation produced this result
    uint32_t flag_res = 0;          // result of the operation
    uint32_t flag_op1 = 0;          // first operand  (for add/sub carry/overflow)
    uint32_t flag_op2 = 0;          // second operand (for add/sub carry/overflow)

    // ── JIT fast-path memory access ───────────────────────────────────────
    // Default to null so missing setup fails clearly instead of using garbage pointers.
    void** page_table_ptr  = nullptr;
    void*  memory_bus      = nullptr;  // MemoryBus* for slow-path helpers

    // Slow-path memory helpers — set by SetupCpuStateForJit() at startup.
    void* read32_helper    = nullptr;
    void* write32_helper   = nullptr;
    void* read16_helper    = nullptr;
    void* write16_helper   = nullptr;
    void* read8_helper     = nullptr;
    void* write8_helper    = nullptr;
};

// ── Layout assertions ─────────────────────────────────────────────────────────
// Keep these static_asserts in sync with jit_emitter offset usage.
// If layout changes, update the emitter and this table together.
static_assert(offsetof(CpuState, registers)     == 0x000, "CpuState layout changed: registers");
static_assert(offsetof(CpuState, cpsr)          == 0x040, "CpuState layout changed: cpsr");
static_assert(offsetof(CpuState, spsr)          == 0x044, "CpuState layout changed: spsr");
static_assert(offsetof(CpuState, bank_r13)      == 0x048, "CpuState layout changed: bank_r13");
static_assert(offsetof(CpuState, bank_r14)      == 0x060, "CpuState layout changed: bank_r14");
static_assert(offsetof(CpuState, bank_r8_r12)   == 0x078, "CpuState layout changed: bank_r8_r12");
static_assert(offsetof(CpuState, bank_spsr)     == 0x0A0, "CpuState layout changed: bank_spsr");
static_assert(offsetof(CpuState, cycle_counter) == 0x0B8, "CpuState layout changed: cycle_counter");
static_assert(offsetof(CpuState, halted)        == 0x0BC, "CpuState layout changed: halted");
static_assert(offsetof(CpuState, flag_op)       == 0x0C0, "CpuState layout changed: flag_op");
static_assert(offsetof(CpuState, flag_res)      == 0x0C4, "CpuState layout changed: flag_res");
static_assert(offsetof(CpuState, flag_op1)      == 0x0C8, "CpuState layout changed: flag_op1");
static_assert(offsetof(CpuState, flag_op2)      == 0x0CC, "CpuState layout changed: flag_op2");
static_assert(offsetof(CpuState, page_table_ptr) == 0x0D0, "CpuState layout changed: page_table_ptr");
static_assert(offsetof(CpuState, memory_bus)    == 0x0D8, "CpuState layout changed: memory_bus");
static_assert(offsetof(CpuState, read32_helper) == 0x0E0, "CpuState layout changed: read32_helper");
static_assert(offsetof(CpuState, write32_helper)== 0x0E8, "CpuState layout changed: write32_helper");
static_assert(offsetof(CpuState, read16_helper) == 0x0F0, "CpuState layout changed: read16_helper");
static_assert(offsetof(CpuState, write16_helper)== 0x0F8, "CpuState layout changed: write16_helper");
static_assert(offsetof(CpuState, read8_helper)  == 0x100, "CpuState layout changed: read8_helper");
static_assert(offsetof(CpuState, write8_helper) == 0x108, "CpuState layout changed: write8_helper");

/** Materialize lazy NZCV into cpu_state->cpsr. Must be called from C++ before
 *  reading CPSR directly (e.g. on IRQ entry, MSR emulation, SPSR capture). */
extern "C" uint32_t MaterializeCPSR(CpuState* state);
