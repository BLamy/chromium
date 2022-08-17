// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/service_process_host.h"

#include "base/strings/utf_string_conversions.h"
#include "content/public/common/content_client.h"
#include "media/media_buildflags.h"

namespace content {

ServiceProcessHost::Options::Options() = default;

ServiceProcessHost::Options::~Options() = default;

ServiceProcessHost::Options::Options(Options&&) = default;

ServiceProcessHost::Options& ServiceProcessHost::Options::WithDisplayName(
    const std::string& name) {
  display_name = base::UTF8ToUTF16(name);
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithDisplayName(
    const std::u16string& name) {
  display_name = name;
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithDisplayName(
    int resource_id) {
  display_name = GetContentClient()->GetLocalizedString(resource_id);
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithSite(
    const GURL& url) {
  site = url;
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithChildFlags(
    int flags) {
  child_flags = flags;
  return *this;
}

ServiceProcessHost::Options&
ServiceProcessHost::Options::WithExtraCommandLineSwitches(
    std::vector<std::string> switches) {
  extra_switches = std::move(switches);
  return *this;
}

ServiceProcessHost::Options&
ServiceProcessHost::Options::WithExtraCommandLineSwitchKeyValues(
    std::vector<std::pair<std::string, std::string>> switch_key_values) {
  extra_switch_key_values = std::move(switch_key_values);
  return *this;
}

#if BUILDFLAG(IS_WIN)
ServiceProcessHost::Options& ServiceProcessHost::Options::WithStdoutHandle(
    base::win::ScopedHandle handle) {
  stdout_handle = std::move(handle);
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithStderrHandle(
    base::win::ScopedHandle handle) {
  stderr_handle = std::move(handle);
  return *this;
}
#elif BUILDFLAG(IS_POSIX)
ServiceProcessHost::Options& ServiceProcessHost::Options::WithAdditionalFds(
    base::FileHandleMappingVector mapping) {
  fds_to_remap = std::move(mapping);
  return *this;
}
#endif

ServiceProcessHost::Options& ServiceProcessHost::Options::WithProcessCallback(
    base::OnceCallback<void(const base::Process&)> callback) {
  process_callback = std::move(callback);
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithObserver(
    base::WeakPtr<Observer> obs) {
  CHECK(!observer) << "Only one per-instance observer may be registered. "
                      "Use a service-specific manager to fan out to multiple "
                      "observers.";
  observer = std::move(obs);
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithCurrentDirectory(
    const base::FilePath& cwd) {
  current_directory = cwd;
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithEnvironment(
    const base::EnvironmentMap& env, bool new_environment) {
  environment = env;
  clear_environment = new_environment;
  return *this;
}

#if BUILDFLAG(IS_WIN)
ServiceProcessHost::Options& ServiceProcessHost::Options::WithFeedbackCursorOff(
    bool turn_feedback_cursor_off) {
  feedback_cursor_off = turn_feedback_cursor_off;
  return *this;
}
#endif  // #if BUILDFLAG(IS_WIN)

#if BUILDFLAG(IS_MAC)
ServiceProcessHost::Options&
ServiceProcessHost::Options::WithDisclaimResponsibility(
    bool should_disclaim_responsibility) {
  disclaim_responsibility = should_disclaim_responsibility;
  return *this;
}
#endif  // BUILDFLAG(IS_MAC)

#if BUILDFLAG(IS_WIN)
ServiceProcessHost::Options&
ServiceProcessHost::Options::WithPreloadedLibraries(
    std::vector<base::FilePath> preloads,
    base::PassKey<ServiceProcessHostPreloadLibraries> passkey) {
  preload_libraries = std::move(preloads);
  return *this;
}
#endif  // #if BUILDFLAG(IS_WIN)

ServiceProcessHost::Options& ServiceProcessHost::Options::WithGpuClient(
    base::PassKey<ServiceProcessHostGpuClient> passkey) {
#if BUILDFLAG(ENABLE_GPU_CHANNEL_MEDIA_CAPTURE)
  allow_gpu_client = true;
#endif  // BUILDFLAG(ENABLE_GPU_CHANNEL_MEDIA_CAPTURE)
  return *this;
}

ServiceProcessHost::Options& ServiceProcessHost::Options::WithPriority(
    base::Process::Priority pri) {
  priority = pri;
  return *this;
}

ServiceProcessHost::Options ServiceProcessHost::Options::Pass() {
  return std::move(*this);
}

}  // namespace content
