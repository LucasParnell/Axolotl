// ============================================================
// CrashHandler — catches fatal signals in JIT code and dumps
// signal info, x86 regs, GBA regs (from JIT mapping), CPSR,
// JIT block lookup, fault block binary, and backtrace.
// ============================================================

#include "system/crash_handler.h"
#include "data/cpu_state.h"

#include <algorithm>
#include <cstdarg>
#include <atomic>
#include <vector>
#include <cstring>

#if defined(__linux__)
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if defined(__linux__)

// Register indices in ucontext_t::uc_mcontext.gregs[] (x86-64); from <sys/ucontext.h>.
// REG_R8=0..REG_R15=7, REG_RDI=8, REG_RSI=9, REG_RBP=10, REG_RBX=11, REG_RDX=12,
// REG_RAX=13, REG_RCX=14, REG_RSP=15, REG_RIP=16, REG_EFL=17.

// CpuState layout (must match data/cpu_state.h)
static constexpr size_t kCpuStateRegsOffset = offsetof(CpuState, registers);
static constexpr size_t kCpuStateCpsrOffset = offsetof(CpuState, cpsr);

struct JitBlockEntry {
  const uint8_t* start;
  size_t size;
  uint32_t guest_block_start;
  std::vector<uint8_t> arm_bytes;
  bool is_thumb = false;
};

static std::atomic_flag g_jit_lock = ATOMIC_FLAG_INIT;
static std::vector<JitBlockEntry> g_jit_blocks;

struct JitBlocksGuard {
  JitBlocksGuard() {
    while (g_jit_lock.test_and_set(std::memory_order_acquire)) {}
  }
  ~JitBlocksGuard() { g_jit_lock.clear(std::memory_order_release); }
};

static const char* SigName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (Segmentation fault)";
    case SIGILL:  return "SIGILL  (Illegal instruction)";
    case SIGBUS:  return "SIGBUS  (Bus error)";
    case SIGFPE:  return "SIGFPE  (Arithmetic exception)";
    case SIGABRT: return "SIGABRT (Abort)";
    default:      return "Unknown signal";
  }
}

static const char* SiCodeName(int sig, int code) {
  if (sig == SIGSEGV) {
    switch (code) {
      case SEGV_MAPERR: return "SEGV_MAPERR (address not mapped)";
      case SEGV_ACCERR: return "SEGV_ACCERR (invalid permissions)";
      default:          return "(unknown SIGSEGV code)";
    }
  }
  if (sig == SIGILL) {
    switch (code) {
      case ILL_ILLOPC: return "ILL_ILLOPC (illegal opcode)";
      case ILL_ILLOPN: return "ILL_ILLOPN (illegal operand)";
      default:         return "(unknown SIGILL code)";
    }
  }
  if (sig == SIGBUS) {
    switch (code) {
      case BUS_ADRALN: return "BUS_ADRALN (invalid address alignment)";
      case BUS_ADRERR: return "BUS_ADRERR (non-existent physical address)";
      default:         return "(unknown SIGBUS code)";
    }
  }
  return "";
}

static const char* kCrashDumpDir = "out/ws/crash_dump";
static const char* kCrashReportFile = "out/ws/crash_dump/crash_report.txt";

// Write formatted line to fd (used for report file so stderr isn't interleaved by other threads).
static void WriteReport(int fd, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0 && n < static_cast<int>(sizeof(buf)))
    write(fd, buf, static_cast<size_t>(n));
}

static void WriteReportStr(int fd, const char* s) {
  write(fd, s, strlen(s));
}

