// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/google/chrome_google_url_tracker_client.h"

#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/google/google_url_tracker.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/web_contents.h"

ChromeGoogleURLTrackerClient::ChromeGoogleURLTrackerClient() {
}

ChromeGoogleURLTrackerClient::~ChromeGoogleURLTrackerClient() {
}

void ChromeGoogleURLTrackerClient::SetListeningForNavigationStart(bool listen) {
  if (listen) {
    registrar_.Add(
        this,
        content::NOTIFICATION_NAV_ENTRY_PENDING,
        content::NotificationService::AllBrowserContextsAndSources());
  } else {
    registrar_.Remove(
        this,
        content::NOTIFICATION_NAV_ENTRY_PENDING,
        content::NotificationService::AllBrowserContextsAndSources());
  }
}

bool ChromeGoogleURLTrackerClient::IsListeningForNavigationStart() {
  return registrar_.IsRegistered(
      this,
      content::NOTIFICATION_NAV_ENTRY_PENDING,
      content::NotificationService::AllBrowserContextsAndSources());
}

void ChromeGoogleURLTrackerClient::Observe(
    int type,
    const content::NotificationSource& source,
    const content::NotificationDetails& details) {
  DCHECK_EQ(content::NOTIFICATION_NAV_ENTRY_PENDING, type);
  content::NavigationController* controller =
      content::Source<content::NavigationController>(source).ptr();
  InfoBarService* infobar_service =
      InfoBarService::FromWebContents(controller->GetWebContents());
  // Because we're listening to all sources, there may be no
  // InfoBarService for some notifications, e.g. navigations in
  // bubbles/balloons etc.
  if (infobar_service) {
    google_url_tracker()->OnNavigationPending(
        controller,
        infobar_service,
        controller->GetPendingEntry()->GetUniqueID());
  }
}
