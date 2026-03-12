#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <cstring>

#include <QApplication>
#include <QCoreApplication>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

#include "data/input_mapping_data.h"
#include "util/logger.h"
#include "system/config/config_reader.h"
#include "system/memory_bus.h"
#include "system/crash_handler.h"
#include "system/display.h"
#include "system/ppu.h"
#include "system/audio_system.h"
#include "system/block_compile_cache.h"
#include "system/jit_emitter.h"
#include "data/block_map.h"
#include "system/input/input_mapping_system.h"
#include "system/input/input_mapping_window.h"
#include "system/seed_queue.h"
#include "system/dispatcher.h"
#include "system/prewarm_pacing.h"
#include "system/prewarmer.h"
#include "util/logo_vram_checker.h"

#ifdef B_DEBUG
#include "util/debug_tools.h"
#include "util/debug_console.h"
// Declared in memory_bus.cc — wires the global watchpoint pointers.
extern void MemoryBus_SetDebugState(WatchpointSet*, std::atomic<bool>*);
#endif

#ifndef ENABLE_LOGO_VRAM_CHECK
#define ENABLE_LOGO_VRAM_CHECK 0
#endif

static void AudioBatchSink(void* user, const int16_t* interleaved, size_t frames) {
    auto* audio = static_cast<AudioSystem*>(user);
    if (!audio) return;
    audio->QueueSamples(interleaved, frames);
}

static bool EnvBool(const char* key, bool default_value) {
    const char* v = std::getenv(key);
    if (!v || !*v) return default_value;
    return std::strcmp(v, "0") != 0;
}

static uint64_t EnvU64(const char* key, uint64_t default_value) {
    const char* v = std::getenv(key);
    if (!v || !*v) return default_value;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) return default_value;
    return static_cast<uint64_t>(parsed);
}

