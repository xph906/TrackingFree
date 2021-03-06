// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/chromeos/login/managed/supervised_user_authentication.h"

#include "base/values.h"
#include "chrome/browser/chromeos/login/users/supervised_user_manager.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {

class SupervisedUserAuthenticationTest : public testing::Test {
 protected:
  SupervisedUserAuthenticationTest();
  virtual ~SupervisedUserAuthenticationTest();

  // testing::Test:
  virtual void SetUp() OVERRIDE;
  virtual void TearDown() OVERRIDE;

  DISALLOW_COPY_AND_ASSIGN(SupervisedUserAuthenticationTest);
};

SupervisedUserAuthenticationTest::SupervisedUserAuthenticationTest() {}

SupervisedUserAuthenticationTest::~SupervisedUserAuthenticationTest() {}

void SupervisedUserAuthenticationTest::SetUp() {}

void SupervisedUserAuthenticationTest::TearDown() {}

TEST_F(SupervisedUserAuthenticationTest, SignatureGeneration) {
  std::string password = "password";
  int revision = 1;
  std::string salt =
      "204cc733ebe526ea9a84885de904eb7a578d86a4c385d252dce275d9d9675c37";
  std::string expected_salted_password =
      "OSL3HZZSfK+mDQTYUh3lXhgAzJNWhYz52ax0Bleny7Q=";
  std::string signature_key = "p5TR/34XX0R7IMuffH14BiL1vcdSD8EajPzdIg09z9M=";
  std::string expected_signature =
      "KOPQmmJcMr9iMkr36N1cX+G9gDdBBu7zutAxNayPMN4=";

  std::string salted_password =
      SupervisedUserAuthentication::BuildPasswordForHashWithSaltSchema(
          salt, password);
  ASSERT_EQ(expected_salted_password, salted_password);
  std::string signature = SupervisedUserAuthentication::BuildPasswordSignature(
      salted_password, revision, signature_key);
  ASSERT_EQ(expected_signature, signature);
}

}  //  namespace chromeos
