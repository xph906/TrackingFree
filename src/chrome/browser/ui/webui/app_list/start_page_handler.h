// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_APP_LIST_START_PAGE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_APP_LIST_START_PAGE_HANDLER_H_

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "base/prefs/pref_change_registrar.h"
#include "chrome/browser/ui/app_list/recommended_apps_observer.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/web_ui_message_handler.h"

namespace base {
class ListValue;
}

namespace app_list {

class RecommendedApps;

// Handler for the app launcher start page.
class StartPageHandler : public content::WebUIMessageHandler,
                         public content::NotificationObserver,
                         public RecommendedAppsObserver {
 public:
  StartPageHandler();
  virtual ~StartPageHandler();

 private:
  // content::WebUIMessageHandler overrides:
  virtual void RegisterMessages() OVERRIDE;

  // Overridden from content::NotificationObserver:
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE;

  // RecommendedAppsObserver overrdies:
  virtual void OnRecommendedAppsChanged() OVERRIDE;

  // Creates a ListValue for the recommended apps and sends it to js side.
  void SendRecommendedApps();

#if defined(OS_CHROMEOS)
  // Called when the pref has been changed.
  void OnHotwordEnabledChanged();
#endif

  // JS callbacks.
  void HandleInitialize(const base::ListValue* args);
  void HandleLaunchApp(const base::ListValue* args);
  void HandleSpeechResult(const base::ListValue* args);
  void HandleSpeechSoundLevel(const base::ListValue* args);
  void HandleSpeechRecognition(const base::ListValue* args);

  RecommendedApps* recommended_apps_;  // Not owned.
  PrefChangeRegistrar pref_change_registrar_;
  content::NotificationRegistrar registrar_;

  DISALLOW_COPY_AND_ASSIGN(StartPageHandler);
};

}  // namespace app_list

#endif  // CHROME_BROWSER_UI_WEBUI_APP_LIST_START_PAGE_HANDLER_H_
