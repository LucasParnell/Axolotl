#include "system/dispatcher.h"
#include "data/block_map.h"
#include "data/cpu_state.h"
#include "data/ir_node.h"
#include "system/block_compile_cache.h"
#include "system/crash_handler.h"
#include "system/memory_bus.h"
#include "system/gba_timing.h"
#include "util/debug_profiler.h"
#include "util/ir_printer.h"
#include "util/logger.h"

#ifdef B_DEBUG
#include "util/debug_tools.h"
#endif

#include <cstdio>
#include <iostream>

#ifdef B_DEBUG
// ── Pause / step ───────────────────────────────────────────────────────────
void JitDispatcher::HandlePauseStep() {
    // Edge-triggered: convert a pause request into the sustained paused state.
    if (pause_requested_.exchange(false, std::memory_order_acq_rel)) {
        std::ostringstream msg;
        msg << "[Dispatcher] Paused at PC=0x" << std::hex << cpu_state_.registers[15];
        Logger::log(msg.str(), LogLevel::DEBUG);
        paused_.store(true, std::memory_order_release);
    }

    // If paused and no step budget exists, block until resume/step.
    // If a step budget exists, release pause and run exactly one block.
    if (paused_.load(std::memory_order_acquire)) {
        if (step_count_.load(std::memory_order_acquire) <= 0) {
            while (paused_.load(std::memory_order_acquire) &&
                   system_running_.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else {
            paused_.store(false, std::memory_order_release);
        }
    }

    // Burn one step token for this block. When the last token is consumed,
    // queue a pause request so the next loop iteration pauses before executing.
    if (int32_t n = step_count_.load(std::memory_order_relaxed); n > 0) {
        if (step_count_.fetch_sub(1, std::memory_order_relaxed) == 1) {
            pause_requested_.store(true, std::memory_order_release);
        }
    }
}

// ── IR print / disasm helpers exposed to DebugConsole ─────────────────────
void JitDispatcher::PrintIrForBlock(uint32_t pc) {
    bool thumb = (cpu_state_.cpsr >> 5) & 1;
    if (thumb) jit_builder_.BuildBlock<true>(pc);
    else       jit_builder_.BuildBlock<false>(pc);
    std::cout << IrPrinter::PrintArena(jit_builder_.GetArena(), pc);
}

void JitDispatcher::DisasmBlockAtAddress(uint32_t pc) {
    bool thumb = (cpu_state_.cpsr >> 5) & 1;
    if (thumb) jit_builder_.BuildBlock<true>(pc);
    else       jit_builder_.BuildBlock<false>(pc);

    uint32_t block_len = jit_builder_.GetBlockLength();
    MemoryBus* bus = jit_builder_.GetBus();

    std::vector<uint8_t> arm_bytes;
    arm_bytes.reserve(thumb ? block_len * 2u : block_len * 4u);
    for (uint32_t a = pc; a < pc + block_len; a += thumb ? 2u : 4u) {
        if (thumb) {
            uint16_t w = bus->Read16(a, a);
            arm_bytes.push_back(static_cast<uint8_t>(w & 0xFF));
            arm_bytes.push_back(static_cast<uint8_t>(w >> 8));
        } else {
            uint32_t w = bus->Read32(a, a);
            arm_bytes.push_back(static_cast<uint8_t>(w        & 0xFF));
            arm_bytes.push_back(static_cast<uint8_t>((w >>  8) & 0xFF));
            arm_bytes.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
            arm_bytes.push_back(static_cast<uint8_t>( w >> 24));
        }
    }

    void*  host_code = jit_emitter_.EmitBlock(jit_builder_.GetArena(), pc,
                                               jit_builder_.GetBlockCycles());
    size_t x86_size  = jit_emitter_.GetLastEmittedBlockSize();
    std::vector<uint8_t> x86_bytes(static_cast<const uint8_t*>(host_code),
                                   static_cast<const uint8_t*>(host_code) + x86_size);
    std::string ir = IrPrinter::PrintArena(jit_builder_.GetArena(), pc);
    RunDisasmScriptForBlock(arm_bytes, x86_bytes, ir, pc, thumb);
}
#endif  // B_DEBUG

void JitDispatcher::HandleHostPause() {
    bool active = host_pause_active_.load(std::memory_order_acquire);
    const bool requested = host_pause_requested_.load(std::memory_order_acquire);
    if (requested != active) {
        host_pause_active_.store(requested, std::memory_order_release);
        active = requested;
    }

    while (active && system_running_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool next_requested = host_pause_requested_.load(std::memory_order_acquire);
        if (next_requested != active) {
            host_pause_active_.store(next_requested, std::memory_order_release);
            active = next_requested;
        }
    }
}

#include <cstdlib>
#include <sys/wait.h>

extern "C" void SwapBankedRegisters(CpuState* state, uint32_t old_mode, uint32_t new_mode);
#include <exception>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

using JitFunc = void (*)(CpuState*);

#ifdef B_DEBUG
#ifndef LOGO_SETUP_TRACE
#define LOGO_SETUP_TRACE 0
#endif
#ifndef ENABLE_BLOCK_PROBE
#define ENABLE_BLOCK_PROBE 0
#endif
#ifndef ENABLE_PC_TRACE
#define ENABLE_PC_TRACE 0
#endif
#ifndef PC_TRACE_MAX_ENTRIES
#define PC_TRACE_MAX_ENTRIES 0
#endif
#ifndef PC_TRACE_REGS_POST
#define PC_TRACE_REGS_POST 0
#endif
#ifndef ENABLE_LOOP_WARNINGS
#define ENABLE_LOOP_WARNINGS 0
#endif
#ifndef ENABLE_PREWARM_DUMP_CAPTURE
#define ENABLE_PREWARM_DUMP_CAPTURE 0
#endif
#ifndef IRQ_PATH_TRACE
#define IRQ_PATH_TRACE 0
#endif
#ifndef LOOP_CYCLE_TRACE
#define LOOP_CYCLE_TRACE 0
#endif
#endif

namespace {

// GBA IE/IF bits 0..13 (GBATEK)
const char* const kIrqSourceNames[] = {
    "VBlank", "HBlank", "VCount", "Timer0", "Timer1", "Timer2", "Timer3",
    "Serial", "DMA0", "DMA1", "DMA2", "DMA3", "Keypad", "GamePak"
};

std::string FormatIrqSources(uint16_t ie_and_req) {
    std::ostringstream out;
    const char* sep = "";
    for (int i = 0; i < 14 && (ie_and_req >> i); ++i) {
        if ((ie_and_req >> i) & 1) {
            out << sep << kIrqSourceNames[i];
            sep = ",";
        }
    }
    return out.str();
}

}  // namespace

namespace {
constexpr size_t kMaxX86BlockSize = 64 * 1024;  // 64 KB cap per block
constexpr const char* kAnalysisPath = "../analysis";

// Performance tuning knobs (kept intentionally small/safe):
// - AXOLOTL_BLOCK_USAGE_STATS: per-block usage accounting for analysis.
// - AXOLOTL_HALT_STEP_CYCLES / AXOLOTL_HALT_STEP_MAX_CYCLES: HALT pacing.
// - AXOLOTL_TIMING_BATCH_ENABLE / AXOLOTL_TIMING_BATCH_MAX_CYCLES: timing flush batching.
constexpr uint32_t kDefaultHaltStepCycles = 64u;
constexpr uint32_t kDefaultHaltStepMaxCycles = 1024u;
constexpr uint32_t kDefaultTimingBatchMaxCycles = 32u;

#ifdef B_DEBUG
struct CpuSnapshot {
    uint32_t regs[16];
    uint32_t cpsr;
};

inline CpuSnapshot TakeSnapshot(const CpuState& cpu) {
    CpuSnapshot s{};
    std::memcpy(s.regs, cpu.registers, sizeof(s.regs));
    s.cpsr = cpu.cpsr;
    return s;
}

inline void LogUnexpectedCpuMutation(const char* phase,
                                     const CpuSnapshot& before,
                                     const CpuSnapshot& after,
                                     uint32_t entry_pc,
                                     bool entry_thumb,
                                     uint32_t now_pc,
                                     bool now_thumb) {
#ifdef B_DEBUG
    static int mutation_log_count = 0;
    if (mutation_log_count >= 96) return;
    std::ostringstream msg;
    msg << "[HostStateMut] phase=" << phase
        << " entry=0x" << std::hex << entry_pc << (entry_thumb ? "T" : "A")
        << " now=0x" << std::hex << now_pc << (now_thumb ? "T" : "A");
    for (int i = 0; i < 16; ++i) {
        if (before.regs[i] != after.regs[i]) {
            msg << " r" << std::dec << i
                << ":0x" << std::hex << before.regs[i]
                << "->0x" << std::hex << after.regs[i];
        }
    }
    if (before.cpsr != after.cpsr) {
        msg << " cpsr:0x" << std::hex << before.cpsr
            << "->0x" << std::hex << after.cpsr;
    }
    Logger::log(msg.str(), LogLevel::WARNING);
    ++mutation_log_count;
#else
    (void)phase; (void)before; (void)after; (void)entry_pc; (void)entry_thumb; (void)now_pc; (void)now_thumb;
#endif
}
#endif

bool ParseEnvBool(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (!value) return fallback;
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
        std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
        std::strcmp(value, "ON") == 0 || std::strcmp(value, "yes") == 0 ||
        std::strcmp(value, "YES") == 0) {
        return true;
    }
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 ||
        std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 ||
        std::strcmp(value, "OFF") == 0 || std::strcmp(value, "no") == 0 ||
        std::strcmp(value, "NO") == 0) {
        return false;
    }
    return fallback;
}

uint32_t ParseEnvU32Clamped(const char* name, uint32_t fallback, uint32_t min_value, uint32_t max_value) {
    const char* value = std::getenv(name);
    if (!value || *value == '\0') return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value) return fallback;
    const uint32_t v = static_cast<uint32_t>(parsed);
    return std::clamp(v, min_value, max_value);
}

inline void EmulateNonExecutableOpenBusStep(CpuState* cpu, uint32_t pc, bool is_thumb) {
    const uint32_t step = is_thumb ? 2u : 4u;
    const uint32_t next_pc = (pc + step) & (is_thumb ? ~1u : ~3u);
    cpu->registers[15] = next_pc;
    cpu->cycle_counter += static_cast<int32_t>(GbaTiming::FetchS(pc >> 24, is_thumb));
}
}  // namespace

