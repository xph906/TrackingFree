// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SYNC_PROFILE_SYNC_COMPONENTS_FACTORY_IMPL_H__
#define CHROME_BROWSER_SYNC_PROFILE_SYNC_COMPONENTS_FACTORY_IMPL_H__

#include <string>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "chrome/browser/sync/profile_sync_components_factory.h"
#include "components/autofill/core/browser/webdata/autofill_webdata_service.h"

class Profile;

namespace base {
class CommandLine;
}

namespace extensions {
class ExtensionSystem;
}

class ProfileSyncComponentsFactoryImpl : public ProfileSyncComponentsFactory {
 public:
  ProfileSyncComponentsFactoryImpl(Profile* profile,
                                   base::CommandLine* command_line);
  virtual ~ProfileSyncComponentsFactoryImpl();

  virtual void RegisterDataTypes(ProfileSyncService* pss) OVERRIDE;

  virtual browser_sync::DataTypeManager* CreateDataTypeManager(
      const syncer::WeakHandle<syncer::DataTypeDebugInfoListener>&
          debug_info_listener,
      const browser_sync::DataTypeController::TypeMap* controllers,
      const browser_sync::DataTypeEncryptionHandler* encryption_handler,
      browser_sync::SyncBackendHost* backend,
      browser_sync::DataTypeManagerObserver* observer,
      browser_sync::FailedDataTypesHandler* failed_data_types_handler)
          OVERRIDE;

  virtual browser_sync::SyncBackendHost* CreateSyncBackendHost(
      const std::string& name,
      Profile* profile,
      const base::WeakPtr<sync_driver::SyncPrefs>& sync_prefs,
      const base::FilePath& sync_folder) OVERRIDE;

  virtual base::WeakPtr<syncer::SyncableService> GetSyncableServiceForType(
      syncer::ModelType type) OVERRIDE;
  virtual scoped_ptr<syncer::AttachmentService> CreateAttachmentService(
      syncer::AttachmentService::Delegate* delegate) OVERRIDE;

  // Legacy datatypes that need to be converted to the SyncableService API.
  virtual SyncComponents CreateBookmarkSyncComponents(
      ProfileSyncService* profile_sync_service,
      browser_sync::DataTypeErrorHandler* error_handler) OVERRIDE;
  virtual SyncComponents CreateTypedUrlSyncComponents(
      ProfileSyncService* profile_sync_service,
      history::HistoryBackend* history_backend,
      browser_sync::DataTypeErrorHandler* error_handler) OVERRIDE;

 private:
  // Register data types which are enabled on desktop platforms only.
  void RegisterDesktopDataTypes(syncer::ModelTypeSet disabled_types,
                                ProfileSyncService* pss);
  // Register data types which are enabled on both desktop and mobile.
  void RegisterCommonDataTypes(syncer::ModelTypeSet disabled_types,
                               ProfileSyncService* pss);

  Profile* profile_;
  base::CommandLine* command_line_;
  // Set on the UI thread (since extensions::ExtensionSystemFactory is
  // non-threadsafe); accessed on both the UI and FILE threads in
  // GetSyncableServiceForType.
  extensions::ExtensionSystem* extension_system_;
  scoped_refptr<autofill::AutofillWebDataService> web_data_service_;

  DISALLOW_COPY_AND_ASSIGN(ProfileSyncComponentsFactoryImpl);
};

#endif  // CHROME_BROWSER_SYNC_PROFILE_SYNC_COMPONENTS_FACTORY_IMPL_H__
