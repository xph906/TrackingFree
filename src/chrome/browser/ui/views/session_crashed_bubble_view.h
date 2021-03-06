// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SESSION_CRASHED_BUBBLE_VIEW_H_
#define CHROME_BROWSER_UI_VIEWS_SESSION_CRASHED_BUBBLE_VIEW_H_

#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/views/bubble/bubble_delegate.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/styled_label_listener.h"

namespace views {
class Checkbox;
class GridLayout;
class LabelButton;
class Widget;
}

namespace content {
class WebContents;
class RenderViewHost;
}

class Browser;

// It creates a session restore request bubble when the previous session has
// crashed. It also presents an option to enable metrics reporting, if it not
// enabled already.
class SessionCrashedBubbleView
    : public views::BubbleDelegateView,
      public views::ButtonListener,
      public views::StyledLabelListener,
      public content::WebContentsObserver,
      public content::NotificationObserver,
      public TabStripModelObserver {
 public:
  static void Show(Browser* browser);

 private:
  SessionCrashedBubbleView(views::View* anchor_view,
                           Browser* browser,
                           content::WebContents* web_contents);
  virtual ~SessionCrashedBubbleView();

  // WidgetDelegateView methods.
  virtual views::View* GetInitiallyFocusedView() OVERRIDE;

  // views::BubbleDelegateView methods.
  virtual void Init() OVERRIDE;

  // views::ButtonListener methods.
  virtual void ButtonPressed(views::Button* sender,
                             const ui::Event& event) OVERRIDE;

  // views::StyledLabelListener methods.
  virtual void StyledLabelLinkClicked(const gfx::Range& range,
                                      int event_flags) OVERRIDE;

  // content::WebContentsObserver methods.
  virtual void DidStartNavigationToPendingEntry(
      const GURL& url,
      content::NavigationController::ReloadType reload_type) OVERRIDE;
  virtual void DidFinishLoad(
      int64 frame_id,
      const GURL& validated_url,
      bool is_main_frame,
      content::RenderViewHost* render_view_host) OVERRIDE;
  virtual void WasShown() OVERRIDE;
  virtual void WasHidden() OVERRIDE;

  // content::NotificationObserver methods.
  virtual void Observe(
      int type,
      const content::NotificationSource& source,
      const content::NotificationDetails& details) OVERRIDE;

  // TabStripModelObserver methods.
  // When the tab with current bubble is being dragged and dropped to a new
  // window or to another window, the bubble will be dismissed as if the user
  // chose not to restore the previous session.
  virtual void TabDetachedAt(
      content::WebContents* contents,
      int index) OVERRIDE;

  // Create the view for user to opt in to Uma.
  void CreateUmaOptinView(views::GridLayout* layout);

  // Restore previous session after user selects so.
  void RestorePreviousSession(views::Button* sender);

  // Close and destroy the bubble.
  void CloseBubble();

  content::NotificationRegistrar registrar_;

  // Used for opening the question mark link as well as access the tab strip.
  Browser* browser_;

  // The web content associated with current bubble.
  content::WebContents* web_contents_;

  // Button for the user to confirm a session restore.
  views::LabelButton* restore_button_;

  // Button for the user to close this bubble.
  views::LabelButton* close_;

  // Checkbox for the user to opt-in to UMA reporting.
  views::Checkbox* uma_option_;

  // Whether or not a navigation has started on current tab.
  bool started_navigation_;

  DISALLOW_COPY_AND_ASSIGN(SessionCrashedBubbleView);
};

#endif  // CHROME_BROWSER_UI_VIEWS_SESSION_CRASHED_BUBBLE_VIEW_H_
