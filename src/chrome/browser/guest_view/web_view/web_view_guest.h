// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GUEST_VIEW_WEB_VIEW_WEB_VIEW_GUEST_H_
#define CHROME_BROWSER_GUEST_VIEW_WEB_VIEW_WEB_VIEW_GUEST_H_

#include <vector>

#include "base/observer_list.h"
#include "chrome/browser/extensions/tab_helper.h"
#include "chrome/browser/guest_view/guest_view.h"
#include "chrome/browser/guest_view/web_view/javascript_dialog_helper.h"
#include "chrome/browser/guest_view/web_view/web_view_find_helper.h"
#include "chrome/browser/guest_view/web_view/web_view_permission_types.h"
#include "chrome/common/extensions/api/webview.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/WebKit/public/web/WebFindOptions.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/accessibility/accessibility_manager.h"
#endif

namespace webview_api = extensions::api::webview;

class RenderViewContextMenu;

namespace extensions {
class ScriptExecutor;
class WebviewFindFunction;
}  // namespace extensions

namespace ui {
class SimpleMenuModel;
}  // namespace ui

// A WebViewGuest is a WebContentsObserver on the guest WebContents of a
// <webview> tag. It provides the browser-side implementation of the <webview>
// API and manages the lifetime of <webview> extension events. WebViewGuest is
// created on attachment. That is, when a guest WebContents is associated with
// a particular embedder WebContents. This happens on either initial navigation
// or through the use of the New Window API, when a new window is attached to
// a particular <webview>.
class WebViewGuest : public GuestView<WebViewGuest>,
                     public content::NotificationObserver,
                     public content::WebContentsObserver {
 public:
  WebViewGuest(int guest_instance_id,
               content::WebContents* guest_web_contents,
               const std::string& embedder_extension_id);

  // Returns guestview::kInstanceIDNone if |contents| does not correspond to a
  // WebViewGuest.
  static int GetViewInstanceId(content::WebContents* contents);
  static const char Type[];

  typedef std::vector<linked_ptr<webview_api::ContextMenuItem> > MenuItemVector;
  // Shows the context menu for the guest.
  // |items| acts as a filter. This restricts the current context's default
  // menu items to contain only the items from |items|.
  // |items| == NULL means no filtering will be applied.
  void ShowContextMenu(int request_id, const MenuItemVector* items);

  // GuestViewBase implementation.
  virtual void Attach(content::WebContents* embedder_web_contents,
                      const base::DictionaryValue& args) OVERRIDE;

  // BrowserPluginGuestDelegate public implementation.
  virtual bool HandleContextMenu(
      const content::ContextMenuParams& params) OVERRIDE;

  // GuestDelegate implementation.
  virtual void AddMessageToConsole(int32 level,
                                   const base::string16& message,
                                   int32 line_no,
                                   const base::string16& source_id) OVERRIDE;
  virtual void LoadProgressed(double progress) OVERRIDE;
  virtual void Close() OVERRIDE;
  virtual void DidAttach() OVERRIDE;
  virtual void EmbedderDestroyed() OVERRIDE;
  virtual void FindReply(int request_id,
                         int number_of_matches,
                         const gfx::Rect& selection_rect,
                         int active_match_ordinal,
                         bool final_update) OVERRIDE;
  virtual void GuestProcessGone(base::TerminationStatus status) OVERRIDE;
  virtual void HandleKeyboardEvent(
      const content::NativeWebKeyboardEvent& event) OVERRIDE;
  virtual bool IsDragAndDropEnabled() OVERRIDE;
  virtual void RendererResponsive() OVERRIDE;
  virtual void RendererUnresponsive() OVERRIDE;
  virtual void SizeChanged(const gfx::Size& old_size, const gfx::Size& new_size)
      OVERRIDE;
  virtual void RequestMediaAccessPermission(
      const content::MediaStreamRequest& request,
      const content::MediaResponseCallback& callback) OVERRIDE;
  virtual void CanDownload(const std::string& request_method,
                           const GURL& url,
                           const base::Callback<void(bool)>& callback) OVERRIDE;
  virtual void RequestPointerLockPermission(
      bool user_gesture,
      bool last_unlocked_by_target,
      const base::Callback<void(bool)>& callback) OVERRIDE;
  virtual content::JavaScriptDialogManager*
      GetJavaScriptDialogManager() OVERRIDE;
  virtual content::ColorChooser* OpenColorChooser(
      content::WebContents* web_contents,
      SkColor color,
      const std::vector<content::ColorSuggestion>& suggestions) OVERRIDE;
  virtual void RunFileChooser(
      content::WebContents* web_contents,
      const content::FileChooserParams& params) OVERRIDE;
  virtual void NavigateGuest(const std::string& src) OVERRIDE;
  virtual void Destroy() OVERRIDE;
  virtual void AddNewContents(content::WebContents* source,
                              content::WebContents* new_contents,
                              WindowOpenDisposition disposition,
                              const gfx::Rect& initial_pos,
                              bool user_gesture,
                              bool* was_blocked) OVERRIDE;
  virtual content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params) OVERRIDE;
  virtual void WebContentsCreated(content::WebContents* source_contents,
                                  int opener_render_frame_id,
                                  const base::string16& frame_name,
                                  const GURL& target_url,
                                  content::WebContents* new_contents) OVERRIDE;

  // NotificationObserver implementation.
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE;

  // Set the zoom factor.
  virtual void SetZoom(double zoom_factor) OVERRIDE;

  // Returns the current zoom factor.
  double GetZoom();

  // Begin or continue a find request.
  void Find(const base::string16& search_text,
            const blink::WebFindOptions& options,
            scoped_refptr<extensions::WebviewFindFunction> find_function);

  // Conclude a find request to clear highlighting.
  void StopFinding(content::StopFindAction);

  // If possible, navigate the guest to |relative_index| entries away from the
  // current navigation entry.
  void Go(int relative_index);

  // Reload the guest.
  void Reload();

  typedef base::Callback<void(bool /* allow */,
                              const std::string& /* user_input */)>
      PermissionResponseCallback;
  int RequestPermission(
      WebViewPermissionType permission_type,
      const base::DictionaryValue& request_info,
      const PermissionResponseCallback& callback,
      bool allowed_by_default);

  // Requests Geolocation Permission from the embedder.
  void RequestGeolocationPermission(int bridge_id,
                                    const GURL& requesting_frame,
                                    bool user_gesture,
                                    const base::Callback<void(bool)>& callback);

  void OnWebViewGeolocationPermissionResponse(
      int bridge_id,
      bool user_gesture,
      const base::Callback<void(bool)>& callback,
      bool allow,
      const std::string& user_input);

  void CancelGeolocationPermissionRequest(int bridge_id);

  void OnWebViewMediaPermissionResponse(
      const content::MediaStreamRequest& request,
      const content::MediaResponseCallback& callback,
      bool allow,
      const std::string& user_input);

  void OnWebViewDownloadPermissionResponse(
      const base::Callback<void(bool)>& callback,
      bool allow,
      const std::string& user_input);

  void OnWebViewPointerLockPermissionResponse(
      const base::Callback<void(bool)>& callback,
      bool allow,
      const std::string& user_input);

  void OnWebViewNewWindowResponse(int new_window_instance_id,
                                  bool allow,
                                  const std::string& user_input);

  enum PermissionResponseAction {
    DENY,
    ALLOW,
    DEFAULT
  };

  enum SetPermissionResult {
    SET_PERMISSION_INVALID,
    SET_PERMISSION_ALLOWED,
    SET_PERMISSION_DENIED
  };

  // Responds to the permission request |request_id| with |action| and
  // |user_input|. Returns whether there was a pending request for the provided
  // |request_id|.
  SetPermissionResult SetPermission(int request_id,
                                    PermissionResponseAction action,
                                    const std::string& user_input);

  // Overrides the user agent for this guest.
  // This affects subsequent guest navigations.
  void SetUserAgentOverride(const std::string& user_agent_override);

  // Stop loading the guest.
  void Stop();

  // Kill the guest process.
  void Terminate();

  // Clears data in the storage partition of this guest.
  //
  // Partition data that are newer than |removal_since| will be removed.
  // |removal_mask| corresponds to bitmask in StoragePartition::RemoveDataMask.
  bool ClearData(const base::Time remove_since,
                 uint32 removal_mask,
                 const base::Closure& callback);

  extensions::ScriptExecutor* script_executor() {
    return script_executor_.get();
  }

 private:
  virtual ~WebViewGuest();

  // A map to store the callback for a request keyed by the request's id.
  struct PermissionResponseInfo {
    PermissionResponseCallback callback;
    WebViewPermissionType permission_type;
    bool allowed_by_default;
    PermissionResponseInfo();
    PermissionResponseInfo(const PermissionResponseCallback& callback,
                           WebViewPermissionType permission_type,
                           bool allowed_by_default);
    ~PermissionResponseInfo();
  };

  static void RecordUserInitiatedUMA(const PermissionResponseInfo& info,
                                     bool allow);

  // Returns the top level items (ignoring submenus) as Value.
  static scoped_ptr<base::ListValue> MenuModelToValue(
      const ui::SimpleMenuModel& menu_model);

  // WebContentsObserver implementation.
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
  virtual void DidStartProvisionalLoadForFrame(
      int64 frame_id,
      int64 parent_frame_id,
      bool is_main_frame,
      const GURL& validated_url,
      bool is_error_page,
      bool is_iframe_srcdoc,
      content::RenderViewHost* render_view_host) OVERRIDE;
  virtual void DocumentLoadedInFrame(
      int64 frame_id,
      content::RenderViewHost* render_view_host) OVERRIDE;
  virtual void DidStopLoading(
      content::RenderViewHost* render_view_host) OVERRIDE;
  virtual void WebContentsDestroyed() OVERRIDE;
  virtual void UserAgentOverrideSet(const std::string& user_agent) OVERRIDE;

  // Called after the load handler is called in the guest's main frame.
  void LoadHandlerCalled();

  // Called when a redirect notification occurs.
  void LoadRedirect(const GURL& old_url,
                    const GURL& new_url,
                    bool is_top_level);

  void AddWebViewToExtensionRendererState();
  static void RemoveWebViewFromExtensionRendererState(
      content::WebContents* web_contents);

