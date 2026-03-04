//
// util/debug_console.cc — Interactive debug command implementation
//

#include "util/debug_console.h"

#ifdef B_DEBUG

#include "data/block_map.h"
#include "data/cpu_state.h"
#include "system/dispatcher.h"
#include "system/memory_bus.h"
#include "util/debug_tools.h"
#include "util/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <sys/select.h>
#include <unistd.h>

// ── ANSI helpers ───────────────────────────────────────────────────────────
static const char* RST        = "\033[0m";
static const char* BOLD       = "\033[1m";
static const char* RED        = "\033[31m";
static const char* GRN        = "\033[32m";
static const char* YEL        = "\033[33m";
static const char* MAG        = "\033[35m";
static const char* CYN        = "\033[36m";
static const char* PROMPT_CLR = "\033[1;36m";

static std::string FmtAddr(uint32_t addr) {
    char buf[11];
    std::snprintf(buf, sizeof(buf), "0x%08X", addr);
    return buf;
}

static bool AddressInBlockRange(uint32_t addr, const BlockDump& d) {
    return addr >= d.pc && addr < (d.pc + d.block_len);
}

// Some debug traces report mirrored/truncated addresses; resolve by low 24 bits.
static bool AddressInBlockRangeMirrored(uint32_t addr, const BlockDump& d) {
    const uint32_t q = addr & 0x00FFFFFFu;
    const uint32_t s = d.pc & 0x00FFFFFFu;
    return q >= s && q < (s + d.block_len);
}

static const BlockDump* FindBlockByAddressOrMirror(const std::vector<BlockDump>& dumps, uint32_t addr) {
    for (const auto& d : dumps) {
        if (AddressInBlockRange(addr, d)) return &d;
    }
    for (const auto& d : dumps) {
        if (AddressInBlockRangeMirrored(addr, d)) return &d;
    }
    return nullptr;
}

static void* FindCachedHostBlock(BlockMap& block_map, uint32_t addr, bool* is_thumb) {
    const bool thumb_hint = (addr & 1u) != 0;
    const uint32_t thumb_pc = addr & ~1u;
    const uint32_t arm_pc = addr & ~3u;

    // Prefer the hinted mode first, then try the other mode.
    const bool first_mode_thumb = thumb_hint;
    for (int pass = 0; pass < 2; ++pass) {
        const bool mode_thumb = (pass == 0) ? first_mode_thumb : !first_mode_thumb;
        const uint32_t pc = mode_thumb ? thumb_pc : arm_pc;
        void* host = block_map.Lookup(pc, mode_thumb);
        if (BlockMap::IsReady(host)) {
            if (is_thumb) *is_thumb = mode_thumb;
            return host;
        }
    }

    // Last check: claimed sentinel means another thread currently owns compilation.
    for (int pass = 0; pass < 2; ++pass) {
        const bool mode_thumb = (pass == 0) ? first_mode_thumb : !first_mode_thumb;
        const uint32_t pc = mode_thumb ? thumb_pc : arm_pc;
        void* host = block_map.Lookup(pc, mode_thumb);
        if (BlockMap::IsClaimed(host)) {
            if (is_thumb) *is_thumb = mode_thumb;
            return host;
        }
    }

    return nullptr;
}

// ── Constructor ────────────────────────────────────────────────────────────

DebugConsole::DebugConsole(CpuState&      cpu_state,
                           MemoryBus&     bus,
                           BlockMap&      block_map,
                           JitDispatcher& dispatcher,
                           WatchpointSet& watchpoints,
                           TraceState&    trace)
    : cpu_state_(cpu_state)
    , bus_(bus)
    , block_map_(block_map)
    , dispatcher_(dispatcher)
    , watchpoints_(watchpoints)
    , trace_(trace)
{
    RegisterCommands();
}

// ── Stop ───────────────────────────────────────────────────────────────────

void DebugConsole::Stop() {
    stop_.store(true, std::memory_order_release);
}

// ── Command registration ───────────────────────────────────────────────────

