// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/printing/print_error_dialog.h"

#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "content/public/browser/browser_thread.h"
#include "grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"

namespace chrome {

namespace {

void ShowPrintErrorDialogTask() {
  Browser* browser = chrome::FindLastActiveWithHostDesktopType(
      chrome::GetActiveDesktop());
  ShowMessageBox(browser ? browser->window()->GetNativeWindow() : NULL,
                 l10n_util::GetStringUTF16(IDS_PRINT_SPOOL_FAILED_TITLE_TEXT),
                 l10n_util::GetStringUTF16(IDS_PRINT_SPOOL_FAILED_ERROR_TEXT),
                 MESSAGE_BOX_TYPE_WARNING);
}

}  // namespace

void ShowPrintErrorDialog() {
  // Nested loop may destroy caller.
  content::BrowserThread::PostTask(content::BrowserThread::UI, FROM_HERE,
                                   base::Bind(&ShowPrintErrorDialogTask));
}

}  // namespace chrome
