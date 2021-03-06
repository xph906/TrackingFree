// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/passwords/manage_passwords_icon_view.h"

#include "chrome/browser/ui/passwords/manage_passwords_icon.h"
#include "chrome/browser/ui/passwords/manage_passwords_ui_controller_mock.h"
#include "chrome/browser/ui/views/passwords/manage_passwords_view_test.h"
#include "components/password_manager/core/common/password_manager_ui.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"
#include "testing/gtest/include/gtest/gtest.h"

typedef ManagePasswordsViewTest ManagePasswordsIconViewTest;

IN_PROC_BROWSER_TEST_F(ManagePasswordsIconViewTest, DefaultStateIsInactive) {
  EXPECT_EQ(password_manager::ui::INACTIVE_STATE, view()->state());
  EXPECT_FALSE(view()->visible());
  EXPECT_EQ(0, view()->icon_id());
  EXPECT_EQ(0, view()->tooltip_text_id());
}

IN_PROC_BROWSER_TEST_F(ManagePasswordsIconViewTest, PendingState) {
  SetupPendingPassword();
  EXPECT_EQ(password_manager::ui::PENDING_PASSWORD_STATE, view()->state());
  EXPECT_TRUE(view()->visible());
  EXPECT_EQ(IDR_SAVE_PASSWORD, view()->icon_id());
  EXPECT_EQ(IDS_PASSWORD_MANAGER_TOOLTIP_SAVE, view()->tooltip_text_id());
}

IN_PROC_BROWSER_TEST_F(ManagePasswordsIconViewTest, ManageState) {
  SetupManagingPasswords();
  EXPECT_EQ(password_manager::ui::MANAGE_STATE, view()->state());
  EXPECT_TRUE(view()->visible());
  EXPECT_EQ(IDR_SAVE_PASSWORD, view()->icon_id());
  EXPECT_EQ(IDS_PASSWORD_MANAGER_TOOLTIP_MANAGE, view()->tooltip_text_id());
}

IN_PROC_BROWSER_TEST_F(ManagePasswordsIconViewTest, BlacklistedState) {
  SetupBlackistedPassword();
  EXPECT_EQ(password_manager::ui::BLACKLIST_STATE, view()->state());
  EXPECT_TRUE(view()->visible());
  EXPECT_EQ(IDR_SAVE_PASSWORD_BLACKLISTED, view()->icon_id());
  EXPECT_EQ(IDS_PASSWORD_MANAGER_TOOLTIP_MANAGE, view()->tooltip_text_id());
}
