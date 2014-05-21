// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SEARCH_HOTWORD_SERVICE_H_
#define CHROME_BROWSER_SEARCH_HOTWORD_SERVICE_H_

#include "base/basictypes.h"
#include "base/prefs/pref_change_registrar.h"
#include "components/keyed_service/core/keyed_service.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"

class ExtensionService;
class HotwordClient;
class Profile;

namespace hotword_internal {
// Constants for the hotword field trial.
extern const char kHotwordFieldTrialName[];
extern const char kHotwordFieldTrialDisabledGroupName[];
}  // namespace hotword_internal

// Provides an interface for the Hotword component that does voice triggered
// search.
class HotwordService : public content::NotificationObserver,
                       public KeyedService {
 public:
  // Returns true if the hotword supports the current system language.
  static bool DoesHotwordSupportLanguage(Profile* profile);

  explicit HotwordService(Profile* profile);
  virtual ~HotwordService();

  // Overridden from content::NotificationObserver:
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE;

  bool ShouldShowOptInPopup();

  // Used in testing to ensure that the popup is not shown more than this
  // maximum number of times.
  int MaxNumberTimesToShowOptInPopup();

  // In addition to showing the popup, the preferences
  // kHotwordOptInPopupTimesShown is also incremented.
  void ShowOptInPopup();

  // Checks for whether all the necessary files have downloaded to allow for
  // using the extension.
  virtual bool IsServiceAvailable();

  // Determine if hotwording is allowed in this profile based on field trials
  // and language.
  virtual bool IsHotwordAllowed();

  // Checks if the user has opted into audio logging. Returns true if the user
  // is opted in, false otherwise..
  bool IsOptedIntoAudioLogging();

  // Used in the case of an error with the current hotword extension. Tries
  // to reload the extension or in the case of failure, tries to re-download it.
  // Returns true upon successful attempt at reload or if the extension has
  // already loaded successfully by some other means.
  virtual bool RetryHotwordExtension();

  // Control the state of the hotword extension.
  void EnableHotwordExtension(ExtensionService* extension_service);
  void DisableHotwordExtension(ExtensionService* extension_service);

  // Handles enabling/disabling the hotword extension when the user
  // turns it off via the settings menu.
  void OnHotwordSearchEnabledChanged(const std::string& pref_name);

  // Called to handle the hotword session from |client|.
  void RequestHotwordSession(HotwordClient* client);
  void StopHotwordSession(HotwordClient* client);
  HotwordClient* client() { return client_; }

 private:
  Profile* profile_;

  PrefChangeRegistrar pref_registrar_;

  content::NotificationRegistrar registrar_;

  HotwordClient* client_;

  DISALLOW_COPY_AND_ASSIGN(HotwordService);
};

#endif  // CHROME_BROWSER_SEARCH_HOTWORD_SERVICE_H_