void DebugConsole::RegisterCommands() {
    commands_["help"] = {
        [this](auto& a){ CmdHelp(a); },
        "help [command]",
        "Show available commands or detailed help for a command"
    };
    commands_["regs"] = {
        [this](auto& a){ CmdRegs(a); },
        "regs",
        "Print CPU registers and CPSR"
    };
    commands_["mem"] = {
        [this](auto& a){ CmdMem(a); },
        "mem <address> [count=64]",
        "Hex dump of <count> bytes starting at <address>"
    };
    commands_["mem8"] = {
        [this](auto& a){ CmdMem8(a); },
        "mem8 <address> [count=8]",
        "Read <count> bytes (8-bit) starting at <address>"
    };
    commands_["mem16"] = {
        [this](auto& a){ CmdMem16(a); },
        "mem16 <address> [count=16]",
        "Read <count> halfwords (16-bit) starting at <address>"
    };
    commands_["mem32"] = {
        [this](auto& a){ CmdMem32(a); },
        "mem32 <address> [count=8]",
        "Read <count> words (32-bit) starting at <address>"
    };
    commands_["iwram"] = {
        [this](auto& a){ CmdIwram(a); },
        "iwram [count=32768]",
        "Hex dump of IWRAM (0x03000000); optional byte count (max 32KB)"
    };
    commands_["block"] = {
        [this](auto& a){ CmdBlock(a); },
        "block <address>",
        "Show compiled block info and hex dump of JIT code at <address>"
    };
    commands_["ir"] = {
        [this](auto& a){ CmdIr(a); },
        "ir <address>",
        "Print IR text for the block at <address>"
    };
    commands_["blocks"] = {
        [this](auto& a){ CmdBlocks(a); },
        "blocks",
        "List all compiled blocks"
    };
    commands_["stats"] = {
        [this](auto& a){ CmdStats(a); },
        "stats",
        "Print execution statistics"
    };
    commands_["irqstat"] = {
        [this](auto& a){ CmdIrqStat(a); },
        "irqstat [reset]",
        "Print IRQ raise/clear counters and key IRQ registers"
    };
    commands_["disasm"] = {
        [this](auto& a){ CmdDisasm(a); },
        "disasm <address>",
        "Disassemble the block at <address> (ARM + x86 via script)"
    };
    commands_["pause"] = {
        [this](auto& a){ CmdPause(a); },
        "pause",
        "Pause execution"
    };
    commands_["resume"] = {
        [this](auto& a){ CmdResume(a); },
        "resume",
        "Resume execution"
    };
    commands_["step"] = {
        [this](auto& a){ CmdStep(a); },
        "step [count=1]",
        "Execute <count> block(s) then pause"
    };
    commands_["break"] = {
        [this](auto& a){ CmdBreak(a); },
        "break <address>",
        "Pause when the block at <address> is about to run"
    };
    commands_["watch_io"] = {
        [this](auto& a){ CmdWatchIo(a); },
        "watch_io <hex_offset>",
        "Pause when I/O at 0x04000000+offset is written (e.g. 0x28 for BG2X)"
    };
    commands_["watch_swi"] = {
        [this](auto& a){ CmdWatchSwi(a); },
        "watch_swi [0x<number>|on|off]",
        "Pause when a watched SWI fires; no arg shows status"
    };
    commands_["pause_bg"] = {
        [this](auto& a){ CmdPauseBg(a); },
        "pause_bg [on|off]",
        "Pause on any BG2X/Y or BG3X/Y write; no arg toggles"
    };
    commands_["pause_io"] = {
        [this](auto& a){ CmdPauseIo(a); },
        "pause_io [on|off]",
        "Pause when a watch_io offset is written; no arg toggles"
    };
    commands_["pause_warning"] = {
        [this](auto& a){ CmdPauseWarning(a); },
        "pause_warning [on|off]",
        "Pause on any log warning; no arg toggles"
    };
    commands_["pause_dma"] = {
        [this](auto& a){ CmdPauseDma(a); },
        "pause_dma [on|off]",
        "Pause when any DMA transfer runs; no arg toggles"
    };
    commands_["pause_irq"] = {
        [this](auto& a){ CmdPauseIrq(a); },
        "pause_irq [on|off]",
        "Pause when an IRQ is taken; no arg toggles"
    };
    commands_["pause_swi"] = {
        [this](auto& a){ CmdPauseSwi(a); },
        "pause_swi [on|off]",
        "Pause on any watched SWI; no arg toggles"
    };
    commands_["log_bg"] = {
        [this](auto& a){ CmdLogBg(a); },
        "log_bg [on|off]",
        "Log every BG affine register write; no arg toggles"
    };
    commands_["log_dma"] = {
        [this](auto& a){ CmdLogDma(a); },
        "log_dma [on|off]",
        "Log every DMA transfer; no arg toggles; default on"
    };
    commands_["log_swi"] = {
        [this](auto& a){ CmdLogSwi(a); },
        "log_swi [on|off]",
        "Log every SWI call; no arg toggles; default on"
    };
    commands_["loop_trace"] = {
        [this](auto& a){ CmdLoopTrace(a); },
        "loop_trace [on|off]",
        "Record loops (15-iteration cap); no arg toggles"
    };
    commands_["dump_loops"] = {
        [this](auto& a){ CmdDumpLoops(a); },
        "dump_loops",
        "Dump all recorded looped block sets"
    };
    commands_["backtrace"] = {
        [this](auto& a){ CmdBacktrace(a); },
        "backtrace",
        "Show collapsed execution path leading to current block"
    };
    commands_["dump"] = {
        [this](auto& a){ CmdDump(a); },
        "dump <address|all|backtrace|bt> [dir]",
        "Dump ARM + x86 + IR for block(s); dump bt [dir] dumps backtrace blocks"
    };
    commands_["dump_game"] = {
        [this](auto& a){ CmdDumpGame(a); },
        "dump_game [dir]",
        "Dump only compiled Game Pak blocks (0x08000000-0x0DFFFFFF)"
    };
    commands_["step_dump"] = {
        [this](auto& a){ CmdStepDump(a); },
        "step_dump <count> [dir]",
        "For <count> iterations: dump current block then step 1"
    };
    commands_["dump_display"] = {
        [this](auto& a){ CmdDumpDisplay(a); },
        "dump_display [path]",
        "Dump palette/VRAM/OAM/regs to file (default: ../analysis/display_dump.txt)"
    };
    commands_["iwram_dump"] = {
        [this](auto& a){ CmdDumpIwram(a); },
        "iwram_dump [path]",
        "Dump IWRAM to file (default: ../analysis/iwram_dump.txt)"
    };
    commands_["dump_asm"] = {
        [this](auto& a){ CmdDumpAsm(a); },
        "dump_asm [path]",
        "Dump combined ARM binary of all compiled blocks + map file"
    };
}

// ── Main loop ──────────────────────────────────────────────────────────────

void DebugConsole::Run() {
    Logger::setInteractivePrompt(true, "gba> ", false);
    std::cout << "\n" << BOLD << CYN
              << "╔══════════════════════════════════════════╗\n"
              << "║         GBAEmu Debug Console             ║\n"
              << "║  Type 'help' for available commands      ║\n"
              << "╚══════════════════════════════════════════╝"
              << RST << "\n\n";

    std::string line;
    bool prompt_shown = false;
    while (!stop_.load(std::memory_order_acquire)) {
        if (!prompt_shown) {
            std::cout << PROMPT_CLR << "gba> " << RST << std::flush;
            prompt_shown = true;
        }

        // Poll stdin with timeout so Stop() can terminate cleanly.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 20000;  // 20 ms
        int ready = ::select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &rfds)) {
            continue;
        }

        if (!std::getline(std::cin, line)) break;
        prompt_shown = false;

        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            CmdResume({});
            continue;
        }
        line = line.substr(start);

        auto tokens = Tokenize(line);
        if (tokens.empty()) continue;

        std::string cmd = tokens[0];
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "quit" || cmd == "exit" || cmd == "q") {
            std::cout << YEL << "Shutting down..." << RST << "\n";
            dispatcher_.Stop();
            break;
        }

        auto it = commands_.find(cmd);
        if (it != commands_.end()) {
            it->second.handler(tokens);
        } else {
            std::cout << RED << "Unknown command: " << cmd
                      << RST << " — type 'help' for a list\n";
        }
    }
    Logger::setInteractivePrompt(false);
}

// ── help ──────────────────────────────────────────────────────────────────

void DebugConsole::CmdHelp(const std::vector<std::string>& args) {
    if (args.size() > 1) {
        auto it = commands_.find(args[1]);
        if (it != commands_.end()) {
            std::cout << GRN << "  " << it->second.usage << RST << "\n"
                      << "  " << it->second.description << "\n";
            return;
        }
        std::cout << RED << "Unknown command: " << args[1] << RST << "\n";
        return;
    }

    std::cout << BOLD << "\nAvailable commands:\n\n" << RST;

    std::vector<std::string> names;
    for (auto& [k, _] : commands_) names.push_back(k);
    std::sort(names.begin(), names.end());

    for (auto& name : names) {
        auto& c = commands_[name];
        std::cout << "  " << GRN << std::left << std::setw(38)
                  << c.usage << RST << " " << c.description << "\n";
    }
    std::cout << "  " << GRN << std::left << std::setw(38)
              << "quit / exit / q" << RST << " Stop emulator and exit\n";
    std::cout << "\n  Addresses can be hex (0x08000000) or decimal.\n\n";
}

// ── regs ──────────────────────────────────────────────────────────────────

