// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/apple/bundle_locations.h"

#include "base/apple/foundation_util.h"
#include "base/check.h"
#include "base/record_replay.h"
#include "base/strings/sys_string_conversions.h"

namespace base::apple {

namespace {

NSBundle* g_override_main_bundle = nil;
NSBundle* g_override_framework_bundle = nil;
NSBundle* g_override_outer_bundle = nil;

}  // namespace

// A bundle's path, the same on a replay as when recording. A replay's main
// executable is the replay engine, not this program, so a bundle found from it
// has another path there, and the system's string functions, whose calls are in
// the recording, would be given another string than the recording gave them.
// When recording, the bundle is asked with events passed through, and its
// answer is recorded.
FilePath RecordReplayBundlePath(const char* why, NSBundle* bundle) {
  if (!recordreplay::IsRecordingOrReplaying() ||
      recordreplay::AreEventsDisallowed() ||
      recordreplay::AreEventsPassedThrough()) {
    return apple::NSStringToFilePath(bundle.bundlePath);
  }
  // A replay doesn't ask the bundle at all: the engine can't pass an
  // Objective-C message it doesn't know through, and the answer would be
  // replaced anyway.
  std::string path;
  if (!recordreplay::IsReplaying()) {
    recordreplay::AutoPassThroughEvents pass_through;
    path = apple::NSStringToFilePath(bundle.bundlePath).value();
  }
  recordreplay::RecordReplayString(why, path);
  return FilePath(path);
}

NSBundle* MainBundle() {
  if (g_override_main_bundle) {
    return g_override_main_bundle;
  }
  return NSBundle.mainBundle;
}

NSURL* MainBundleURL() {
  return MainBundle().bundleURL;
}

FilePath MainBundlePath() {
  return RecordReplayBundlePath("MainBundlePath", MainBundle());
}

std::string MainBundleIdentifier() {
  return base::SysNSStringToUTF8(MainBundle().bundleIdentifier);
}

NSBundle* OuterBundle() {
  if (g_override_outer_bundle) {
    return g_override_outer_bundle;
  }
  return NSBundle.mainBundle;
}

NSURL* OuterBundleURL() {
  return OuterBundle().bundleURL;
}

FilePath OuterBundlePath() {
  return RecordReplayBundlePath("OuterBundlePath", OuterBundle());
}

NSBundle* FrameworkBundle() {
  if (g_override_framework_bundle) {
    return g_override_framework_bundle;
  }
  return NSBundle.mainBundle;
}

FilePath FrameworkBundlePath() {
  return RecordReplayBundlePath("FrameworkBundlePath", FrameworkBundle());
}

namespace {

NSBundle* BundleFromPath(const FilePath& file_path) {
  if (file_path.empty()) {
    return nil;
  }

  NSBundle* bundle = [NSBundle bundleWithURL:apple::FilePathToNSURL(file_path)];
  CHECK(bundle) << "Failed to load the bundle at " << file_path.value();

  return bundle;
}

}  // namespace

void SetOverrideMainBundle(NSBundle* bundle) {
  g_override_main_bundle = bundle;
}

void SetOverrideOuterBundle(NSBundle* bundle) {
  g_override_outer_bundle = bundle;
}

void SetOverrideFrameworkBundle(NSBundle* bundle) {
  g_override_framework_bundle = bundle;
}

void SetOverrideMainBundlePath(const FilePath& file_path) {
  NSBundle* bundle = BundleFromPath(file_path);
  g_override_main_bundle = bundle;
}

void SetOverrideOuterBundlePath(const FilePath& file_path) {
  NSBundle* bundle = BundleFromPath(file_path);
  g_override_outer_bundle = bundle;
}

void SetOverrideFrameworkBundlePath(const FilePath& file_path) {
  NSBundle* bundle = BundleFromPath(file_path);
  g_override_framework_bundle = bundle;
}

}  // namespace base::apple
