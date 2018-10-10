// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/base/accelerators/media_keys_listener.h"

namespace ui {

MediaKeysListener::Delegate::~Delegate() = default;

MediaKeysListener::~MediaKeysListener() = default;

// static
bool MediaKeysListener::IsMediaKeycode(KeyboardCode key_code) {
  return key_code == VKEY_MEDIA_PLAY_PAUSE || key_code == VKEY_MEDIA_STOP ||
         key_code == VKEY_MEDIA_PREV_TRACK || key_code == VKEY_MEDIA_NEXT_TRACK ||
         key_code == VKEY_VOLUME_UP || key_code == VKEY_VOLUME_DOWN || key_code == VKEY_VOLUME_MUTE;
}

}  // namespace ui