void DebugConsole::CmdRegs(const std::vector<std::string>&) {
    std::ostringstream oss;
    oss << "\n" << BOLD << "  CPU Registers:\n" << RST;
    oss << "  " << std::string(50, '-') << "\n";

    for (int i = 0; i < 16; i++) {
        std::string name = RegName(static_cast<uint8_t>(i));
        oss << "  " << CYN << std::left << std::setw(6) << name << RST
            << FmtAddr(cpu_state_.registers[i]);
        if (i % 4 == 3) oss << "\n";
        else            oss << "  ";
    }

    uint32_t cpsr = cpu_state_.cpsr;
    oss << "\n  " << std::string(50, '-') << "\n";
    oss << "  " << MAG << "CPSR:   " << RST << FmtAddr(cpsr) << "\n";
    oss << "  " << MAG << "Flags:  " << RST
        << ((cpsr >> 31) & 1 ? "N" : "n")
        << ((cpsr >> 30) & 1 ? "Z" : "z")
        << ((cpsr >> 29) & 1 ? "C" : "c")
        << ((cpsr >> 28) & 1 ? "V" : "v") << "\n";
    oss << "  " << MAG << "State:  " << RST
        << ((cpsr >> 5) & 1 ? "Thumb" : "ARM") << "\n";

    uint32_t mode = cpsr & 0x1F;
    const char* mode_name = "Unknown";
    switch (mode) {
        case 0x10: mode_name = "User";       break;
        case 0x11: mode_name = "FIQ";        break;
        case 0x12: mode_name = "IRQ";        break;
        case 0x13: mode_name = "Supervisor"; break;
        case 0x17: mode_name = "Abort";      break;
        case 0x1B: mode_name = "Undefined";  break;
        case 0x1F: mode_name = "System";     break;
    }
    oss << "  " << MAG << "Mode:   " << RST << mode_name
        << " (" << FmtAddr(mode) << ")\n";

    // Key I/O registers
    struct IoReg { uint32_t addr; const char* name; };
    static const IoReg kIoRegs[] = {
        { 0x04000000, "DISPCNT"  }, { 0x04000004, "DISPSTAT" },
        { 0x04000006, "VCOUNT"   }, { 0x04000008, "BG0CNT"   },
        { 0x0400000A, "BG1CNT"   }, { 0x0400000C, "BG2CNT"   },
        { 0x0400000E, "BG3CNT"   }, { 0x04000200, "IE"        },
        { 0x04000202, "IF"       }, { 0x04000204, "WAITCNT"  },
        { 0x04000208, "IME"      },
    };

    oss << "\n  " << BOLD << "Key I/O Registers:\n" << RST;
    oss << "  " << std::string(50, '-') << "\n";
    for (const auto& r : kIoRegs) {
        uint16_t val = bus_.Read16(r.addr, 0);
        char vbuf[7];
        std::snprintf(vbuf, sizeof(vbuf), "0x%04X", val);
        oss << "  " << CYN << std::left << std::setw(12) << r.name << RST
            << vbuf << "\n";
    }
    oss << "\n";
    std::cout << oss.str();
}

// ── mem ───────────────────────────────────────────────────────────────────

void DebugConsole::CmdMem(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: mem <address> [count=64]" << RST << "\n"; return; }
    PrintHexDump(ParseAddress(args[1]), args.size() > 2 ? ParseAddress(args[2]) : 64);
}

void DebugConsole::CmdMem8(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: mem8 <address> [count=8]" << RST << "\n"; return; }
    PrintHexDump(ParseAddress(args[1]), args.size() > 2 ? ParseAddress(args[2]) : 8);
}

void DebugConsole::CmdMem16(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: mem16 <address> [count=16]" << RST << "\n"; return; }
    uint32_t addr  = ParseAddress(args[1]);
    uint32_t count = args.size() > 2 ? ParseAddress(args[2]) : 16;
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << "\n";
    for (uint32_t i = 0; i < count; i++) {
        if (i % 8 == 0) oss << " " << YEL << FmtAddr(addr + i * 2) << RST << ": ";
        uint16_t val = bus_.Read16(addr + i * 2, 0);
        // Show canonical 16-bit value so little-endian memory does not appear swapped.
        oss << CYN << std::setw(4) << static_cast<unsigned>(val) << RST << " ";
        if (i % 8 == 7) oss << "\n";
    }
    oss << "\n";
    std::cout << oss.str();
}

void DebugConsole::CmdMem32(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: mem32 <address> [count=8]" << RST << "\n"; return; }
    uint32_t addr  = ParseAddress(args[1]);
    uint32_t count = args.size() > 2 ? ParseAddress(args[2]) : 8;
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << "\n";
    for (uint32_t i = 0; i < count; i++) {
        if (i % 4 == 0) oss << " " << YEL << FmtAddr(addr + i * 4) << RST << ": ";
        uint32_t val = bus_.Read32(addr + i * 4, 0);
        oss << CYN
            << std::setw(2) << static_cast<unsigned>((val >> 24) & 0xFF)
            << std::setw(2) << static_cast<unsigned>((val >> 16) & 0xFF)
            << std::setw(2) << static_cast<unsigned>((val >>  8) & 0xFF)
            << std::setw(2) << static_cast<unsigned>( val        & 0xFF)
            << RST << " ";
        if (i % 4 == 3) oss << "\n";
    }
    oss << "\n";
    std::cout << oss.str();
}

// ── iwram ─────────────────────────────────────────────────────────────────

void DebugConsole::CmdIwram(const std::vector<std::string>& args) {
    static constexpr uint32_t kIwramBase = 0x03000000u;
    static constexpr uint32_t kIwramSize = 32768u;
    uint32_t count = kIwramSize;
    if (args.size() >= 2) count = ParseAddress(args[1]);
    count = std::min(count, kIwramSize);
    if (count == 0) return;
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (uint32_t i = 0; i < count; i++) {
        if (i % 16 == 0) oss << FmtAddr(kIwramBase + i) << ": ";
        oss << std::setw(2) << static_cast<unsigned>(bus_.Read8(kIwramBase + i, 0)) << " ";
        if (i % 16 == 15) oss << "\n";
    }
    if (count % 16 != 0) oss << "\n";
    std::cout << oss.str();
}

// ── block ─────────────────────────────────────────────────────────────────

