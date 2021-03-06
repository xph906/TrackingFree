// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SYNC_FILE_SYSTEM_DRIVE_BACKEND_REMOTE_CHANGE_PROCESSOR_WRAPPER_H_
#define CHROME_BROWSER_SYNC_FILE_SYSTEM_DRIVE_BACKEND_REMOTE_CHANGE_PROCESSOR_WRAPPER_H_

#include "base/memory/weak_ptr.h"
#include "chrome/browser/sync_file_system/remote_change_processor.h"

namespace sync_file_system {
namespace drive_backend {

// This class wraps a part of RemoteChangeProcessor class to support weak
// pointer.  Each method wraps corresponding name method of
// RemoteChangeProcessor.  See comments in remote_change_processor.h
// for details.
class RemoteChangeProcessorWrapper
    : public base::SupportsWeakPtr<RemoteChangeProcessorWrapper> {
 public:
  explicit RemoteChangeProcessorWrapper(
      RemoteChangeProcessor* remote_change_processor);

  void PrepareForProcessRemoteChange(
      const fileapi::FileSystemURL& url,
      const RemoteChangeProcessor::PrepareChangeCallback& callback);

  void ApplyRemoteChange(
      const FileChange& change,
      const base::FilePath& local_path,
      const fileapi::FileSystemURL& url,
      const SyncStatusCallback& callback);

  void FinalizeRemoteSync(
      const fileapi::FileSystemURL& url,
      bool clear_local_changes,
      const base::Closure& completion_callback);

  void RecordFakeLocalChange(
      const fileapi::FileSystemURL& url,
      const FileChange& change,
      const SyncStatusCallback& callback);

 private:
  RemoteChangeProcessor* remote_change_processor_;

  DISALLOW_COPY_AND_ASSIGN(RemoteChangeProcessorWrapper);
};

}  // namespace drive_backend
}  // namespace sync_file_system

#endif  // CHROME_BROWSER_SYNC_FILE_SYSTEM_DRIVE_BACKEND_REMOTE_CHANGE_PROCESSOR_WRAPPER_H_
