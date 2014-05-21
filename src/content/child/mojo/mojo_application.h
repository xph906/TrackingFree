// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_CHILD_MOJO_MOJO_APPLICATION_H_
#define CONTENT_CHILD_MOJO_MOJO_APPLICATION_H_

#include "ipc/ipc_platform_file.h"
#include "mojo/common/channel_init.h"
#include "mojo/public/interfaces/shell/shell.mojom.h"

namespace IPC {
class Message;
}

namespace content {

// MojoApplication represents the code needed to setup a child process as a
// Mojo application via Chrome IPC. Instantiate MojoApplication and call its
// OnMessageReceived method to give it a shot at handling Chrome IPC messages.
// It makes the mojo::Shell interface available and calls methods on the given
// mojo::ShellClient interface as calls come in.
class MojoApplication {
 public:
  // The ShellClient pointer must remain valid for the lifetime of the
  // MojoApplication instance.
  explicit MojoApplication(mojo::ShellClient* shell_client);
  ~MojoApplication();

  bool OnMessageReceived(const IPC::Message& msg);

  mojo::Shell* shell() { return shell_.get(); }

 private:
  void OnActivate(const IPC::PlatformFileForTransit& file);

  mojo::common::ChannelInit channel_init_;
  mojo::ShellPtr shell_;
  mojo::ShellClient* shell_client_;

  DISALLOW_COPY_AND_ASSIGN(MojoApplication);
};

}  // namespace content

#endif  // CONTENT_CHILD_MOJO_MOJO_APPLICATION_H_