void DebugConsole::CmdBlock(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: block <address>" << RST << "\n"; return; }
    uint32_t addr = ParseAddress(args[1]);

    const auto& dumps = dispatcher_.GetBlockDumps();
    const BlockDump* found = FindBlockByAddressOrMirror(dumps, addr);

    if (!found) {
        bool cached_thumb = false;
        void* cached = FindCachedHostBlock(block_map_, addr, &cached_thumb);
        if (BlockMap::IsClaimed(cached)) {
            std::cout << YEL << "Block " << FmtAddr(addr)
                      << " is currently being compiled (" << (cached_thumb ? "Thumb" : "ARM")
                      << "). Try again in a moment." << RST << "\n";
            return;
        }
        if (cached) {
            std::cout << YEL << "Block " << FmtAddr(addr)
                      << " is cached (" << (cached_thumb ? "Thumb" : "ARM")
                      << ") but not present in debug dump cache.\n"
                      << "Use `disasm " << FmtAddr(addr)
                      << "` for an on-demand decode." << RST << "\n";
            return;
        }
        std::cout << RED << "No compiled block covering " << FmtAddr(addr) << RST << "\n";
        return;
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    oss << "\n" << BOLD << " Compiled block at " << FmtAddr(found->pc)
        << " (" << (found->is_thumb ? "Thumb" : "ARM") << ")" << RST << "\n";
    if (!AddressInBlockRange(addr, *found))
        oss << "  Resolved from " << FmtAddr(addr) << "\n";
    oss << "  Length: " << std::dec << found->block_len
        << " instr  ARM src: " << found->arm_bytes.size()
        << "B  x86: " << found->x86_bytes.size() << "B\n\n";

    // Hex dump of x86 JIT bytes
    const size_t max_bytes = std::min<size_t>(found->x86_bytes.size(), 256);
    for (size_t i = 0; i < max_bytes; i++) {
        if (i % 16 == 0) oss << "  " << YEL << "0x" << std::setw(4) << i << RST << ":  ";
        oss << std::setw(2) << static_cast<unsigned>(found->x86_bytes[i]) << " ";
        if (i % 16 == 15 || i == max_bytes - 1) {
            size_t pad = (i % 16 == 15) ? 0 : 15 - (i % 16);
            for (size_t p = 0; p < pad; p++) oss << "   ";
            oss << " |";
            size_t ls = i - (i % 16);
            for (size_t j = ls; j <= i; j++) {
                uint8_t c = found->x86_bytes[j];
                oss << (c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
            }
            oss << "|\n";
        }
    }
    if (found->x86_bytes.size() > max_bytes)
        oss << "  ... and " << std::dec << (found->x86_bytes.size() - max_bytes) << " more bytes\n";
    oss << "\n";
    std::cout << oss.str();
}

// ── ir ────────────────────────────────────────────────────────────────────

void DebugConsole::CmdIr(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: ir <address>" << RST << "\n"; return; }
    uint32_t addr = ParseAddress(args[1]);
    // Use dispatcher helper which rebuilds IR on demand
    dispatcher_.PrintIrForBlock(addr);
}

// ── blocks ────────────────────────────────────────────────────────────────

void DebugConsole::CmdBlocks(const std::vector<std::string>&) {
    const auto& dumps = dispatcher_.GetBlockDumps();
    std::ostringstream oss;
    oss << "\n" << BOLD << " Compiled blocks (" << std::dec << dumps.size() << " total):\n" << RST;
    oss << " " << std::string(60, '-') << "\n";
    oss << " " << BOLD << std::left
        << std::setw(6)  << "#"
        << std::setw(14) << "Address"
        << std::setw(8)  << "Mode"
        << std::setw(10) << "ARM bytes"
        << std::setw(10) << "x86 bytes"
        << RST << "\n";
    oss << " " << std::string(60, '-') << "\n";

    size_t max_show = std::min<size_t>(dumps.size(), 100);
    for (size_t i = 0; i < max_show; i++) {
        const auto& d = dumps[i];
        oss << " " << std::left << std::setfill(' ') << std::dec
            << std::setw(6)  << i
            << std::setw(14) << FmtAddr(d.pc)
            << std::setw(8)  << (d.is_thumb ? "Thumb" : "ARM")
            << std::setw(10) << d.arm_bytes.size()
            << std::setw(10) << d.x86_bytes.size() << "\n";
    }
    if (dumps.size() > max_show)
        oss << " ... and " << std::dec << (dumps.size() - max_show) << " more\n";
    oss << "\n";
    std::cout << oss.str();
}

// ── stats ─────────────────────────────────────────────────────────────────

void DebugConsole::CmdStats(const std::vector<std::string>&) {
    const auto& dumps = dispatcher_.GetBlockDumps();
    std::ostringstream oss;
    oss << "\n" << BOLD << "  Execution Statistics:\n" << RST;
    oss << "  " << std::string(40, '-') << "\n";
    oss << "  " << CYN << "Blocks compiled:     " << RST << std::dec << dumps.size() << "\n";
    oss << "  " << CYN << "Cycle counter:       " << RST << std::dec << cpu_state_.cycle_counter << "\n";
    oss << "  " << CYN << "PC:                  " << RST << FmtAddr(cpu_state_.registers[15]) << "\n";
    oss << "  " << CYN << "Mode:                " << RST
        << ((cpu_state_.cpsr >> 5) & 1 ? "Thumb" : "ARM") << "\n";
    oss << "  " << CYN << "Halted:              " << RST << (cpu_state_.halted ? "yes" : "no") << "\n";
    oss << "  " << CYN << "Pause on BG write:   " << RST << (watchpoints_.pause_on_bg_write  ? "on" : "off") << "\n";
    oss << "  " << CYN << "Pause on I/O write:  " << RST << (watchpoints_.pause_on_io_write  ? "on" : "off") << "\n";
    oss << "  " << CYN << "Pause on warning:    " << RST << (watchpoints_.pause_on_warning   ? "on" : "off") << "\n";
    oss << "  " << CYN << "Pause on DMA:        " << RST << (watchpoints_.pause_on_dma       ? "on" : "off") << "\n";
    oss << "  " << CYN << "Pause on IRQ:        " << RST << (watchpoints_.pause_on_irq       ? "on" : "off") << "\n";
    oss << "  " << CYN << "Pause on SWI:        " << RST << (watchpoints_.pause_on_swi       ? "on" : "off") << "\n";
    oss << "  " << CYN << "Loop tracing:        " << RST << (trace_.loop_tracing_enabled     ? "on" : "off") << "\n";
    oss << "  " << CYN << "Log BG writes:       " << RST << (watchpoints_.log_bg_writes      ? "on" : "off") << "\n";
    oss << "  " << CYN << "Log DMA:             " << RST << (watchpoints_.log_dma            ? "on" : "off") << "\n";
    oss << "  " << CYN << "Log SWI:             " << RST << (watchpoints_.log_swi            ? "on" : "off") << "\n";
    oss << "\n";
    std::cout << oss.str();
}

// ── irqstat ────────────────────────────────────────────────────────────────

void DebugConsole::CmdIrqStat(const std::vector<std::string>& args) {
    if (args.size() >= 2) {
        std::string sub = args[1];
        std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);
        if (sub == "reset" || sub == "clear") {
            bus_.ResetIrqDebugCounters();
            std::cout << GRN << "IRQ debug counters reset.\n" << RST;
            return;
        }
    }

    const auto snap = bus_.GetIrqDebugSnapshot();
    const uint16_t ie = bus_.Read16(0x04000200, 0);
    const uint16_t if_reg = bus_.Read16(0x04000202, 0);
    const uint16_t ime = bus_.Read16(0x04000208, 0);
    const uint16_t dispstat = bus_.Read16(0x04000004, 0);
    const uint16_t vcount = bus_.Read16(0x04000006, 0);
    const uint16_t wait_word = bus_.Read16(0x0300310C, cpu_state_.registers[15]);

    std::ostringstream oss;
    oss << "\n" << BOLD << " IRQ Diagnostics:\n" << RST;
    oss << "  " << std::string(44, '-') << "\n";
    oss << "  " << CYN << "VBlank raises:     " << RST << std::dec << snap.vblank_raised << "\n";
    oss << "  " << CYN << "HBlank raises:     " << RST << std::dec << snap.hblank_raised << "\n";
    oss << "  " << CYN << "VCount raises:     " << RST << std::dec << snap.vcount_raised << "\n";
    oss << "  " << CYN << "IF clears:         " << RST << std::dec << snap.if_clears << "\n";
    oss << "  " << CYN << "IE/IF/IME:         " << RST
        << FmtAddr(ie) << " / " << FmtAddr(if_reg) << " / " << FmtAddr(ime) << "\n";
    oss << "  " << CYN << "DISPSTAT/VCOUNT:   " << RST
        << FmtAddr(dispstat) << " / " << FmtAddr(vcount) << "\n";
    oss << "  " << CYN << "wait[0x0300310C]:  " << RST << FmtAddr(wait_word) << "\n\n";
    std::cout << oss.str();
}

// ── disasm ────────────────────────────────────────────────────────────────

void DebugConsole::CmdDisasm(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: disasm <address>" << RST << "\n"; return; }
    dispatcher_.DisasmBlockAtAddress(ParseAddress(args[1]));
}

// ── pause / resume / step ─────────────────────────────────────────────────

void DebugConsole::CmdPause(const std::vector<std::string>&) {
    dispatcher_.RequestPause();
    std::cout << YEL << "Pause requested.  PC = "
              << FmtAddr(cpu_state_.registers[15])
              << "  (" << ((cpu_state_.cpsr & (1u << 5)) ? "Thumb" : "ARM") << ")\n" << RST;
}

void DebugConsole::CmdResume(const std::vector<std::string>&) {
    dispatcher_.Resume();
    std::cout << GRN << "Execution resumed.\n" << RST;
}

void DebugConsole::CmdStep(const std::vector<std::string>& args) {
    uint32_t count = args.size() > 1 ? ParseAddress(args[1]) : 1;
    dispatcher_.StepBlocks(count);
    std::cout << GRN << "Stepping " << std::dec << count << " block(s)...\n" << RST;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "  PC = " << FmtAddr(cpu_state_.registers[15]) << "\n";
}

// ── break ─────────────────────────────────────────────────────────────────

void DebugConsole::CmdBreak(const std::vector<std::string>& args) {
    if (args.size() < 2) { std::cout << RED << "Usage: break <address>" << RST << "\n"; return; }
    uint32_t addr = ParseAddress(args[1]);
    watchpoints_.block_addresses.insert(addr);
    std::cout << GRN << "Breakpoint set at " << FmtAddr(addr)
              << " — will pause before executing this block.\n" << RST;
}

// ── WriteBlockFiles (file-static helper) ──────────────────────────────────

static void WriteBlockFiles(const BlockDump& d, const std::string& dir) {
    char prefix[32];
    std::snprintf(prefix, sizeof(prefix), "block_%08X", d.pc);
    std::string stem = dir + "/" + prefix;

    // ARM/Thumb source bytes
    {
        std::string name = stem + (d.is_thumb ? "_thumb.bin" : "_arm.bin");
        std::ofstream f(name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(d.arm_bytes.data()),
                static_cast<std::streamsize>(d.arm_bytes.size()));
    }
    // x86-64 JIT bytes
    {
        std::ofstream f(stem + "_x86.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(d.x86_bytes.data()),
                static_cast<std::streamsize>(d.x86_bytes.size()));
    }
    // IR text
    if (!d.ir_text.empty()) {
        std::ofstream f(stem + "_ir.txt");
        f << d.ir_text;
    }
    // Metadata: guest address list for cross-referencing disassembly
    {
        std::ofstream f(stem + "_meta.txt");
        f << std::hex << std::uppercase << std::setfill('0');
        f << "# Block " << FmtAddr(d.pc)
          << " - " << FmtAddr(d.pc + d.block_len)
          << " " << (d.is_thumb ? "Thumb" : "ARM") << "\n";
        f << "# arm_bytes=" << d.arm_bytes.size()
          << " x86_bytes=" << d.x86_bytes.size() << "\n\n";
        for (uint32_t a = d.pc; a < d.pc + d.block_len; a += d.is_thumb ? 2u : 4u)
            f << FmtAddr(a) << "\n";
    }

    std::cout << "  " << FmtAddr(d.pc)
              << " [" << (d.is_thumb ? "Thumb" : "ARM  ") << "] "
              << std::dec << d.arm_bytes.size() << "B src → "
              << d.x86_bytes.size() << "B x86  "
              << CYN << prefix << (d.is_thumb ? "_thumb.bin" : "_arm.bin") << RST << "\n";
}

// ── dump ──────────────────────────────────────────────────────────────────

void DebugConsole::CmdDump(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << RED
                  << "Usage:\n"
                  << "  dump <address> [dir]       — ARM + x86 + IR for one block\n"
                  << "  dump all [dir]             — dump every compiled block\n"
                  << "  dump backtrace|bt [dir]    — dump backtrace blocks\n"
                  << RST;
        return;
    }

    std::string subcmd = args[1];
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::tolower);

    // ── dump backtrace / dump bt ───────────────────────────────────────
    if (subcmd == "backtrace" || subcmd == "bt") {
        auto bt = GetBacktrace(trace_);
        if (bt.empty()) {
            std::cout << YEL << "No backtrace (run at least 1 block).\n" << RST;
            return;
        }

        std::string out_dir = "../analysis/dump_bt";
        if (args.size() >= 3) out_dir = args[2];
        std::filesystem::create_directories(out_dir);

        // Build lookup: (pc << 1 | is_thumb) → BlockDump* — same key scheme as MakeKey().
        const auto& dumps = dispatcher_.GetBlockDumps();
        std::unordered_map<uint64_t, const BlockDump*> dump_map;
        dump_map.reserve(dumps.size());
        for (const auto& d : dumps) {
            uint64_t key = (static_cast<uint64_t>(d.pc) << 1) | (d.is_thumb ? 1u : 0u);
            dump_map.emplace(key, &d);
        }

        std::cout << BOLD << CYN
                  << "Dumping backtrace (" << bt.size() << " entries, oldest → newest):\n"
                  << RST;

        // Deduplicate: only write files once per unique (addr, is_thumb) pair.
        std::unordered_map<uint64_t, bool> already_dumped;
        size_t dumped = 0;
        size_t missing = 0;

        for (size_t i = 0; i < bt.size(); i++) {
            const auto& e = bt[i];
            std::cout << "  [" << std::dec << std::setw(2) << i << "] "
                      << FmtAddr(e.block.address)
                      << (e.block.is_thumb ? " (T)" : " (A)");
            if (e.count > 1) std::cout << MAG << " ×" << e.count << RST;

            uint64_t key = (static_cast<uint64_t>(e.block.address) << 1)
                         | (e.block.is_thumb ? 1u : 0u);

            if (already_dumped.count(key)) {
                std::cout << "  " << YEL << "(already dumped)" << RST << "\n";
                continue;
            }
            already_dumped[key] = true;

            auto it = dump_map.find(key);
            if (it != dump_map.end()) {
                std::cout << "\n";
                WriteBlockFiles(*it->second, out_dir);
                dumped++;
            } else {
                // Should not happen: every executed block is now captured in
                // block_dumps_ (pre-warmed blocks are captured on first execution
                // in JitDispatcher::Run).
                std::cout << "  " << RED << "(not in block_dumps_ — unexpected)" << RST << "\n";
                missing++;
            }
        }
        std::cout << GRN << "Dumped " << dumped << " block(s) to " << out_dir << "/";
        if (missing) std::cout << "  " << RED << "(" << missing << " missing)" << RST;
        std::cout << "\n";
        return;
    }

    // ── dump all ──────────────────────────────────────────────────────
    if (subcmd == "all") {
        std::string dir = args.size() >= 3 ? args[2] : "../analysis/dump";
        std::filesystem::create_directories(dir);
        const auto& dumps = dispatcher_.GetBlockDumps();
        for (const auto& d : dumps)
            WriteBlockFiles(d, dir);
        std::cout << GRN << "Dumped " << dumps.size()
                  << " block(s) to " << dir << "/\n" << RST;
        return;
    }

    // ── dump <address> ────────────────────────────────────────────────
    uint32_t addr = ParseAddress(subcmd);
    std::string dir = args.size() >= 3 ? args[2] : "../analysis/dump";
    std::filesystem::create_directories(dir);

    const auto& dumps = dispatcher_.GetBlockDumps();
    const BlockDump* found = FindBlockByAddressOrMirror(dumps, addr);
    if (found) {
        WriteBlockFiles(*found, dir);
        if (!AddressInBlockRange(addr, *found)) {
            std::cout << CYN << "  Resolved " << FmtAddr(addr)
                      << " to mirrored block start " << FmtAddr(found->pc) << RST << "\n";
        } else if (addr != found->pc) {
            std::cout << CYN << "  Resolved " << FmtAddr(addr)
                      << " to block start " << FmtAddr(found->pc) << RST << "\n";
        }
    } else {
        bool cached_thumb = false;
        void* cached = FindCachedHostBlock(block_map_, addr, &cached_thumb);
        if (BlockMap::IsClaimed(cached)) {
            std::cout << YEL << "Block " << FmtAddr(addr)
                      << " is currently being compiled (" << (cached_thumb ? "Thumb" : "ARM")
                      << "). Try again in a moment." << RST << "\n";
            return;
        }
        if (cached) {
            std::cout << YEL << "Block " << FmtAddr(addr)
                      << " is cached (" << (cached_thumb ? "Thumb" : "ARM")
                      << ") but not in `dump` cache (likely prewarmed).\n"
                      << "Use `disasm " << FmtAddr(addr)
                      << "` for on-demand decode." << RST << "\n";
            return;
        }
        std::cout << RED << "No compiled block covering " << FmtAddr(addr) << RST << "\n";
    }
}