#ifdef B_DEBUG
namespace {

enum class ProbePhase { kPre, kPost };

struct BlockProbeTarget {
    uint32_t pc;
    bool thumb;
    const char* name;
    bool dump_pre;
    bool dump_post;
};

constexpr BlockProbeTarget kBlockProbeTargets[] = {
    {0x00000BA4u, false, "cpuset_setup_a", true, true},
    {0x00000BACu, false, "cpuset_setup_b", true, true},
    {0x00000BC4u, false, "cpuset_entry", true, true},
    {0x00000BD4u, false, "cpuset_copy_prologue", true, true},
    {0x00000BD8u, false, "cpuset_copy_load", true, true},
    {0x00000BE4u, false, "cpuset_copy_ctrl", true, true},
    {0x00000C04u, false, "cpuset_copy_loop", true, true},
    {0x000013C4u, true,  "logo_prod_13c4", true, true},
    {0x00001434u, true,  "logo_prod_1434", true, true},
    {0x0000159Cu, true,  "logo_prod_159c", true, true},
    {0x000015A6u, true,  "logo_15a6_path_a", true, true},
    {0x00001608u, true,  "logo_1608_path_b", true, true},
    {0x00001664u, true,  "logo_prod_1664", true, true},
    {0x00001992u, true,  "logo_call_1664", true, true},
    {0x0000199Eu, true,  "logo_call_13c4_a", true, true},
    {0x000019A8u, true,  "logo_call_13c4_b", true, true},
    {0x00001BEAu, true,  "logo_call_1434", true, true},
    {0x000022F0u, true,  "logo_22f0_call_159c", true, true},
    {0x000022FAu, true,  "logo_22fa_gate", true, true},
    {0x00002301u, true,  "logo_2301_path_a", true, true},
    {0x0000237Eu, true,  "logo_237e_path_b", true, true},
    {0x00002424u, true,  "logo_2424_worker_a", true, true},
    {0x0000244Cu, true,  "logo_244c_scan_bytes", true, true},
    {0x00002456u, true,  "logo_2456_scan_bytes", true, true},
    {0x00002468u, true,  "logo_2468_worker_b", true, true},
    {0x000024C0u, true,  "logo_24c0_worker_c", true, true},
    {0x000023C6u, true,  "logo_23c6_queue_head", true, true},
    {0x000023CCu, true,  "logo_23cc_queue_link", true, true},
    {0x000023D8u, true,  "logo_23d8_queue_store", true, true},
    {0x000023E0u, true,  "logo_23e0_queue_clear", true, true},
    {0x0000256Eu, true,  "logo_256e_link_prep", true, true},
    {0x0000257Cu, true,  "logo_257c_link_commit", true, true},
};

inline uint64_t ProbeKey(uint32_t pc, bool thumb) {
    return (static_cast<uint64_t>(pc) << 1) | static_cast<uint64_t>(thumb ? 1u : 0u);
}

class BlockProbeDumper {
public:
    void Initialize(const std::string& analysis_root) {
        const std::filesystem::path dir = std::filesystem::path(analysis_root) / "block_probe";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        csv_path_ = (dir / "block_probe.csv").string();
        meta_path_ = (dir / "README.txt").string();
        file_ = std::fopen(csv_path_.c_str(), "w");
        if (!file_) return;
        enabled_ = true;

        for (size_t i = 0; i < std::size(kBlockProbeTargets); ++i) {
            const auto& t = kBlockProbeTargets[i];
            target_by_key_[ProbeKey(t.pc, t.thumb)] = i;
        }

        std::fprintf(file_,
            "event,total_hits,streak_hits,phase,target_name,entry_pc,entry_mode,next_pc,next_mode,"
            "cycle,cpsr,sp,lr,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,"
            "mem_r0,mem_r1,m03007ea0,m03003b2c,m030036ec,m0300372c,m0300390c,m0300394c,"
            "v06002440,v06002840,v06002c40,v06017400,v06017600,"
            "m_r5_p20,m_r4_p34\n");

        std::ofstream meta(meta_path_, std::ios::trunc);
        if (meta.is_open()) {
            meta << "Block probe dump\n";
            meta << "CSV: " << csv_path_ << "\n";
            meta << "Fields include PRE/POST phase, loop streak counter, full register state, and key memory probes.\n\n";
            meta << "Targets:\n";
            for (const auto& t : kBlockProbeTargets) {
                meta << "  PC=0x" << std::hex << std::setw(8) << std::setfill('0') << t.pc
                     << " mode=" << (t.thumb ? "T" : "A")
                     << " pre=" << (t.dump_pre ? "1" : "0")
                     << " post=" << (t.dump_post ? "1" : "0")
                     << " name=" << t.name << "\n";
            }
        }
        Logger::log("[BlockProbe] writing " + csv_path_, LogLevel::INFO);
    }

    void Shutdown() {
        if (!file_) return;
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
        enabled_ = false;
    }

    void DumpPre(uint32_t entry_pc, bool entry_thumb, const CpuState& cpu, MemoryBus* bus) {
        DumpInternal(ProbePhase::kPre, entry_pc, entry_thumb, 0xFFFFFFFFu, false, cpu, bus);
    }

    void DumpPost(uint32_t entry_pc, bool entry_thumb,
                  uint32_t next_pc, bool next_thumb,
                  const CpuState& cpu, MemoryBus* bus) {
        DumpInternal(ProbePhase::kPost, entry_pc, entry_thumb, next_pc, next_thumb, cpu, bus);
    }

private:
    void DumpInternal(ProbePhase phase,
                      uint32_t entry_pc, bool entry_thumb,
                      uint32_t next_pc, bool next_thumb,
                      const CpuState& cpu, MemoryBus* bus) {
        if (!enabled_ || !file_ || !bus) return;
        const auto it = target_by_key_.find(ProbeKey(entry_pc, entry_thumb));
        if (it == target_by_key_.end()) return;
        const BlockProbeTarget& t = kBlockProbeTargets[it->second];
        if ((phase == ProbePhase::kPre && !t.dump_pre) ||
            (phase == ProbePhase::kPost && !t.dump_post)) {
            return;
        }

        const uint64_t key = ProbeKey(entry_pc, entry_thumb);
        const uint64_t event = ++event_id_;
        const uint64_t total = ++total_hits_[key];

        if (last_phase_ == phase && last_key_ == key) {
            ++streak_hits_;
        } else {
            streak_hits_ = 1;
        }
        last_phase_ = phase;
        last_key_ = key;

        const uint32_t r0 = cpu.registers[0];
        const uint32_t r1 = cpu.registers[1];
        const uint32_t mem_r0 = bus->Read32(r0, entry_pc);
        const uint32_t mem_r1 = bus->Read32(r1, entry_pc);

        const uint32_t m_307ea0 = bus->Read32(0x03007EA0u, entry_pc);
        const uint32_t m_303b2c = bus->Read32(0x03003B2Cu, entry_pc);
        const uint32_t m_3036ec = bus->Read32(0x030036ECu, entry_pc);
        const uint32_t m_30372c = bus->Read32(0x0300372Cu, entry_pc);
        const uint32_t m_30390c = bus->Read32(0x0300390Cu, entry_pc);
        const uint32_t m_30394c = bus->Read32(0x0300394Cu, entry_pc);

        const uint32_t v_2440 = bus->Read32(0x06002440u, entry_pc);
        const uint32_t v_2840 = bus->Read32(0x06002840u, entry_pc);
        const uint32_t v_2c40 = bus->Read32(0x06002C40u, entry_pc);
        const uint32_t v_17400 = bus->Read32(0x06017400u, entry_pc);
        const uint32_t v_17600 = bus->Read32(0x06017600u, entry_pc);
        const uint32_t m_r5_p20 = bus->Read32(cpu.registers[5] + 0x20u, entry_pc);
        const uint32_t m_r4_p34 = bus->Read32(cpu.registers[4] + 0x34u, entry_pc);

        std::ostringstream row;
        row << std::dec
            << static_cast<unsigned long long>(event) << ','
            << static_cast<unsigned long long>(total) << ','
            << static_cast<unsigned long long>(streak_hits_) << ','
            << (phase == ProbePhase::kPre ? "pre" : "post") << ','
            << t.name << ','
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << entry_pc << ','
            << (entry_thumb ? 'T' : 'A') << ','
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << next_pc << ','
            << (next_thumb ? 'T' : 'A') << ','
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << cpu.cycle_counter << ','
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << cpu.cpsr << ','
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << cpu.registers[13] << ','
            << "0x" << std::hex << std::setw(8) << std::setfill('0') << cpu.registers[14];

        for (int i = 0; i < 16; ++i) {
            row << ",0x" << std::hex << std::setw(8) << std::setfill('0') << cpu.registers[i];
        }

        row << ",0x" << std::hex << std::setw(8) << std::setfill('0') << mem_r0
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << mem_r1
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_307ea0
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_303b2c
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_3036ec
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_30372c
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_30390c
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_30394c
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << v_2440
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << v_2840
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << v_2c40
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << v_17400
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << v_17600
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_r5_p20
            << ",0x" << std::hex << std::setw(8) << std::setfill('0') << m_r4_p34
            << '\n';
        const std::string row_str = row.str();
        std::fwrite(row_str.data(), 1, row_str.size(), file_);

        if ((event & 0xFFu) == 0) std::fflush(file_);
    }

    bool enabled_ = false;
    FILE* file_ = nullptr;
    std::string csv_path_;
    std::string meta_path_;
    std::unordered_map<uint64_t, size_t> target_by_key_;
    std::unordered_map<uint64_t, uint64_t> total_hits_;
    ProbePhase last_phase_ = ProbePhase::kPre;
    uint64_t last_key_ = 0;
    uint64_t streak_hits_ = 0;
    uint64_t event_id_ = 0;
};

}  // namespace
#endif

JitDispatcher::JitDispatcher(MemoryBus* bus, BlockMap* block_map, SeedQueue* seed_queue,
                             PrewarmPacingState* prewarm_pacing,
                             BlockCompileCache* block_cache)
    : jit_arena_(1024 * 1024),
      jit_builder_(bus, &jit_arena_),
      jit_emitter_(),
      block_map_(block_map),
      seed_queue_(seed_queue),
      prewarm_pacing_(prewarm_pacing),
      block_cache_(block_cache),
      collect_block_usage_stats_(ParseEnvBool(
          "AXOLOTL_BLOCK_USAGE_STATS",
#ifdef B_DEBUG
          true
#else
          false
#endif
          )),
      capture_block_dumps_(ParseEnvBool("AXOLOTL_CAPTURE_BLOCK_DUMPS", false)),
      capture_crash_arm_bytes_(ParseEnvBool("AXOLOTL_CAPTURE_CRASH_ARM_BYTES", false)),
      halt_step_cycles_(ParseEnvU32Clamped("AXOLOTL_HALT_STEP_CYCLES", kDefaultHaltStepCycles, 16u, 8192u)),
      halt_step_max_cycles_(ParseEnvU32Clamped("AXOLOTL_HALT_STEP_MAX_CYCLES", kDefaultHaltStepMaxCycles, 16u, 65536u)),
      timing_batch_enable_(ParseEnvBool("AXOLOTL_TIMING_BATCH_ENABLE", true)),
      timing_batch_max_cycles_(ParseEnvU32Clamped(
          "AXOLOTL_TIMING_BATCH_MAX_CYCLES", kDefaultTimingBatchMaxCycles, 1u, 256u)) {
    if (halt_step_max_cycles_ < halt_step_cycles_) {
        halt_step_max_cycles_ = halt_step_cycles_;
    }
}

uint64_t JitDispatcher::MakeBlockKey(uint32_t pc, bool is_thumb) {
    const uint32_t aligned_pc = pc & (is_thumb ? ~1u : ~3u);
    return (static_cast<uint64_t>(aligned_pc) << 1) | static_cast<uint64_t>(is_thumb ? 1u : 0u);
}

void JitDispatcher::RememberReadyBlock(uint64_t block_key, void* host_code) {
    if (!BlockMap::IsReady(host_code)) return;
    recent_ready_block_keys_[recent_ready_block_insert_idx_] = block_key;
    recent_ready_block_hosts_[recent_ready_block_insert_idx_] = host_code;
    recent_ready_block_insert_idx_ ^= 1u;
}

void* JitDispatcher::LookupReadyBlockCache(uint64_t block_key) const {
    if (recent_ready_block_keys_[0] == block_key) return recent_ready_block_hosts_[0];
    if (recent_ready_block_keys_[1] == block_key) return recent_ready_block_hosts_[1];
    return nullptr;
}

