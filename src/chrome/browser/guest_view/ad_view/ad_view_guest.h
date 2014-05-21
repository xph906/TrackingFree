// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GUEST_VIEW_AD_VIEW_AD_VIEW_GUEST_H_
#define CHROME_BROWSER_GUEST_VIEW_AD_VIEW_AD_VIEW_GUEST_H_

#include "base/values.h"
#include "chrome/browser/guest_view/guest_view.h"
#include "content/public/browser/web_contents_observer.h"

// An AdViewGuest is a WebContentsObserver on the guest WebContents of a
// <adview> tag. It provides the browser-side implementation of the <adview>
// API and manages the lifetime of <adview> extension events. AdViewGuest is
// created on attachment. When a guest WebContents is associated with
// a particular embedder WebContents, we call this "attachment".
// TODO(fsamuel): There might be an opportunity here to refactor and reuse code
// between AdViewGuest and WebViewGuest.
class AdViewGuest : public GuestView<AdViewGuest>,
                    public content::WebContentsObserver {
 public:
  AdViewGuest(int guest_instance_id,
              content::WebContents* guest_web_contents,
              const std::string& extension_id);

  static const char Type[];

 private:
  virtual ~AdViewGuest();

  virtual void DidCommitProvisionalLoadForFrame(
      int64 frame_id,
      const base::string16& frame_unique_name,
      bool is_main_frame,
      const GURL& url,
      content::PageTransition transition_type,
      content::RenderViewHost* render_view_host) OVERRIDE;
  virtual void DidFailProvisionalLoad(
      int64 frame_id,
      const base::string16& frame_unique_name,
      bool is_main_frame,
      const GURL& validated_url,
      int error_code,
      const base::string16& error_description,
      content::RenderViewHost* render_view_host) OVERRIDE;

  DISALLOW_COPY_AND_ASSIGN(AdViewGuest);
};

#endif  // CHROME_BROWSER_GUEST_VIEW_AD_VIEW_AD_VIEW_GUEST_H_
