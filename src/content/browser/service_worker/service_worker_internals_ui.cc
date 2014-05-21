// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_internals_ui.h"

#include <string>
#include <vector>

#include "base/bind.h"
#include "base/memory/scoped_vector.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "content/browser/devtools/devtools_manager_impl.h"
#include "content/browser/devtools/embedded_worker_devtools_manager.h"
#include "content/browser/service_worker/service_worker_context_observer.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "grit/content_resources.h"

using base::DictionaryValue;
using base::FundamentalValue;
using base::ListValue;
using base::StringValue;
using base::Value;
using base::WeakPtr;

namespace content {

// This class proxies calls to the ServiceWorker APIs on the IO
// thread, and then calls back JavaScript on the UI thread.
class ServiceWorkerInternalsUI::OperationProxy
    : public base::RefCountedThreadSafe<
          ServiceWorkerInternalsUI::OperationProxy> {
 public:
  OperationProxy(const WeakPtr<ServiceWorkerInternalsUI> internals,
                 scoped_ptr<ListValue> original_args)
      : internals_(internals), original_args_(original_args.Pass()) {}

  void GetRegistrationsOnIOThread(int partition_id,
                                  ServiceWorkerContextWrapper* context,
                                  const base::FilePath& context_path);
  void UnregisterOnIOThread(scoped_refptr<ServiceWorkerContextWrapper> context,
                            const GURL& scope);
  void StartWorkerOnIOThread(scoped_refptr<ServiceWorkerContextWrapper> context,
                             const GURL& scope);
  void StopWorkerOnIOThread(scoped_refptr<ServiceWorkerContextWrapper> context,
                            const GURL& scope);
  void DispatchSyncEventToWorkerOnIOThread(
      scoped_refptr<ServiceWorkerContextWrapper> context,
      const GURL& scope);
  void InspectWorkerOnIOThread(
      scoped_refptr<ServiceWorkerContextWrapper> context,
      const GURL& scope);

 private:
  friend class base::RefCountedThreadSafe<OperationProxy>;
  ~OperationProxy() {}
  void OnHaveRegistrations(
      int partition_id,
      const base::FilePath& context_path,
      const std::vector<ServiceWorkerRegistrationInfo>& registrations);

  void OperationComplete(ServiceWorkerStatusCode status);

  void StartActiveWorker(
      ServiceWorkerStatusCode status,
      const scoped_refptr<ServiceWorkerRegistration>& registration);

  void StopActiveWorker(
      ServiceWorkerStatusCode status,
      const scoped_refptr<ServiceWorkerRegistration>& registration);

  void DispatchSyncEventToActiveWorker(
      ServiceWorkerStatusCode status,
      const scoped_refptr<ServiceWorkerRegistration>& registration);

  void InspectActiveWorker(
      const ServiceWorkerContextCore* const service_worker_context,
      ServiceWorkerStatusCode status,
      const scoped_refptr<ServiceWorkerRegistration>& registration);

  void InspectWorkerOnUIThread(
      const ServiceWorkerContextCore* const service_worker_context,
      int64 version_id);

  WeakPtr<ServiceWorkerInternalsUI> internals_;
  scoped_ptr<ListValue> original_args_;
};

class ServiceWorkerInternalsUI::PartitionObserver
    : public ServiceWorkerContextObserver {
 public:
  PartitionObserver(int partition_id, WebUI* web_ui)
      : partition_id_(partition_id), web_ui_(web_ui) {}
  virtual ~PartitionObserver() {}
  // ServiceWorkerContextObserver overrides:
  virtual void OnWorkerStarted(int64 version_id,
                               int process_id,
                               int thread_id) OVERRIDE {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    web_ui_->CallJavascriptFunction(
        "serviceworker.onWorkerStarted",
        FundamentalValue(partition_id_),
        StringValue(base::Int64ToString(version_id)),
        FundamentalValue(process_id),
        FundamentalValue(thread_id));
  }
  virtual void OnWorkerStopped(int64 version_id,
                               int process_id,
                               int thread_id) OVERRIDE {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    web_ui_->CallJavascriptFunction(
        "serviceworker.onWorkerStopped",
        FundamentalValue(partition_id_),
        StringValue(base::Int64ToString(version_id)),
        FundamentalValue(process_id),
        FundamentalValue(thread_id));
  }
  virtual void OnVersionStateChanged(int64 version_id) OVERRIDE {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    web_ui_->CallJavascriptFunction(
        "serviceworker.onVersionStateChanged",
        FundamentalValue(partition_id_),
        StringValue(base::Int64ToString(version_id)));
  }
  virtual void OnErrorReported(int64 version_id,
                               int process_id,
                               int thread_id,
                               const ErrorInfo& info) OVERRIDE {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    ScopedVector<const Value> args;
    args.push_back(new FundamentalValue(partition_id_));
    args.push_back(new StringValue(base::Int64ToString(version_id)));
    args.push_back(new FundamentalValue(process_id));
    args.push_back(new FundamentalValue(thread_id));
    scoped_ptr<DictionaryValue> value(new DictionaryValue());
    value->SetString("message", info.error_message);
    value->SetInteger("lineNumber", info.line_number);
    value->SetInteger("columnNumber", info.column_number);
    value->SetString("sourceURL", info.source_url.spec());
    args.push_back(value.release());
    web_ui_->CallJavascriptFunction("serviceworker.onErrorReported",
                                    args.get());
  }
  virtual void OnReportConsoleMessage(int64 version_id,
                                      int process_id,
                                      int thread_id,
                                      const ConsoleMessage& message) OVERRIDE {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    ScopedVector<const Value> args;
    args.push_back(new FundamentalValue(partition_id_));
    args.push_back(new StringValue(base::Int64ToString(version_id)));
    args.push_back(new FundamentalValue(process_id));
    args.push_back(new FundamentalValue(thread_id));
    scoped_ptr<DictionaryValue> value(new DictionaryValue());
    value->SetInteger("sourceIdentifier", message.source_identifier);
    value->SetInteger("message_level", message.message_level);
    value->SetString("message", message.message);
    value->SetInteger("lineNumber", message.line_number);
    value->SetString("sourceURL", message.source_url.spec());
    args.push_back(value.release());
    web_ui_->CallJavascriptFunction("serviceworker.onConsoleMessageReported",
                                    args.get());
  }
  virtual void OnRegistrationStored(const GURL& pattern) OVERRIDE {
    DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
    web_ui_->CallJavascriptFunction("serviceworker.onRegistrationStored",
                                    StringValue(pattern.spec()));
  }
  virtual void OnRegistrationDeleted(const GURL& pattern) OVERRIDE {
    web_ui_->CallJavascriptFunction("serviceworker.onRegistrationDeleted",
                                    StringValue(pattern.spec()));
  }
  int partition_id() const { return partition_id_; }

 private:
  const int partition_id_;
  WebUI* const web_ui_;
};

ServiceWorkerInternalsUI::ServiceWorkerInternalsUI(WebUI* web_ui)
    : WebUIController(web_ui), next_partition_id_(0) {
  WebUIDataSource* source =
      WebUIDataSource::Create(kChromeUIServiceWorkerInternalsHost);
  source->SetUseJsonJSFormatV2();
  source->SetJsonPath("strings.js");
  source->AddResourcePath("serviceworker_internals.js",
                          IDR_SERVICE_WORKER_INTERNALS_JS);
  source->AddResourcePath("serviceworker_internals.css",
                          IDR_SERVICE_WORKER_INTERNALS_CSS);
  source->SetDefaultResource(IDR_SERVICE_WORKER_INTERNALS_HTML);

  BrowserContext* browser_context =
      web_ui->GetWebContents()->GetBrowserContext();
  WebUIDataSource::Add(browser_context, source);

  web_ui->RegisterMessageCallback(
      "getAllRegistrations",
      base::Bind(&ServiceWorkerInternalsUI::GetAllRegistrations,
                 base::Unretained(this)));
  web_ui->RegisterMessageCallback(
      "start",
      base::Bind(&ServiceWorkerInternalsUI::StartWorker,
                 base::Unretained(this)));
  web_ui->RegisterMessageCallback(
      "stop",
      base::Bind(&ServiceWorkerInternalsUI::StopWorker,
                 base::Unretained(this)));
  web_ui->RegisterMessageCallback(
      "unregister",
      base::Bind(&ServiceWorkerInternalsUI::Unregister,
                 base::Unretained(this)));
  web_ui->RegisterMessageCallback(
      "sync",
      base::Bind(&ServiceWorkerInternalsUI::DispatchSyncEventToWorker,
                 base::Unretained(this)));
  web_ui->RegisterMessageCallback(
      "inspect",
      base::Bind(&ServiceWorkerInternalsUI::InspectWorker,
                 base::Unretained(this)));
}

ServiceWorkerInternalsUI::~ServiceWorkerInternalsUI() {
  BrowserContext* browser_context =
      web_ui()->GetWebContents()->GetBrowserContext();
  // Safe to use base::Unretained(this) because
  // ForEachStoragePartition is synchronous.
  BrowserContext::StoragePartitionCallback remove_observer_cb =
      base::Bind(&ServiceWorkerInternalsUI::RemoveObserverFromStoragePartition,
                 base::Unretained(this));
  BrowserContext::ForEachStoragePartition(browser_context, remove_observer_cb);
}

void ServiceWorkerInternalsUI::GetAllRegistrations(const ListValue* args) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  BrowserContext* browser_context =
      web_ui()->GetWebContents()->GetBrowserContext();