void JitDispatcher::ApplyUsageStatsToDump(BlockDump* dump) const {
    if (!collect_block_usage_stats_ || !dump) return;
    const auto it = block_usage_stats_.find(MakeBlockKey(dump->pc, dump->is_thumb));
    if (it == block_usage_stats_.end()) return;
    dump->execution_count = it->second.execution_count;
    dump->total_guest_cycles = it->second.total_guest_cycles;
    dump->first_seen_guest_cycle = it->second.first_seen_guest_cycle;
    dump->last_seen_guest_cycle = it->second.last_seen_guest_cycle;
}

void JitDispatcher::UpsertBlockDump(BlockDump&& dump) {
    const uint64_t key = MakeBlockKey(dump.pc, dump.is_thumb);
    const auto it = block_dump_index_.find(key);
    if (it == block_dump_index_.end()) {
        block_dump_index_[key] = block_dumps_.size();
        block_dumps_.push_back(std::move(dump));
        ApplyUsageStatsToDump(&block_dumps_.back());
        return;
    }

    BlockDump& existing = block_dumps_[it->second];
    dump.execution_count = existing.execution_count;
    dump.total_guest_cycles = existing.total_guest_cycles;
    dump.first_seen_guest_cycle = existing.first_seen_guest_cycle;
    dump.last_seen_guest_cycle = existing.last_seen_guest_cycle;
    existing = std::move(dump);
    ApplyUsageStatsToDump(&existing);
}

