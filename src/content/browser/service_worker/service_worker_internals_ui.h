// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_INTERNALS_UI_H_
#define CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_INTERNALS_UI_H_

#include <set>

#include "base/containers/scoped_ptr_hash_map.h"
#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/weak_ptr.h"
#include "content/browser/service_worker/service_worker_context_observer.h"
#include "content/common/service_worker/service_worker_status_code.h"
#include "content/public/browser/web_ui_controller.h"

namespace base {
class ListValue;
class DictionaryValue;
}

namespace content {

class StoragePartition;
class ServiceWorkerContextWrapper;
class ServiceWorkerRegistration;
class ServiceWorkerVersion;

class ServiceWorkerInternalsUI
    : public WebUIController,
      public base::SupportsWeakPtr<ServiceWorkerInternalsUI> {
 public:
  explicit ServiceWorkerInternalsUI(WebUI* web_ui);

 private:
  class OperationProxy;
  class PartitionObserver;

  virtual ~ServiceWorkerInternalsUI();
  void AddContextFromStoragePartition(StoragePartition* partition);

  void RemoveObserverFromStoragePartition(StoragePartition* partition);

  // Called from Javascript.
  void GetAllRegistrations(const base::ListValue* args);
  void StartWorker(const base::ListValue* args);
  void StopWorker(const base::ListValue* args);
  void DispatchSyncEventToWorker(const base::ListValue* args);
  void InspectWorker(const base::ListValue* args);
  void Unregister(const base::ListValue* args);

  bool GetRegistrationInfo(
      const base::ListValue* args,
      base::FilePath* partition_path,
      GURL* scope,
      scoped_refptr<ServiceWorkerContextWrapper>* context) const;

  base::ScopedPtrHashMap<uintptr_t, PartitionObserver> observers_;
  int next_partition_id_;
};

}  // namespace content

#endif  // CONTENT_BROWSER_SERVICE_WORKER_SERVICE_WORKER_INTERNALS_UI_H_
