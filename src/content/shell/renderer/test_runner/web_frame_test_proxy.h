// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_SHELL_RENDERER_TEST_RUNNER_WEB_FRAME_TEST_PROXY_H_
#define CONTENT_SHELL_RENDERER_TEST_RUNNER_WEB_FRAME_TEST_PROXY_H_

#include "base/basictypes.h"
#include "content/shell/renderer/test_runner/TestInterfaces.h"
#include "content/shell/renderer/test_runner/WebTestDelegate.h"
#include "content/shell/renderer/test_runner/test_runner.h"
#include "content/shell/renderer/test_runner/web_test_proxy.h"
#include "third_party/WebKit/public/platform/WebString.h"

namespace content {

// Templetized wrapper around RenderFrameImpl objects, which implement
// the WebFrameClient interface.
template <class Base, typename P, typename R>
class WebFrameTestProxy : public Base {
 public:
  WebFrameTestProxy(P p, R r) : Base(p, r), base_proxy_(NULL) {}

  virtual ~WebFrameTestProxy() {}

  void set_base_proxy(WebTestProxyBase* proxy) { base_proxy_ = proxy; }

  // WebFrameClient implementation.
  virtual blink::WebPlugin* createPlugin(blink::WebLocalFrame* frame,
                                         const blink::WebPluginParams& params) {
    blink::WebPlugin* plugin = base_proxy_->CreatePlugin(frame, params);
    if (plugin) return plugin;
    return Base::createPlugin(frame, params);
  }

  virtual void didAddMessageToConsole(const blink::WebConsoleMessage& message,
                                      const blink::WebString& sourceName,
                                      unsigned sourceLine,
                                      const blink::WebString& stackTrace) {
    base_proxy_->DidAddMessageToConsole(message, sourceName, sourceLine);
    Base::didAddMessageToConsole(message, sourceName, sourceLine, stackTrace);
  }

  virtual bool canCreatePluginWithoutRenderer(
      const blink::WebString& mimeType) {
    using blink::WebString;

    const CR_DEFINE_STATIC_LOCAL(WebString, suffix,
                                 ("-can-create-without-renderer"));
    return mimeType.utf8().find(suffix.utf8()) != std::string::npos;
  }

  virtual void loadURLExternally(blink::WebLocalFrame* frame,
                                 const blink::WebURLRequest& request,
                                 blink::WebNavigationPolicy policy,
                                 const blink::WebString& suggested_name) {
    base_proxy_->LoadURLExternally(frame, request, policy, suggested_name);
    Base::loadURLExternally(frame, request, policy, suggested_name);
  }

  virtual void didStartProvisionalLoad(blink::WebLocalFrame* frame) {
    base_proxy_->DidStartProvisionalLoad(frame);
    Base::didStartProvisionalLoad(frame);
  }

  virtual void didReceiveServerRedirectForProvisionalLoad(
      blink::WebLocalFrame* frame) {
    base_proxy_->DidReceiveServerRedirectForProvisionalLoad(frame);
    Base::didReceiveServerRedirectForProvisionalLoad(frame);
  }

  virtual void didFailProvisionalLoad(blink::WebLocalFrame* frame,
                                      const blink::WebURLError& error) {
    // If the test finished, don't notify the embedder of the failed load,
    // as we already destroyed the document loader.
    if (base_proxy_->DidFailProvisionalLoad(frame, error)) return;
    Base::didFailProvisionalLoad(frame, error);
  }

  virtual void didCommitProvisionalLoad(
      blink::WebLocalFrame* frame, const blink::WebHistoryItem& item,
      blink::WebHistoryCommitType commit_type) {
    base_proxy_->DidCommitProvisionalLoad(frame, item, commit_type);
    Base::didCommitProvisionalLoad(frame, item, commit_type);
  }

  virtual void didReceiveTitle(blink::WebLocalFrame* frame,
                               const blink::WebString& title,
                               blink::WebTextDirection direction) {
    base_proxy_->DidReceiveTitle(frame, title, direction);
    Base::didReceiveTitle(frame, title, direction);
  }