void JitDispatcher::Run(uint32_t start_pc, bool is_thumb) {
    AXOLOTL_PROFILE_SCOPE("dispatcher.run");
    MemoryBus* run_bus = jit_builder_.GetBus();
    const uint32_t rom_loaded_size = run_bus->GetRomLoadedSize();
    CodeEmitter::SetupCpuStateForJit(&cpu_state_, run_bus);
    CodeEmitter::SetJitCrashDumpCallback([this](uint32_t block_pc, uint32_t /*target_pc*/) {
        std::filesystem::create_directories(kAnalysisPath);
        DumpAnalysisFiles(kAnalysisPath);
        for (const auto& d : block_dumps_)
            if (d.pc == block_pc && !d.arm_bytes.empty() && !d.x86_bytes.empty()) {
                RunDisasmScriptForBlock(d.arm_bytes, d.x86_bytes, d.ir_text, d.pc, d.is_thumb);
                return;
            }
    });
    cpu_state_.registers[15] = start_pc;
    cpu_state_.cpsr = (cpu_state_.cpsr & ~(1u << 5)) | (is_thumb ? (1u << 5) : 0u);
    cpu_state_.cycle_counter = 0;
    timing_last_counter_ = 0;
    timing_scanline_ = 0;
    timing_scanline_cycle_ = 0;
    halt_idle_streak_ = 0;
    recent_ready_block_keys_[0] = kInvalidBlockKey;
    recent_ready_block_keys_[1] = kInvalidBlockKey;
    recent_ready_block_hosts_[0] = nullptr;
    recent_ready_block_hosts_[1] = nullptr;
    recent_ready_block_insert_idx_ = 0;
    block_dumps_.clear();
    block_dump_index_.clear();
    block_usage_stats_.clear();

    size_t blocks_compiled_in_burst = 0;
    const size_t kBatchLogThreshold = 10;
#if ENABLE_LOOP_WARNINGS
    const size_t kLoopLogThreshold = 1000;
    size_t loop_iter_count = 0;
    bool loop_logged = false;
#endif
    uint32_t last_break_pc = 0xFFFFFFFF;

#ifdef B_DEBUG
#if ENABLE_PC_TRACE
    // ── PC execution trace ──────────────────────────────────────────────
    // Logs entry→exit control-flow edges to pc_trace.log.
    // Repeated identical edges are compressed into a single count line.
    FILE* pc_trace_file = fopen("pc_trace.log", "w");
    size_t pc_trace_count = 0;
    const size_t kMaxPcTrace = static_cast<size_t>(PC_TRACE_MAX_ENTRIES);
    bool pc_trace_done = false;
    uint32_t pc_trace_last_region = 0xFFFFFFFF;
    bool pc_trace_have_last_edge = false;
    uint32_t pc_trace_last_entry = 0xFFFFFFFF;
    bool pc_trace_last_entry_thumb = false;
    uint32_t pc_trace_last_next = 0xFFFFFFFF;
    bool pc_trace_last_next_thumb = false;
    size_t pc_trace_loop_count = 0;
    if (pc_trace_file) {
        fprintf(pc_trace_file, "#  idx   entry_pc mode ->  next_pc mode  cycles  region\n");
        if (kMaxPcTrace > 0) {
            Logger::log("[JitDispatcher] PC trace enabled (max " +
                        std::to_string(kMaxPcTrace) + " entries) -> pc_trace.log", LogLevel::INFO);
        } else {
            Logger::log("[JitDispatcher] PC trace enabled (full run) -> pc_trace.log", LogLevel::INFO);
        }
    }
#endif  // ENABLE_PC_TRACE

#if ENABLE_BLOCK_PROBE
    BlockProbeDumper block_probe;
    block_probe.Initialize(kAnalysisPath);
#endif
#endif

    while (system_running_.load(std::memory_order_relaxed)) {
        AXOLOTL_PROFILE_SCOPE("dispatcher.loop");
        HandleHostPause();
#ifdef B_DEBUG
        // Handle manual pause/step controls on every loop iteration, including
        // periods where the CPU is halted and would otherwise short-circuit.
        HandlePauseStep();
#endif

        // Hardware wake-up logic. The CPU wakes up if any enabled interrupt
        // is pending, even if the master interrupt enable (IME) is off!
        if (cpu_state_.halted) {
            MemoryBus* bus = jit_builder_.GetBus();
            uint16_t ie = bus->Read16(0x04000200, 0);
            uint16_t if_reg = bus->Read16(0x04000202, 0);

            if ((ie & if_reg & 0x3FFF) != 0) {
                cpu_state_.halted = false;
                halt_idle_streak_ = 0;
            } else {
                // Keep advancing time while halted so scheduled events still fire.
                const uint32_t halt_step = ComputeAdaptiveHaltStep(bus, ie);
                cpu_state_.cycle_counter += static_cast<int32_t>(halt_step);
                ++halt_idle_streak_;
                {
                    AXOLOTL_PROFILE_SCOPE("dispatcher.advance_timing_halted");
                    AdvanceTiming(cpu_state_.cycle_counter);
                }

                // Yield to prevent pegging a host CPU core at 100% while idle
                std::this_thread::yield();
                continue;
            }
        }

        CheckAndDispatchIRQ();
#ifdef B_DEBUG
        // If IRQ handling requested a pause (pause_irq), stop immediately
        // before running the next guest block.
        HandlePauseStep();
#endif

        uint32_t pc = cpu_state_.registers[15];
        // Honour open-bus behaviour: when PC is in non-executable regions (I/O 0x04, SRAM etc. >0x0D),
        // still fetch and execute — the bus returns open-bus values for unmapped/I/O reads.
        bool is_thumb = (cpu_state_.cpsr >> 5) & 1;
        // Canonicalize PC before cache lookup/compile so ARM never aliases on bit1.
        pc &= is_thumb ? ~1u : ~3u;
        cpu_state_.registers[15] = pc;

        // Prevent compile storms in non-executable regions (e.g. I/O/open-bus space).
        // We still advance architectural state with a minimal open-bus step.
        if (!BlockCacheData::IsCacheableExecPc(pc)) {
            static uint32_t non_exec_pc_warns = 0;
            if (non_exec_pc_warns < 16u) {
                std::ostringstream msg;
                msg << "[JitDispatcher] non-exec PC fallback @0x" << std::hex << pc
                    << (is_thumb ? " T" : " A");
                Logger::log(msg.str(), LogLevel::WARNING);
                ++non_exec_pc_warns;
            }
            EmulateNonExecutableOpenBusStep(&cpu_state_, pc, is_thumb);
            {
                AXOLOTL_PROFILE_SCOPE("dispatcher.advance_timing_non_exec");
                AdvanceTiming(cpu_state_.cycle_counter);
            }
            continue;
        }

#ifdef B_DEBUG
#if IRQ_PATH_TRACE
        {
            static int bios_irq_entry_logs = 0;
            static int user_irq_entry_logs = 0;
            if (!is_thumb && pc == 0x00000128u && bios_irq_entry_logs < 64) {
                MemoryBus* bus = jit_builder_.GetBus();
                std::ostringstream msg;
                msg << "[IRQ-PATH] hit BIOS IRQ body @0x00000128"
                    << " IF=0x" << std::hex << bus->Read16(0x04000202, 0)
                    << " IE=0x" << bus->Read16(0x04000200, 0)
                    << " IME=0x" << bus->Read16(0x04000208, 0)
                    << " wait=0x" << bus->Read16(0x0300310C, pc);
                Logger::log(msg.str(), LogLevel::WARNING);
                ++bios_irq_entry_logs;
            }
            if (!is_thumb && pc == 0x03003580u && user_irq_entry_logs < 64) {
                MemoryBus* bus = jit_builder_.GetBus();
                std::ostringstream msg;
                msg << "[IRQ-PATH] hit USER IRQ handler @0x03003580"
                    << " IF=0x" << std::hex << bus->Read16(0x04000202, 0)
                    << " wait=0x" << bus->Read16(0x0300310C, pc)
                    << " chk=0x" << bus->Read16(0x03007FF8, pc);
                Logger::log(msg.str(), LogLevel::WARNING);
                ++user_irq_entry_logs;
            }
        }
#endif
#endif

#ifdef B_DEBUG
        uint32_t aligned_pc = pc & (is_thumb ? ~1u : ~3u);

        if (watchpoints_ && ShouldPauseOnBlock(*watchpoints_, aligned_pc)) {
            // Only pause if we didn't just break on this exact PC
            if (aligned_pc != last_break_pc) {
                pause_requested_.store(true, std::memory_order_release);
                Logger::log("[Dispatcher] Breakpoint hit at PC=0x" +
                            [aligned_pc]{ std::ostringstream s; s << std::hex << aligned_pc; return s.str(); }(),
                            LogLevel::DEBUG);
                last_break_pc = aligned_pc;
            }
        } else {
            // We moved to a different block, reset the latch
            last_break_pc = 0xFFFFFFFF;
        }

        // Record execution immediately after unpausing (or if not paused)
        if (trace_) {
            RecordBlockExecution(*trace_, aligned_pc, is_thumb);
        }
#endif

        uint32_t entry_pc = pc;
        bool entry_is_thumb = is_thumb;
        const uint64_t block_key = MakeBlockKey(entry_pc, entry_is_thumb);
        const uint64_t guest_cycle_before = static_cast<uint64_t>(cpu_state_.cycle_counter);
        void* host_code = LookupReadyBlockCache(block_key);
        if (!BlockMap::IsReady(host_code)) {
            AXOLOTL_PROFILE_SCOPE("dispatcher.lookup_compile");
            host_code = block_map_->Lookup(pc, is_thumb);
            if (BlockMap::IsReady(host_code)) RememberReadyBlock(block_key, host_code);
        }
#ifdef B_DEBUG
        const CpuSnapshot snap_before_compile = TakeSnapshot(cpu_state_);
#endif

#ifdef B_DEBUG
#if ENABLE_BLOCK_PROBE
        block_probe.DumpPre(entry_pc, entry_is_thumb, cpu_state_, jit_builder_.GetBus());
#endif

#if LOGO_SETUP_TRACE
        if (!entry_is_thumb &&
            (entry_pc == 0x00000BD8u || entry_pc == 0x00000BE4u || entry_pc == 0x00000C14u)) {
            // Keep this very small; enough to inspect CpuSet/copy setup without log floods.
            static int b8_count = 0;
            static int be4_count = 0;
            static int c14_count = 0;
            int* ctr = (entry_pc == 0x00000BD8u) ? &b8_count
                     : (entry_pc == 0x00000BE4u) ? &be4_count
                                                  : &c14_count;
            if (*ctr < 24) {
                MemoryBus* bus = jit_builder_.GetBus();
                const uint32_t r0 = cpu_state_.registers[0];
                const uint32_t r1 = cpu_state_.registers[1];
                const uint32_t r2 = cpu_state_.registers[2];
                const uint32_t sl = cpu_state_.registers[10];
                const uint32_t src32 = bus->Read32(r0, entry_pc);
                std::ostringstream msg;
                msg << "[LogoTrace] entry=0x" << std::hex << entry_pc
                    << " r0=0x" << r0
                    << " r1=0x" << r1
                    << " r2=0x" << r2
                    << " sl=0x" << sl
                    << " [r0]=0x" << src32;
                Logger::log(msg.str(), LogLevel::WARNING);
                ++(*ctr);
            }
        }
        if (entry_is_thumb &&
            (entry_pc == 0x000013C4u || entry_pc == 0x00001434u ||
             entry_pc == 0x0000159Cu || entry_pc == 0x00001664u)) {
            // Trace likely producers of logo source data before CpuFastSet consumes it.
            static int c13c4_count = 0;
            static int c1434_count = 0;
            static int c159c_count = 0;
            static int c1664_count = 0;
            int* ctr = (entry_pc == 0x000013C4u) ? &c13c4_count
                     : (entry_pc == 0x00001434u) ? &c1434_count
                     : (entry_pc == 0x0000159Cu) ? &c159c_count
                     :                              &c1664_count;
            if (*ctr < 48) {
                MemoryBus* bus = jit_builder_.GetBus();
                const uint32_t r0 = cpu_state_.registers[0];
                const uint32_t r1 = cpu_state_.registers[1];
                const uint32_t r2 = cpu_state_.registers[2];
                const uint32_t r3 = cpu_state_.registers[3];
                const uint32_t r4 = cpu_state_.registers[4];
                const uint32_t r7 = cpu_state_.registers[7];
                const uint32_t lr = cpu_state_.registers[14];
                const uint32_t sp = cpu_state_.registers[13];
                const uint32_t src0 = bus->Read32(r0, entry_pc);
                const uint32_t src1 = bus->Read32(r1, entry_pc);
                std::ostringstream msg;
                msg << "[LogoProd] entry=0x" << std::hex << entry_pc
                    << " r0=0x" << r0
                    << " r1=0x" << r1
                    << " r2=0x" << r2
                    << " r3=0x" << r3
                    << " r4=0x" << r4
                    << " r7=0x" << r7
                    << " sp=0x" << sp
                    << " lr=0x" << lr
                    << " [r0]=0x" << src0
                    << " [r1]=0x" << src1;
                Logger::log(msg.str(), LogLevel::DEBUG);
                ++(*ctr);
            }
        }
#endif
#endif

        bool did_compile = false;
        bool own_compile_claim = false;
        constexpr int kPrewarmerWaitYields = 4;

        if (!BlockMap::IsReady(host_code)) {
            if (host_code == nullptr) {
                own_compile_claim = block_map_->TryClaimIfEmpty(
                    pc, is_thumb, BlockMap::ClaimOwner::kDispatcher);
                if (!own_compile_claim) {
                    host_code = block_map_->Lookup(pc, is_thumb);
                }
            }

            if (!own_compile_claim && BlockMap::IsClaimedBy(host_code, BlockMap::ClaimOwner::kPrewarmer)) {
                for (int i = 0; i < kPrewarmerWaitYields; ++i) {
                    std::this_thread::yield();
                    host_code = block_map_->Lookup(pc, is_thumb);
                    if (BlockMap::IsReady(host_code)) break;
                }

                if (!BlockMap::IsReady(host_code)) {
                    own_compile_claim = block_map_->TryStealClaim(
                        pc, is_thumb,
                        BlockMap::ClaimOwner::kPrewarmer,
                        BlockMap::ClaimOwner::kDispatcher);
                    if (!own_compile_claim) {
                        host_code = block_map_->Lookup(pc, is_thumb);
                    }
                }
            }
        }

        if (own_compile_claim) {
            if (is_thumb) {
                jit_builder_.BuildBlock<true>(pc);
            } else {
                jit_builder_.BuildBlock<false>(pc);
            }
            const uint32_t block_len = jit_builder_.GetBlockLength();
            const uint32_t block_cycles = jit_builder_.GetBlockCycles();
            std::vector<uint8_t> arm_bytes_for_crash;
            if (capture_block_dumps_ || capture_crash_arm_bytes_) {
                MemoryBus* bus = jit_builder_.GetBus();
                arm_bytes_for_crash.reserve(static_cast<size_t>(block_len) * (is_thumb ? 1u : 1u));
                for (uint32_t a = pc; a < pc + block_len; a += is_thumb ? 2u : 4u) {
                    if (is_thumb) {
                        uint16_t w = bus->Read16(a, a);
                        arm_bytes_for_crash.push_back(static_cast<uint8_t>(w & 0xFF));
                        arm_bytes_for_crash.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
                    } else {
                        uint32_t w = bus->Read32(a, a);
                        arm_bytes_for_crash.push_back(static_cast<uint8_t>(w & 0xFF));
                        arm_bytes_for_crash.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
                        arm_bytes_for_crash.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
                        arm_bytes_for_crash.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
                    }
                }
            }

            CodeEmitter::EmittedBlockArtifact emitted{};
            try {
                emitted = jit_emitter_.EmitBlockWithRelocs(jit_builder_.GetArena(),
                                                           pc,
                                                           block_cycles,
                                                           is_thumb,
                                                           block_len);
                host_code = emitted.host_code;
            } catch (const std::exception& e) {
                std::string msg(e.what());
                if (msg.find("code is too big") != std::string::npos) {
                    DumpCallingBlockAndAbort(pc, is_thumb, 0);
                }
                throw;
            }
            size_t x86_size = emitted.x86_size;
            if (x86_size > kMaxX86BlockSize) {
                DumpCallingBlockAndAbort(pc, is_thumb, x86_size);
            }
            const uint8_t* x86_ptr = emitted.x86_bytes.data();
            CrashHandler::registerJitBlock(x86_ptr, x86_size, pc,
                                          capture_crash_arm_bytes_ ? arm_bytes_for_crash.data() : nullptr,
                                          capture_crash_arm_bytes_ ? arm_bytes_for_crash.size() : 0u,
                                          is_thumb);
            if (capture_block_dumps_) {
                BlockDump dump;
                dump.pc = pc;
                dump.is_thumb = is_thumb;
                dump.block_len = block_len;
                dump.block_cycles = block_cycles;
                dump.x86_size = static_cast<uint32_t>(x86_size);
                dump.arm_bytes = std::move(arm_bytes_for_crash);
                dump.ir_text = IrPrinter::PrintArena(jit_builder_.GetArena(), pc);
                dump.x86_bytes = emitted.x86_bytes;
                UpsertBlockDump(std::move(dump));
            }
            if (!block_map_->PublishIfClaimedBy(pc, is_thumb,
                                                BlockMap::ClaimOwner::kDispatcher,
                                                host_code)) {
                // Lost publication race to a block that was installed while compiling.
                // Use canonical mapping pointer for execution.
                void* published = block_map_->Lookup(pc, is_thumb);
                if (BlockMap::IsReady(published)) {
                    host_code = published;
                    RememberReadyBlock(block_key, host_code);
                }
            } else if (block_cache_) {
                RememberReadyBlock(block_key, host_code);
                const BlockCompileCache::OptimizationDecision decision =
                    block_cache_->DecideOptimization(pc, is_thumb, emitted.block_cycles, emitted.block_len);
                if (decision.persist_seed) {
                    block_cache_->RecordCompiled(pc, is_thumb);
                }
                if (decision.persist_x86) {
                    std::vector<BlockCacheData::X86RelocEntry> reloc_entries;
                    reloc_entries.reserve(emitted.relocs.size());
                    for (const auto& r : emitted.relocs) {
                        BlockCacheData::X86RelocEntry re{};
                        re.offset = r.offset;
                        re.type = static_cast<uint32_t>(r.type);
                        re.symbol_id = static_cast<uint32_t>(r.symbol);
                        reloc_entries.push_back(re);
                    }
                    block_cache_->RecordCompiledX86(pc,
                                                    is_thumb,
                                                    emitted.x86_bytes.data(),
                                                    emitted.x86_bytes.size(),
                                                    reloc_entries.data(),
                                                    reloc_entries.size(),
                                                    emitted.block_cycles,
                                                    emitted.block_len,
                                                    emitted.x86_crc32);
                }
            }
            did_compile = true;
            blocks_compiled_in_burst++;
        }

        if (!BlockMap::IsReady(host_code)) {
            // Another thread may still be compiling; keep dispatcher non-blocking.
            std::this_thread::yield();
            continue;
        }
        RememberReadyBlock(block_key, host_code);

        if (!did_compile) {
            if (blocks_compiled_in_burst > kBatchLogThreshold) {
                std::ostringstream batch_msg;
                batch_msg << "[JitDispatcher] " << blocks_compiled_in_burst << " blocks created (total: "
                          << block_dumps_.size() << ")";
                Logger::log(batch_msg.str(), LogLevel::INFO);
            }
            blocks_compiled_in_burst = 0;

#ifdef B_DEBUG
#if ENABLE_PREWARM_DUMP_CAPTURE
            // Pre-warmed blocks were compiled by the PreWarmer thread and written
            // directly into block_map_ via pw_emitter_, so they never pass through
            // the did_compile path above and are absent from block_dumps_.
            // On first execution we rebuild through jit_builder_ + jit_emitter_
            // (the IR is deterministic, so the x86 output is identical) purely to
            // capture ARM bytes, IR text, and x86 bytes for debug commands.
            // We do NOT use the re-emitted host pointer for execution — host_code
            // from block_map_ (the pre-warmed copy) is what actually runs.
            {
                const bool already_recorded = block_dump_index_.find(MakeBlockKey(pc, is_thumb)) != block_dump_index_.end();

                if (!already_recorded) {
                    if (is_thumb) jit_builder_.BuildBlock<true>(pc);
                    else          jit_builder_.BuildBlock<false>(pc);

                    BlockDump dump;
                    dump.pc       = pc;
                    dump.is_thumb = is_thumb;
                    dump.block_len = jit_builder_.GetBlockLength();
                    dump.block_cycles = jit_builder_.GetBlockCycles();
                    dump.captured_from_prewarm = true;
                    dump.ir_text   = IrPrinter::PrintArena(jit_builder_.GetArena(), pc);

                    MemoryBus* bus = jit_builder_.GetBus();
                    for (uint32_t a = pc; a < pc + dump.block_len; a += is_thumb ? 2u : 4u) {
                        if (is_thumb) {
                            uint16_t w = bus->Read16(a, a);
                            dump.arm_bytes.push_back(static_cast<uint8_t>(w & 0xFF));
                            dump.arm_bytes.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
                        } else {
                            uint32_t w = bus->Read32(a, a);
                            dump.arm_bytes.push_back(static_cast<uint8_t>( w        & 0xFF));
                            dump.arm_bytes.push_back(static_cast<uint8_t>((w >>  8) & 0xFF));
                            dump.arm_bytes.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
                            dump.arm_bytes.push_back(static_cast<uint8_t>( w >> 24));
                        }
                    }

                    void* dump_code = jit_emitter_.EmitBlock(jit_builder_.GetArena(), pc,
                                                              jit_builder_.GetBlockCycles());
                    size_t x86_size = jit_emitter_.GetLastEmittedBlockSize();
                    const uint8_t* x86_ptr = static_cast<const uint8_t*>(dump_code);
                    dump.x86_bytes.assign(x86_ptr, x86_ptr + x86_size);
                    dump.x86_size = static_cast<uint32_t>(x86_size);
                    // Note: do NOT call CrashHandler::registerJitBlock here — the
                    // pre-warmed copy in block_map_ is what executes, not dump_code.
                    UpsertBlockDump(std::move(dump));
                }
            }
#endif  // ENABLE_PREWARM_DUMP_CAPTURE
#endif  // B_DEBUG
        }

        // Compile path should not mutate guest-visible CPU state.
#ifdef B_DEBUG
        {
            const CpuSnapshot snap_after_compile = TakeSnapshot(cpu_state_);
            if (std::memcmp(&snap_before_compile, &snap_after_compile, sizeof(CpuSnapshot)) != 0) {
                LogUnexpectedCpuMutation("compile",
                                         snap_before_compile, snap_after_compile,
                                         entry_pc, entry_is_thumb,
                                         cpu_state_.registers[15], (cpu_state_.cpsr >> 5) & 1);
            }
        }
#endif

        {
            AXOLOTL_PROFILE_SCOPE("dispatcher.execute_block");
            reinterpret_cast<JitFunc>(host_code)(&cpu_state_);
        }
        if (prewarm_pacing_) {
            prewarm_pacing_->dispatcher_blocks_executed.fetch_add(1, std::memory_order_relaxed);
        }
#ifdef B_DEBUG
        const CpuSnapshot snap_after_exec = TakeSnapshot(cpu_state_);
#endif
        {
            AXOLOTL_PROFILE_SCOPE("dispatcher.advance_timing_call");
            bool should_flush_timing = true;
            if (timing_batch_enable_) {
                int32_t pending_delta = cpu_state_.cycle_counter - timing_last_counter_;
                if (pending_delta < 0) pending_delta += GbaTiming::kCyclesPerFrame;
                const uint32_t threshold = ComputeTimingFlushThresholdCycles();
                should_flush_timing = pending_delta >= static_cast<int32_t>(threshold);
            }
            if (should_flush_timing) {
                AdvanceTiming(cpu_state_.cycle_counter);
            }
        }
#ifdef B_DEBUG
        const CpuSnapshot snap_after_timing = TakeSnapshot(cpu_state_);
#endif
        const uint64_t guest_cycle_after = static_cast<uint64_t>(cpu_state_.cycle_counter);
        const uint64_t consumed_guest_cycles =
            guest_cycle_after >= guest_cycle_before ? (guest_cycle_after - guest_cycle_before) : 0u;
        if (block_cache_) {
            static thread_local uint32_t heuristic_sample_counter = 0;
            const bool take_sample = ((++heuristic_sample_counter & 0x3u) == 0u) || consumed_guest_cycles >= 16u;
            if (take_sample) {
                block_cache_->RecordExecutionSample(entry_pc, entry_is_thumb,
                                                    static_cast<uint32_t>(consumed_guest_cycles));
            }
        }
        if (collect_block_usage_stats_) {
            BlockUsageStats& usage = block_usage_stats_[block_key];
            if (usage.execution_count == 0) usage.first_seen_guest_cycle = guest_cycle_before;
            usage.execution_count += 1;
            usage.total_guest_cycles += consumed_guest_cycles;
            usage.last_seen_guest_cycle = guest_cycle_after;
            const auto dump_it = block_dump_index_.find(block_key);
            if (dump_it != block_dump_index_.end()) {
                ApplyUsageStatsToDump(&block_dumps_[dump_it->second]);
            }
        }

#ifdef B_DEBUG
#if LOOP_CYCLE_TRACE
        if (entry_is_thumb && entry_pc == 0x080008BEu) {
            static int loop_cycle_logs = 0;
            if (loop_cycle_logs < 128) {
                MemoryBus* bus = jit_builder_.GetBus();
                std::ostringstream msg;
                msg << "[LOOP-CYCLE] pc=0x080008BE"
                    << " cyc=0x" << std::hex << cpu_state_.cycle_counter
                    << " vcount=0x" << bus->Read16(0x04000006, 0)
                    << " dispstat=0x" << bus->Read16(0x04000004, 0)
                    << " if=0x" << bus->Read16(0x04000202, 0)
                    << " ie=0x" << bus->Read16(0x04000200, 0);
                Logger::log(msg.str(), LogLevel::DEBUG);
                ++loop_cycle_logs;
            }
        }
#endif
#endif
#ifdef B_DEBUG
        if (std::memcmp(&snap_after_exec, &snap_after_timing, sizeof(CpuSnapshot)) != 0) {
            LogUnexpectedCpuMutation("advance_timing",
                                     snap_after_exec, snap_after_timing,
                                     entry_pc, entry_is_thumb,
                                     cpu_state_.registers[15], (cpu_state_.cpsr >> 5) & 1);
        }
#endif

// Old bottom-of-loop debug block removed; debug handling now occurs at
// the top of the loop after CheckAndDispatchIRQ().
#ifdef B_DEBUG
        // Log register state at decompression / CpuSet / CpuFastSet entry points
        // to see what addresses are being passed (R0=src, R1=dst for SWI decompress routines)
#if ENABLE_PC_TRACE && PC_TRACE_REGS_POST
        if (pc_trace_file && !pc_trace_done) {
            uint32_t epc = entry_pc;
            // Decompression and CpuSet entries - dump post-block CPU state.
            if (epc == 0x1010 || epc == 0x0F5C || epc == 0x0BC4 ||
                epc == 0x0B4C || epc == 0x0874 || epc == 0x05A4 ||
                epc == 0x0800 || epc == 0x08B4 || epc == 0x088A ||
                epc == 0x08C4 || epc == 0x10FC) {
                fprintf(pc_trace_file,
                    "# REGS_POST @0x%04X: R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R12=%08X LR=%08X SP=%08X\n",
                    epc,
                    cpu_state_.registers[0], cpu_state_.registers[1],
                    cpu_state_.registers[2], cpu_state_.registers[3],
                    cpu_state_.registers[4], cpu_state_.registers[12],
                    cpu_state_.registers[14], cpu_state_.registers[13]);
            }
        }
#endif
#endif
        last_caller_pc_ = last_executed_pc_;
        last_caller_is_thumb_ = last_executed_is_thumb_;
        last_executed_pc_ = entry_pc;
        last_executed_is_thumb_ = entry_is_thumb;

        pc = cpu_state_.registers[15];
        is_thumb = (cpu_state_.cpsr >> 5) & 1;

        // Enforce architectural PC alignment. ARMv4T does not interwork on LDR PC / POP {PC},
        // but the lower bits may contain garbage that will cause BlockMap::Lookup to fail.
        pc &= is_thumb ? ~1u : ~3u;
        cpu_state_.registers[15] = pc;

#ifdef B_DEBUG
        // Focused handoff check: 0x1664 prepares args for call to 0x0B4C.
        // Verify guest R2 survives into 0x0B4C (critical for logo producer path).
#if LOGO_SETUP_TRACE
        static int logo_handoff_1664_to_b4c_count = 0;
        static int logo_cpuset_b4c_count = 0;
        static int logo_cpuset_b5e_count = 0;
        if (entry_is_thumb && entry_pc == 0x00001664u &&
            pc == 0x00000B4Cu &&
            logo_handoff_1664_to_b4c_count < 64) {
            MemoryBus* bus = jit_builder_.GetBus();
            const uint32_t r0 = cpu_state_.registers[0];
            const uint32_t r1 = cpu_state_.registers[1];
            const uint32_t r2 = cpu_state_.registers[2];
            const uint32_t sp = cpu_state_.registers[13];
            const uint32_t lr = cpu_state_.registers[14];
            const uint32_t m_r0 = bus->Read32(r0, entry_pc);
            const uint32_t m_r1 = bus->Read32(r1, entry_pc);
            const uint32_t m_r2 = bus->Read32(r2, entry_pc);
            const uint32_t m_sp = bus->Read32(sp, entry_pc);

            std::ostringstream msg;
            msg << "[LogoHandoff] 1664T->0B4CA"
                << " r0=0x" << std::hex << r0
                << " r1=0x" << std::hex << r1
                << " r2=0x" << std::hex << r2
                << " sp=0x" << std::hex << sp
                << " lr=0x" << std::hex << lr
                << " [r0]=0x" << std::hex << m_r0
                << " [r1]=0x" << std::hex << m_r1
                << " [r2]=0x" << std::hex << m_r2
                << " [sp]=0x" << std::hex << m_sp;
            Logger::log(msg.str(), LogLevel::DEBUG);
            ++logo_handoff_1664_to_b4c_count;
        }

        // Decode CpuSet/CpuFastSet-style control flow in the BIOS helper.
        if (entry_is_thumb && entry_pc == 0x00000B4Cu && logo_cpuset_b4c_count < 128) {
            const uint32_t src = cpu_state_.registers[0];
            const uint32_t dst = cpu_state_.registers[1];
            const uint32_t ctrl = cpu_state_.registers[2];
            const uint32_t count_units = (ctrl & 0x001FFFFFu);
            const bool fixed_source = (ctrl & (1u << 24)) != 0;
            const bool is_32bit = (ctrl & (1u << 26)) != 0;
            const uint32_t unit_bytes = is_32bit ? 4u : 2u;
            const uint32_t span_bytes = count_units * unit_bytes;
            const uint32_t src_word = jit_builder_.GetBus()->Read32(src, entry_pc);

            std::ostringstream msg;
            msg << "[LogoCpuSet] entry=0x0B4C"
                << " src=0x" << std::hex << src
                << " dst=0x" << std::hex << dst
                << " ctrl=0x" << std::hex << ctrl
                << " units=" << std::dec << count_units
                << " unitBytes=" << unit_bytes
                << " spanBytes=" << span_bytes
                << " fixed=" << (fixed_source ? 1 : 0)
                << " [src]=0x" << std::hex << src_word;
            Logger::log(msg.str(), LogLevel::DEBUG);
            ++logo_cpuset_b4c_count;
        }

        if (entry_is_thumb && entry_pc == 0x00000B5Eu && logo_cpuset_b5e_count < 128) {
            const uint32_t ctrl = cpu_state_.registers[2];
            const uint32_t chunk_words = ctrl >> 25;  // as used by LSRS r3, r2, #25
            const bool carry = ((cpu_state_.cpsr >> 29) & 1u) != 0;
            std::ostringstream msg;
            msg << "[LogoCpuSet] entry=0x0B5E"
                << " ctrl=0x" << std::hex << ctrl
                << " chunkWords=0x" << std::hex << chunk_words
                << " carry=" << std::dec << (carry ? 1 : 0)
                << " r0=0x" << std::hex << cpu_state_.registers[0]
                << " r1=0x" << std::hex << cpu_state_.registers[1]
                << " r4=0x" << std::hex << cpu_state_.registers[4]
                << " r5=0x" << std::hex << cpu_state_.registers[5];
            Logger::log(msg.str(), LogLevel::DEBUG);
            ++logo_cpuset_b5e_count;
        }
#endif
#if ENABLE_BLOCK_PROBE
        block_probe.DumpPost(entry_pc, entry_is_thumb, pc, is_thumb, cpu_state_, jit_builder_.GetBus());
#endif
#endif

#ifdef B_DEBUG
#if ENABLE_PC_TRACE
        if (pc_trace_file && !pc_trace_done) {
            if (pc_trace_have_last_edge &&
                pc_trace_last_entry == entry_pc &&
                pc_trace_last_entry_thumb == entry_is_thumb &&
                pc_trace_last_next == pc &&
                pc_trace_last_next_thumb == is_thumb) {
                pc_trace_loop_count++;
            } else {
                if (pc_trace_loop_count > 0) {
                    fprintf(pc_trace_file, "       ... (×%zu repeated %08X %s -> %08X %s, cy=%u)\n",
                            pc_trace_loop_count,
                            pc_trace_last_entry, pc_trace_last_entry_thumb ? "T" : "A",
                            pc_trace_last_next, pc_trace_last_next_thumb ? "T" : "A",
                            cpu_state_.cycle_counter);
                }
                pc_trace_loop_count = 0;
                pc_trace_have_last_edge = true;
                pc_trace_last_entry = entry_pc;
                pc_trace_last_entry_thumb = entry_is_thumb;
                pc_trace_last_next = pc;
                pc_trace_last_next_thumb = is_thumb;

                uint32_t region = entry_pc >> 24;
                const char* region_name = "???";
                switch (region) {
                    case 0x00: region_name = "BIOS"; break;
                    case 0x02: region_name = "EWRAM"; break;
                    case 0x03: region_name = "IWRAM"; break;
                    case 0x04: region_name = "IO"; break;
                    case 0x05: region_name = "PAL"; break;
                    case 0x06: region_name = "VRAM"; break;
                    case 0x08: case 0x09: region_name = "ROM"; break;
                    case 0x0E: case 0x0F: region_name = "SRAM"; break;
                }
                if (region != pc_trace_last_region) {
                    fprintf(pc_trace_file, "# ── region change: %s (0x%02X) ──\n", region_name, region);
                    pc_trace_last_region = region;
                }
                fprintf(pc_trace_file, "%5zu  %08X %s -> %08X %s  cy=%-6u  %s\n",
                        pc_trace_count,
                        entry_pc, entry_is_thumb ? "T" : "A",
                        pc, is_thumb ? "T" : "A",
                        cpu_state_.cycle_counter, region_name);
            }
            pc_trace_count++;

            bool stop = (kMaxPcTrace > 0 && pc_trace_count >= kMaxPcTrace);
            if (stop) {
                if (pc_trace_loop_count > 0) {
                    fprintf(pc_trace_file, "       ... (×%zu repeated %08X %s -> %08X %s)\n",
                            pc_trace_loop_count,
                            pc_trace_last_entry, pc_trace_last_entry_thumb ? "T" : "A",
                            pc_trace_last_next, pc_trace_last_next_thumb ? "T" : "A");
                }
                fprintf(pc_trace_file, "# trace stopped after %zu entries\n", pc_trace_count);
                fflush(pc_trace_file);
                fclose(pc_trace_file);
                pc_trace_file = nullptr;
                pc_trace_done = true;
                Logger::log("[JitDispatcher] PC trace written: pc_trace.log (" +
                            std::to_string(pc_trace_count) + " entries)", LogLevel::INFO);
            }
        }
#endif
#endif

        // Unconditionally queue dynamically reached targets so the PreWarmer or the next loop
        // iteration can catch them.
        const uint32_t pc_aligned = pc & (is_thumb ? ~1u : ~3u);
        if (BlockCacheData::IsCacheableExecPcWithRomSize(pc_aligned, rom_loaded_size) &&
            block_map_->Lookup(pc, is_thumb) == nullptr) {
            seed_queue_->Push(pc, is_thumb);
        }

        if (did_compile) {
            for (const auto& target : jit_builder_.GetDiscoveredTargets()) {
                if (target.first == pc && target.second == is_thumb) continue;
                const uint32_t target_aligned = target.first & (target.second ? ~1u : ~3u);
                if (!BlockCacheData::IsCacheableExecPcWithRomSize(target_aligned, rom_loaded_size)) {
                    continue;
                }
                if (block_map_->Lookup(target.first, target.second) == nullptr) {
                    seed_queue_->Push(target.first, target.second);
                }
            }
        }

#if ENABLE_LOOP_WARNINGS
        if (pc == last_executed_pc_ && is_thumb == last_executed_is_thumb_) {
            ++loop_iter_count;
            if (loop_iter_count >= kLoopLogThreshold && !loop_logged) {
                std::ostringstream msg;
                msg << "[JitDispatcher] loop 0x" << std::hex << pc << (is_thumb ? " T" : " A") << " (" << std::dec << loop_iter_count << " iters)";
                Logger::log(msg.str(), LogLevel::WARNING);
                loop_logged = true;
            }
        } else {
            loop_iter_count = 0;
            loop_logged = false;
        }
#endif
    }

#ifdef B_DEBUG
#if ENABLE_BLOCK_PROBE
    block_probe.Shutdown();
#endif
#if ENABLE_PC_TRACE
    if (pc_trace_file) {
        if (pc_trace_loop_count > 0) {
            fprintf(pc_trace_file, "       ... (×%zu repeated %08X %s -> %08X %s)\n",
                    pc_trace_loop_count,
                    pc_trace_last_entry, pc_trace_last_entry_thumb ? "T" : "A",
                    pc_trace_last_next, pc_trace_last_next_thumb ? "T" : "A");
        }
        fprintf(pc_trace_file, "# trace stopped: system shutdown (%zu entries)\n", pc_trace_count);
        fclose(pc_trace_file);
        pc_trace_file = nullptr;
        Logger::log("[JitDispatcher] PC trace written: pc_trace.log (" +
                    std::to_string(pc_trace_count) + " entries)", LogLevel::INFO);
    }
#endif
#endif
    AXOLOTL_PROFILE_DUMP("dispatcher.run_exit");
}