// Write fault block to out/ws/crash_dump (same layout as DumpAnalysisFiles).
// Uses only open/write/close/mkdir for signal-handler safety.
static void WriteFaultBlockToFile(const uint8_t* host_start, size_t host_size,
                                  uint32_t guest_pc, uint32_t rip_offset,
                                  const uint8_t* arm_start, size_t arm_size,
                                  bool is_thumb) {
  mkdir("out", 0755);
  mkdir("out/ws", 0755);
  mkdir(kCrashDumpDir, 0755);

  char path[128];
  int n = snprintf(path, sizeof(path), "%s/fault_0x%08x_x86.bin", kCrashDumpDir, guest_pc);
  if (n <= 0 || n >= static_cast<int>(sizeof(path))) return;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  size_t written = 0;
  while (written < host_size) {
    ssize_t r = write(fd, host_start + written, host_size - written);
    if (r <= 0) break;
    written += static_cast<size_t>(r);
  }
  close(fd);

  if (arm_start && arm_size > 0) {
    const char* suffix = is_thumb ? "thumb" : "arm";
    n = snprintf(path, sizeof(path), "%s/fault_0x%08x_%s.bin", kCrashDumpDir, guest_pc, suffix);
    if (n > 0 && n < static_cast<int>(sizeof(path))) {
      fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) {
        written = 0;
        while (written < arm_size) {
          ssize_t r = write(fd, arm_start + written, arm_size - written);
          if (r <= 0) break;
          written += static_cast<size_t>(r);
        }
        close(fd);
      }
    }
  }

  n = snprintf(path, sizeof(path), "%s/fault_0x%08x_meta.txt", kCrashDumpDir, guest_pc);
  if (n <= 0 || n >= static_cast<int>(sizeof(path))) return;
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  char meta[384];
  n = snprintf(meta, sizeof(meta),
               "# Crash fault block (same layout as dump <addr>)\n"
               "guest_pc=0x%08x\nrip_offset=0x%x\nx86_block_size=%zu\n",
               guest_pc, rip_offset, host_size);
  if (n > 0 && n < static_cast<int>(sizeof(meta)))
    write(fd, meta, static_cast<size_t>(n));
  if (arm_start && arm_size > 0) {
    n = snprintf(meta, sizeof(meta), "arm_block_size=%zu\nis_thumb=%u\n", arm_size, is_thumb ? 1u : 0u);
    if (n > 0 && n < static_cast<int>(sizeof(meta)))
      write(fd, meta, static_cast<size_t>(n));
  }
  close(fd);
}

