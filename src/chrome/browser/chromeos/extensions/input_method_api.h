// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_EXTENSIONS_INPUT_METHOD_API_H_
#define CHROME_BROWSER_CHROMEOS_EXTENSIONS_INPUT_METHOD_API_H_

#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "extensions/browser/browser_context_keyed_api_factory.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_function.h"

namespace chromeos {
class ExtensionInputMethodEventRouter;
}

namespace extensions {

// Implements the experimental.inputMethod.get method.
class GetInputMethodFunction : public SyncExtensionFunction {
 public:
  GetInputMethodFunction();

 protected:
  virtual ~GetInputMethodFunction();

  virtual bool RunSync() OVERRIDE;

 private:
  DECLARE_EXTENSION_FUNCTION("inputMethodPrivate.get", INPUTMETHODPRIVATE_GET)
};

// Notify the initialization is done to input method engine.
// TODO(nona): remove this function.
class StartImeFunction : public SyncExtensionFunction {
 public:
  StartImeFunction();

 protected:
  virtual ~StartImeFunction();

  virtual bool RunSync() OVERRIDE;

 private:
  DECLARE_EXTENSION_FUNCTION("inputMethodPrivate.startIme",
                             INPUTMETHODPRIVATE_STARTIME)
};

class InputMethodAPI : public BrowserContextKeyedAPI,
                       public extensions::EventRouter::Observer {
 public:
  static const char kOnInputMethodChanged[];

  explicit InputMethodAPI(content::BrowserContext* context);
  virtual ~InputMethodAPI();

  // Returns input method name for the given XKB (X keyboard extensions in X
  // Window System) id.
  static std::string GetInputMethodForXkb(const std::string& xkb_id);

  // BrowserContextKeyedAPI implementation.
  static BrowserContextKeyedAPIFactory<InputMethodAPI>* GetFactoryInstance();

  // BrowserContextKeyedAPI implementation.
  virtual void Shutdown() OVERRIDE;

  // EventRouter::Observer implementation.
  virtual void OnListenerAdded(const extensions::EventListenerInfo& details)
      OVERRIDE;

 private:
  friend class BrowserContextKeyedAPIFactory<InputMethodAPI>;

  // BrowserContextKeyedAPI implementation.
  static const char* service_name() {
    return "InputMethodAPI";
  }
  static const bool kServiceIsNULLWhileTesting = true;

  content::BrowserContext* const context_;

  // Created lazily upon OnListenerAdded.
  scoped_ptr<chromeos::ExtensionInputMethodEventRouter>
      input_method_event_router_;

  DISALLOW_COPY_AND_ASSIGN(InputMethodAPI);
};

}  // namespace extensions

#endif  // CHROME_BROWSER_CHROMEOS_EXTENSIONS_INPUT_METHOD_API_H_
