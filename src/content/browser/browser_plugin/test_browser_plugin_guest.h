// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_BROWSER_PLUGIN_TEST_BROWSER_PLUGIN_GUEST_H_
#define CONTENT_BROWSER_BROWSER_PLUGIN_TEST_BROWSER_PLUGIN_GUEST_H_

#include "base/compiler_specific.h"
#include "content/browser/browser_plugin/browser_plugin_guest.h"
#include "content/public/test/test_utils.h"
#include "ui/gfx/size.h"

namespace content {

class RenderViewHost;
class WebContentsImpl;

// Test class for BrowserPluginGuest.
//
// Provides utilities to wait for certain state/messages in BrowserPluginGuest
// to be used in tests.
class TestBrowserPluginGuest : public BrowserPluginGuest {
 public:
  TestBrowserPluginGuest(int instance_id, WebContentsImpl* web_contents);
  virtual ~TestBrowserPluginGuest();

  WebContentsImpl* web_contents() const;

  // Overridden methods from BrowserPluginGuest to intercept in test objects.
  virtual void OnHandleInputEvent(int instance_id,
                                  const gfx::Rect& guest_window_rect,
                                  const blink::WebInputEvent* event) OVERRIDE;
  virtual void OnSetFocus(int instance_id, bool focused) OVERRIDE;
  virtual void OnTakeFocus(bool reverse) OVERRIDE;
  virtual void DidStopLoading(RenderViewHost* render_view_host) OVERRIDE;
  virtual void OnImeCancelComposition() OVERRIDE;

  // Test utilities to wait for a event we are interested in.
  // Waits until UpdateRect message is sent from the guest, meaning it is
  // ready/rendered.
  void WaitForUpdateRectMsg();
  void ResetUpdateRectCount();
  // Waits for focus to reach this guest.
  void WaitForFocus();
  // Waits for blur to reach this guest.
  void WaitForBlur();
  // Waits for focus to move out of this guest.
  void WaitForAdvanceFocus();
  // Waits until input is observed.
  void WaitForInput();
  // Waits until 'loadstop' is observed.
  void WaitForLoadStop();
  // Waits until UpdateRect with a particular |view_size| is observed.
  void WaitForViewSize(const gfx::Size& view_size);
  // Waits until IME cancellation is observed.
  void WaitForImeCancel();

  ui::TextInputType last_text_input_type() {
    return last_text_input_type_;
  }

 private:
  // Overridden methods from BrowserPluginGuest to intercept in test objects.
  virtual void SendMessageToEmbedder(IPC::Message* msg) OVERRIDE;

  int update_rect_count_;
  bool focus_observed_;
  bool blur_observed_;
  bool advance_focus_observed_;
  bool input_observed_;
  bool load_stop_observed_;
  bool ime_cancel_observed_;
  gfx::Size last_view_size_observed_;
  gfx::Size expected_auto_view_size_;

  scoped_refptr<MessageLoopRunner> send_message_loop_runner_;
  scoped_refptr<MessageLoopRunner> focus_message_loop_runner_;
  scoped_refptr<MessageLoopRunner> blur_message_loop_runner_;
  scoped_refptr<MessageLoopRunner> advance_focus_message_loop_runner_;
  scoped_refptr<MessageLoopRunner> input_message_loop_runner_;
  scoped_refptr<MessageLoopRunner> load_stop_message_loop_runner_;
  scoped_refptr<MessageLoopRunner> auto_view_size_message_loop_runner_;
  scoped_refptr<MessageLoopRunner> ime_cancel_message_loop_runner_;

  DISALLOW_COPY_AND_ASSIGN(TestBrowserPluginGuest);
};

}  // namespace content

#endif  // CONTENT_BROWSER_BROWSER_PLUGIN_TEST_BROWSER_PLUGIN_GUEST_H_
