// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_APP_ELECTRON_MAIN_DELEGATE_H_
#define ELECTRON_SHELL_APP_ELECTRON_MAIN_DELEGATE_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "components/memory_system/memory_system.h"
#include "content/public/app/content_main_delegate.h"

namespace tracing {
class TracingSamplerProfiler;
}

namespace electron {

// Whether locale resources loaded normally, fell back to the default locale, or
// could not be loaded at all. A damaged installation can leave a locale pak
// missing or truncated on disk. Rather than terminating the process before the
// JS environment exists — which would leave the app unable to update or repair
// itself — LoadResourceBundle() degrades and records the outcome here.
enum class LocaleResourceStatus {
  // LoadResourceBundle() has not run yet in this process. Electron loads and
  // joins the app's JS in PostEarlyInitialization(), which precedes the
  // PreCreateThreads() call that reads the locale pak — so a caller at module
  // scope sees this rather than a falsely healthy kLoaded. Treat it as "asked
  // too early", not as a diagnosis.
  kUnknown,
  kLoaded,
  kFallback,
  kUnavailable,
};

struct LocaleResourceState {
  LocaleResourceStatus status = LocaleResourceStatus::kUnknown;
  // The locale whose pak could not be loaded, identifying the damaged file.
  // This is the *resolved* locale. Empty unless `status` is kFallback or
  // kUnavailable.
  std::string failed_locale;
};

// Reports kUnknown until LoadResourceBundle() has run in this process.
const LocaleResourceState& GetLocaleResourceState();

std::string LoadResourceBundle(const std::string& locale);

class ElectronMainDelegate : public content::ContentMainDelegate {
 public:
  static base::span<const char* const> GetNonWildcardDomainNonPortSchemes();
  ElectronMainDelegate();
  ~ElectronMainDelegate() override;

  // disable copy
  ElectronMainDelegate(const ElectronMainDelegate&) = delete;
  ElectronMainDelegate& operator=(const ElectronMainDelegate&) = delete;

#if BUILDFLAG(IS_MAC)
  void OverrideChildProcessPath();
  void OverrideFrameworkBundlePath();
  void SetUpBundleOverrides();
#endif

 protected:
  // content::ContentMainDelegate:
  std::string_view GetBrowserV8SnapshotFilename() override;
  std::optional<int> BasicStartupComplete() override;
  void PreSandboxStartup() override;
  void SandboxInitialized(const std::string& process_type) override;
  std::optional<int> PreBrowserMain() override;
  std::optional<int> PostEarlyInitialization(InvokedIn invoked_in) override;
  content::ContentClient* CreateContentClient() override;
  content::ContentBrowserClient* CreateContentBrowserClient() override;
  content::ContentGpuClient* CreateContentGpuClient() override;
  content::ContentRendererClient* CreateContentRendererClient() override;
  content::ContentUtilityClient* CreateContentUtilityClient() override;
  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override;
  bool ShouldCreateFeatureList(InvokedIn invoked_in) override;
  bool ShouldInitializeMojo(InvokedIn invoked_in) override;
  bool ShouldLoadV8Snapshot(const std::string& process_type) override;
  bool ShouldLockSchemeRegistry() override;
#if BUILDFLAG(IS_LINUX)
  void ZygoteForked() override;
#endif

  void InitializeMemorySystem();

 private:
  std::unique_ptr<content::ContentBrowserClient> browser_client_;
  std::unique_ptr<content::ContentClient> content_client_;
  std::unique_ptr<content::ContentGpuClient> gpu_client_;
  std::unique_ptr<content::ContentRendererClient> renderer_client_;
  std::unique_ptr<content::ContentUtilityClient> utility_client_;
  std::unique_ptr<tracing::TracingSamplerProfiler> tracing_sampler_profiler_;

  memory_system::MemorySystem memory_system_;
};

}  // namespace electron

#endif  // ELECTRON_SHELL_APP_ELECTRON_MAIN_DELEGATE_H_
