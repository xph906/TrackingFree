// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/invalidation/ticl_invalidation_service.h"

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/invalidation/gcm_invalidation_bridge.h"
#include "chrome/browser/invalidation/invalidation_service_test_template.h"
#include "chrome/browser/services/gcm/gcm_driver.h"
#include "google_apis/gaia/fake_identity_provider.h"
#include "google_apis/gaia/fake_oauth2_token_service.h"
#include "net/url_request/url_request_context_getter.h"
#include "sync/notifier/fake_invalidation_state_tracker.h"
#include "sync/notifier/fake_invalidator.h"
#include "sync/notifier/invalidation_state_tracker.h"
#include "sync/notifier/invalidator.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace invalidation {

namespace {

class FakeTiclSettingsProvider : public TiclSettingsProvider {
 public:
  FakeTiclSettingsProvider();
  virtual ~FakeTiclSettingsProvider();

  // TiclSettingsProvider:
  virtual bool UseGCMChannel() const OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(FakeTiclSettingsProvider);
};

class FakeGCMDriver : public gcm::GCMDriver {
 public:
  explicit FakeGCMDriver(OAuth2TokenService* token_service);
  virtual ~FakeGCMDriver();

 protected:
  // gcm::GCMDriver:
  virtual bool ShouldStartAutomatically() const OVERRIDE;
  virtual base::FilePath GetStorePath() const OVERRIDE;
  virtual scoped_refptr<net::URLRequestContextGetter>
      GetURLRequestContextGetter() const OVERRIDE;

 private:
  DISALLOW_COPY_AND_ASSIGN(FakeGCMDriver);
};

FakeTiclSettingsProvider::FakeTiclSettingsProvider() {
}

FakeTiclSettingsProvider::~FakeTiclSettingsProvider() {
}

bool FakeTiclSettingsProvider::UseGCMChannel() const {
  return false;
}

FakeGCMDriver::FakeGCMDriver(OAuth2TokenService* token_service)
    : GCMDriver(scoped_ptr<IdentityProvider>(
          new FakeIdentityProvider(token_service))) {
}

FakeGCMDriver::~FakeGCMDriver() {
}

bool FakeGCMDriver::ShouldStartAutomatically() const {
  return false;
}

base::FilePath FakeGCMDriver::GetStorePath() const {
  return base::FilePath();
}

scoped_refptr<net::URLRequestContextGetter>
FakeGCMDriver::GetURLRequestContextGetter() const {
  return NULL;
}

}  // namespace

class TiclInvalidationServiceTestDelegate {
 public:
  TiclInvalidationServiceTestDelegate() {}

  ~TiclInvalidationServiceTestDelegate() {
    DestroyInvalidationService();
  }

  void CreateInvalidationService() {
    CreateUninitializedInvalidationService();
    InitializeInvalidationService();
  }

  void CreateUninitializedInvalidationService() {
    gcm_service_.reset(new FakeGCMDriver(&token_service_));
    invalidation_service_.reset(new TiclInvalidationService(
        scoped_ptr<IdentityProvider>(new FakeIdentityProvider(&token_service_)),
        scoped_ptr<TiclSettingsProvider>(new FakeTiclSettingsProvider),
        gcm_service_.get(),
        NULL));
  }

  void InitializeInvalidationService() {
    fake_invalidator_ = new syncer::FakeInvalidator();
    invalidation_service_->InitForTest(
        scoped_ptr<syncer::InvalidationStateTracker>(
            new syncer::FakeInvalidationStateTracker),
        fake_invalidator_);
  }

  InvalidationService* GetInvalidationService() {
    return invalidation_service_.get();
  }

  void DestroyInvalidationService() {
    invalidation_service_->Shutdown();
  }

  void TriggerOnInvalidatorStateChange(syncer::InvalidatorState state) {
    fake_invalidator_->EmitOnInvalidatorStateChange(state);
  }

  void TriggerOnIncomingInvalidation(
      const syncer::ObjectIdInvalidationMap& invalidation_map) {
    fake_invalidator_->EmitOnIncomingInvalidation(invalidation_map);
  }

  FakeOAuth2TokenService token_service_;
  scoped_ptr<gcm::GCMDriver> gcm_service_;
  syncer::FakeInvalidator* fake_invalidator_;  // Owned by the service.

  scoped_ptr<TiclInvalidationService> invalidation_service_;
};

INSTANTIATE_TYPED_TEST_CASE_P(
    TiclInvalidationServiceTest, InvalidationServiceTest,
    TiclInvalidationServiceTestDelegate);

namespace internal {

class FakeCallbackContainer {
  public:
    FakeCallbackContainer() : called_(false),
                              weak_ptr_factory_(this) {}

    void FakeCallback(const base::DictionaryValue& value) {
      called_ = true;
    }

    bool called_;
    base::WeakPtrFactory<FakeCallbackContainer> weak_ptr_factory_;
};

}  // namespace internal

// Test that requesting for detailed status doesn't crash even if the
// underlying invalidator is not initialized.
TEST(TiclInvalidationServiceLoggingTest, DetailedStatusCallbacksWork) {
  scoped_ptr<TiclInvalidationServiceTestDelegate> delegate (
      new TiclInvalidationServiceTestDelegate());

  delegate->CreateUninitializedInvalidationService();
  invalidation::InvalidationService* const invalidator =
      delegate->GetInvalidationService();

  internal::FakeCallbackContainer fake_container;
  invalidator->RequestDetailedStatus(
      base::Bind(&internal::FakeCallbackContainer::FakeCallback,
                 fake_container.weak_ptr_factory_.GetWeakPtr()));
  EXPECT_FALSE(fake_container.called_);

  delegate->InitializeInvalidationService();

  invalidator->RequestDetailedStatus(
      base::Bind(&internal::FakeCallbackContainer::FakeCallback,
                 fake_container.weak_ptr_factory_.GetWeakPtr()));
  EXPECT_TRUE(fake_container.called_);
}

}  // namespace invalidation