int main(int argc, char* argv[]) {
    QApplication qt_app(argc, argv);
    qt_app.setQuitOnLastWindowClosed(false);
    const std::filesystem::path executable_dir =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    const std::string config_path =
        (executable_dir / "config.ini").lexically_normal().string();

    Logger::setOnWarning([](const std::string& msg) {
        std::cerr << "WARN: " << msg << std::endl;
    });
    Logger::log("Starting GBA JIT...", LogLevel::INFO);

    CrashHandler::install();

    MemoryBus bus;
    if (!bus.LoadBios("res/gba_bios.bin")) {
        Logger::log("Failed to load gba_bios.bin.", LogLevel::ERR);
        return -1;
    }

    if (argc > 1) {
        if (!bus.LoadRom(argv[1])) {
            Logger::log("Failed to load ROM: " + std::string(argv[1]), LogLevel::ERR);
            return -1;
        }
    } else {
        Logger::log("No ROM specified. Running BIOS only (open bus for GamePak).",
                     LogLevel::INFO);
    }

    auto block_map = std::make_unique<BlockMap>();
    SeedQueue seed_queue;
    PrewarmPacingState prewarm_pacing;
    std::unique_ptr<BlockCompileCache> block_cache;
#if defined(__linux__) || defined(__APPLE__)
    std::vector<std::pair<void*, size_t>> loaded_x86_mappings;
#endif
    {
        const std::string rom_path = (argc > 1) ? std::string(argv[1]) : std::string();
        const uint32_t rom_loaded_size = bus.GetRomLoadedSize();
        const bool x86_cache_enable = EnvBool("AXOLOTL_X86_CACHE_ENABLE", true);
        const bool x86_load_enable = EnvBool("AXOLOTL_X86_CACHE_LOAD_ENABLE", true);
        const bool x86_save_enable = EnvBool("AXOLOTL_X86_CACHE_SAVE_ENABLE", true);
        const char* env_x86_log_level = std::getenv("AXOLOTL_X86_CACHE_LOG_LEVEL");
        const int x86_log_level = (env_x86_log_level && *env_x86_log_level) ? std::atoi(env_x86_log_level) : 1;
        const char* env_cache_dir = std::getenv("AXOLOTL_X86_CACHE_DIR");
        if (!env_cache_dir || !*env_cache_dir) env_cache_dir = std::getenv("AXOLOTL_BLOCK_CACHE_DIR");
        const std::string cache_root = (env_cache_dir && *env_cache_dir) ? std::string(env_cache_dir) : std::string("./cache");
        BlockCompileCache::Config cfg;
        cfg.root_dir = cache_root;
        cfg.cache_key = BuildBlockCompileCacheKey(rom_path, "res/gba_bios.bin");
        cfg.x86_cache_enable = x86_cache_enable;
        cfg.x86_load_enable = x86_load_enable;
        cfg.x86_save_enable = x86_save_enable;
        cfg.x86_log_level = x86_log_level;
        cfg.host_feature_mask = CodeEmitter::HostFeatureMaskString();
        block_cache = std::make_unique<BlockCompileCache>(std::move(cfg));
        if (block_cache->Initialize()) {
            Logger::log("[BlockCache] root=" + std::filesystem::absolute(cache_root).string(), LogLevel::INFO);
            Logger::log("[BlockCache] dir=" + std::filesystem::absolute(block_cache->CacheDir()).string(), LogLevel::INFO);
            std::vector<CompileTarget> cached_seeds;
            if (block_cache->LoadSeedRecords(&cached_seeds)) {
                const size_t seed_preload_limit = static_cast<size_t>(EnvU64(
                    "AXOLOTL_BLOCK_CACHE_SEED_PRELOAD_LIMIT",
                    static_cast<uint64_t>(SeedQueue::MaxQueued())));
                size_t enqueued = 0;
                const size_t to_try = std::min(seed_preload_limit, cached_seeds.size());
                for (size_t i = 0; i < to_try; ++i) {
                    const auto& t = cached_seeds[i];
                    const uint32_t aligned_pc = t.pc & (t.is_thumb ? ~1u : ~3u);
                    if (!BlockCacheData::IsPersistentCachePcWithRomSize(aligned_pc, rom_loaded_size)) {
                        continue;
                    }
                    if (!seed_queue.Push(t.pc, t.is_thumb)) break;
                    ++enqueued;
                }
                Logger::log("[BlockCache] seed records=" + std::to_string(cached_seeds.size()) +
                                " enqueued=" + std::to_string(enqueued) +
                                " (limit=" + std::to_string(seed_preload_limit) + ")",
                            LogLevel::INFO);
            }
#if defined(__linux__) || defined(__APPLE__)
            if (x86_cache_enable && x86_load_enable) {
                std::vector<BlockCompileCache::LoadedX86Block> loaded;
                constexpr size_t kMiB = 1024 * 1024;
                const size_t x86_preload_max_blocks = static_cast<size_t>(EnvU64("AXOLOTL_X86_PRELOAD_MAX_BLOCKS", 8000));
                const size_t x86_preload_max_mb = static_cast<size_t>(EnvU64("AXOLOTL_X86_PRELOAD_MAX_MB", 96));
                const size_t x86_preload_max_bytes =
                    x86_preload_max_mb == 0 ? 0 : (x86_preload_max_mb * kMiB);
                uint64_t patched = 0;
                uint64_t rejected = 0;
                uint64_t published = 0;
                if (x86_preload_max_blocks != 0 && x86_preload_max_bytes != 0 &&
                    block_cache->LoadX86Blocks(&loaded, {x86_preload_max_blocks, x86_preload_max_bytes})) {
                    size_t total_bytes = 0;
                    for (const auto& b : loaded) total_bytes += (b.x86_bytes.size() + 15u) & ~size_t(15u);
                    if (total_bytes != 0) {
                        void* rw = mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                        if (rw != MAP_FAILED) {
                            uint8_t* cursor = static_cast<uint8_t*>(rw);
                            struct PendingPublish {
                                uint32_t pc;
                                bool thumb;
                                void* host_ptr;
                            };
                            std::vector<PendingPublish> publish_list;
                            publish_list.reserve(loaded.size());
                            for (const auto& b : loaded) {
                                if (!BlockCacheData::IsPersistentCachePcWithRomSize(b.pc, rom_loaded_size)) {
                                    ++rejected;
                                    continue;
                                }
                                uint8_t* block_dst = cursor;
                                std::memcpy(block_dst, b.x86_bytes.data(), b.x86_bytes.size());
                                bool ok = true;
                                for (const auto& r : b.relocs) {
                                    if (r.type != static_cast<uint32_t>(BlockCacheData::X86RelocType::kAbs64)) {
                                        ok = false;
                                        break;
                                    }
                                    if (static_cast<size_t>(r.offset) + sizeof(uint64_t) > b.x86_bytes.size()) {
                                        ok = false;
                                        break;
                                    }
                                    const auto sym = static_cast<BlockCacheData::X86SymbolId>(r.symbol_id);
                                    uintptr_t addr = CodeEmitter::ResolveCacheSymbol(sym);
                                    if (addr == 0) {
                                        ok = false;
                                        break;
                                    }
                                    std::memcpy(block_dst + r.offset, &addr, sizeof(addr));
                                    ++patched;
                                }
                                if (ok) {
                                    publish_list.push_back({b.pc, b.is_thumb, block_dst});
                                } else {
                                    ++rejected;
                                }
                                cursor += (b.x86_bytes.size() + 15u) & ~size_t(15u);
                            }
                            if (mprotect(rw, total_bytes, PROT_READ | PROT_EXEC) == 0) {
                                for (const auto& p : publish_list) {
                                    block_map->ForcePublish(p.pc, p.thumb, p.host_ptr);
                                    ++published;
                                }
                                loaded_x86_mappings.push_back({rw, total_bytes});
                            } else {
                                rejected += publish_list.size();
                                munmap(rw, total_bytes);
                            }
                        }
                    }
                }
                const bool hit_block_limit = loaded.size() >= x86_preload_max_blocks;
                Logger::log("[BlockCache] x86 preload published=" + std::to_string(published) +
                                " rejected=" + std::to_string(rejected) +
                                " patched=" + std::to_string(patched) +
                                " loaded=" + std::to_string(loaded.size()) +
                                " max_blocks=" + std::to_string(x86_preload_max_blocks) +
                                " max_mb=" + std::to_string(x86_preload_max_mb) +
                                (hit_block_limit ? " (hit block limit)" : ""),
                            LogLevel::INFO);
            }
#endif
            block_cache->StartWriterThread();
        } else {
            block_cache.reset();
        }
    }

    GlDisplay display;
    if (!display.init()) {
        Logger::log("Failed to init display.", LogLevel::ERR);
        return -1;
    }

    AudioSystem audio;
    audio.Start();
    bus.SetAudioSampleCallback(&AudioBatchSink, &audio);

    ConfigReader config_reader(config_path);
    InputConfigData input_config;
    config_reader.Load(&input_config);
    InputMappingSystem input_mapping_system(&display);
    input_mapping_system.SyncProfilesWithDevices(&input_config);
    InputMappingWindow input_window(&input_config, &config_reader, &input_mapping_system);
    input_window.hide();

    PPU ppu;
#if ENABLE_LOGO_VRAM_CHECK
    LogoVramChecker logo_vram_checker;
    {
        Logger::log("[LogoVram] cwd=" + std::filesystem::current_path().string(), LogLevel::INFO);
        const std::vector<std::string> roots = {
            "../analysis",
            "analysis/logo_assets",
            "../analysis/logo_assets",
            "../../analysis/logo_assets",
            "build-debug/../analysis/logo_assets",
        };
        bool loaded = false;
        for (const auto& root : roots) {
            const std::string csv = root + "/expected_vram.csv";
            if (!std::filesystem::exists(csv)) continue;
            // Support both layouts:
            // 1) expected_vram.csv + assets in same dir
            // 2) expected_vram.csv in base dir, assets in base/logo_assets
            if (logo_vram_checker.Load(csv, root) ||
                logo_vram_checker.Load(csv, root + "/logo_assets")) {
                Logger::log("[LogoVram] enabled from " + root, LogLevel::INFO);
                loaded = true;
                break;
            }
        }
        if (!loaded) Logger::log("[LogoVram] disabled (expectations not loaded)", LogLevel::WARNING);
    }
#endif

    JitDispatcher dispatcher(&bus, block_map.get(), &seed_queue, &prewarm_pacing, block_cache.get());
    PreWarmer prewarmer(&bus, block_map.get(), &seed_queue, &prewarm_pacing, block_cache.get());

#ifdef B_DEBUG
    WatchpointSet watchpoints;
    TraceState    trace;
    
    //watchpoints.block_addresses.insert(0xC04);

    watchpoints.pause_on_io_write = true;
    dispatcher.SetDebugState(&watchpoints, &trace);
    // Wire memory_bus BG/IO watchpoints into the dispatcher's pause flag.
    MemoryBus_SetDebugState(&watchpoints, &dispatcher.PauseFlag());
#endif

    std::atomic<bool> vblank_ready{false};
    using Clock = std::chrono::steady_clock;
    auto next_frame_deadline = Clock::now();
    constexpr auto kFramePeriod = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / 59.7275));  // GBA frame rate

    TimingCallbacks timing_cb;
    // Frame counter used to trigger a one-time pause at a target frame.
    std::atomic<int> frame_count{0};
    timing_cb.on_hblank = [&](int line) {
        // Render every visible scanline when the emulated CPU reaches HBlank.
        ppu.RenderScanline(line);
    };
    timing_cb.on_scanline_start = [&](int line) {
        if (line == 0) {
            // Pace emulation to real hardware frame rate.
            next_frame_deadline += kFramePeriod;
            const auto now = Clock::now();
            if (next_frame_deadline < now) next_frame_deadline = now;
            std::this_thread::sleep_until(next_frame_deadline);

            // Frame boundary (scanline 228→0): snapshot bus state for the next
            // frame.  CPU has had the full VBlank period (lines 160–227) to
            // update VRAM/palette/OAM, so the snapshot reflects those writes.
            ppu.OnVBlank(&bus);
            vblank_ready.store(true, std::memory_order_release);
            // Increment frame counter and pause at frame 60 (one-based).
            [[maybe_unused]] int f = frame_count.fetch_add(1, std::memory_order_acq_rel) + 1;
#if ENABLE_LOGO_VRAM_CHECK
            if (logo_vram_checker.IsLoaded()) {
                // Probe early transition points around BIOS logo setup.
                if (f == 1 || f == 6 || f == 30 || f == 60 || f == 115) {
                    logo_vram_checker.VerifyFrame(bus, f);
                }
            }
#endif
            //if (f == 60) {
                //Logger::log("Reached frame 60 — requesting pause.", LogLevel::INFO);
                //dispatcher.RequestPause();
            //}
        }
    };
    dispatcher.SetTimingCallbacks(std::move(timing_cb));

    ppu.OnVBlank(&bus);  // Prime snapshot so first frame has data to render from

    std::thread pw_thread([&prewarmer]() { prewarmer.ThreadLoop(); });
    std::thread jit_thread([&dispatcher]() { dispatcher.Run(0x00000000, false); });

    bool f11_prev_down = false;
    bool f12_prev_down = false;
    bool menu_open_prev = false;

