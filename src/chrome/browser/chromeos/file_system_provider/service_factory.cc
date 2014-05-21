// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/file_system_provider/service_factory.h"

#include "chrome/browser/chromeos/file_system_provider/service.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_factory.h"

namespace chromeos {
namespace file_system_provider {

// static
Service* ServiceFactory::Get(content::BrowserContext* context) {
  return static_cast<Service*>(
      GetInstance()->GetServiceForBrowserContext(context, true));
}

ServiceFactory* ServiceFactory::GetInstance() {
  return Singleton<ServiceFactory>::get();
}

ServiceFactory::ServiceFactory()
    : BrowserContextKeyedServiceFactory(
          "Service",
          BrowserContextDependencyManager::GetInstance()) {
  DependsOn(extensions::ExtensionRegistryFactory::GetInstance());
}

ServiceFactory::~ServiceFactory() {}

KeyedService* ServiceFactory::BuildServiceInstanceFor(
    content::BrowserContext* profile) const {
  return new Service(
      Profile::FromBrowserContext(profile),
      extensions::ExtensionRegistry::Get(Profile::FromBrowserContext(profile)));
}

bool ServiceFactory::ServiceIsCreatedWithBrowserContext() const { return true; }

}  // namespace file_system_provider
}  // namespace chromeos