  // Safe to use base::Unretained(this) because
  // ForEachStoragePartition is synchronous.
  BrowserContext::StoragePartitionCallback add_context_cb =
      base::Bind(&ServiceWorkerInternalsUI::AddContextFromStoragePartition,
                 base::Unretained(this));
  BrowserContext::ForEachStoragePartition(browser_context, add_context_cb);
}

void ServiceWorkerInternalsUI::AddContextFromStoragePartition(
    StoragePartition* partition) {
  int partition_id = 0;
  scoped_refptr<ServiceWorkerContextWrapper> context =
      static_cast<ServiceWorkerContextWrapper*>(
          partition->GetServiceWorkerContext());
  if (PartitionObserver* observer =
          observers_.get(reinterpret_cast<uintptr_t>(partition))) {
    partition_id = observer->partition_id();
  } else {
    partition_id = next_partition_id_++;
    scoped_ptr<PartitionObserver> new_observer(
        new PartitionObserver(partition_id, web_ui()));
    context->AddObserver(new_observer.get());
    observers_.set(reinterpret_cast<uintptr_t>(partition), new_observer.Pass());
  }
  BrowserThread::PostTask(
      BrowserThread::IO,
      FROM_HERE,
      base::Bind(
          &ServiceWorkerInternalsUI::OperationProxy::GetRegistrationsOnIOThread,
          new OperationProxy(AsWeakPtr(), scoped_ptr<ListValue>()),
          partition_id,
          context,
          partition->GetPath()));
}

