#pragma once

#include <cstdint>

class DebugProfiler {
 public:
    class Scope {
     public:
        explicit Scope(uint64_t scope_id);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

     private:
        uint64_t scope_id_{0};
        uint64_t start_ns_{0};
        bool enabled_{false};
    };

    static DebugProfiler& Instance();

    bool Enabled() const;
    uint64_t RegisterScope(const char* scope_name);
    void AddSample(uint64_t scope_id, uint64_t elapsed_ns);
    void MaybeLogPeriodic();
    void LogSummary(const char* reason);

 private:
    DebugProfiler();
    ~DebugProfiler() = default;

    DebugProfiler(const DebugProfiler&) = delete;
    DebugProfiler& operator=(const DebugProfiler&) = delete;
};

#define AXOLOTL_PROFILE_SCOPE_JOIN2(a, b) a##b
#define AXOLOTL_PROFILE_SCOPE_JOIN(a, b) AXOLOTL_PROFILE_SCOPE_JOIN2(a, b)
#define AXOLOTL_PROFILE_SCOPE(name_literal) \
    static const uint64_t AXOLOTL_PROFILE_SCOPE_JOIN(axolotl_profile_scope_id_, __LINE__) = \
        ::DebugProfiler::Instance().RegisterScope(name_literal); \
    ::DebugProfiler::Scope AXOLOTL_PROFILE_SCOPE_JOIN(axolotl_profile_scope_, __LINE__)( \
        AXOLOTL_PROFILE_SCOPE_JOIN(axolotl_profile_scope_id_, __LINE__))
#define AXOLOTL_PROFILE_DUMP(reason_literal) ::DebugProfiler::Instance().LogSummary(reason_literal)
