#include <iostream>
#include <memory>
#include <thread>

#include "util/logger.h"
#include "system/memory_bus.h"
#include "data/block_map.h"
#include "system/seed_queue.h"
#include "system/dispatcher.h"
#include "system/prewarmer.h"

int main() {
    Logger::setOnWarning([](const std::string& msg) {
        std::cerr << "WARN: " << msg << std::endl;
    });
    Logger::log("Starting GBA JIT (no code emission yet)...", LogLevel::INFO);

    MemoryBus bus;
    if (!bus.LoadBios("res/gba_bios.bin")) {
        Logger::log("Failed to load gba_bios.bin.", LogLevel::ERR);
        return -1;
    }

    auto block_map = std::make_unique<BlockMap>();
    SeedQueue seed_queue;

    JitDispatcher dispatcher(&bus, block_map.get(), &seed_queue);
    PreWarmer prewarmer(&bus, block_map.get(), &seed_queue);

    std::thread pw_thread([&prewarmer]() { prewarmer.ThreadLoop(); });

    constexpr size_t kMaxBlocks = 50;
    dispatcher.RunSimulated(0x00000000, false, kMaxBlocks);

    prewarmer.Stop();
    pw_thread.join();

    Logger::log("Run complete. Dumping to latest.log...", LogLevel::INFO);
    Logger::writeToFile("latest.log");

    return 0;
}
