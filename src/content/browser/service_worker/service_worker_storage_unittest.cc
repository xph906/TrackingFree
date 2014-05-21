// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_storage.h"

#include "base/logging.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "content/browser/browser_thread_impl.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace content {

namespace {

void StatusCallback(bool* was_called,
                    ServiceWorkerStatusCode* result,
                    ServiceWorkerStatusCode status) {
  *was_called = true;
  *result = status;
}

ServiceWorkerStorage::StatusCallback MakeStatusCallback(
    bool* was_called,
    ServiceWorkerStatusCode* result) {
  return base::Bind(&StatusCallback, was_called, result);
}

void FindCallback(
    bool* was_called,
    ServiceWorkerStatusCode* result,
    scoped_refptr<ServiceWorkerRegistration>* found,
    ServiceWorkerStatusCode status,
    const scoped_refptr<ServiceWorkerRegistration>& registration) {
  *was_called = true;
  *result = status;
  *found = registration;
}

ServiceWorkerStorage::FindRegistrationCallback MakeFindCallback(
    bool* was_called,
    ServiceWorkerStatusCode* result,
    scoped_refptr<ServiceWorkerRegistration>* found) {
  return base::Bind(&FindCallback, was_called, result, found);
}

void GetAllCallback(
    bool* was_called,
    std::vector<ServiceWorkerRegistrationInfo>* all_out,
    const std::vector<ServiceWorkerRegistrationInfo>& all) {
  *was_called = true;
  *all_out = all;
}

ServiceWorkerStorage::GetAllRegistrationInfosCallback MakeGetAllCallback(
    bool* was_called,
    std::vector<ServiceWorkerRegistrationInfo>* all) {
  return base::Bind(&GetAllCallback, was_called, all);
}

}  // namespace

class ServiceWorkerStorageTest : public testing::Test {
 public:
  ServiceWorkerStorageTest()
      : browser_thread_bundle_(TestBrowserThreadBundle::IO_MAINLOOP) {
  }

  virtual void SetUp() OVERRIDE {
    context_.reset(new ServiceWorkerContextCore(
        base::FilePath(),
        base::MessageLoopProxy::current(),
        base::MessageLoopProxy::current(),
        NULL,
        NULL,
        scoped_ptr<ServiceWorkerProcessManager>()));
    context_ptr_ = context_->AsWeakPtr();
  }

  virtual void TearDown() OVERRIDE {
    context_.reset();
  }

  ServiceWorkerStorage* storage() { return context_->storage(); }

 protected:
  scoped_ptr<ServiceWorkerContextCore> context_;
  base::WeakPtr<ServiceWorkerContextCore> context_ptr_;
  TestBrowserThreadBundle browser_thread_bundle_;
};