// ── dump_game ─────────────────────────────────────────────────────────────

void DebugConsole::CmdDumpGame(const std::vector<std::string>& args) {
    std::string dir = args.size() >= 2 ? args[1] : "../analysis/dump_game";
    std::filesystem::create_directories(dir);

    const auto& dumps = dispatcher_.GetBlockDumps();
    size_t dumped = 0;
    for (const auto& d : dumps) {
        const uint32_t region = d.pc >> 24;
        if (region >= 0x08 && region <= 0x0D) {
            WriteBlockFiles(d, dir);
            ++dumped;
        }
    }

    if (dumped == 0) {
        std::cout << YEL << "No compiled Game Pak blocks found in block dump cache.\n" << RST;
        return;
    }

    std::cout << GRN << "Dumped " << dumped
              << " Game Pak block(s) to " << dir << "/\n" << RST;
}

// ── step_dump ─────────────────────────────────────────────────────────────

void DebugConsole::CmdStepDump(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << RED << "Usage: step_dump <count> [dir]\n" << RST;
        return;
    }
    uint32_t count = ParseAddress(args[1]);
    if (count == 0) { std::cout << RED << "Count must be >= 1\n" << RST; return; }
    std::string dir = args.size() >= 3 ? args[2] : "../analysis/dump";

    std::cout << GRN << "step_dump: " << std::dec << count
              << " iteration(s), dir=" << dir << RST << "\n";

    std::filesystem::create_directories(dir);
    const auto& dumps = dispatcher_.GetBlockDumps();

    for (uint32_t i = 0; i < count; i++) {
        uint32_t pc = cpu_state_.registers[15];
        bool is_thumb = (cpu_state_.cpsr >> 5) & 1;
        bool found = false;
        for (const auto& d : dumps) {
            if (d.pc == pc && d.is_thumb == is_thumb) {
                WriteBlockFiles(d, dir);
                found = true;
                break;
            }
        }
        if (!found)
            std::cout << YEL << "  No dump for PC=" << FmtAddr(pc) << " at iter " << (i + 1) << RST << "\n";
        dispatcher_.StepBlocks(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << GRN << "Done. PC = " << FmtAddr(cpu_state_.registers[15]) << RST << "\n";
}

// ── watch_io ──────────────────────────────────────────────────────────────

void DebugConsole::CmdWatchIo(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cout << "Usage: watch_io <hex_offset>   e.g. watch_io 0x28\n";
        return;
    }
    uint32_t off = ParseAddress(args[1]);
    if (off > 0x3FF) { std::cout << RED << "I/O offset must be 0x000–0x3FF\n" << RST; return; }
    watchpoints_.io_offsets.insert(off);
    std::cout << GRN << "Will pause on write to I/O offset 0x" << std::hex << off
              << " (0x04000000+" << off << ")\n" << RST;
}