void CrashHandler::signalHandler(int sig, void* siginfo_ptr, void* uctx) {
  siginfo_t* info = static_cast<siginfo_t*>(siginfo_ptr);
  ucontext_t* ctx = static_cast<ucontext_t*>(uctx);

  static volatile sig_atomic_t s_in_handler = 0;
  if (s_in_handler) {
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }
  s_in_handler = 1;

  mkdir("out", 0755);
  mkdir("out/ws", 0755);
  mkdir(kCrashDumpDir, 0755);
  int report_fd = open(kCrashReportFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (report_fd < 0) report_fd = -1;

  const auto& regs = ctx->uc_mcontext.gregs;
  const uint64_t rip = static_cast<uint64_t>(regs[REG_RIP]);
  const uint64_t rsp = static_cast<uint64_t>(regs[REG_RSP]);
  const uint64_t rbp = static_cast<uint64_t>(regs[REG_RBP]);
  const uint64_t rax = static_cast<uint64_t>(regs[REG_RAX]);
  const uint64_t rbx = static_cast<uint64_t>(regs[REG_RBX]);
  const uint64_t rcx = static_cast<uint64_t>(regs[REG_RCX]);
  const uint64_t rdx = static_cast<uint64_t>(regs[REG_RDX]);
  const uint64_t rsi = static_cast<uint64_t>(regs[REG_RSI]);
  const uint64_t rdi = static_cast<uint64_t>(regs[REG_RDI]);
  const uint64_t r8  = static_cast<uint64_t>(regs[REG_R8]);
  const uint64_t r9  = static_cast<uint64_t>(regs[REG_R9]);
  const uint64_t r10 = static_cast<uint64_t>(regs[REG_R10]);
  const uint64_t r11 = static_cast<uint64_t>(regs[REG_R11]);
  const uint64_t r12 = static_cast<uint64_t>(regs[REG_R12]);
  const uint64_t r13 = static_cast<uint64_t>(regs[REG_R13]);
  const uint64_t r14 = static_cast<uint64_t>(regs[REG_R14]);
  const uint64_t r15 = static_cast<uint64_t>(regs[REG_R15]);
  const uint64_t efl = static_cast<uint64_t>(regs[REG_EFL]);

  if (report_fd >= 0) {
    WriteReportStr(report_fd, "\n");
    WriteReportStr(report_fd, "╔══════════════════════════════════════════════════════════╗\n");
    WriteReportStr(report_fd, "║              Axolotl — FATAL CRASH REPORT               ║\n");
    WriteReportStr(report_fd, "╚══════════════════════════════════════════════════════════╝\n\n");

    WriteReport(report_fd, "Signal   : %s\n", SigName(sig));
    if (info) {
      WriteReport(report_fd, "Code     : %s\n", SiCodeName(sig, info->si_code));
      WriteReport(report_fd, "FaultAddr: %p\n", info->si_addr);
    }
    WriteReportStr(report_fd, "\n");

    WriteReportStr(report_fd, "── x86-64 Registers ──────────────────────────────────────\n");
    WriteReport(report_fd, "  RIP = 0x%016llx   RFLAGS = 0x%016llx\n",
                static_cast<unsigned long long>(rip), static_cast<unsigned long long>(efl));
    WriteReport(report_fd, "  RAX = 0x%016llx   RBX    = 0x%016llx\n",
                static_cast<unsigned long long>(rax), static_cast<unsigned long long>(rbx));
    WriteReport(report_fd, "  RCX = 0x%016llx   RDX    = 0x%016llx\n",
                static_cast<unsigned long long>(rcx), static_cast<unsigned long long>(rdx));
    WriteReport(report_fd, "  RSI = 0x%016llx   RDI    = 0x%016llx\n",
                static_cast<unsigned long long>(rsi), static_cast<unsigned long long>(rdi));
    WriteReport(report_fd, "  RBP = 0x%016llx   RSP    = 0x%016llx\n",
                static_cast<unsigned long long>(rbp), static_cast<unsigned long long>(rsp));
    WriteReport(report_fd, "  R8  = 0x%016llx   R9     = 0x%016llx\n",
                static_cast<unsigned long long>(r8),  static_cast<unsigned long long>(r9));
    WriteReport(report_fd, "  R10 = 0x%016llx   R11    = 0x%016llx\n",
                static_cast<unsigned long long>(r10), static_cast<unsigned long long>(r11));
    WriteReport(report_fd, "  R12 = 0x%016llx   R13    = 0x%016llx\n",
                static_cast<unsigned long long>(r12), static_cast<unsigned long long>(r13));
    WriteReport(report_fd, "  R14 = 0x%016llx   R15    = 0x%016llx\n",
                static_cast<unsigned long long>(r14), static_cast<unsigned long long>(r15));
    WriteReportStr(report_fd, "\n");
  }

  bool in_jit = false;
  uint32_t guest_pc = 0;
  uint32_t x86_off = 0;

  for (const auto& blk : g_jit_blocks) {
    if (rip >= reinterpret_cast<uint64_t>(blk.start) &&
        rip < reinterpret_cast<uint64_t>(blk.start) + blk.size) {
      in_jit = true;
      x86_off = static_cast<uint32_t>(rip - reinterpret_cast<uint64_t>(blk.start));
      guest_pc = blk.guest_block_start;

      const uint8_t* arm_ptr = blk.arm_bytes.empty() ? nullptr : blk.arm_bytes.data();
      size_t arm_sz = blk.arm_bytes.size();
      WriteFaultBlockToFile(blk.start, blk.size, guest_pc, x86_off, arm_ptr, arm_sz, blk.is_thumb);

      if (report_fd >= 0) {
        WriteReportStr(report_fd, "── JIT Block ─────────────────────────────────────────────\n");
        WriteReport(report_fd, "  Block host range   : [%p, %p)\n",
                   static_cast<const void*>(blk.start),
                   static_cast<const void*>(blk.start + blk.size));
        WriteReport(report_fd, "  RIP offset in block: +0x%x\n", x86_off);
        WriteReport(report_fd, "  → Guest block PC   : 0x%08x\n", guest_pc);
        WriteReport(report_fd, "  Block binary written to %s/fault_0x%08x_x86.bin", kCrashDumpDir, guest_pc);
        if (arm_sz > 0)
          WriteReport(report_fd, " + fault_0x%08x_%s.bin", guest_pc, blk.is_thumb ? "thumb" : "arm");
        WriteReportStr(report_fd, " (+ _meta.txt)\n\n");
      }
      break;
    }
  }

  if (!in_jit && report_fd >= 0) {
    WriteReport(report_fd, "  RIP 0x%016llx is NOT in any registered JIT block.\n",
                static_cast<unsigned long long>(rip));
    WriteReportStr(report_fd, "  (Crash is in native code — check the backtrace below.)\n\n");
  }

  if (in_jit && report_fd >= 0) {
    WriteReportStr(report_fd, "── GBA Register State (from x86 register mapping) ──────\n");
    WriteReport(report_fd, "  r0  (R8 ) = 0x%08x\n",  static_cast<uint32_t>(r8));
    WriteReport(report_fd, "  r1  (R9 ) = 0x%08x\n",  static_cast<uint32_t>(r9));
    WriteReport(report_fd, "  r2  (R10) = 0x%08x\n",  static_cast<uint32_t>(r10));
    WriteReport(report_fd, "  r3  (R11) = 0x%08x\n",  static_cast<uint32_t>(r11));
    WriteReportStr(report_fd, "  r4, r5   = in CpuState (memory)\n");
    WriteReport(report_fd, "  r6  (R14) = 0x%08x\n", static_cast<uint32_t>(r14));
    WriteReport(report_fd, "  r7  (R15) = 0x%08x\n", static_cast<uint32_t>(r15));
    WriteReportStr(report_fd, "  r8–r12   = in CpuState (memory)\n");
    WriteReport(report_fd, "  r13/SP (RBP) = 0x%08x\n", static_cast<uint32_t>(rbp));
    WriteReport(report_fd, "  r14/LR (R12) = 0x%08x\n", static_cast<uint32_t>(r12));
    WriteReport(report_fd, "  r15/PC (RBX) = 0x%08x\n", static_cast<uint32_t>(rbx));
    WriteReport(report_fd, "  CpuState* (R13) = 0x%016llx\n", static_cast<unsigned long long>(r13));

    if (r13 >= 0x1000 && r13 < 0x0000800000000000ULL) {
      volatile const uint32_t* state_regs =
          reinterpret_cast<volatile const uint32_t*>(r13 + kCpuStateRegsOffset);
      volatile const uint32_t* cpsr_ptr =
          reinterpret_cast<volatile const uint32_t*>(r13 + kCpuStateCpsrOffset);
      uint32_t r4_val = state_regs[4];
      uint32_t r5_val = state_regs[5];
      uint32_t pc_val = state_regs[15];
      uint32_t cpsr_val = *cpsr_ptr;
      WriteReport(report_fd, "  r4  (CpuState) = 0x%08x\n", r4_val);
      WriteReport(report_fd, "  r5  (CpuState) = 0x%08x\n", r5_val);
      WriteReport(report_fd, "  r15/PC (CpuState) = 0x%08x\n", pc_val);
      WriteReport(report_fd, "  CPSR (CpuState)   = 0x%08x\n", cpsr_val);
      WriteReport(report_fd, "    N=%u Z=%u C=%u V=%u T=%u Mode=0x%02x\n",
                  (cpsr_val >> 31) & 1, (cpsr_val >> 30) & 1, (cpsr_val >> 29) & 1,
                  (cpsr_val >> 28) & 1, (cpsr_val >> 5) & 1, cpsr_val & 0x1F);
    } else {
      WriteReportStr(report_fd, "  R13 does not look like a valid CpuState* pointer.\n");
    }
    WriteReportStr(report_fd, "\n");
  }

  if (report_fd >= 0) {
    WriteReportStr(report_fd, "── Stack top (8 quadwords) ───────────────────────────────\n");
    if (rsp >= 0x1000 && rsp < 0x0000800000000000ULL) {
      volatile const uint64_t* sp = reinterpret_cast<volatile const uint64_t*>(rsp);
      for (int i = 0; i < 8; ++i)
        WriteReport(report_fd, "  [RSP+%02d] = 0x%016llx\n", i * 8, static_cast<unsigned long long>(sp[i]));
    } else {
      WriteReportStr(report_fd, "  RSP looks invalid — skipping.\n");
    }
    WriteReportStr(report_fd, "\n");

    WriteReportStr(report_fd, "── Backtrace ─────────────────────────────────────────────\n");
    void* bt[64];
    int n_frames = backtrace(bt, 64);
    char** symbols = backtrace_symbols(bt, n_frames);
    if (symbols) {
      for (int i = 0; i < n_frames; ++i)
        WriteReport(report_fd, "%s\n", symbols[i] ? symbols[i] : "???");
      free(symbols);
    }
    WriteReportStr(report_fd, "\n");
    WriteReportStr(report_fd, "── End of crash report ───────────────────────────────────\n");
    close(report_fd);
  }

  char msg[256];
  int n = snprintf(msg, sizeof(msg), "Crash report written to %s\n", kCrashReportFile);
  if (n > 0 && n < static_cast<int>(sizeof(msg)))
    write(STDERR_FILENO, msg, static_cast<size_t>(n));

  signal(sig, SIG_DFL);
  raise(sig);
}

static void StaticSignalHandler(int sig, siginfo_t* info, void* uctx) {
  CrashHandler::signalHandler(sig, info, uctx);
}

void CrashHandler::install() {
  struct sigaction sa {};
  sa.sa_sigaction = &StaticSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);

  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGILL, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGABRT, &sa, nullptr);

  fprintf(stderr, "[CrashHandler] Signal handlers installed.\n");
}