TEST_F(ServiceWorkerStorageTest, StoreFindUpdateDeleteRegistration) {
  const GURL kScope("http://www.test.com/scope/*");
  const GURL kScript("http://www.test.com/script.js");
  const GURL kDocumentUrl("http://www.test.com/scope/document.html");
  const int64 kRegistrationId = 0;
  const int64 kVersionId = 0;

  bool was_called = false;
  ServiceWorkerStatusCode result = SERVICE_WORKER_OK;
  scoped_refptr<ServiceWorkerRegistration> found_registration;

  // We shouldn't find anything without having stored anything.
  storage()->FindRegistrationForDocument(
      kDocumentUrl,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;
  storage()->FindRegistrationForPattern(
      kScope,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;
  storage()->FindRegistrationForId(
      kRegistrationId,
      kScope.GetOrigin(),
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;

  // Store something.
  scoped_refptr<ServiceWorkerRegistration> live_registration =
      new ServiceWorkerRegistration(
          kScope, kScript, kRegistrationId, context_ptr_);
  scoped_refptr<ServiceWorkerVersion> live_version =
      new ServiceWorkerVersion(
          live_registration, kVersionId, context_ptr_);
  live_version->SetStatus(ServiceWorkerVersion::INSTALLED);
  live_registration->set_pending_version(live_version);
  storage()->StoreRegistration(live_registration, live_version,
                               MakeStatusCallback(&was_called, &result));
  EXPECT_FALSE(was_called);  // always async
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  was_called = false;

  // Now we should find it and get the live ptr back immediately.
  storage()->FindRegistrationForDocument(
      kDocumentUrl,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  EXPECT_EQ(live_registration, found_registration);
  was_called = false;
  found_registration = NULL;

  // But FindRegistrationForPattern is always async.
  storage()->FindRegistrationForPattern(
      kScope,
      MakeFindCallback(&was_called, &result, &found_registration));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  EXPECT_EQ(live_registration, found_registration);
  was_called = false;
  found_registration = NULL;

  // Can be found by id too.
  storage()->FindRegistrationForId(
      kRegistrationId,
      kScope.GetOrigin(),
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  ASSERT_TRUE(found_registration);
  EXPECT_EQ(kRegistrationId, found_registration->id());
  EXPECT_EQ(live_registration, found_registration);
  was_called = false;
  found_registration = NULL;

  // Drop the live registration, but keep the version live.
  live_registration = NULL;

  // Now FindRegistrationForDocument should be async.
  storage()->FindRegistrationForDocument(
      kDocumentUrl,
      MakeFindCallback(&was_called, &result, &found_registration));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  ASSERT_TRUE(found_registration);
  EXPECT_EQ(kRegistrationId, found_registration->id());
  EXPECT_TRUE(found_registration->HasOneRef());
  EXPECT_EQ(live_version,
            found_registration->pending_version());
  was_called = false;
  found_registration = NULL;

  // Drop the live version too.
  live_version = NULL;

  // And FindRegistrationForPattern is always async.
  storage()->FindRegistrationForPattern(
      kScope,
      MakeFindCallback(&was_called, &result, &found_registration));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  ASSERT_TRUE(found_registration);
  EXPECT_EQ(kRegistrationId, found_registration->id());
  EXPECT_TRUE(found_registration->HasOneRef());
  EXPECT_FALSE(found_registration->active_version());
  ASSERT_TRUE(found_registration->pending_version());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED,
            found_registration->pending_version()->status());
  was_called = false;

  // Update to active.
  scoped_refptr<ServiceWorkerVersion> temp_version =
      found_registration->pending_version();
  found_registration->set_pending_version(NULL);
  temp_version->SetStatus(ServiceWorkerVersion::ACTIVE);
  found_registration->set_active_version(temp_version);
  temp_version = NULL;
  storage()->UpdateToActiveState(
        found_registration,
        MakeStatusCallback(&was_called, &result));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  was_called = false;
  found_registration = NULL;

  // Trying to update a unstored registration to active should fail.
  scoped_refptr<ServiceWorkerRegistration> unstored_registration =
      new ServiceWorkerRegistration(
          kScope, kScript, kRegistrationId + 1, context_ptr_);
  storage()->UpdateToActiveState(
        unstored_registration,
        MakeStatusCallback(&was_called, &result));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  was_called = false;
  unstored_registration = NULL;

  // The Find methods should return a registration with an active version.
  storage()->FindRegistrationForDocument(
      kDocumentUrl,
      MakeFindCallback(&was_called, &result, &found_registration));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  ASSERT_TRUE(found_registration);
  EXPECT_EQ(kRegistrationId, found_registration->id());
  EXPECT_TRUE(found_registration->HasOneRef());
  EXPECT_FALSE(found_registration->pending_version());
  ASSERT_TRUE(found_registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVE,
            found_registration->active_version()->status());
  was_called = false;

  // Delete from storage but with a instance still live.
  EXPECT_TRUE(context_->GetLiveVersion(kRegistrationId));
  storage()->DeleteRegistration(
      kRegistrationId,
      kScope.GetOrigin(),
      MakeStatusCallback(&was_called, &result));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  EXPECT_TRUE(context_->GetLiveVersion(kRegistrationId));
  was_called = false;

  // Should no longer be found.
  storage()->FindRegistrationForId(
      kRegistrationId,
      kScope.GetOrigin(),
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;

  // Deleting an unstored registration should succeed.
  storage()->DeleteRegistration(
      kRegistrationId + 1,
      kScope.GetOrigin(),
      MakeStatusCallback(&was_called, &result));
  EXPECT_FALSE(was_called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  was_called = false;
}

TEST_F(ServiceWorkerStorageTest, InstallingRegistrationsAreFindable) {
  const GURL kScope("http://www.test.com/scope/*");
  const GURL kScript("http://www.test.com/script.js");
  const GURL kDocumentUrl("http://www.test.com/scope/document.html");
  const int64 kRegistrationId = 0;
  const int64 kVersionId = 0;

  bool was_called = false;
  ServiceWorkerStatusCode result = SERVICE_WORKER_OK;
  scoped_refptr<ServiceWorkerRegistration> found_registration;

  // Create an unstored registration.
  scoped_refptr<ServiceWorkerRegistration> live_registration =
      new ServiceWorkerRegistration(
          kScope, kScript, kRegistrationId, context_ptr_);
  scoped_refptr<ServiceWorkerVersion> live_version =
      new ServiceWorkerVersion(
          live_registration, kVersionId, context_ptr_);
  live_version->SetStatus(ServiceWorkerVersion::INSTALLING);
  live_registration->set_pending_version(live_version);

  // Should not be findable, including by GetAllRegistrations.
  storage()->FindRegistrationForId(
      kRegistrationId,
      kScope.GetOrigin(),
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;
  storage()->FindRegistrationForDocument(
      kDocumentUrl,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;
  storage()->FindRegistrationForPattern(
      kScope,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;
  std::vector<ServiceWorkerRegistrationInfo> all_registrations;
  storage()->GetAllRegistrations(
      MakeGetAllCallback(&was_called, &all_registrations));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_TRUE(all_registrations.empty());
  was_called = false;

  // Notify storage of it being installed.
  storage()->NotifyInstallingRegistration(live_registration);

  // Now should be findable.
  storage()->FindRegistrationForId(
      kRegistrationId,
      kScope.GetOrigin(),
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  EXPECT_EQ(live_registration, found_registration);
  was_called = false;
  found_registration = NULL;
  storage()->FindRegistrationForDocument(
      kDocumentUrl,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  EXPECT_EQ(live_registration, found_registration);
  was_called = false;
  found_registration = NULL;
  storage()->FindRegistrationForPattern(
      kScope,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_OK, result);
  EXPECT_EQ(live_registration, found_registration);
  was_called = false;
  found_registration = NULL;
  storage()->GetAllRegistrations(
      MakeGetAllCallback(&was_called, &all_registrations));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(1u, all_registrations.size());
  was_called = false;
  all_registrations.clear();

  // Notify storage of installation no longer happening.
  storage()->NotifyDoneInstallingRegistration(live_registration);

  // Once again, should not be findable.
  storage()->FindRegistrationForId(
      kRegistrationId,
      kScope.GetOrigin(),
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;
  storage()->FindRegistrationForDocument(
      kDocumentUrl,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;
  storage()->FindRegistrationForPattern(
      kScope,
      MakeFindCallback(&was_called, &result, &found_registration));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_EQ(SERVICE_WORKER_ERROR_NOT_FOUND, result);
  EXPECT_FALSE(found_registration);
  was_called = false;

  storage()->GetAllRegistrations(
      MakeGetAllCallback(&was_called, &all_registrations));
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(was_called);
  EXPECT_TRUE(all_registrations.empty());
  was_called = false;
}

}  // namespace content