void JitDispatcher::DumpCallingBlockAndAbort(uint32_t overflowed_pc, bool overflowed_thumb,
                                             size_t x86_size_or_zero) {
    std::filesystem::create_directories(kAnalysisPath);

    const char* thumb_suffix = overflowed_thumb ? "_thumb" : "_arm";
    char name[64];
    std::snprintf(name, sizeof(name), "overflowed_block_%08x%s.ir", overflowed_pc, thumb_suffix);
    std::string overflowed_ir = IrPrinter::PrintArena(jit_builder_.GetArena(), overflowed_pc);
    std::ofstream(std::string(kAnalysisPath) + "/" + name).write(overflowed_ir.c_str(), overflowed_ir.size());

    // Calling block = the one that branched to the overflowed block (last_executed_pc_).
    // For the log we show the block that branched to *that* (last_caller_pc_) so user sees the real caller, not the big block that just ran.
    uint32_t caller_pc = last_caller_pc_ != kNoPreviousBlock ? last_caller_pc_ : last_executed_pc_;
    bool caller_thumb = last_caller_pc_ != kNoPreviousBlock ? last_caller_is_thumb_ : last_executed_is_thumb_;
    std::string calling_ir;
    if (caller_pc != kNoPreviousBlock) {
        if (caller_thumb) {
            jit_builder_.BuildBlock<true>(caller_pc);
        } else {
            jit_builder_.BuildBlock<false>(caller_pc);
        }
        calling_ir = IrPrinter::PrintArena(jit_builder_.GetArena(), caller_pc);
        std::snprintf(name, sizeof(name), "calling_block_%08x%s.ir", caller_pc,
                      caller_thumb ? "_thumb" : "_arm");
        std::ofstream(std::string(kAnalysisPath) + "/" + name).write(calling_ir.c_str(), calling_ir.size());
    } else {
        calling_ir = "(no calling block; overflowed block was first or RunSimulated)\n";
    }

    std::ostringstream meta;
    meta << "x86 block size overflow\n"
         << "overflowed_pc=0x" << std::hex << overflowed_pc << " " << (overflowed_thumb ? "thumb" : "arm") << "\n"
         << "x86_size=" << std::dec << x86_size_or_zero << "\n"
         << "calling_pc=0x" << caller_pc << " (block that branched to overflowed)\n"
         << "last_executed_pc=0x" << last_executed_pc_ << "\n";
    std::ofstream(std::string(kAnalysisPath) + "/x86_overflow_meta.txt").write(meta.str().c_str(), meta.str().size());

    std::ostringstream log_msg;
    log_msg << "[JitDispatcher] x86 block size exceeded (cap " << kMaxX86BlockSize << "); overflowed PC=0x"
            << std::hex << overflowed_pc << (overflowed_thumb ? " T" : " A")
            << (x86_size_or_zero ? std::string(", size=") + std::to_string(x86_size_or_zero) : ", Xbyak threw")
            << "\nCalling block (PC=0x" << caller_pc << ", branched to overflowed) IR:\n"
            << calling_ir;
    Logger::log(log_msg.str(), LogLevel::ERR);
    std::abort();
}

