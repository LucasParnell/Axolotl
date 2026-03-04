//
// util/debug_console.h — Interactive debug command interface
//
// Only compiled in debug builds (B_DEBUG defined). In release the entire
// class is absent; callers should guard any DebugConsole usage with #ifdef.
//

#pragma once

#ifdef B_DEBUG

#include <functional>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/debug_tools.h"   // WatchpointSet, TraceState

// Forward declarations — avoids pulling heavy headers into every TU.
class MemoryBus;
class BlockMap;
class JitDispatcher;
struct CpuState;

class DebugConsole {
public:
    // All references must outlive the console.
    // |dispatcher| is used for pause/resume and block dump access.
    DebugConsole(CpuState&       cpu_state,
                 MemoryBus&      bus,
                 BlockMap&       block_map,
                 JitDispatcher&  dispatcher,
                 WatchpointSet&  watchpoints,
                 TraceState&     trace);

    // Blocks on stdin, processing one command per line until "quit" / "exit".
    // Intended to run on a dedicated thread.
    void Run();
    void Stop();

private:
    // ── Command handlers ───────────────────────────────────────────────
    void CmdHelp       (const std::vector<std::string>& args);
    void CmdRegs       (const std::vector<std::string>& args);
    void CmdMem        (const std::vector<std::string>& args);
    void CmdMem8       (const std::vector<std::string>& args);
    void CmdMem16      (const std::vector<std::string>& args);
    void CmdMem32      (const std::vector<std::string>& args);
    void CmdIwram      (const std::vector<std::string>& args);
    void CmdBlock      (const std::vector<std::string>& args);
    void CmdIr         (const std::vector<std::string>& args);
    void CmdBlocks     (const std::vector<std::string>& args);
    void CmdStats      (const std::vector<std::string>& args);
    void CmdIrqStat    (const std::vector<std::string>& args);
    void CmdDisasm     (const std::vector<std::string>& args);
    void CmdPause      (const std::vector<std::string>& args);
    void CmdResume     (const std::vector<std::string>& args);
    void CmdStep       (const std::vector<std::string>& args);
    void CmdBreak      (const std::vector<std::string>& args);      // block-address breakpoint
    void CmdWatchIo    (const std::vector<std::string>& args);      // add/remove I/O watchpoint
    void CmdWatchSwi   (const std::vector<std::string>& args);      // add/remove SWI watchpoint
    void CmdPauseBg    (const std::vector<std::string>& args);      // toggle BG-write pause
    void CmdPauseIo    (const std::vector<std::string>& args);      // toggle I/O-write pause
    void CmdPauseWarning(const std::vector<std::string>& args);
    void CmdPauseDma   (const std::vector<std::string>& args);
    void CmdPauseIrq   (const std::vector<std::string>& args);
    void CmdPauseSwi   (const std::vector<std::string>& args);
    void CmdLogBg      (const std::vector<std::string>& args);
    void CmdLogDma     (const std::vector<std::string>& args);
    void CmdLogSwi     (const std::vector<std::string>& args);
    void CmdLoopTrace  (const std::vector<std::string>& args);
    void CmdDumpLoops  (const std::vector<std::string>& args);
    void CmdBacktrace  (const std::vector<std::string>& args);
    void CmdDump       (const std::vector<std::string>& args);      // dump all compiled blocks
    void CmdDumpGame   (const std::vector<std::string>& args);      // dump only Game Pak blocks
    void CmdStepDump   (const std::vector<std::string>& args);      // step + dump each block
    void CmdDumpDisplay(const std::vector<std::string>& args);      // dump palette/VRAM/OAM/regs
    void CmdDumpIwram  (const std::vector<std::string>& args);
    void CmdDumpAsm    (const std::vector<std::string>& args);

    // ── Utilities ──────────────────────────────────────────────────────
    std::vector<std::string> Tokenize(const std::string& line);
    uint32_t                 ParseAddress(const std::string& s);
    void                     PrintHexDump(uint32_t start_addr, uint32_t count);
    static const char*       RegName(uint8_t reg);

    // ── Data ───────────────────────────────────────────────────────────
    CpuState&      cpu_state_;
    MemoryBus&     bus_;
    BlockMap&      block_map_;
    JitDispatcher& dispatcher_;
    WatchpointSet& watchpoints_;
    TraceState&    trace_;

    struct Command {
        std::function<void(const std::vector<std::string>&)> handler;
        std::string usage;
        std::string description;
    };
    std::unordered_map<std::string, Command> commands_;

    void RegisterCommands();

private:
    std::atomic<bool> stop_{false};
};

#endif  // B_DEBUG
