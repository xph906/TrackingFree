// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_POLICY_SCHEMA_REGISTRY_SERVICE_H_
#define CHROME_BROWSER_POLICY_SCHEMA_REGISTRY_SERVICE_H_

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/policy/core/common/schema_registry.h"

namespace policy {

class Schema;

// A SchemaRegistry that is also a KeyedService, and is associated
// with a Profile.
class SchemaRegistryService : public SchemaRegistry, public KeyedService {
 public:
  // This SchemaRegistry will initially contain only the |chrome_schema|, if
  // it's valid. The optional |global_registry| must outlive this, and will
  // track this registry.
  SchemaRegistryService(const Schema& chrome_schema,
                        CombinedSchemaRegistry* global_registry);
  virtual ~SchemaRegistryService();

  // KeyedService:
  virtual void Shutdown() OVERRIDE;

 private:
  CombinedSchemaRegistry* global_registry_;

  DISALLOW_COPY_AND_ASSIGN(SchemaRegistryService);
};

}  // namespace policy

#endif  // CHROME_BROWSER_POLICY_SCHEMA_REGISTRY_SERVICE_H_