// ── watch_swi ─────────────────────────────────────────────────────────────

void DebugConsole::CmdWatchSwi(const std::vector<std::string>& args) {
    if (args.size() >= 2) {
        std::string v = args[1];
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        if (v == "on")  { watchpoints_.pause_on_swi = true;  std::cout << GRN << "Pause on SWI: on\n"  << RST; return; }
        if (v == "off") { watchpoints_.pause_on_swi = false; std::cout << YEL << "Pause on SWI: off\n" << RST; return; }
        uint32_t n = ParseAddress(args[1]);
        if (n > 0xFF) { std::cout << RED << "SWI number must be 0x00–0xFF\n" << RST; return; }
        watchpoints_.swi_numbers.insert(n);
        watchpoints_.pause_on_swi = true;
        std::cout << GRN << "Watchpoint: will pause when SWI 0x" << std::hex << n << " fires.\n" << RST;
        return;
    }
    std::cout << "Pause on SWI: " << (watchpoints_.pause_on_swi ? GRN : YEL)
              << (watchpoints_.pause_on_swi ? "on" : "off") << RST << "\n";
}

// ── Toggle helpers (pause_bg, pause_io, pause_warning, pause_dma,
//                   pause_irq, pause_swi, log_bg, log_dma, log_swi) ─────────