void ServiceWorkerInternalsUI::RemoveObserverFromStoragePartition(
    StoragePartition* partition) {
  scoped_ptr<PartitionObserver> observer(
      observers_.take_and_erase(reinterpret_cast<uintptr_t>(partition)));
  if (!observer.get())
    return;
  scoped_refptr<ServiceWorkerContextWrapper> context =
      static_cast<ServiceWorkerContextWrapper*>(
          partition->GetServiceWorkerContext());
  context->RemoveObserver(observer.get());
}

namespace {
void FindContext(const base::FilePath& partition_path,
                 StoragePartition** result_partition,
                 scoped_refptr<ServiceWorkerContextWrapper>* result_context,
                 StoragePartition* storage_partition) {
  if (storage_partition->GetPath() == partition_path) {
    *result_partition = storage_partition;
    *result_context = static_cast<ServiceWorkerContextWrapper*>(
        storage_partition->GetServiceWorkerContext());
  }
}
}  // namespace

bool ServiceWorkerInternalsUI::GetRegistrationInfo(
    const ListValue* args,
    base::FilePath* partition_path,
    GURL* scope,
    scoped_refptr<ServiceWorkerContextWrapper>* context) const {
  base::FilePath::StringType path_string;
  if (!args->GetString(0, &path_string))
    return false;
  *partition_path = base::FilePath(path_string);

  std::string scope_string;
  if (!args->GetString(1, &scope_string))
    return false;
  *scope = GURL(scope_string);

  BrowserContext* browser_context =
      web_ui()->GetWebContents()->GetBrowserContext();

  StoragePartition* result_partition(NULL);
  BrowserContext::StoragePartitionCallback find_context_cb =
      base::Bind(&FindContext, *partition_path, &result_partition, context);
  BrowserContext::ForEachStoragePartition(browser_context, find_context_cb);

  if (!result_partition || !(*context))
    return false;

  return true;
}

