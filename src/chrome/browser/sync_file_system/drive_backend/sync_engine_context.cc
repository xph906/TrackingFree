// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sync_file_system/drive_backend/sync_engine_context.h"

#include "base/basictypes.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/sequenced_task_runner.h"
#include "base/single_thread_task_runner.h"
#include "chrome/browser/drive/drive_service_interface.h"
#include "chrome/browser/drive/drive_uploader.h"
#include "chrome/browser/sync_file_system/drive_backend/metadata_database.h"
#include "chrome/browser/sync_file_system/remote_change_processor.h"

namespace sync_file_system {
namespace drive_backend {

SyncEngineContext::SyncEngineContext(
    scoped_ptr<drive::DriveServiceInterface> drive_service,
    scoped_ptr<drive::DriveUploaderInterface> drive_uploader,
    base::SingleThreadTaskRunner* ui_task_runner,
    base::SequencedTaskRunner* worker_task_runner,
    base::SequencedTaskRunner* file_task_runner)
    : drive_service_(drive_service.Pass()),
      drive_uploader_(drive_uploader.Pass()),
      remote_change_processor_(NULL),
      ui_task_runner_(ui_task_runner),
      worker_task_runner_(worker_task_runner),
      file_task_runner_(file_task_runner) {}

SyncEngineContext::~SyncEngineContext() {}

drive::DriveServiceInterface* SyncEngineContext::GetDriveService() {
  return drive_service_.get();
}

drive::DriveUploaderInterface* SyncEngineContext::GetDriveUploader() {
  return drive_uploader_.get();
}

MetadataDatabase* SyncEngineContext::GetMetadataDatabase() {
  return metadata_database_.get();
}

scoped_ptr<MetadataDatabase> SyncEngineContext::PassMetadataDatabase() {
  return metadata_database_.Pass();
}

RemoteChangeProcessor* SyncEngineContext::GetRemoteChangeProcessor() {
  return remote_change_processor_;
}

base::SingleThreadTaskRunner* SyncEngineContext::GetUITaskRunner() {
  return ui_task_runner_.get();
}

base::SequencedTaskRunner* SyncEngineContext::GetWorkerTaskRunner() {
  return worker_task_runner_.get();
}

base::SequencedTaskRunner* SyncEngineContext::GetFileTaskRunner() {
  return file_task_runner_.get();
}

void SyncEngineContext::SetMetadataDatabase(
    scoped_ptr<MetadataDatabase> metadata_database) {
  if (metadata_database)
    metadata_database_ = metadata_database.Pass();
}

void SyncEngineContext::SetRemoteChangeProcessor(
    RemoteChangeProcessor* remote_change_processor) {
  DCHECK(remote_change_processor);
  remote_change_processor_ = remote_change_processor;
}

}  // namespace drive_backend
}  // namespace sync_file_system