#define DEFINE_TOGGLE(CmdName, field, label) \
void DebugConsole::CmdName(const std::vector<std::string>& args) { \
    if (args.size() >= 2) { \
        std::string v = args[1]; \
        std::transform(v.begin(), v.end(), v.begin(), ::tolower); \
        if (v == "on")  { field = true;  std::cout << GRN << label ": on\n"  << RST; return; } \
        if (v == "off") { field = false; std::cout << YEL << label ": off\n" << RST; return; } \
    } \
    field = !field; \
    std::cout << (field ? GRN : YEL) << label ": " << (field ? "on" : "off") << RST << "\n"; \
}

DEFINE_TOGGLE(CmdPauseBg,      watchpoints_.pause_on_bg_write, "Pause on BG register write")
DEFINE_TOGGLE(CmdPauseIo,      watchpoints_.pause_on_io_write, "Pause on I/O watchpoint")
DEFINE_TOGGLE(CmdPauseWarning, watchpoints_.pause_on_warning,  "Pause on warning")
DEFINE_TOGGLE(CmdPauseDma,     watchpoints_.pause_on_dma,      "Pause on DMA")
DEFINE_TOGGLE(CmdPauseIrq,     watchpoints_.pause_on_irq,      "Pause on IRQ")
DEFINE_TOGGLE(CmdPauseSwi,     watchpoints_.pause_on_swi,      "Pause on SWI")
DEFINE_TOGGLE(CmdLogBg,        watchpoints_.log_bg_writes,     "Log BG writes")
DEFINE_TOGGLE(CmdLogDma,       watchpoints_.log_dma,           "Log DMA")
DEFINE_TOGGLE(CmdLogSwi,       watchpoints_.log_swi,           "Log SWI")

#undef DEFINE_TOGGLE

// ── loop_trace ────────────────────────────────────────────────────────────

void DebugConsole::CmdLoopTrace(const std::vector<std::string>& args) {
    if (args.size() >= 2) {
        std::string v = args[1];
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        if (v == "on")  { trace_.loop_tracing_enabled = true;  std::cout << GRN << "Loop trace: on\n"  << RST; return; }
        if (v == "off") { trace_.loop_tracing_enabled = false; std::cout << YEL << "Loop trace: off\n" << RST; return; }
    }
    trace_.loop_tracing_enabled = !trace_.loop_tracing_enabled;
    std::cout << (trace_.loop_tracing_enabled ? GRN : YEL)
              << "Loop trace: " << (trace_.loop_tracing_enabled ? "on" : "off") << RST << "\n";
}

// ── dump_loops ────────────────────────────────────────────────────────────

void DebugConsole::CmdDumpLoops(const std::vector<std::string>&) {
    auto loops = GetRecordedLoopsSnapshot(trace_);
    if (loops.empty()) {
        std::cout << YEL << "No recorded loops. Use " << BOLD << "loop_trace on"
                  << RST << YEL << " then run the emulator.\n" << RST;
        return;
    }
    std::cout << BOLD << CYN << "Recorded loops (" << loops.size() << "):\n\n" << RST;
    for (size_t i = 0; i < loops.size(); i++) {
        const auto& body = loops[i];
        std::cout << BOLD << "Loop " << (i + 1) << RST << " (" << body.size() << " blocks): ";
        for (size_t j = 0; j < body.size(); j++) {
            if (j > 0) std::cout << " → ";
            std::cout << FmtAddr(body[j].address)
                      << (body[j].is_thumb ? " (T)" : " (A)");
        }
        std::cout << "\n";
    }
}

// ── backtrace ─────────────────────────────────────────────────────────────

void DebugConsole::CmdBacktrace(const std::vector<std::string>&) {
    auto bt = GetBacktrace(trace_);
    if (bt.empty()) {
        std::cout << YEL << "No backtrace available.\n" << RST;
        return;
    }

    std::cout << BOLD << CYN << "Backtrace (" << bt.size()
              << " entries, oldest → newest):\n" << RST;

    for (size_t i = 0; i < bt.size(); i++) {
        const auto& e = bt[i];
        std::cout << "  [" << std::dec << std::setw(2) << i << "] "
                  << CYN << FmtAddr(e.block.address) << RST
                  << (e.block.is_thumb ? " (T)" : " (A)");
        if (e.count > 1) std::cout << MAG << "  ×" << e.count << RST;
        std::cout << "\n";
    }
    std::cout << "\n";
}

// ── dump_display ──────────────────────────────────────────────────────────