void ServiceWorkerInternalsUI::DispatchSyncEventToWorker(
    const ListValue* args) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  base::FilePath partition_path;
  GURL scope;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  if (!GetRegistrationInfo(args, &partition_path, &scope, &context))
    return;

  scoped_ptr<ListValue> args_copy(args->DeepCopy());
  BrowserThread::PostTask(
      BrowserThread::IO,
      FROM_HERE,
      base::Bind(&ServiceWorkerInternalsUI::OperationProxy::
                     DispatchSyncEventToWorkerOnIOThread,
                 new OperationProxy(AsWeakPtr(), args_copy.Pass()),
                 context,
                 scope));
}

void ServiceWorkerInternalsUI::InspectWorker(const ListValue* args) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  base::FilePath partition_path;
  GURL scope;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  if (!GetRegistrationInfo(args, &partition_path, &scope, &context))
    return;
  scoped_ptr<ListValue> args_copy(args->DeepCopy());
  BrowserThread::PostTask(
      BrowserThread::IO,
      FROM_HERE,
      base::Bind(
          &ServiceWorkerInternalsUI::OperationProxy::InspectWorkerOnIOThread,
          new OperationProxy(AsWeakPtr(), args_copy.Pass()),
          context,
          scope));
}

void ServiceWorkerInternalsUI::Unregister(const ListValue* args) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  base::FilePath partition_path;
  GURL scope;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  if (!GetRegistrationInfo(args, &partition_path, &scope, &context))
    return;

  scoped_ptr<ListValue> args_copy(args->DeepCopy());
  BrowserThread::PostTask(
      BrowserThread::IO,
      FROM_HERE,
      base::Bind(
          &ServiceWorkerInternalsUI::OperationProxy::UnregisterOnIOThread,
          new OperationProxy(AsWeakPtr(), args_copy.Pass()),
          context,
          scope));
}

void ServiceWorkerInternalsUI::StartWorker(const ListValue* args) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  base::FilePath partition_path;
  GURL scope;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  if (!GetRegistrationInfo(args, &partition_path, &scope, &context))
    return;

  scoped_ptr<ListValue> args_copy(args->DeepCopy());
  BrowserThread::PostTask(
      BrowserThread::IO,
      FROM_HERE,
      base::Bind(
          &ServiceWorkerInternalsUI::OperationProxy::StartWorkerOnIOThread,
          new OperationProxy(AsWeakPtr(), args_copy.Pass()),
          context,
          scope));
}

void ServiceWorkerInternalsUI::StopWorker(const ListValue* args) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  base::FilePath partition_path;
  GURL scope;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  if (!GetRegistrationInfo(args, &partition_path, &scope, &context))
    return;

  scoped_ptr<ListValue> args_copy(args->DeepCopy());
  BrowserThread::PostTask(
      BrowserThread::IO,
      FROM_HERE,
      base::Bind(
          &ServiceWorkerInternalsUI::OperationProxy::StopWorkerOnIOThread,
          new OperationProxy(AsWeakPtr(), args_copy.Pass()),
          context,
          scope));
}

void ServiceWorkerInternalsUI::OperationProxy::GetRegistrationsOnIOThread(
    int partition_id,
    ServiceWorkerContextWrapper* context,
    const base::FilePath& context_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));

  context->context()->storage()->GetAllRegistrations(
      base::Bind(&ServiceWorkerInternalsUI::OperationProxy::OnHaveRegistrations,
                 this,
                 partition_id,
                 context_path));
}

