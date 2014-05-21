// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_SHELL_RENDERER_TEST_RUNNER_WEBTESTRUNNER_H_
#define CONTENT_SHELL_RENDERER_TEST_RUNNER_WEBTESTRUNNER_H_

#include <vector>

namespace blink {
class WebPermissionClient;
}

namespace content {

class WebTestRunner {
public:
    // Returns a mock WebPermissionClient that is used for layout tests. An
    // embedder should use this for all WebViews it creates.
    virtual blink::WebPermissionClient* webPermissions() const = 0;

    // After WebTestDelegate::testFinished was invoked, the following methods
    // can be used to determine what kind of dump the main WebTestProxy can
    // provide.

    // If true, WebTestDelegate::audioData returns an audio dump and no text
    // or pixel results are available.
    virtual bool shouldDumpAsAudio() const = 0;
    virtual void getAudioData(std::vector<unsigned char>* bufferView) const = 0;

    // Returns true if the call to WebTestProxy::captureTree will invoke
    // WebTestDelegate::captureHistoryForWindow.
    virtual bool shouldDumpBackForwardList() const = 0;

    // Returns true if WebTestProxy::capturePixels should be invoked after
    // capturing text results.
    virtual bool shouldGeneratePixelResults() = 0;
};

}  // namespace content

#endif  // CONTENT_SHELL_RENDERER_TEST_RUNNER_WEBTESTRUNNER_H_