void DebugConsole::CmdDumpDisplay(const std::vector<std::string>& args) {
    std::string path = "../analysis/display_dump.txt";
    if (args.size() >= 2) path = args[1];

    std::ofstream out(path);
    if (!out) { std::cout << RED << "Could not open " << path << " for writing\n" << RST; return; }
    out << std::hex << std::uppercase << std::setfill('0');

    // Key display registers
    out << "=== Display Registers (0x04000000) ===\n";
    struct { uint32_t addr; const char* name; } dregs[] = {
        { 0x04000000, "DISPCNT"  }, { 0x04000004, "DISPSTAT" },
        { 0x04000006, "VCOUNT"   }, { 0x04000008, "BG0CNT"   },
        { 0x0400000A, "BG1CNT"   }, { 0x0400000C, "BG2CNT"   },
        { 0x0400000E, "BG3CNT"   },
    };
    for (const auto& r : dregs)
        out << r.name << "  " << FmtAddr(r.addr) << " = 0x"
            << std::setw(4) << bus_.Read16(r.addr, 0) << "\n";
    out << "\n";

    // Palette RAM (0x05000000, 512 bytes)
    out << "=== Palette RAM (0x05000000, 512 bytes) ===\n";
    const uint8_t* pal = bus_.GetPalettePtr();
    for (uint32_t i = 0; i < bus_.GetPaletteSize(); i++) {
        if (i % 16 == 0) out << FmtAddr(0x05000000u + i) << ": ";
        out << std::setw(2) << static_cast<unsigned>(pal[i]) << " ";
        if (i % 16 == 15) out << "\n";
    }
    out << "\n";

    // VRAM (0x06000000, first 0x400 bytes of char data)
    out << "=== VRAM char block 0 (0x06000000, first 0x400 bytes) ===\n";
    const uint8_t* vram = bus_.GetVramPtr();
    for (uint32_t i = 0; i < std::min<size_t>(0x400u, bus_.GetVramSize()); i++) {
        if (i % 16 == 0) out << FmtAddr(0x06000000u + i) << ": ";
        out << std::setw(2) << static_cast<unsigned>(vram[i]) << " ";
        if (i % 16 == 15) out << "\n";
    }
    out << "\n";

    // OAM (0x07000000, 1 KB)
    out << "=== OAM (0x07000000, 1 KB) ===\n";
    const uint8_t* oam = bus_.GetOamPtr();
    for (uint32_t i = 0; i < bus_.GetOamSize(); i++) {
        if (i % 16 == 0) out << FmtAddr(0x07000000u + i) << ": ";
        out << std::setw(2) << static_cast<unsigned>(oam[i]) << " ";
        if (i % 16 == 15) out << "\n";
    }
    out << "\n";

    out.flush();
    std::cout << GRN << "Display dump written to " << path << RST << "\n";
}

// ── iwram_dump ────────────────────────────────────────────────────────────

void DebugConsole::CmdDumpIwram(const std::vector<std::string>& args) {
    std::string path = "../analysis/iwram_dump.txt";
    if (args.size() >= 2) path = args[1];

    std::ofstream out(path);
    if (!out) { std::cout << RED << "Could not open " << path << " for writing\n" << RST; return; }
    out << std::hex << std::uppercase << std::setfill('0');
    out << "=== IWRAM (0x03000000, 32 KB) ===\n\n";

    constexpr uint32_t kBase = 0x03000000u;
    constexpr uint32_t kSize = 0x8000u;
    for (uint32_t i = 0; i < kSize; i++) {
        if (i % 16 == 0) out << FmtAddr(kBase + i) << ": ";
        out << std::setw(2) << static_cast<unsigned>(bus_.Read8(kBase + i, 0)) << " ";
        if (i % 16 == 15) out << "\n";
    }
    out << "\n";
    out.flush();
    std::cout << GRN << "IWRAM dump written to " << path << RST << "\n";
}

// ── dump_asm ──────────────────────────────────────────────────────────────

void DebugConsole::CmdDumpAsm(const std::vector<std::string>& args) {
    std::string out_dir = args.size() >= 2 ? args[1] : "../analysis/dump_asm";
    std::filesystem::create_directories(out_dir);

    const auto& dumps = dispatcher_.GetBlockDumps();

    // Sort by start address for a deterministic combined binary
    std::vector<const BlockDump*> sorted;
    sorted.reserve(dumps.size());
    for (const auto& d : dumps) sorted.push_back(&d);
    std::sort(sorted.begin(), sorted.end(), [](const BlockDump* a, const BlockDump* b) {
        if (a->pc != b->pc) return a->pc < b->pc;
        return !a->is_thumb && b->is_thumb;
    });

    std::string bin_path = out_dir + "/combined_arm.bin";
    std::string map_path = out_dir + "/combined_arm.map";
    std::ofstream bin_out(bin_path, std::ios::binary);
    std::ofstream map_out(map_path);
    if (!bin_out || !map_out) {
        std::cout << RED << "Failed to open output files in " << out_dir << RST << "\n";
        return;
    }

    map_out << std::hex << std::uppercase
            << "# file_offset_hex  start_address  size_bytes  mode\n";

    size_t file_off = 0;
    size_t total    = 0;
    for (const BlockDump* d : sorted) {
        if (d->arm_bytes.empty()) continue;
        bin_out.write(reinterpret_cast<const char*>(d->arm_bytes.data()),
                      static_cast<std::streamsize>(d->arm_bytes.size()));
        map_out << std::setw(8) << std::setfill('0') << file_off << "  "
                << FmtAddr(d->pc) << "  "
                << std::dec << d->arm_bytes.size() << "  "
                << (d->is_thumb ? "thumb" : "arm") << "\n";
        file_off += d->arm_bytes.size();
        total    += d->arm_bytes.size();
    }

    std::cout << GRN << "Dumped " << sorted.size() << " block(s), "
              << std::dec << total << " bytes total\n"
              << "  " << bin_path << "\n  " << map_path << RST << "\n";
}



// ── PrintHexDump ──────────────────────────────────────────────────────────

void DebugConsole::PrintHexDump(uint32_t start, uint32_t count) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0') << "\n";
    for (uint32_t i = 0; i < count; i++) {
        if (i % 16 == 0) oss << "  " << YEL << FmtAddr(start + i) << RST << ":  ";
        uint8_t val = bus_.Read8(start + i, 0);
        oss << std::setw(2) << static_cast<unsigned>(val) << " ";
        if (i % 16 == 15 || i == count - 1) {
            size_t pad = (i % 16 == 15) ? 0 : 15 - (i % 16);
            for (size_t p = 0; p < pad; p++) oss << "   ";
            oss << " |";
            uint32_t ls = i - (i % 16);
            for (uint32_t j = ls; j <= i; j++) {
                uint8_t c = bus_.Read8(start + j, 0);
                oss << (c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
            }
            oss << "|\n";
        }
    }
    oss << "\n";
    std::cout << oss.str();
}

// ── Tokenize ──────────────────────────────────────────────────────────────

std::vector<std::string> DebugConsole::Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);
    return tokens;
}

// ── ParseAddress ──────────────────────────────────────────────────────────

uint32_t DebugConsole::ParseAddress(const std::string& s) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return static_cast<uint32_t>(std::stoul(s, nullptr, 16));
    return static_cast<uint32_t>(std::stoul(s, nullptr, 0));
}

// ── RegName ───────────────────────────────────────────────────────────────

const char* DebugConsole::RegName(uint8_t reg) {
    switch (reg) {
        case 13: return "SP";
        case 14: return "LR";
        case 15: return "PC";
        default: break;
    }
    // Static storage for r0–r12
    static const char* names[] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12"
    };
    if (reg < 13) return names[reg];
    return "??";
}

#endif  // B_DEBUG
