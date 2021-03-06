// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_PASSWORDS_MANAGE_PASSWORDS_ICON_H_
#define CHROME_BROWSER_UI_PASSWORDS_MANAGE_PASSWORDS_ICON_H_

#include "base/basictypes.h"
#include "components/password_manager/core/common/password_manager_ui.h"

// Abstract base class for platform-specific password management icon views.
class ManagePasswordsIcon {
 public:
  // Get/set the icon's state. Implementations of this class must implement
  // SetStateInternal to do reasonable platform-specific things to represent
  // the icon's state to the user.
  void SetState(password_manager::ui::State state);
  password_manager::ui::State state() const { return state_; }

 protected:
  ManagePasswordsIcon();
  ~ManagePasswordsIcon();

  // Called from SetState() iff the icon's state has changed in order to do
  // whatever platform-specific UI work is necessary given the new state.
  virtual void UpdateVisibleUI() = 0;

 private:
  password_manager::ui::State state_;

  DISALLOW_COPY_AND_ASSIGN(ManagePasswordsIcon);
};

#endif  // CHROME_BROWSER_UI_PASSWORDS_MANAGE_PASSWORDS_ICON_H_