void ServiceWorkerInternalsUI::OperationProxy::UnregisterOnIOThread(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    const GURL& scope) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  context->context()->UnregisterServiceWorker(
      scope,
      base::Bind(&ServiceWorkerInternalsUI::OperationProxy::OperationComplete,
                 this));
}

void ServiceWorkerInternalsUI::OperationProxy::StartWorkerOnIOThread(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    const GURL& scope) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  // TODO(alecflett): Add support for starting/stopping workers for
  // pending versions too.
  context->context()->storage()->FindRegistrationForPattern(
      scope,
      base::Bind(&ServiceWorkerInternalsUI::OperationProxy::StartActiveWorker,
                 this));
}

void ServiceWorkerInternalsUI::OperationProxy::StopWorkerOnIOThread(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    const GURL& scope) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  // TODO(alecflett): Add support for starting/stopping workers for
  // pending versions too.
  context->context()->storage()->FindRegistrationForPattern(
      scope,
      base::Bind(&ServiceWorkerInternalsUI::OperationProxy::StopActiveWorker,
                 this));
}

void
ServiceWorkerInternalsUI::OperationProxy::DispatchSyncEventToWorkerOnIOThread(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    const GURL& scope) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  context->context()->storage()->FindRegistrationForPattern(
      scope,
      base::Bind(&ServiceWorkerInternalsUI::OperationProxy::
                     DispatchSyncEventToActiveWorker,
                 this));
}

void ServiceWorkerInternalsUI::OperationProxy::InspectWorkerOnIOThread(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    const GURL& scope) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  context->context()->storage()->FindRegistrationForPattern(
      scope,
      base::Bind(&ServiceWorkerInternalsUI::OperationProxy::InspectActiveWorker,
                 this,
                 context->context()));
}

namespace {
void UpdateVersionInfo(const ServiceWorkerVersionInfo& version,
                       DictionaryValue* info) {
  switch (version.running_status) {
    case ServiceWorkerVersion::STOPPED:
      info->SetString("running_status", "STOPPED");
      break;
    case ServiceWorkerVersion::STARTING:
      info->SetString("running_status", "STARTING");
      break;
    case ServiceWorkerVersion::RUNNING:
      info->SetString("running_status", "RUNNING");
      break;
    case ServiceWorkerVersion::STOPPING:
      info->SetString("running_status", "STOPPING");
      break;
  }

  switch (version.status) {
    case ServiceWorkerVersion::NEW:
      info->SetString("status", "NEW");
      break;
    case ServiceWorkerVersion::INSTALLING:
      info->SetString("status", "INSTALLING");
      break;
    case ServiceWorkerVersion::INSTALLED:
      info->SetString("status", "INSTALLED");
      break;
    case ServiceWorkerVersion::ACTIVATING:
      info->SetString("status", "ACTIVATING");
      break;
    case ServiceWorkerVersion::ACTIVE:
      info->SetString("status", "ACTIVE");
      break;
    case ServiceWorkerVersion::DEACTIVATED:
      info->SetString("status", "DEACTIVATED");
      break;
  }
  info->SetString("version_id", base::Int64ToString(version.version_id));
  info->SetInteger("process_id", version.process_id);
  info->SetInteger("thread_id", version.thread_id);
}
}  // namespace

void ServiceWorkerInternalsUI::OperationProxy::OnHaveRegistrations(
    int partition_id,
    const base::FilePath& context_path,
    const std::vector<ServiceWorkerRegistrationInfo>& registrations) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(
        BrowserThread::UI,
        FROM_HERE,
        base::Bind(
            &ServiceWorkerInternalsUI::OperationProxy::OnHaveRegistrations,
            this,
            partition_id,
            context_path,
            registrations));
    return;
  }

  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  ListValue result;
  for (std::vector<ServiceWorkerRegistrationInfo>::const_iterator it =
           registrations.begin();
       it != registrations.end();
       ++it) {
    const ServiceWorkerRegistrationInfo& registration = *it;
    DictionaryValue* registration_info = new DictionaryValue();
    registration_info->SetString("scope", registration.pattern.spec());
    registration_info->SetString("script_url", registration.script_url.spec());

    if (!registration.active_version.is_null) {
      DictionaryValue* active_info = new DictionaryValue();
      UpdateVersionInfo(registration.active_version, active_info);
      registration_info->Set("active", active_info);
    }

    if (!registration.pending_version.is_null) {
      DictionaryValue* pending_info = new DictionaryValue();
      UpdateVersionInfo(registration.pending_version, pending_info);
      registration_info->Set("pending", pending_info);
    }

    result.Append(registration_info);
  }

  if (internals_)
    internals_->web_ui()->CallJavascriptFunction(
        "serviceworker.onPartitionData",
        result,
        FundamentalValue(partition_id),
        StringValue(context_path.value()));
}

