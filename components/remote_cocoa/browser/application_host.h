// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_REMOTE_COCOA_BROWSER_APPLICATION_HOST_H_
#define COMPONENTS_REMOTE_COCOA_BROWSER_APPLICATION_HOST_H_

#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/process/process_handle.h"
#include "components/remote_cocoa/browser/remote_cocoa_browser_export.h"
#include "components/remote_cocoa/common/application.mojom.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "ui/gfx/native_ui_types.h"

namespace remote_cocoa {

// This class is the browser-side component corresponding to the NSApplication
// running in an app shim process. There exists one ApplicationHost per app shim
// process.
class REMOTE_COCOA_BROWSER_EXPORT ApplicationHost {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnApplicationHostDestroying(ApplicationHost* host) = 0;

   protected:
    ~Observer() override = default;
  };

  explicit ApplicationHost(
      mojo::PendingAssociatedReceiver<mojom::Application>* receiver,
      const std::string& bundle_id);
  ~ApplicationHost();

  mojom::Application* GetApplication();

  void AddObserver(Observer* observer);
  void RemoveObserver(const Observer* observer);

  static ApplicationHost* GetForNativeView(gfx::NativeView view);

  const std::string& bundle_id() const { return bundle_id_; }

  // The process that hosts the application, when the embedder knows it.
  // Activation is then yielded to that process, and not to whichever running
  // app declares |bundle_id|.
  void set_pid(base::ProcessId pid) { pid_ = pid; }
  base::ProcessId pid() const { return pid_; }

 private:
  mojo::AssociatedRemote<mojom::Application> application_remote_;
  const std::string bundle_id_;
  base::ProcessId pid_ = base::kNullProcessId;
  base::ObserverList<Observer> observers_;
};

}  // namespace remote_cocoa

#endif  // COMPONENTS_REMOTE_COCOA_BROWSER_APPLICATION_HOST_H_
