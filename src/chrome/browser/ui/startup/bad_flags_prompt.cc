// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/startup/bad_flags_prompt.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/infobars/simple_alert_infobar_delegate.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/switch_utils.h"
#include "components/invalidation/invalidation_switches.h"
#include "components/nacl/common/nacl_switches.h"
#include "components/startup_metric_utils/startup_metric_utils.h"
#include "components/translate/core/common/translate_switches.h"
#include "content/public/common/content_switches.h"
#include "extensions/common/switches.h"
#include "google_apis/gaia/gaia_switches.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"

namespace chrome {

void ShowBadFlagsPrompt(Browser* browser) {
  content::WebContents* web_contents =
      browser->tab_strip_model()->GetActiveWebContents();
  if (!web_contents)
    return;

  // Unsupported flags for which to display a warning that "stability and
  // security will suffer".
  static const char* kBadFlags[] = {
    // These flags disable sandbox-related security.
    switches::kDisableGpuSandbox,
    switches::kDisableSeccompFilterSandbox,
    switches::kDisableSetuidSandbox,
    switches::kDisableWebSecurity,
    switches::kNaClDangerousNoSandboxNonSfi,
    switches::kNoSandbox,
    switches::kSingleProcess,

    // These flags disable or undermine the Same Origin Policy.
    switches::kTrustedSpdyProxy,
    translate::switches::kTranslateSecurityOrigin,

    // These flags undermine HTTPS / connection security.
    switches::kDisableUserMediaSecurity,
#if defined(ENABLE_WEBRTC)
    switches::kDisableWebRtcEncryption,
#endif
    switches::kIgnoreCertificateErrors,
    switches::kReduceSecurityForTesting,
    invalidation::switches::kSyncAllowInsecureXmppConnection,

    // These flags change the URLs that handle PII.
    autofill::switches::kWalletSecureServiceUrl,
    switches::kGaiaUrl,
    translate::switches::kTranslateScriptURL,

    // This flag gives extensions more powers.
    extensions::switches::kExtensionsOnChromeURLs,

#if defined(OS_LINUX) && !defined(OS_CHROMEOS)
    // Speech dispatcher is buggy, it can crash and it can make Chrome freeze.
    // http://crbug.com/327295
    switches::kEnableSpeechDispatcher,
#endif
    NULL
  };

  for (const char** flag = kBadFlags; *flag; ++flag) {
    if (CommandLine::ForCurrentProcess()->HasSwitch(*flag)) {
      SimpleAlertInfoBarDelegate::Create(
          InfoBarService::FromWebContents(web_contents),
          infobars::InfoBarDelegate::kNoIconID,
          l10n_util::GetStringFUTF16(IDS_BAD_FLAGS_WARNING_MESSAGE,
                                     base::UTF8ToUTF16(
                                         std::string("--") + *flag)),
          false);
      return;
    }
  }
}

void MaybeShowInvalidUserDataDirWarningDialog() {
  const base::FilePath& user_data_dir = GetInvalidSpecifiedUserDataDir();
  if (user_data_dir.empty())
    return;

  startup_metric_utils::SetNonBrowserUIDisplayed();

  // Ensure the ResourceBundle is initialized for string resource access.
  bool cleanup_resource_bundle = false;
  if (!ResourceBundle::HasSharedInstance()) {
    cleanup_resource_bundle = true;
    std::string locale = l10n_util::GetApplicationLocale(std::string());
    const char kUserDataDirDialogFallbackLocale[] = "en-US";
    if (locale.empty())
      locale = kUserDataDirDialogFallbackLocale;
    ResourceBundle::InitSharedInstanceWithLocale(locale, NULL);
  }

  const base::string16& title =
      l10n_util::GetStringUTF16(IDS_CANT_WRITE_USER_DIRECTORY_TITLE);
  const base::string16& message =
      l10n_util::GetStringFUTF16(IDS_CANT_WRITE_USER_DIRECTORY_SUMMARY,
                                 user_data_dir.LossyDisplayName());

  if (cleanup_resource_bundle)
    ResourceBundle::CleanupSharedInstance();

  // More complex dialogs cannot be shown before the earliest calls here.
  ShowMessageBox(NULL, title, message, chrome::MESSAGE_BOX_TYPE_WARNING);
}

}  // namespace chrome
