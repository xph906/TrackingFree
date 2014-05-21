// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GUEST_VIEW_GUEST_VIEW_MANAGER_H_
#define CHROME_BROWSER_GUEST_VIEW_GUEST_VIEW_MANAGER_H_

#include <map>

#include "base/lazy_instance.h"
#include "base/macros.h"
#include "content/public/browser/browser_plugin_guest_manager.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"

class GuestViewBase;
class GuestWebContentsObserver;
class GURL;

namespace content {
class BrowserContext;
}  // namespace content

class GuestViewManager : public content::BrowserPluginGuestManager,
                         public base::SupportsUserData::Data {
 public:
  explicit GuestViewManager(content::BrowserContext* context);
  virtual ~GuestViewManager();

  static GuestViewManager* FromBrowserContext(content::BrowserContext* context);

  // Returns the guest WebContents associated with the given |guest_instance_id|
  // if the provided |embedder_render_process_id| is allowed to access it.
  // If the embedder is not allowed access, the embedder will be killed, and
  // this method will return NULL. If no WebContents exists with the given
  // instance ID, then NULL will also be returned.
  content::WebContents* GetGuestByInstanceIDSafely(
      int guest_instance_id,
      int embedder_render_process_id);

  // BrowserPluginGuestManager implementation.
  virtual content::WebContents* CreateGuest(
      content::SiteInstance* embedder_site_instance,
      int instance_id,
      const std::string& storage_partition_id,
      bool persist_storage,
      scoped_ptr<base::DictionaryValue> extra_params) OVERRIDE;
  virtual int GetNextInstanceID() OVERRIDE;
  virtual void MaybeGetGuestByInstanceIDOrKill(
      int guest_instance_id,
      int embedder_render_process_id,
      const GuestByInstanceIDCallback& callback) OVERRIDE;
  virtual bool ForEachGuest(content::WebContents* embedder_web_contents,
                            const GuestCallback& callback) OVERRIDE;

 private:
  friend class GuestViewBase;
  friend class GuestWebContentsObserver;

  void AddGuest(int guest_instance_id,
                content::WebContents* guest_web_contents);

  void RemoveGuest(int guest_instance_id);

  void AddRenderProcessHostID(int render_process_host_id);

  content::SiteInstance* GetGuestSiteInstance(
      const GURL& guest_site);

  content::WebContents* GetGuestByInstanceID(
      int guest_instance_id,
      int embedder_render_process_id);

  bool CanEmbedderAccessInstanceIDMaybeKill(
      int embedder_render_process_id,
      int guest_instance_id);

  bool CanEmbedderAccessInstanceID(int embedder_render_process_id,
                                   int guest_instance_id);

  static bool CanEmbedderAccessGuest(int embedder_render_process_id,
                                     GuestViewBase* guest);

  // Counts RenderProcessHost IDs of GuestViewBases.
  std::multiset<int> render_process_host_id_multiset_;

  // Contains guests' WebContents, mapping from their instance ids.
  typedef std::map<int, content::WebContents*> GuestInstanceMap;
  GuestInstanceMap guest_web_contents_by_instance_id_;

  int current_instance_id_;
  content::BrowserContext* context_;

  DISALLOW_COPY_AND_ASSIGN(GuestViewManager);
};

#endif  // CHROME_BROWSER_GUEST_VIEW_GUEST_VIEW_MANAGER_H_