void JitDispatcher::CheckAndDispatchIRQ() {
    AXOLOTL_PROFILE_SCOPE("dispatcher.check_irq");
    MemoryBus* bus = jit_builder_.GetBus();
    // If interrupts are disabled, do nothing
    if ((cpu_state_.cpsr & 0x80) || !(bus->Read16(0x04000208, 0) & 1)) return;

    uint16_t ie = bus->Read16(0x04000200, 0);
    uint16_t req = bus->Read16(0x04000202, 0);
    uint16_t taken = ie & req;

    if (taken) {
#ifdef B_DEBUG
        uint16_t ime = bus->Read16(0x04000208, 0);
        // Compress repeated VBlank-only dispatches into a single line with a count.
        static uint16_t last_dispatch_taken = 0;
        static int dispatch_repeat_count = 0;
        if (taken == last_dispatch_taken && taken == 0x0001 /* VBlank only */) {
            ++dispatch_repeat_count;
        } else {
            if (dispatch_repeat_count > 1) {
                Logger::log("[IRQ] dispatch VBlank (x" + std::to_string(dispatch_repeat_count) + ")", LogLevel::DEBUG);
            }
            dispatch_repeat_count = 1;
            last_dispatch_taken = taken;
            std::ostringstream irq_msg;
            irq_msg << "[IRQ] dispatch " << FormatIrqSources(taken) << " at PC=0x"
                    << std::hex << cpu_state_.registers[15]
                    << " IE=0x" << ie << " IF=0x" << req << " IME=0x" << (ime & 1)
                    << " CPSR_I=" << ((cpu_state_.cpsr >> 7) & 1);
            Logger::log(irq_msg.str(), LogLevel::DEBUG);
        }

        if (watchpoints_ && watchpoints_->pause_on_irq) {
            std::ostringstream wp_msg;
            wp_msg << "[DBG] pause_irq hit: " << FormatIrqSources(taken)
                   << " at PC=0x" << std::hex << cpu_state_.registers[15]
                   << " IE=0x" << ie << " IF=0x" << req << " IME=0x" << (ime & 1);
            Logger::log(wp_msg.str(), LogLevel::WARNING);
            pause_requested_.store(true, std::memory_order_release);
        }
#endif

        // Flush lazy NZCV state before exception entry captures CPSR.
        MaterializeCPSR(&cpu_state_);

        uint32_t old_cpsr = cpu_state_.cpsr;
        uint32_t old_pc = cpu_state_.registers[15];

        // Exception entry: IRQ mode, I=1, T=0.
        cpu_state_.cpsr = (old_cpsr & ~0xBFu) | 0x92u;

        SwapBankedRegisters(&cpu_state_, old_cpsr & 0x1F, 0x12);

        // Store precise SPSR and Link Register
        cpu_state_.spsr = old_cpsr;

        // IRQ LR expects PC + 4 (Thumb) or PC + 4 (ARM) of the NEXT instruction.
        cpu_state_.registers[14] = old_pc + 4;

        // Vector to IRQ handler
        cpu_state_.registers[15] = 0x00000018;
        cpu_state_.halted = false;
    }
}

