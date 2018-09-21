// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/theme_helper_mac.h"

#include <Cocoa/Cocoa.h>

#include "base/record_replay.h"
#include "base/strings/sys_string_conversions.h"
#include "electron/mas.h"

#if !IS_MAS_BUILD()
extern "C" {
bool CGFontRenderingGetFontSmoothingDisabled(void);
}
#endif
namespace content {

bool IsSubpixelAntialiasingAvailable() {
#if !IS_MAS_BUILD()
  // See https://trac.webkit.org/changeset/239306/webkit for more info.
  return !CGFontRenderingGetFontSmoothingDisabled();
#else
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  NSString *default_key = @"CGFontRenderingGetFontSmoothingDisabled";
  // Check that key exists since boolForKey defaults to NO when the
  // key is missing and this key in fact defaults to YES;
  if ([defaults objectForKey:default_key] == nil)
    return false;
  return ![defaults boolForKey:default_key];
#endif
}

}  // namespace content
