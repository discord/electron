#include "discord/script_compilation_timing.h"

#include <utility>

#include "base/no_destructor.h"

namespace discord {

ScriptCompilationTimingCollector::ScriptCompilationTimingCollector() = default;
ScriptCompilationTimingCollector::~ScriptCompilationTimingCollector() = default;

// static
ScriptCompilationTimingCollector&
ScriptCompilationTimingCollector::GetInstance() {
  static base::NoDestructor<ScriptCompilationTimingCollector> instance;
  return *instance;
}

void ScriptCompilationTimingCollector::Record(std::string url,
                                              int64_t foreground_time_us,
                                              int64_t background_time_us,
                                              bool streamed) {
  base::AutoLock lock(lock_);
  entries_.push_back(
      {std::move(url), foreground_time_us, background_time_us, streamed});
}

std::vector<ScriptCompilationEntry>
ScriptCompilationTimingCollector::GetEntries() const {
  base::AutoLock lock(lock_);
  return entries_;
}

}  // namespace discord
