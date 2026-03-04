#include "system/audio_system.h"

#include "util/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>

#if defined(__linux__) && defined(AXOLOTL_HAS_ALSA)
#include <alsa/asoundlib.h>
#endif

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() {
    Stop();
}

bool AudioSystem::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }

    underrun_frames_.store(0, std::memory_order_relaxed);
    dropped_frames_.store(0, std::memory_order_relaxed);
    thread_ = std::thread([this]() { ThreadMain(); });
    Logger::log("[Audio] started", LogLevel::INFO);
    return true;
}

void AudioSystem::Stop() {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) return;

    if (thread_.joinable()) thread_.join();

    const uint64_t underruns = underrun_frames_.load(std::memory_order_relaxed);
    const uint64_t dropped = dropped_frames_.load(std::memory_order_relaxed);
    Logger::log("[Audio] stopped (underrun_frames=" + std::to_string(underruns) +
                    ", dropped_frames=" + std::to_string(dropped) + ")",
                LogLevel::INFO);
}

void AudioSystem::QueueSamples(const int16_t* interleaved, size_t frames) {
    if (!running_.load(std::memory_order_relaxed) || !interleaved || frames == 0) return;

    const size_t head = head_.load(std::memory_order_acquire);
    size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t used = (tail - head) & kQueueMask;
    const size_t free_slots = kQueueMask - used;
    const size_t write_frames = (frames < free_slots) ? frames : free_slots;
    if (write_frames > 0) {
        const size_t first = std::min(write_frames, kQueueFrames - tail);
        std::memcpy(&queue_[tail * 2], interleaved, first * 2 * sizeof(int16_t));
        if (first < write_frames) {
            const size_t remain = write_frames - first;
            std::memcpy(&queue_[0], interleaved + first * 2, remain * 2 * sizeof(int16_t));
        }
        tail = (tail + write_frames) & kQueueMask;
        tail_.store(tail, std::memory_order_release);
    }

    if (write_frames < frames) {
        dropped_frames_.fetch_add(frames - write_frames, std::memory_order_relaxed);
    }
}

size_t AudioSystem::PopFrames(int16_t* interleaved, size_t frames) {
    size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    const size_t available = (tail - head) & kQueueMask;
    const size_t read_frames = (frames < available) ? frames : available;
    if (read_frames == 0) return 0;

    const size_t first = std::min(read_frames, kQueueFrames - head);
    std::memcpy(interleaved, &queue_[head * 2], first * 2 * sizeof(int16_t));
    if (first < read_frames) {
        const size_t remain = read_frames - first;
        std::memcpy(interleaved + first * 2, &queue_[0], remain * 2 * sizeof(int16_t));
    }
    head = (head + read_frames) & kQueueMask;
    head_.store(head, std::memory_order_release);
    return read_frames;
}

size_t AudioSystem::QueueDepth(size_t head, size_t tail) const {
    return (tail - head) & kQueueMask;
}

void AudioSystem::ThreadMain() {
#if defined(__linux__) && defined(AXOLOTL_HAS_ALSA)
    snd_pcm_t* pcm = nullptr;
    const char* device = std::getenv("AXOLOTL_ALSA_DEVICE");
    if (!device || device[0] == '\0') device = "default";
    int rc = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        Logger::log(std::string("[Audio] ALSA open failed: ") + snd_strerror(rc), LogLevel::WARNING);
        while (running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return;
    }
    Logger::log(std::string("[Audio] ALSA device: ") + device, LogLevel::INFO);

    rc = snd_pcm_set_params(pcm,
                            SND_PCM_FORMAT_S16_LE,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            2,
                            kOutputRate,
                            1,
                            40000);
    if (rc < 0) {
        Logger::log(std::string("[Audio] ALSA set_params failed: ") + snd_strerror(rc), LogLevel::WARNING);
        snd_pcm_close(pcm);
        while (running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return;
    }

    std::array<int16_t, kChunkFrames * 2> interleaved{};
    int16_t last_left = 0;
    int16_t last_right = 0;
    bool primed = false;
    constexpr size_t kPrimeFrames = 4096;
    while (running_.load(std::memory_order_relaxed)) {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t depth = QueueDepth(head, tail);
        if (!primed) {
            if (depth < kPrimeFrames) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            primed = true;
        }

        size_t wrote = PopFrames(interleaved.data(), kChunkFrames);
        if (wrote < kChunkFrames) {
            underrun_frames_.fetch_add(kChunkFrames - wrote, std::memory_order_relaxed);
            for (size_t i = wrote; i < kChunkFrames; ++i) {
                interleaved[i * 2] = last_left;
                interleaved[i * 2 + 1] = last_right;
            }
            wrote = kChunkFrames;
        } else {
            last_left = interleaved[(kChunkFrames - 1) * 2];
            last_right = interleaved[(kChunkFrames - 1) * 2 + 1];
        }

        snd_pcm_sframes_t remaining = static_cast<snd_pcm_sframes_t>(wrote);
        const int16_t* ptr = interleaved.data();
        while (remaining > 0 && running_.load(std::memory_order_relaxed)) {
            snd_pcm_sframes_t written = snd_pcm_writei(pcm, ptr, remaining);
            if (written < 0) {
                written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
                if (written < 0) {
                    snd_pcm_prepare(pcm);
                    break;
                }
                continue;
            }
            if (written == 0) break;
            ptr += static_cast<size_t>(written) * 2;
            remaining -= written;
        }
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
#else
    // Fallback: keep draining so producer backpressure behavior is still exercised.
    while (running_.load(std::memory_order_relaxed)) {
        int16_t sink[kChunkFrames * 2];
        (void)PopFrames(sink, kChunkFrames);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}