uint32_t JitDispatcher::ComputeAdaptiveHaltStep(MemoryBus* bus, uint16_t ie) {
    if (!bus) return halt_step_cycles_;

    uint32_t step = halt_step_cycles_;
    const uint32_t ramp_shift = std::min<uint32_t>(halt_idle_streak_, 10u);
    const uint64_t ramped = static_cast<uint64_t>(halt_step_cycles_) << ramp_shift;
    if (ramped < static_cast<uint64_t>(step)) {
        step = halt_step_max_cycles_;
    } else {
        step = static_cast<uint32_t>(std::min<uint64_t>(ramped, halt_step_max_cycles_));
    }

    // Stay conservative when non-PPU IRQ classes are enabled (timer/DMA/serial/keypad/gamepak).
    constexpr uint16_t kNonPpuIrqMask = static_cast<uint16_t>(0x3FF8u);
    if ((ie & kNonPpuIrqMask) != 0) {
        step = std::min(step, halt_step_cycles_);
    }

    // Bound step to the next enabled PPU IRQ source so HALT wake-up latency stays tight.
    if ((ie & 0x0007u) != 0) {
        const uint16_t dispstat = bus->Read16(0x04000004, 0);
        uint32_t ppu_limit = step;

        auto ClampToLimit = [&](int32_t dist_cycles) {
            if (dist_cycles <= 0) return;
            const uint32_t dist = static_cast<uint32_t>(dist_cycles);
            if (dist < ppu_limit) ppu_limit = dist;
        };

        const int32_t line = timing_scanline_;
        const int32_t cycle = timing_scanline_cycle_;

        // HBlank IRQ (IE bit1 + DISPSTAT bit4)
        if ((ie & 0x0002u) != 0 && (dispstat & (1u << 4)) != 0) {
            const int32_t dist = (cycle < GbaTiming::kCyclesHBlankFlag)
                                     ? (GbaTiming::kCyclesHBlankFlag - cycle)
                                     : (GbaTiming::kCyclesPerScanline - cycle + GbaTiming::kCyclesHBlankFlag);
            ClampToLimit(dist);
        }

        // VBlank IRQ (IE bit0 + DISPSTAT bit3)
        if ((ie & 0x0001u) != 0 && (dispstat & (1u << 3)) != 0) {
            int32_t dist = 0;
            if (line < GbaTiming::kScanlinesVisible) {
                dist = (GbaTiming::kScanlinesVisible - line) * GbaTiming::kCyclesPerScanline - cycle;
            } else {
                dist = (GbaTiming::kScanlinesTotal - line + GbaTiming::kScanlinesVisible) *
                           GbaTiming::kCyclesPerScanline -
                       cycle;
            }
            ClampToLimit(dist);
        }

        // VCount IRQ (IE bit2 + DISPSTAT bit5)
        if ((ie & 0x0004u) != 0 && (dispstat & (1u << 5)) != 0) {
            const int32_t lyc = static_cast<int32_t>((dispstat >> 8) & 0xFFu);
            int32_t lines_until = lyc - line;
            if (lines_until < 0) lines_until += GbaTiming::kScanlinesTotal;
            int32_t dist = lines_until * GbaTiming::kCyclesPerScanline - cycle;
            if (dist <= 0) dist += GbaTiming::kCyclesPerFrame;
            ClampToLimit(dist);
        }

        step = std::min(step, ppu_limit);
    }

    if (step == 0) step = 1;
    return step;
}

uint32_t JitDispatcher::ComputeTimingFlushThresholdCycles() const {
    const int32_t line_cycle = timing_scanline_cycle_;
    const int32_t to_scanline_end = GbaTiming::kCyclesPerScanline - line_cycle;
    int32_t to_hblank = std::numeric_limits<int32_t>::max();
    if (line_cycle < GbaTiming::kCyclesHBlankFlag) {
        to_hblank = GbaTiming::kCyclesHBlankFlag - line_cycle;
    }

    int32_t threshold = std::min(to_scanline_end, to_hblank);
    threshold = std::min<int32_t>(threshold, static_cast<int32_t>(timing_batch_max_cycles_));
    if (threshold <= 0) return 1;
    return static_cast<uint32_t>(threshold);
}

void JitDispatcher::AdvanceTiming(int32_t new_counter) {
    AXOLOTL_PROFILE_SCOPE("dispatcher.advance_timing");
    int32_t delta = new_counter - timing_last_counter_;
    if (delta < 0) delta += GbaTiming::kCyclesPerFrame;
    if (delta <= 0) {
        timing_last_counter_ = new_counter;
        return;
    }

    // Fast path: no HBlank/scanline boundary crossed, so only timer stepping is needed.
    {
        const int32_t line_cycle = timing_scanline_cycle_;
        const int32_t to_scanline_end = GbaTiming::kCyclesPerScanline - line_cycle;
        int32_t to_hblank = std::numeric_limits<int32_t>::max();
        if (line_cycle < GbaTiming::kCyclesHBlankFlag) {
            to_hblank = GbaTiming::kCyclesHBlankFlag - line_cycle;
        }

        if (delta < to_scanline_end && delta < to_hblank) {
            MemoryBus* bus = jit_builder_.GetBus();
            bus->StepTimers(static_cast<uint32_t>(delta));
            if (timing_callbacks_.on_cycles_advanced) {
                timing_callbacks_.on_cycles_advanced(static_cast<uint32_t>(delta));
            }
            timing_last_counter_ = new_counter;
            timing_scanline_cycle_ = line_cycle + delta;
            return;
        }
    }

    // Full path: at least one timing boundary will be crossed.
    jit_builder_.GetBus()->StepTimers(static_cast<uint32_t>(delta));
    if (timing_callbacks_.on_cycles_advanced) {
        timing_callbacks_.on_cycles_advanced(static_cast<uint32_t>(delta));
    }
    timing_last_counter_ = new_counter;

    while (true) {
        int32_t cycles_into_scanline = timing_scanline_cycle_;

        // HBlank flag — fires in ALL scanlines (0..227) per GBATEK:
        //   DISPSTAT bit 1: "H-Blank flag (1=HBlank) (toggled in all lines, 0..227)"
        //   "H-Blank conditions are generated once per scanline, including for
        //    the 'hidden' scanlines during V-Blank."
        // Note: The GBATEK LCD-Dimensions page note "no H-Blank interrupts
        // during V-Blank" is a known documentation error — hardware tests
        // confirm both the flag and IRQ fire in all 228 lines.
        {
            int32_t hblank_abs = timing_scanline_ * GbaTiming::kCyclesPerScanline
                               + GbaTiming::kCyclesHBlankFlag;
            if (new_counter >= hblank_abs && cycles_into_scanline < GbaTiming::kCyclesHBlankFlag) {
                MemoryBus* bus = jit_builder_.GetBus();
                uint16_t dispstat = bus->Read16(0x04000004, 0);
                dispstat |= (1u << 1);  // set H-Blank flag
                bus->Write16(nullptr, 0x04000004, dispstat);
                if (dispstat & (1u << 4))  // H-Blank IRQ enable
                    bus->RaiseHBlankIRQ();
                // GBATEK: HBlank DMA fires on all visible scanlines (0–159).
                if (timing_scanline_ < GbaTiming::kScanlinesVisible)
                    bus->TriggerHBlankDma();
                // PPU scanline render only during visible lines (0..159).
                if (timing_scanline_ < GbaTiming::kScanlinesVisible) {
                    if (timing_callbacks_.on_hblank)
                        timing_callbacks_.on_hblank(timing_scanline_);
                }
            }
        }

        // Scanline end
        int32_t scanline_end_abs = (timing_scanline_ + 1) * GbaTiming::kCyclesPerScanline;
        if (new_counter < scanline_end_abs) {
            timing_scanline_cycle_ = static_cast<int32_t>(
                new_counter - timing_scanline_ * GbaTiming::kCyclesPerScanline);
            break;
        }

        // Advance to next scanline
        timing_scanline_++;
        MemoryBus* bus = jit_builder_.GetBus();

        // Clear H-Blank flag at start of new scanline
        uint16_t dispstat = bus->Read16(0x04000004, 0);
        dispstat &= ~(1u << 1);
        bus->Write16(nullptr, 0x04000004, dispstat);

        // Update VCOUNT register
        if (timing_scanline_ < GbaTiming::kScanlinesTotal) {
            bus->Write16(nullptr, 0x04000006,
                         static_cast<uint16_t>(timing_scanline_));
        }

        // V-Count match IRQ (DISPSTAT bit 5, trigger line in bits 8..15)
        dispstat = bus->Read16(0x04000004, 0);
        uint8_t lyc = static_cast<uint8_t>(dispstat >> 8);
        if (timing_scanline_ == lyc) {
            dispstat |= (1u << 2);  // V-Counter match flag
            if (dispstat & (1u << 5))
                bus->RaiseVCountIRQ();
        } else {
            dispstat &= ~(1u << 2);
        }
        bus->Write16(nullptr, 0x04000004, dispstat);

        // GBATEK: "V-Blank flag (set in line 160..226; not 227)"
        // Clear V-Blank flag when entering line 227.
        if (timing_scanline_ == GbaTiming::kScanlinesTotal - 1) {
            dispstat = bus->Read16(0x04000004, 0);
            dispstat &= ~(1u << 0);
            bus->Write16(nullptr, 0x04000004, dispstat);
        }

        // VBlank: DISPSTAT bit 0 and IF bit 0 at first cycle of scanline 160 (per GBATEK).
        if (timing_scanline_ == GbaTiming::kScanlinesVisible) {
            dispstat |= (1u << 0);  // set V-Blank flag
            bus->Write16(nullptr, 0x04000004, dispstat);
            if (dispstat & (1u << 3))  // V-Blank IRQ Enable (GBATEK DISPSTAT bit 3)
                bus->RaiseVBlank();
            // GBATEK: VBlank DMA fires at start of VBlank (scanline 160).
            bus->TriggerVBlankDma();
            if (timing_callbacks_.on_vblank)
                timing_callbacks_.on_vblank();
        }

        if (timing_scanline_ == GbaTiming::kScanlinesTotal) {
            // Frame boundary (end of scanline 228 / start of next frame)
            timing_scanline_ = 0;
            timing_scanline_cycle_ = 0;
            cpu_state_.cycle_counter -= GbaTiming::kCyclesPerFrame;
            new_counter -= GbaTiming::kCyclesPerFrame;

            // VCOUNT = 0 for the new frame.
            bus->Write16(nullptr, 0x04000006, 0);

            // V-Count match check for line 0 — the normal scanline-advance
            // path never reaches here because we break out of the loop.
            // GBATEK: V-Counter flag (DISPSTAT bit 2) must update every line.
            dispstat = bus->Read16(0x04000004, 0);
            {
                uint8_t lyc = static_cast<uint8_t>(dispstat >> 8);
                if (0 == lyc) {
                    dispstat |= (1u << 2);
                    if (dispstat & (1u << 5))
                        bus->RaiseVCountIRQ();
                } else {
                    dispstat &= ~(1u << 2);
                }
            }
            // VBlank flag already cleared at line 227; no need to touch bit 0.
            bus->Write16(nullptr, 0x04000004, dispstat);

            // Snapshot PPU here: CPU has had the full VBlank period to update
            // VRAM/palette/OAM (lines 160–227).  Do not snapshot at on_vblank
            // (scanline 160) or the frame would be stale.
            if (timing_callbacks_.on_scanline_start)
                timing_callbacks_.on_scanline_start(0);
            break;
        }

        timing_scanline_cycle_ = static_cast<int32_t>(
            new_counter - timing_scanline_ * GbaTiming::kCyclesPerScanline);

        if (timing_callbacks_.on_scanline_start)
            timing_callbacks_.on_scanline_start(timing_scanline_);
    }
}

