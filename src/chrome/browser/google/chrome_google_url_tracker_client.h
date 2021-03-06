// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GOOGLE_CHROME_GOOGLE_URL_TRACKER_CLIENT_H_
#define CHROME_BROWSER_GOOGLE_CHROME_GOOGLE_URL_TRACKER_CLIENT_H_

#include "components/google/core/browser/google_url_tracker_client.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"

class ChromeGoogleURLTrackerClient : public GoogleURLTrackerClient,
                                     public content::NotificationObserver {
 public:
  ChromeGoogleURLTrackerClient();
  virtual ~ChromeGoogleURLTrackerClient();

  // GoogleURLTrackerClient:
  virtual void SetListeningForNavigationStart(bool listen) OVERRIDE;
  virtual bool IsListeningForNavigationStart() OVERRIDE;

 private:
  // content::NotificationObserver:
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE;

  content::NotificationRegistrar registrar_;

  DISALLOW_COPY_AND_ASSIGN(ChromeGoogleURLTrackerClient);
};

#endif  // CHROME_BROWSER_GOOGLE_CHROME_GOOGLE_URL_TRACKER_CLIENT_H_