void CrashHandler::registerJitBlock(const uint8_t* host_start,
                                     size_t host_size,
                                     uint32_t guest_block_start,
                                     const uint8_t* arm_bytes,
                                     size_t arm_size,
                                     bool is_thumb) {
  JitBlocksGuard lock;
  g_jit_blocks.erase(
      std::remove_if(g_jit_blocks.begin(), g_jit_blocks.end(),
                    [host_start](const JitBlockEntry& e) { return e.start == host_start; }),
      g_jit_blocks.end());

  JitBlockEntry entry;
  entry.start = host_start;
  entry.size = host_size;
  entry.guest_block_start = guest_block_start;
  entry.is_thumb = is_thumb;
  if (arm_bytes && arm_size > 0)
    entry.arm_bytes.assign(arm_bytes, arm_bytes + arm_size);

  auto pos = std::lower_bound(g_jit_blocks.begin(), g_jit_blocks.end(), host_start,
                              [](const JitBlockEntry& e, const uint8_t* p) { return e.start < p; });
  g_jit_blocks.insert(pos, std::move(entry));
}

void CrashHandler::unregisterJitBlock(const uint8_t* host_start) {
  JitBlocksGuard lock;
  g_jit_blocks.erase(
      std::remove_if(g_jit_blocks.begin(), g_jit_blocks.end(),
                    [host_start](const JitBlockEntry& e) { return e.start == host_start; }),
      g_jit_blocks.end());
}

#else  // !__linux__

void CrashHandler::install() {}

void CrashHandler::registerJitBlock(const uint8_t* /*host_start*/,
                                    size_t /*host_size*/,
                                    uint32_t /*guest_block_start*/,
                                    const uint8_t* /*arm_bytes*/,
                                    size_t /*arm_size*/,
                                    bool /*is_thumb*/) {}

void CrashHandler::unregisterJitBlock(const uint8_t* /*host_start*/) {}

void CrashHandler::signalHandler(int /*sig*/, void* /*siginfo_ptr*/, void* /*uctx*/) {}

#endif  // __linux__