void JitDispatcher::DumpSingleBlockToPath(const BlockDump& d, const std::string& path) const {
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "block_%08x", d.pc);
    const std::string base = path + "/" + prefix;

    const char* arm_suffix = d.is_thumb ? "_thumb.bin" : "_arm.bin";
    std::ofstream(base + arm_suffix, std::ios::binary)
        .write(reinterpret_cast<const char*>(d.arm_bytes.data()), d.arm_bytes.size());

    std::ostringstream meta;
    meta << "# Block 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << d.pc
         << " - 0x" << std::setw(8) << (d.pc + d.block_len) << (d.is_thumb ? " Thumb" : " ARM") << "\n";
    meta << "block_len: " << std::dec << d.block_len << "\n";
    meta << "block_cycles: " << d.block_cycles << "\n";
    meta << "x86_size: " << d.x86_size << "\n";
    meta << "captured_from_prewarm: " << (d.captured_from_prewarm ? 1 : 0) << "\n";
    meta << "execution_count: " << d.execution_count << "\n";
    meta << "total_guest_cycles: " << d.total_guest_cycles << "\n";
    if (d.execution_count != 0) {
        meta << "avg_guest_cycles_per_exec: " << std::fixed << std::setprecision(2)
             << (static_cast<double>(d.total_guest_cycles) / static_cast<double>(d.execution_count)) << "\n";
    } else {
        meta << "avg_guest_cycles_per_exec: 0.00\n";
    }
    meta << "first_seen_guest_cycle: " << d.first_seen_guest_cycle << "\n";
    meta << "last_seen_guest_cycle: " << d.last_seen_guest_cycle << "\n";
    meta << "\n";
    for (uint32_t a = d.pc; a < d.pc + d.block_len; a += d.is_thumb ? 2u : 4u)
        meta << "0x" << std::setw(8) << a << "\n";
    std::ofstream(base + "_meta.txt").write(meta.str().c_str(), meta.str().size());

    std::ofstream(base + "_x86.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(d.x86_bytes.data()), d.x86_bytes.size());

    std::ofstream(base + "_ir.txt").write(d.ir_text.c_str(), d.ir_text.size());
}

void JitDispatcher::DumpAnalysisFiles(const std::string& path) const {
    AXOLOTL_PROFILE_SCOPE("dispatcher.dump_analysis_files");
    std::filesystem::create_directories(path);
    for (const BlockDump& d : block_dumps_)
        DumpSingleBlockToPath(d, path);

    std::vector<const BlockDump*> by_hotness;
    by_hotness.reserve(block_dumps_.size());
    for (const BlockDump& d : block_dumps_) by_hotness.push_back(&d);
    std::sort(by_hotness.begin(), by_hotness.end(), [](const BlockDump* a, const BlockDump* b) {
        if (a->execution_count != b->execution_count) return a->execution_count > b->execution_count;
        return a->total_guest_cycles > b->total_guest_cycles;
    });

    const std::string csv_path = path + "/block_profile.csv";
    std::ofstream csv(csv_path, std::ios::trunc);
    if (csv.is_open()) {
        csv << "pc,mode,block_len,block_cycles,x86_size,captured_from_prewarm,execution_count,total_guest_cycles,avg_guest_cycles_per_exec\n";
        for (const BlockDump* d : by_hotness) {
            const double avg = d->execution_count != 0
                                   ? static_cast<double>(d->total_guest_cycles) / static_cast<double>(d->execution_count)
                                   : 0.0;
            csv << "0x" << std::hex << std::setw(8) << std::setfill('0') << d->pc << std::dec
                << ',' << (d->is_thumb ? 'T' : 'A')
                << ',' << d->block_len
                << ',' << d->block_cycles
                << ',' << d->x86_size
                << ',' << (d->captured_from_prewarm ? 1 : 0)
                << ',' << d->execution_count
                << ',' << d->total_guest_cycles
                << ',' << std::fixed << std::setprecision(2) << avg
                << '\n';
        }
    }
}

void JitDispatcher::PrintLastBlockIfDebug() {
#ifdef B_DEBUG
    // Show the calling block (the one that branched to the last-executed block), not the last-executed block itself.
    uint32_t pc = last_caller_pc_ != kNoPreviousBlock ? last_caller_pc_ : last_executed_pc_;
    bool thumb = last_caller_pc_ != kNoPreviousBlock ? last_caller_is_thumb_ : last_executed_is_thumb_;
    if (pc == kNoPreviousBlock) return;
    if (thumb) {
        jit_builder_.BuildBlock<true>(pc);
    } else {
        jit_builder_.BuildBlock<false>(pc);
    }
    std::string ir = IrPrinter::PrintArena(jit_builder_.GetArena(), pc);
    std::ostringstream msg;
    msg << "[JitDispatcher] Calling block (branched to last executed 0x" << std::hex << last_executed_pc_
        << "): PC=0x" << pc << " (" << (thumb ? "Thumb" : "ARM") << ")\n" << ir;
    Logger::log(msg.str(), LogLevel::INFO);
#endif
}

void JitDispatcher::RunDisasmScriptForBlock(const std::vector<uint8_t>& arm_bytes,
                                            const std::vector<uint8_t>& x86_bytes,
                                            const std::string& ir_text,
                                            uint32_t pc,
                                            bool is_thumb) const {
    if (arm_bytes.empty() || x86_bytes.empty()) return;
    namespace fs = std::filesystem;
    fs::path tmp_dir = fs::temp_directory_path();
    std::string tmp_arm = (tmp_dir / "axolotl_last_arm.bin").string();
    std::string tmp_x86 = (tmp_dir / "axolotl_last_x86.bin").string();
    std::string tmp_ir = (tmp_dir / "axolotl_last_ir.txt").string();
    std::ofstream(tmp_arm, std::ios::binary).write(reinterpret_cast<const char*>(arm_bytes.data()), arm_bytes.size());
    std::ofstream(tmp_x86, std::ios::binary).write(reinterpret_cast<const char*>(x86_bytes.data()), x86_bytes.size());
    std::ofstream(tmp_ir).write(ir_text.c_str(), ir_text.size());

    std::string script_path;
    if (const char* root = std::getenv("AXOLOTL_ROOT")) {
        script_path = std::string(root) + "/scripts/disasm_last_block.py";
    } else {
        fs::path cwd = fs::current_path();
        script_path = (cwd / "scripts" / "disasm_last_block.py").string();
        if (!fs::exists(script_path)) {
            script_path = (cwd.parent_path() / "scripts" / "disasm_last_block.py").string();
        }
    }
    if (!fs::exists(script_path)) {
        Logger::log("[JitDispatcher] disasm script not found at " + script_path + " (set AXOLOTL_ROOT?)", LogLevel::WARNING);
        return;
    }

    std::ostringstream cmd;
    cmd << "python3 " << script_path << " --arm " << tmp_arm << " --x86 " << tmp_x86
        << " --ir " << tmp_ir
        << " --pc 0x" << std::hex << pc << std::dec
        << " --arm-len " << arm_bytes.size() << " --x86-len " << x86_bytes.size();
    if (is_thumb) cmd << " --thumb";
    int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
        if (WIFEXITED(ret)) {
            Logger::log("[JitDispatcher] disasm script exit code " + std::to_string(WEXITSTATUS(ret)),
                        LogLevel::WARNING);
        } else if (WIFSIGNALED(ret)) {
            Logger::log("[JitDispatcher] disasm script terminated by signal " +
                            std::to_string(WTERMSIG(ret)),
                        LogLevel::WARNING);
        } else {
            Logger::log("[JitDispatcher] disasm script exited (raw status " + std::to_string(ret) + ")",
                        LogLevel::WARNING);
        }
    }
}

void JitDispatcher::DisasmLastBlockIfEnabled() {
#ifdef DISASM_LAST_BLOCK
    if (last_executed_pc_ == kNoPreviousBlock) return;

    std::vector<uint8_t> arm_bytes;
    std::vector<uint8_t> x86_bytes;

    const BlockDump* dump = nullptr;
    for (const auto& d : block_dumps_) {
        if (d.pc == last_executed_pc_ && d.is_thumb == last_executed_is_thumb_) {
            dump = &d;
            break;
        }
    }

    if (dump != nullptr) {
        arm_bytes = dump->arm_bytes;
        x86_bytes = dump->x86_bytes;
    } else {
        if (last_executed_is_thumb_) {
            jit_builder_.BuildBlock<true>(last_executed_pc_);
        } else {
            jit_builder_.BuildBlock<false>(last_executed_pc_);
        }
        uint32_t block_len = jit_builder_.GetBlockLength();
        MemoryBus* bus = jit_builder_.GetBus();
        arm_bytes.reserve(last_executed_is_thumb_ ? block_len * 2u : block_len * 4u);
        for (uint32_t a = last_executed_pc_; a < last_executed_pc_ + block_len; a += last_executed_is_thumb_ ? 2u : 4u) {
            if (last_executed_is_thumb_) {
                uint16_t w = bus->Read16(a, a);
                arm_bytes.push_back(static_cast<uint8_t>(w & 0xFF));
                arm_bytes.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
            } else {
                uint32_t w = bus->Read32(a, a);
                arm_bytes.push_back(static_cast<uint8_t>(w & 0xFF));
                arm_bytes.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));
                arm_bytes.push_back(static_cast<uint8_t>((w >> 16) & 0xFF));
                arm_bytes.push_back(static_cast<uint8_t>((w >> 24) & 0xFF));
            }
        }
        void* host_code = jit_emitter_.EmitBlock(jit_builder_.GetArena(), last_executed_pc_,
                                                 jit_builder_.GetBlockCycles());
        size_t x86_size = jit_emitter_.GetLastEmittedBlockSize();
        const uint8_t* x86_ptr = static_cast<const uint8_t*>(host_code);
        x86_bytes.assign(x86_ptr, x86_ptr + x86_size);
    }

    if (arm_bytes.empty() || x86_bytes.empty()) return;

    std::string ir_text;
    if (dump != nullptr)
        ir_text = dump->ir_text;
    else
        ir_text = IrPrinter::PrintArena(jit_builder_.GetArena(), last_executed_pc_);

    RunDisasmScriptForBlock(arm_bytes, x86_bytes, ir_text, last_executed_pc_, last_executed_is_thumb_);
#endif
}