void ServiceWorkerInternalsUI::OperationProxy::OperationComplete(
    ServiceWorkerStatusCode status) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(
        BrowserThread::UI,
        FROM_HERE,
        base::Bind(&ServiceWorkerInternalsUI::OperationProxy::OperationComplete,
                   this,
                   status));
    return;
  }

  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  original_args_->Insert(0, new FundamentalValue(static_cast<int>(status)));
  if (internals_)
    internals_->web_ui()->CallJavascriptFunction(
        "serviceworker.onOperationComplete",
        std::vector<const Value*>(original_args_->begin(),
                                  original_args_->end()));
}

void ServiceWorkerInternalsUI::OperationProxy::StartActiveWorker(
    ServiceWorkerStatusCode status,
    const scoped_refptr<ServiceWorkerRegistration>& registration) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (status == SERVICE_WORKER_OK) {
    registration->active_version()->StartWorker(base::Bind(
        &ServiceWorkerInternalsUI::OperationProxy::OperationComplete, this));
    return;
  }

  OperationComplete(status);
}

void ServiceWorkerInternalsUI::OperationProxy::StopActiveWorker(
    ServiceWorkerStatusCode status,
    const scoped_refptr<ServiceWorkerRegistration>& registration) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (status == SERVICE_WORKER_OK) {
    registration->active_version()->StopWorker(base::Bind(
        &ServiceWorkerInternalsUI::OperationProxy::OperationComplete, this));
    return;
  }

  OperationComplete(status);
}

void ServiceWorkerInternalsUI::OperationProxy::DispatchSyncEventToActiveWorker(
    ServiceWorkerStatusCode status,
    const scoped_refptr<ServiceWorkerRegistration>& registration) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (status == SERVICE_WORKER_OK && registration->active_version() &&
      registration->active_version()->status() ==
          ServiceWorkerVersion::ACTIVE) {
    registration->active_version()->DispatchSyncEvent(base::Bind(
        &ServiceWorkerInternalsUI::OperationProxy::OperationComplete, this));
    return;
  }

  OperationComplete(SERVICE_WORKER_ERROR_FAILED);
}

void ServiceWorkerInternalsUI::OperationProxy::InspectActiveWorker(
    const ServiceWorkerContextCore* const service_worker_context,
    ServiceWorkerStatusCode status,
    const scoped_refptr<ServiceWorkerRegistration>& registration) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (status == SERVICE_WORKER_OK) {
    BrowserThread::PostTask(
        BrowserThread::UI,
        FROM_HERE,
        base::Bind(&OperationProxy::InspectWorkerOnUIThread,
                   this,
                   service_worker_context,
                   registration->active_version()->version_id()));
    return;
  }

  OperationComplete(status);
}

void ServiceWorkerInternalsUI::OperationProxy::InspectWorkerOnUIThread(
    const ServiceWorkerContextCore* const service_worker_context,
    int64 version_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  scoped_refptr<DevToolsAgentHost> agent_host(
      EmbeddedWorkerDevToolsManager::GetInstance()
          ->GetDevToolsAgentHostForServiceWorker(
              EmbeddedWorkerDevToolsManager::ServiceWorkerIdentifier(
                  service_worker_context, version_id)));
  if (agent_host) {
    DevToolsManagerImpl::GetInstance()->Inspect(
        internals_->web_ui()->GetWebContents()->GetBrowserContext(),
        agent_host.get());
    OperationComplete(SERVICE_WORKER_OK);
    return;
  }
  OperationComplete(SERVICE_WORKER_ERROR_NOT_FOUND);
}

}  // namespace content
