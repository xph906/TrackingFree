// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SYNC_FILE_SYSTEM_DRIVE_BACKEND_SYNC_WORKER_H_
#define CHROME_BROWSER_SYNC_FILE_SYSTEM_DRIVE_BACKEND_SYNC_WORKER_H_

#include <set>
#include <string>

#include "base/memory/scoped_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "chrome/browser/drive/drive_notification_observer.h"
#include "chrome/browser/drive/drive_service_interface.h"
#include "chrome/browser/sync_file_system/drive_backend/sync_task_manager.h"
#include "chrome/browser/sync_file_system/local_change_processor.h"
#include "chrome/browser/sync_file_system/remote_file_sync_service.h"
#include "chrome/browser/sync_file_system/sync_action.h"
#include "chrome/browser/sync_file_system/sync_direction.h"
#include "net/base/network_change_notifier.h"

class ExtensionServiceInterface;

namespace base {
class SequencedTaskRunner;
}

namespace drive {
class DriveServiceInterface;
class DriveNotificationManager;
class DriveUploaderInterface;
}

namespace leveldb {
class Env;
}

namespace sync_file_system {

class RemoteChangeProcessor;

namespace drive_backend {

class LocalToRemoteSyncer;
class MetadataDatabase;
class RemoteChangeProcessorOnWorker;
class RemoteToLocalSyncer;
class SyncEngineContext;
class SyncEngineInitializer;

class SyncWorker : public SyncTaskManager::Client {
 public:
  enum AppStatus {
    APP_STATUS_ENABLED,
    APP_STATUS_DISABLED,
    APP_STATUS_UNINSTALLED,
  };

  typedef base::hash_map<std::string, AppStatus> AppStatusMap;

  class Observer {
   public:
    virtual void OnPendingFileListUpdated(int item_count) = 0;
    virtual void OnFileStatusChanged(const fileapi::FileSystemURL& url,
                                     SyncFileStatus file_status,
                                     SyncAction sync_action,
                                     SyncDirection direction) = 0;
    virtual void UpdateServiceState(RemoteServiceState state,
                                    const std::string& description) = 0;

   protected:
    virtual ~Observer() {}
  };

  static scoped_ptr<SyncWorker> CreateOnWorker(
      const base::FilePath& base_dir,
      Observer* observer,
      const base::WeakPtr<ExtensionServiceInterface>& extension_service,
      scoped_ptr<SyncEngineContext> sync_engine_context,
      leveldb::Env* env_override);

  virtual ~SyncWorker();

  void Initialize();

  // SyncTaskManager::Client overrides
  virtual void MaybeScheduleNextTask() OVERRIDE;
  virtual void NotifyLastOperationStatus(
      SyncStatusCode sync_status, bool used_network) OVERRIDE;

  void RegisterOrigin(const GURL& origin, const SyncStatusCallback& callback);
  void EnableOrigin(const GURL& origin, const SyncStatusCallback& callback);
  void DisableOrigin(const GURL& origin, const SyncStatusCallback& callback);
  void UninstallOrigin(
      const GURL& origin,
      RemoteFileSyncService::UninstallFlag flag,
      const SyncStatusCallback& callback);
  void ProcessRemoteChange(const SyncFileCallback& callback);
  void SetRemoteChangeProcessor(
      RemoteChangeProcessorOnWorker* remote_change_processor_on_worker);
  RemoteServiceState GetCurrentState() const;
  void GetOriginStatusMap(RemoteFileSyncService::OriginStatusMap* status_map);
  scoped_ptr<base::ListValue> DumpFiles(const GURL& origin);
  scoped_ptr<base::ListValue> DumpDatabase();
  void SetSyncEnabled(bool enabled);
  SyncStatusCode SetDefaultConflictResolutionPolicy(
      ConflictResolutionPolicy policy);
  SyncStatusCode SetConflictResolutionPolicy(
      const GURL& origin,
      ConflictResolutionPolicy policy);
  ConflictResolutionPolicy GetDefaultConflictResolutionPolicy()
      const;
  ConflictResolutionPolicy GetConflictResolutionPolicy(
      const GURL& origin) const;

  void ApplyLocalChange(
      const FileChange& local_change,
      const base::FilePath& local_path,
      const SyncFileMetadata& local_metadata,
      const fileapi::FileSystemURL& url,
      const SyncStatusCallback& callback);

  void OnNotificationReceived();

  void OnReadyToSendRequests(const std::string& account_id);
  void OnRefreshTokenInvalid();

  void OnNetworkChanged(net::NetworkChangeNotifier::ConnectionType type);

  drive::DriveServiceInterface* GetDriveService();
  drive::DriveUploaderInterface* GetDriveUploader();
  MetadataDatabase* GetMetadataDatabase();
  SyncTaskManager* GetSyncTaskManager();

  void AddObserver(Observer* observer);

 private:
  friend class DriveBackendSyncTest;
  friend class SyncEngineTest;

  SyncWorker(const base::FilePath& base_dir,
             const base::WeakPtr<ExtensionServiceInterface>& extension_service,
             scoped_ptr<SyncEngineContext> sync_engine_context,
             leveldb::Env* env_override);

  void DoDisableApp(const std::string& app_id,
                    const SyncStatusCallback& callback);
  void DoEnableApp(const std::string& app_id,
                   const SyncStatusCallback& callback);

  void PostInitializeTask();
  void DidInitialize(SyncEngineInitializer* initializer,
                     SyncStatusCode status);
  void UpdateRegisteredApp();
  void DidQueryAppStatus(const AppStatusMap* app_status);
  void DidProcessRemoteChange(RemoteToLocalSyncer* syncer,
                              const SyncFileCallback& callback,
                              SyncStatusCode status);
  void DidApplyLocalChange(LocalToRemoteSyncer* syncer,
                           const SyncStatusCallback& callback,
                           SyncStatusCode status);

  void MaybeStartFetchChanges();
  void DidResolveConflict(SyncStatusCode status);
  void DidFetchChanges(SyncStatusCode status);

  void UpdateServiceStateFromSyncStatusCode(SyncStatusCode state,
                                            bool used_network);
  void UpdateServiceState(RemoteServiceState state,
                          const std::string& description);
  void UpdateRegisteredApps();

  base::FilePath base_dir_;

  leveldb::Env* env_override_;

  // Sync with SyncEngine.
  RemoteServiceState service_state_;

  bool should_check_conflict_;
  bool should_check_remote_change_;
  bool listing_remote_changes_;
  base::TimeTicks time_to_check_changes_;

  bool sync_enabled_;
  ConflictResolutionPolicy default_conflict_resolution_policy_;
  bool network_available_;

  scoped_ptr<SyncTaskManager> task_manager_;

  base::WeakPtr<ExtensionServiceInterface> extension_service_;

  scoped_ptr<SyncEngineContext> context_;
  ObserverList<Observer> observers_;

  base::WeakPtrFactory<SyncWorker> weak_ptr_factory_;
  DISALLOW_COPY_AND_ASSIGN(SyncWorker);
};

}  // namespace drive_backend
}  // namespace sync_file_system

#endif  // CHROME_BROWSER_SYNC_FILE_SYSTEM_DRIVE_BACKEND_SYNC_WORKER_H_