#ifdef B_DEBUG
    // Console blocks on stdin; run it on its own thread so the emulator keeps going.
    // The console calls dispatcher.RequestPause() / Resume() / StepBlocks() to
    // control execution from the keyboard.
    DebugConsole console(dispatcher.GetCpuState(), bus, *block_map,
                         dispatcher, watchpoints, trace);
    std::thread console_thread([&console]() { console.Run(); });
#endif
    while (display.tick()) {
        QCoreApplication::processEvents();

        const bool f11_now_down = glfwGetKey(display.GetWindow(), GLFW_KEY_F11) == GLFW_PRESS;
        if (!f11_prev_down && f11_now_down) {
            display.toggleFullscreen();
        }
        f11_prev_down = f11_now_down;

        const bool f12_now_down = glfwGetKey(display.GetWindow(), GLFW_KEY_F12) == GLFW_PRESS;
        if (!f12_prev_down && f12_now_down) {
            if (input_window.isVisible()) {
                input_window.hide();
            } else {
                input_window.show();
                input_window.raise();
                input_window.activateWindow();
            }
        }
        f12_prev_down = f12_now_down;

        const bool menu_open = input_window.isVisible();
        if (menu_open != menu_open_prev) {
            dispatcher.SetHostPaused(menu_open);
            menu_open_prev = menu_open;
        }

        input_window.Tick();
        input_mapping_system.PollAndApply(input_config, &bus);

        // The display loop only cares about pushing completed frames to OpenGL.
        // PPU snapshot + framebuffer swap happen on the JIT thread (on_scanline_start)
        // so we just submit the already-completed framebuffer here.
        if (vblank_ready.exchange(false, std::memory_order_acquire)) {
            constexpr uint32_t kFramebufferSize = 240 * 160 * 3;  // PPU dimensions
            if (!dispatcher.IsPaused()) {
                display.submitFrame(reinterpret_cast<const uint8_t*>(ppu.GetFramebuffer()), kFramebufferSize);
            }
        }
    }

    dispatcher.Stop();
    jit_thread.join();

    dispatcher.PrintLastBlockIfDebug();
    dispatcher.DisasmLastBlockIfEnabled();

    prewarmer.Stop();
    pw_thread.join();
    if (block_cache) block_cache->StopWriterThread();
#if defined(__linux__) || defined(__APPLE__)
    for (const auto& m : loaded_x86_mappings) {
        if (m.first && m.second) munmap(m.first, m.second);
    }
#endif

#ifdef B_DEBUG
    console.Stop();          // sets stop_ so console loop exits
    console_thread.join();
#endif

    display.shutdown();
    audio.Stop();

    Logger::log("Run complete. Dumping to latest.log...", LogLevel::INFO);
    Logger::writeToFile("latest.log");

    if (EnvBool("AXOLOTL_DUMP_ANALYSIS_ON_EXIT", false)) {
        dispatcher.DumpAnalysisFiles("../analysis");
    }

    return 0;
}
