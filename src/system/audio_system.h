#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

class AudioSystem {
 public:
    AudioSystem();
    ~AudioSystem();

    bool Start();
    void Stop();

    // Called from the JIT thread with interleaved stereo samples: LRLR...
    void QueueSamples(const int16_t* interleaved, size_t frames);

 private:
    static constexpr size_t kQueueFrames = 131072; // SPSC ring capacity (must stay power-of-two)
    static constexpr size_t kQueueMask = kQueueFrames - 1;
    static_assert((kQueueFrames & kQueueMask) == 0, "kQueueFrames must be power-of-two");
    static constexpr uint32_t kOutputRate = 48000; // Host output rate; must match bus resampler
    static constexpr size_t kChunkFrames = 512;

    size_t PopFrames(int16_t* interleaved, size_t frames);
    size_t QueueDepth(size_t head, size_t tail) const;
    void ThreadMain();

    std::array<int16_t, kQueueFrames * 2> queue_{};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::atomic<uint64_t> underrun_frames_{0};
    std::atomic<uint64_t> dropped_frames_{0};
};
