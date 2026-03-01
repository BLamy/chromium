// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/version_info/nix/version_extra_utils.h"

#include <string>
#include <string_view>

#include "base/containers/fixed_flat_map.h"
#include "base/environment.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "build/branding_buildflags.h"

namespace {

constexpr std::string_view kDesktopSuffix = ".desktop";
std::optional<std::string> GetChromeDesktopBaseName(base::Environment& env) {
  auto desktop = env.GetVar("CHROME_DESKTOP");
  if (!desktop.has_value() || desktop->empty()) {
    return std::nullopt;
  }
  std::string_view name = *desktop;
  if (name.ends_with(kDesktopSuffix)) {
    name.remove_suffix(kDesktopSuffix.size());
  }
  if (name.empty()) {
    return std::nullopt;
  }
  return std::string(name);
}

}  // namespace

namespace version_info::nix {

version_info::Channel GetChannel(base::Environment& env) {
  std::string env_str = env.GetVar(kChromeVersionExtra).value_or(std::string());

  static constexpr auto kEnvToChannel =
      base::MakeFixedFlatMap<std::string_view, version_info::Channel>(
          {{"beta", version_info::Channel::BETA},
           {"canary", version_info::Channel::CANARY},
           {"extended", version_info::Channel::STABLE},
           {"stable", version_info::Channel::STABLE},
           {"unstable", version_info::Channel::DEV}});
  if (auto it = kEnvToChannel.find(env_str); it != kEnvToChannel.end()) {
    return it->second;
  }

  return version_info::Channel::UNKNOWN;
}

bool IsExtendedStable(base::Environment& env) {
  return env.GetVar(kChromeVersionExtra) == "extended";
}

std::string GetAppName(base::Environment& env) {
  if (auto name = GetChromeDesktopBaseName(env)) {
    return *name;
  }

#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
  static constexpr std::string_view kAppName = "com.google.Chrome";
#else
  static constexpr std::string_view kAppName = "org.chromium.Chromium";
#endif

  std::string_view suffix;
  switch (GetChannel(env)) {
    case version_info::Channel::BETA:
      suffix = ".beta";
      break;
    case version_info::Channel::DEV:
      suffix = ".unstable";
      break;
    case version_info::Channel::CANARY:
      suffix = ".canary";
      break;
    case version_info::Channel::UNKNOWN:
    case version_info::Channel::STABLE:
      break;
  }
  return base::StrCat({kAppName, suffix});
}

std::string GetSessionNamePrefix(base::Environment& env) {
  if (auto name = GetChromeDesktopBaseName(env)) {
    // DBus object paths have stricter requirements than names.
    for (char& c : *name) {
      if (!base::IsAsciiAlphaNumeric(c) && c != '_') {
        c = '_';
      }
    }
    return base::ToLowerASCII(*name);
  }

#if BUILDFLAG(GOOGLE_CHROME_BRANDING)
  static constexpr std::string_view kSessionNamePrefix = "chrome";
#else
  static constexpr std::string_view kSessionNamePrefix = "chromium";
#endif

  std::string_view suffix;
  switch (GetChannel(env)) {
    case version_info::Channel::BETA:
      suffix = "_beta";
      break;
    case version_info::Channel::DEV:
      suffix = "_unstable";
      break;
    case version_info::Channel::CANARY:
      suffix = "_canary";
      break;
    case version_info::Channel::UNKNOWN:
    case version_info::Channel::STABLE:
      break;
  }
  return base::StrCat({kSessionNamePrefix, suffix});
}

}  // namespace version_info::nix
