// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <queue>

#include "ash/shell.h"
#include "base/command_line.h"
#include "base/strings/string_util.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/chromeos/accessibility/accessibility_manager.h"
#include "chrome/browser/chromeos/accessibility/speech_monitor.h"
#include "chrome/browser/chromeos/login/ui/login_display_host.h"
#include "chrome/browser/chromeos/login/ui/login_display_host_impl.h"
#include "chrome/browser/chromeos/login/ui/webui_login_view.h"
#include "chrome/browser/chromeos/login/users/user_manager.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/extensions/api/braille_display_private/stub_braille_controller.h"
#include "chrome/browser/speech/tts_controller.h"
#include "chrome/browser/speech/tts_platform.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/ui_test_utils.h"
#include "chromeos/chromeos_switches.h"
#include "content/public/common/url_constants.h"
#include "content/public/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/test/ui_controls.h"
#include "ui/views/widget/widget.h"

using extensions::api::braille_display_private::StubBrailleController;

namespace chromeos {

//
// Spoken feedback tests in a normal browser window.
//

enum SpokenFeedbackTestVariant {
  kTestAsNormalUser,
  kTestAsGuestUser
};

class SpokenFeedbackTest
    : public InProcessBrowserTest,
      public ::testing::WithParamInterface<SpokenFeedbackTestVariant> {
 protected:
  SpokenFeedbackTest() {}
  virtual ~SpokenFeedbackTest() {}

  virtual void SetUpInProcessBrowserTestFixture() OVERRIDE {
    AccessibilityManager::SetBrailleControllerForTest(&braille_controller_);
  }

  virtual void CleanUpOnMainThread() OVERRIDE {
    AccessibilityManager::SetBrailleControllerForTest(NULL);
  }

  virtual void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    if (GetParam() == kTestAsGuestUser) {
      command_line->AppendSwitch(chromeos::switches::kGuestSession);
      command_line->AppendSwitch(::switches::kIncognito);
      command_line->AppendSwitchASCII(chromeos::switches::kLoginProfile,
                                      "user");
      command_line->AppendSwitchASCII(chromeos::switches::kLoginUser,
                                      chromeos::UserManager::kGuestUserName);
    }
  }

 private:
  StubBrailleController braille_controller_;
  DISALLOW_COPY_AND_ASSIGN(SpokenFeedbackTest);
};

INSTANTIATE_TEST_CASE_P(
    TestAsNormalAndGuestUser,
    SpokenFeedbackTest,
    ::testing::Values(kTestAsNormalUser,
                      kTestAsGuestUser));

IN_PROC_BROWSER_TEST_P(SpokenFeedbackTest, EnableSpokenFeedback) {
  EXPECT_FALSE(AccessibilityManager::Get()->IsSpokenFeedbackEnabled());

  SpeechMonitor monitor;
  AccessibilityManager::Get()->EnableSpokenFeedback(
      true, ash::A11Y_NOTIFICATION_NONE);
  EXPECT_TRUE(monitor.SkipChromeVoxEnabledMessage());
}

IN_PROC_BROWSER_TEST_P(SpokenFeedbackTest, FocusToolbar) {
  EXPECT_FALSE(AccessibilityManager::Get()->IsSpokenFeedbackEnabled());

  SpeechMonitor monitor;
  AccessibilityManager::Get()->EnableSpokenFeedback(
      true, ash::A11Y_NOTIFICATION_NONE);
  EXPECT_TRUE(monitor.SkipChromeVoxEnabledMessage());

  chrome::ExecuteCommand(browser(), IDC_FOCUS_TOOLBAR);
  // Might be "Google Chrome Toolbar" or "Chromium Toolbar".
  EXPECT_TRUE(MatchPattern(monitor.GetNextUtterance(), "*oolbar*"));
  EXPECT_EQ("Reload,", monitor.GetNextUtterance());
  EXPECT_EQ("button", monitor.GetNextUtterance());
}

