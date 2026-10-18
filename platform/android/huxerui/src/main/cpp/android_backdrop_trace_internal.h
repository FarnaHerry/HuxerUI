#pragma once

#include <cstdint>

#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
#include <android/log.h>
#include <android/trace.h>
#include <time.h>
#endif

namespace huxerui::detail {

class AndroidBackdropTrace final {
public:
  explicit AndroidBackdropTrace(const char* name, std::uint64_t frame = 0) {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    enabled_ = ATrace_isEnabled();
    frame_ = frame;
    if (enabled_) {
      if (frame_ != 0) {
        start_ns_ = ClockNs(CLOCK_MONOTONIC);
        thread_ns_ = ClockNs(CLOCK_THREAD_CPUTIME_ID);
      }
      ATrace_beginSection(name);
    }
#else
    static_cast<void>(name);
    static_cast<void>(frame);
#endif
  }
  ~AndroidBackdropTrace() {
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
    if (enabled_) {
      ATrace_endSection();
      if (frame_ != 0) {
        __android_log_print(ANDROID_LOG_INFO, "HuxerUI",
                            "Backdrop frame frame=%llu begin_ns=%llu end_ns=%llu thread_ns=%llu",
                            static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(start_ns_),
                            static_cast<unsigned long long>(ClockNs(CLOCK_MONOTONIC)),
                            static_cast<unsigned long long>(ClockNs(CLOCK_THREAD_CPUTIME_ID) - thread_ns_));
      }
    }
#endif
  }

private:
#if defined(HUXERUI_ENABLE_PROFILING) && HUXERUI_ENABLE_PROFILING
  static std::uint64_t ClockNs(clockid_t clock) {
    timespec value{};
    clock_gettime(clock, &value);
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
  }
  bool enabled_ = false;
  std::uint64_t frame_ = 0;
  std::uint64_t start_ns_ = 0;
  std::uint64_t thread_ns_ = 0;
#endif
};

} // namespace huxerui::detail