  virtual void didChangeIcon(blink::WebLocalFrame* frame,
                             blink::WebIconURL::Type iconType) {
    base_proxy_->DidChangeIcon(frame, iconType);
    Base::didChangeIcon(frame, iconType);
  }

  virtual void didFinishDocumentLoad(blink::WebLocalFrame* frame) {
    base_proxy_->DidFinishDocumentLoad(frame);
    Base::didFinishDocumentLoad(frame);
  }

  virtual void didHandleOnloadEvents(blink::WebLocalFrame* frame) {
    base_proxy_->DidHandleOnloadEvents(frame);
    Base::didHandleOnloadEvents(frame);
  }

  virtual void didFailLoad(blink::WebLocalFrame* frame,
                           const blink::WebURLError& error) {
    base_proxy_->DidFailLoad(frame, error);
    Base::didFailLoad(frame, error);
  }

  virtual void didFinishLoad(blink::WebLocalFrame* frame) {
    base_proxy_->DidFinishLoad(frame);
    Base::didFinishLoad(frame);
  }

  virtual blink::WebNotificationPresenter* notificationPresenter() {
    return base_proxy_->GetNotificationPresenter();
  }

  virtual void didChangeSelection(bool is_selection_empty) {
    base_proxy_->DidChangeSelection(is_selection_empty);
    Base::didChangeSelection(is_selection_empty);
  }

  virtual blink::WebColorChooser* createColorChooser(
      blink::WebColorChooserClient* client,
      const blink::WebColor& initial_color,
      const blink::WebVector<blink::WebColorSuggestion>& suggestions) {
    return base_proxy_->CreateColorChooser(client, initial_color, suggestions);
  }

  virtual void runModalAlertDialog(const blink::WebString& message) {
    base_proxy_->delegate_->printMessage(std::string("ALERT: ") +
                                         message.utf8().data() + "\n");
  }

  virtual bool runModalConfirmDialog(const blink::WebString& message) {
    base_proxy_->delegate_->printMessage(std::string("CONFIRM: ") +
                                         message.utf8().data() + "\n");
    return true;
  }

  virtual bool runModalPromptDialog(const blink::WebString& message,
                                    const blink::WebString& defaultValue,
                                    blink::WebString*) {
    base_proxy_->delegate_->printMessage(
        std::string("PROMPT: ") + message.utf8().data() + ", default text: " +
        defaultValue.utf8().data() + "\n");
    return true;
  }

  virtual bool runModalBeforeUnloadDialog(bool is_reload,
                                          const blink::WebString& message) {
    base_proxy_->delegate_->printMessage(std::string("CONFIRM NAVIGATION: ") +
                                         message.utf8().data() + "\n");
    return !base_proxy_->test_interfaces_->testRunner()
                ->shouldStayOnPageAfterHandlingBeforeUnload();
  }

  virtual void showContextMenu(
      const blink::WebContextMenuData& contextMenuData) {
    base_proxy_->ShowContextMenu(Base::GetWebFrame()->toWebLocalFrame(),
                                 contextMenuData);
    Base::showContextMenu(contextMenuData);
  }

  virtual void didDetectXSS(blink::WebLocalFrame* frame,
                            const blink::WebURL& insecureURL,
                            bool didBlockEntirePage) {
    // This is not implemented in RenderFrameImpl, so need to explicitly call
    // into the base proxy.
    base_proxy_->DidDetectXSS(frame, insecureURL, didBlockEntirePage);
    Base::didDetectXSS(frame, insecureURL, didBlockEntirePage);
  }

  virtual void didDispatchPingLoader(blink::WebLocalFrame* frame,
                                     const blink::WebURL& url) {
    // This is not implemented in RenderFrameImpl, so need to explicitly call
    // into the base proxy.
    base_proxy_->DidDispatchPingLoader(frame, url);
    Base::didDispatchPingLoader(frame, url);
  }

