#ifndef DISCORD_SCRIPT_COMPILATION_TIMING_H_
#define DISCORD_SCRIPT_COMPILATION_TIMING_H_

#include <string>
#include <vector>

#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace base {
template <typename T>
class NoDestructor;
}  // namespace base

namespace discord {

struct ScriptCompilationEntry {
  std::string url;
  int64_t foreground_time_us;  // V8 main-thread compile finalization (µs)
  int64_t background_time_us;  // V8 background parse + compile (µs)
  bool streamed;               // true if streaming compilation was used
};

// Per-renderer singleton that collects V8 script compilation timing.
// Written by Blink (v8_script_runner) on the main thread after Compile()
// returns, read by the discord renderer module to expose to JS.
class ScriptCompilationTimingCollector {
 public:
  static ScriptCompilationTimingCollector& GetInstance();

  ScriptCompilationTimingCollector(const ScriptCompilationTimingCollector&) =
      delete;
  ScriptCompilationTimingCollector& operator=(
      const ScriptCompilationTimingCollector&) = delete;

  void Record(std::string url,
              int64_t foreground_time_us,
              int64_t background_time_us,
              bool streamed);

  std::vector<ScriptCompilationEntry> GetEntries() const;

 private:
  friend class base::NoDestructor<ScriptCompilationTimingCollector>;

  ScriptCompilationTimingCollector();
  ~ScriptCompilationTimingCollector();

  mutable base::Lock lock_;
  std::vector<ScriptCompilationEntry> entries_ GUARDED_BY(lock_);
};

}  // namespace discord

#endif  // DISCORD_SCRIPT_COMPILATION_TIMING_H_