#if defined(OS_CHROMEOS)
  // Notification of a change in the state of an accessibility setting.
  void OnAccessibilityStatusChanged(
    const chromeos::AccessibilityStatusEventDetails& details);
#endif

  void InjectChromeVoxIfNeeded(content::RenderViewHost* render_view_host);

  // Bridge IDs correspond to a geolocation request. This method will remove
  // the bookkeeping for a particular geolocation request associated with the
  // provided |bridge_id|. It returns the request ID of the geolocation request.
  int RemoveBridgeID(int bridge_id);

  void LoadURLWithParams(const GURL& url,
                         const content::Referrer& referrer,
                         content::PageTransition transition_type,
                         content::WebContents* web_contents);

  void RequestNewWindowPermission(
      WindowOpenDisposition disposition,
      const gfx::Rect& initial_bounds,
      bool user_gesture,
      content::WebContents* new_contents);

  // Destroy unattached new windows that have been opened by this
  // WebViewGuest.
  void DestroyUnattachedWindows();

  // Requests resolution of a potentially relative URL.
  GURL ResolveURL(const std::string& src);

  // Notification that a load in the guest resulted in abort. Note that |url|
  // may be invalid.
  void LoadAbort(bool is_top_level,
                 const GURL& url,
                 const std::string& error_type);


  // Creates a new guest window owned by this WebViewGuest.
  WebViewGuest* CreateNewGuestWindow(const content::OpenURLParams& params);

  bool HandleKeyboardShortcuts(const content::NativeWebKeyboardEvent& event);

  ObserverList<extensions::TabHelper::ScriptExecutionObserver>
      script_observers_;
  scoped_ptr<extensions::ScriptExecutor> script_executor_;

  content::NotificationRegistrar notification_registrar_;

  // A counter to generate a unique request id for a context menu request.
  // We only need the ids to be unique for a given WebViewGuest.
  int pending_context_menu_request_id_;

  // A counter to generate a unique request id for a permission request.
  // We only need the ids to be unique for a given WebViewGuest.
  int next_permission_request_id_;

  typedef std::map<int, PermissionResponseInfo> RequestMap;
  RequestMap pending_permission_requests_;

  // True if the user agent is overridden.
  bool is_overriding_user_agent_;

  // Indicates that the page needs to be reloaded once it has been attached to
  // an embedder.
  bool pending_reload_on_attachment_;

  // Main frame ID of last committed page.
  int64 main_frame_id_;

  // Set to |true| if ChromeVox was already injected in main frame.
  bool chromevox_injected_;

  // Stores the current zoom factor.
  double current_zoom_factor_;

  // Handles find requests and replies for the webview find API.
  WebviewFindHelper find_helper_;

  // Handles the JavaScript dialog requests.
  JavaScriptDialogHelper javascript_dialog_helper_;

  friend void WebviewFindHelper::DispatchFindUpdateEvent(bool canceled,
                                                         bool final_update);

  // Holds the RenderViewContextMenu that has been built but yet to be
  // shown. This is .Reset() after ShowContextMenu().
  scoped_ptr<RenderViewContextMenu> pending_menu_;

#if defined(OS_CHROMEOS)
  // Subscription to receive notifications on changes to a11y settings.
  scoped_ptr<chromeos::AccessibilityStatusSubscription>
      accessibility_subscription_;
#endif

  std::map<int, int> bridge_id_to_request_id_map_;

  // Tracks the name, and target URL of the new window. Once the first
  // navigation commits, we no longer track this information.
  struct NewWindowInfo {
    GURL url;
    std::string name;
    NewWindowInfo(const GURL& url, const std::string& name) :
        url(url),
        name(name) {}
  };

  typedef std::map<WebViewGuest*, NewWindowInfo> PendingWindowMap;
  PendingWindowMap pending_new_windows_;

  DISALLOW_COPY_AND_ASSIGN(WebViewGuest);
};

#endif  // CHROME_BROWSER_GUEST_VIEW_WEB_VIEW_WEB_VIEW_GUEST_H_
