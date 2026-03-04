#pragma once

#include <cstddef>
#include <cstdint>

// ============================================================
// CrashHandler — diagnose JIT emitter segfaults
// ============================================================
// On Linux, catches fatal signals in JIT-emitted code and dumps:
//   1. Signal + faulting address
//   2. Full x86-64 register file (from ucontext_t)
//   3. GBA register state decoded from the JIT x86↔GBA mapping:
//        R8=r0  R9=r1  R10=r2  R11=r3
//        R14=r6 R15=r7  RBP=r13/SP  R12=r14/LR  RBX=r15/PC
//        R13 = CpuState* (r4,r5,r8–r12 live in CpuState)
//   4. CPSR from CPUState (via R13 if valid)
//   5. JIT block lookup: RIP → guest block PC + offset
//   6. Fault block binary + meta to out/ws/crash_dump
//   7. Best-effort backtrace via execinfo
// ============================================================

class CrashHandler {
 public:
  /** Install signal handlers for SIGSEGV, SIGILL, SIGBUS, SIGFPE, SIGABRT. Call once at startup. */
  static void install();

  /** Register a JIT block so the crash handler can map RIP → guest PC and dump the block. */
  static void registerJitBlock(const uint8_t* host_start,
                               size_t host_size,
                               uint32_t guest_block_start,
                               const uint8_t* arm_bytes,
                               size_t arm_size,
                               bool is_thumb);

  /** Unregister a JIT block by host start (e.g. when recompiling). */
  static void unregisterJitBlock(const uint8_t* host_start);

  /** Signal handler (called by kernel; do not call directly). */
  static void signalHandler(int sig, void* siginfo, void* uctx);
};