  virtual void willRequestResource(blink::WebLocalFrame* frame,
                                   const blink::WebCachedURLRequest& request) {
    // This is not implemented in RenderFrameImpl, so need to explicitly call
    // into the base proxy.
    base_proxy_->WillRequestResource(frame, request);
    Base::willRequestResource(frame, request);
  }

  virtual void didCreateDataSource(blink::WebLocalFrame* frame,
                                   blink::WebDataSource* ds) {
    Base::didCreateDataSource(frame, ds);
  }

  virtual void willSendRequest(blink::WebLocalFrame* frame, unsigned identifier,
                               blink::WebURLRequest& request,
                               const blink::WebURLResponse& redirectResponse) {
    base_proxy_->WillSendRequest(frame, identifier, request, redirectResponse);
    Base::willSendRequest(frame, identifier, request, redirectResponse);
  }

  virtual void didReceiveResponse(blink::WebLocalFrame* frame,
                                  unsigned identifier,
                                  const blink::WebURLResponse& response) {
    base_proxy_->DidReceiveResponse(frame, identifier, response);
    Base::didReceiveResponse(frame, identifier, response);
  }

  virtual void didChangeResourcePriority(
      blink::WebLocalFrame* frame, unsigned identifier,
      const blink::WebURLRequest::Priority& priority,
      int intra_priority_value) {
    // This is not implemented in RenderFrameImpl, so need to explicitly call
    // into the base proxy.
    base_proxy_->DidChangeResourcePriority(frame, identifier, priority,
                                           intra_priority_value);
    Base::didChangeResourcePriority(frame, identifier, priority,
                                    intra_priority_value);
  }

  virtual void didFinishResourceLoad(blink::WebLocalFrame* frame,
                                     unsigned identifier) {
    base_proxy_->DidFinishResourceLoad(frame, identifier);
    Base::didFinishResourceLoad(frame, identifier);
  }

  virtual blink::WebNavigationPolicy decidePolicyForNavigation(
      blink::WebLocalFrame* frame, blink::WebDataSource::ExtraData* extraData,
      const blink::WebURLRequest& request, blink::WebNavigationType type,
      blink::WebNavigationPolicy defaultPolicy, bool isRedirect) {
    blink::WebNavigationPolicy policy = base_proxy_->DecidePolicyForNavigation(
        frame, extraData, request, type, defaultPolicy, isRedirect);
    if (policy == blink::WebNavigationPolicyIgnore) return policy;

    return Base::decidePolicyForNavigation(frame, extraData, request, type,
                                           defaultPolicy, isRedirect);
  }

  virtual void willStartUsingPeerConnectionHandler(
      blink::WebLocalFrame* frame,
      blink::WebRTCPeerConnectionHandler* handler) {
    // RenderFrameImpl::willStartUsingPeerConnectionHandler can not be mocked.
    // See http://crbug/363285.
  }

  virtual blink::WebUserMediaClient* userMediaClient() {
    return base_proxy_->GetUserMediaClient();
  }

  virtual bool willCheckAndDispatchMessageEvent(
      blink::WebLocalFrame* sourceFrame, blink::WebFrame* targetFrame,
      blink::WebSecurityOrigin target, blink::WebDOMMessageEvent event) {
    if (base_proxy_->WillCheckAndDispatchMessageEvent(sourceFrame, targetFrame,
                                                      target, event))
      return true;
    return Base::willCheckAndDispatchMessageEvent(sourceFrame, targetFrame,
                                                  target, event);
  }

  virtual void didStopLoading() {
    base_proxy_->DidStopLoading();
    Base::didStopLoading();
  }

 private:
  WebTestProxyBase* base_proxy_;

  DISALLOW_COPY_AND_ASSIGN(WebFrameTestProxy);
};

}  // namespace content

#endif  // CONTENT_SHELL_RENDERER_TEST_RUNNER_WEB_FRAME_TEST_PROXY_H_