IN_PROC_BROWSER_TEST_P(SpokenFeedbackTest, TypeInOmnibox) {
  EXPECT_FALSE(AccessibilityManager::Get()->IsSpokenFeedbackEnabled());

  SpeechMonitor monitor;
  AccessibilityManager::Get()->EnableSpokenFeedback(
      true, ash::A11Y_NOTIFICATION_NONE);
  EXPECT_TRUE(monitor.SkipChromeVoxEnabledMessage());

  chrome::ExecuteCommand(browser(), IDC_FOCUS_LOCATION);

  gfx::NativeWindow window = ash::Shell::GetInstance()->GetPrimaryRootWindow();
  ui_controls::SendKeyPress(window, ui::VKEY_X, false, false, false, false);
  while (true) {
    std::string utterance = monitor.GetNextUtterance();
    VLOG(0) << "Got utterance: " << utterance;
    if (utterance == "x")
      break;
  }

  ui_controls::SendKeyPress(window, ui::VKEY_Y, false, false, false, false);
  EXPECT_EQ("y", monitor.GetNextUtterance());

  ui_controls::SendKeyPress(window, ui::VKEY_Z, false, false, false, false);
  EXPECT_EQ("z", monitor.GetNextUtterance());

  ui_controls::SendKeyPress(window, ui::VKEY_BACK, false, false, false, false);
  EXPECT_EQ("z", monitor.GetNextUtterance());
}

//
// Spoken feedback tests that run in guest mode.
//

class GuestSpokenFeedbackTest : public SpokenFeedbackTest {
 protected:
  GuestSpokenFeedbackTest() {}
  virtual ~GuestSpokenFeedbackTest() {}

  virtual void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    command_line->AppendSwitch(chromeos::switches::kGuestSession);
    command_line->AppendSwitch(::switches::kIncognito);
    command_line->AppendSwitchASCII(chromeos::switches::kLoginProfile, "user");
    command_line->AppendSwitchASCII(chromeos::switches::kLoginUser,
                                    chromeos::UserManager::kGuestUserName);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(GuestSpokenFeedbackTest);
};

IN_PROC_BROWSER_TEST_F(GuestSpokenFeedbackTest, FocusToolbar) {
  EXPECT_FALSE(AccessibilityManager::Get()->IsSpokenFeedbackEnabled());

  SpeechMonitor monitor;
  AccessibilityManager::Get()->EnableSpokenFeedback(
      true, ash::A11Y_NOTIFICATION_NONE);
  EXPECT_TRUE(monitor.SkipChromeVoxEnabledMessage());

  chrome::ExecuteCommand(browser(), IDC_FOCUS_TOOLBAR);
  // Might be "Google Chrome Toolbar" or "Chromium Toolbar".
  EXPECT_TRUE(MatchPattern(monitor.GetNextUtterance(), "*oolbar*"));
  EXPECT_EQ("Reload,", monitor.GetNextUtterance());
  EXPECT_EQ("button", monitor.GetNextUtterance());
}

//
// Spoken feedback tests of the out-of-box experience.
//

class OobeSpokenFeedbackTest : public InProcessBrowserTest {
 protected:
  OobeSpokenFeedbackTest() {}
  virtual ~OobeSpokenFeedbackTest() {}

  virtual void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    command_line->AppendSwitch(chromeos::switches::kLoginManager);
    command_line->AppendSwitch(chromeos::switches::kForceLoginManagerInTests);
    command_line->AppendSwitchASCII(chromeos::switches::kLoginProfile, "user");
  }

  virtual void SetUpOnMainThread() OVERRIDE {
    AccessibilityManager::Get()->
        SetProfileForTest(ProfileHelper::GetSigninProfile());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(OobeSpokenFeedbackTest);
};

// Test is flaky: http://crbug.com/346797
IN_PROC_BROWSER_TEST_F(OobeSpokenFeedbackTest, DISABLED_SpokenFeedbackInOobe) {
  ui_controls::EnableUIControls();
  EXPECT_FALSE(AccessibilityManager::Get()->IsSpokenFeedbackEnabled());

  LoginDisplayHost* login_display_host = LoginDisplayHostImpl::default_host();
  WebUILoginView* web_ui_login_view = login_display_host->GetWebUILoginView();
  views::Widget* widget = web_ui_login_view->GetWidget();
  gfx::NativeWindow window = widget->GetNativeWindow();

  SpeechMonitor monitor;
  AccessibilityManager::Get()->EnableSpokenFeedback(
      true, ash::A11Y_NOTIFICATION_NONE);
  EXPECT_TRUE(monitor.SkipChromeVoxEnabledMessage());

  EXPECT_EQ("Select your language:", monitor.GetNextUtterance());
  EXPECT_EQ("English ( United States)", monitor.GetNextUtterance());
  EXPECT_TRUE(MatchPattern(monitor.GetNextUtterance(), "Combo box * of *"));
  ui_controls::SendKeyPress(window, ui::VKEY_TAB, false, false, false, false);
  EXPECT_EQ("Select your keyboard:", monitor.GetNextUtterance());
}

}  // namespace chromeos
