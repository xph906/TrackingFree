// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_ASH_ACCESSIBILITY_AUTOMATION_MANAGER_VIEWS_H_
#define CHROME_BROWSER_UI_ASH_ACCESSIBILITY_AUTOMATION_MANAGER_VIEWS_H_

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"

#include "chrome/browser/ui/ash/accessibility/ax_tree_source_views.h"
#include "ui/accessibility/ax_tree_serializer.h"

template <typename T> struct DefaultSingletonTraits;

namespace content {
class BrowserContext;
}  // namespace content

namespace views {
class AXAuraObjWrapper;
class View;
}  // namespace views

// Manages a tree of automation nodes.
class AutomationManagerViews {
 public:
  // Get the single instance of this class.
  static AutomationManagerViews* GetInstance();

  // Enable automation support for views.
  void Enable(content::BrowserContext* context);

  // Disable automation support for views.
  void Disable();

  // Handle an event fired upon a |View|.
  void HandleEvent(content::BrowserContext* context,
                   views::View* view,
                   ui::AXEvent event_type);

 private:
  friend struct DefaultSingletonTraits<AutomationManagerViews>;

  AutomationManagerViews();
  ~AutomationManagerViews();

    // Reset all state in this manager.
  void Reset();

  void SendEvent(content::BrowserContext* context,
                 views::AXAuraObjWrapper* aura_obj,
                 ui::AXEvent event_type);

  // Whether automation support for views is enabled.
  bool enabled_;

  // Holds the active views-based accessibility tree. A tree currently consists
  // of all views descendant to a |Widget| (see |AXTreeSourceViews|).
  // A tree becomes active when an event is fired on a descendant view.
  scoped_ptr <AXTreeSourceViews> current_tree_;

  // Serializes incremental updates on the currently active tree
  // |current_tree_|.
  scoped_ptr<ui::AXTreeSerializer<views::AXAuraObjWrapper*> >
      current_tree_serializer_;

  DISALLOW_COPY_AND_ASSIGN(AutomationManagerViews);
};

#endif  // CHROME_BROWSER_UI_ASH_ACCESSIBILITY_AUTOMATION_MANAGER_VIEWS_H_
