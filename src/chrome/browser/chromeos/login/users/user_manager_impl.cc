// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/users/user_manager_impl.h"

#include <cstddef>
#include <set>

#include "ash/multi_profile_uma.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/files/file_path.h"
#include "base/format_macros.h"
#include "base/logging.h"
#include "base/metrics/histogram.h"
#include "base/prefs/pref_registry_simple.h"
#include "base/prefs/pref_service.h"
#include "base/prefs/scoped_user_pref_update.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/sys_info.h"
#include "base/threading/worker_pool.h"
#include "base/values.h"
#include "chrome/browser/app_mode/app_mode_utils.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/chromeos/base/locale_util.h"
#include "chrome/browser/chromeos/login/auth/user_context.h"
#include "chrome/browser/chromeos/login/demo_mode/demo_app_launcher.h"
#include "chrome/browser/chromeos/login/login_utils.h"
#include "chrome/browser/chromeos/login/signin/auth_sync_observer.h"
#include "chrome/browser/chromeos/login/signin/auth_sync_observer_factory.h"
#include "chrome/browser/chromeos/login/ui/login_display.h"
#include "chrome/browser/chromeos/login/users/avatar/user_image_manager_impl.h"
#include "chrome/browser/chromeos/login/users/multi_profile_user_controller.h"
#include "chrome/browser/chromeos/login/users/remove_user_delegate.h"
#include "chrome/browser/chromeos/login/users/supervised_user_manager_impl.h"
#include "chrome/browser/chromeos/login/wizard_controller.h"
#include "chrome/browser/chromeos/net/network_portal_detector.h"
#include "chrome/browser/chromeos/net/network_portal_detector_strategy.h"
#include "chrome/browser/chromeos/policy/browser_policy_connector_chromeos.h"
#include "chrome/browser/chromeos/policy/device_local_account.h"
#include "chrome/browser/chromeos/profiles/multiprofiles_session_aborted_dialog.h"
#include "chrome/browser/chromeos/profiles/profile_helper.h"
#include "chrome/browser/chromeos/session_length_limiter.h"
#include "chrome/browser/managed_mode/chromeos/managed_user_password_service_factory.h"
#include "chrome/browser/managed_mode/chromeos/manager_password_service_factory.h"
#include "chrome/browser/net/nss_context.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profiles_state.h"
#include "chrome/browser/sync/profile_sync_service.h"
#include "chrome/browser/sync/profile_sync_service_factory.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/crash_keys.h"
#include "chrome/common/pref_names.h"
#include "chromeos/cert_loader.h"
#include "chromeos/chromeos_switches.h"
#include "chromeos/cryptohome/async_method_caller.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/login/login_state.h"
#include "chromeos/settings/cros_settings_names.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_service.h"
#include "google_apis/gaia/gaia_auth_util.h"
#include "google_apis/gaia/google_service_auth_error.h"
#include "policy/policy_constants.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/wm/core/wm_core_switches.h"

using content::BrowserThread;

namespace chromeos {
namespace {

// A vector pref of the the regular users known on this device, arranged in LRU
// order.
const char kRegularUsers[] = "LoggedInUsers";

// A vector pref of the public accounts defined on this device.
const char kPublicAccounts[] = "PublicAccounts";

// A string pref that gets set when a public account is removed but a user is
// currently logged into that account, requiring the account's data to be
// removed after logout.
const char kPublicAccountPendingDataRemoval[] =
    "PublicAccountPendingDataRemoval";

// A dictionary that maps user IDs to the displayed name.
const char kUserDisplayName[] = "UserDisplayName";

// A dictionary that maps user IDs to the user's given name.
const char kUserGivenName[] = "UserGivenName";

// A dictionary that maps user IDs to the displayed (non-canonical) emails.
const char kUserDisplayEmail[] = "UserDisplayEmail";

// A dictionary that maps user IDs to OAuth token presence flag.
const char kUserOAuthTokenStatus[] = "OAuthTokenStatus";

// A dictionary that maps user IDs to a flag indicating whether online
// authentication against GAIA should be enforced during the next sign-in.
const char kUserForceOnlineSignin[] = "UserForceOnlineSignin";

// A string pref containing the ID of the last user who logged in if it was
// a regular user or an empty string if it was another type of user (guest,
// kiosk, public account, etc.).
const char kLastLoggedInRegularUser[] = "LastLoggedInRegularUser";

// Upper bound for a histogram metric reporting the amount of time between
// one regular user logging out and a different regular user logging in.
const int kLogoutToLoginDelayMaxSec = 1800;

// Callback that is called after user removal is complete.
void OnRemoveUserComplete(const std::string& user_email,
                          bool success,
                          cryptohome::MountError return_code) {
  // Log the error, but there's not much we can do.
  if (!success) {
    LOG(ERROR) << "Removal of cryptohome for " << user_email
               << " failed, return code: " << return_code;
  }
}

// Helper function that copies users from |users_list| to |users_vector| and
// |users_set|. Duplicates and users already present in |existing_users| are
// skipped.
void ParseUserList(const base::ListValue& users_list,
                   const std::set<std::string>& existing_users,
                   std::vector<std::string>* users_vector,
                   std::set<std::string>* users_set) {
  users_vector->clear();
  users_set->clear();
  for (size_t i = 0; i < users_list.GetSize(); ++i) {
    std::string email;
    if (!users_list.GetString(i, &email) || email.empty()) {
      LOG(ERROR) << "Corrupt entry in user list at index " << i << ".";
      continue;
    }
    if (existing_users.find(email) != existing_users.end() ||
        !users_set->insert(email).second) {
      LOG(ERROR) << "Duplicate user: " << email;
      continue;
    }
    users_vector->push_back(email);
  }
}

class UserHashMatcher {
 public:
  explicit UserHashMatcher(const std::string& h) : username_hash(h) {}
  bool operator()(const User* user) const {
    return user->username_hash() == username_hash;
  }

 private:
  const std::string& username_hash;
};

// Runs on SequencedWorkerPool thread. Passes resolved locale to
// |on_resolve_callback| on UI thread.
void ResolveLocale(
    const std::string& raw_locale,
    base::Callback<void(const std::string&)> on_resolve_callback) {
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::UI));
  std::string resolved_locale;
  // Ignore result
  l10n_util::CheckAndResolveLocale(raw_locale, &resolved_locale);
  BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
      base::Bind(on_resolve_callback, resolved_locale));
}

// Callback to GetNSSCertDatabaseForProfile. It starts CertLoader using the
// provided NSS database. It must be called for primary user only.
void OnGetNSSCertDatabaseForUser(net::NSSCertDatabase* database) {
  if (!CertLoader::IsInitialized())
    return;

  CertLoader::Get()->StartWithNSSDB(database);
}

}  // namespace

// static
void UserManager::RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(kRegularUsers);
  registry->RegisterListPref(kPublicAccounts);
  registry->RegisterStringPref(kPublicAccountPendingDataRemoval, "");
  registry->RegisterStringPref(kLastLoggedInRegularUser, "");
  registry->RegisterDictionaryPref(kUserDisplayName);
  registry->RegisterDictionaryPref(kUserGivenName);
  registry->RegisterDictionaryPref(kUserDisplayEmail);
  registry->RegisterDictionaryPref(kUserOAuthTokenStatus);
  registry->RegisterDictionaryPref(kUserForceOnlineSignin);
  SupervisedUserManager::RegisterPrefs(registry);
  SessionLengthLimiter::RegisterPrefs(registry);
}

UserManagerImpl::UserManagerImpl()
    : cros_settings_(CrosSettings::Get()),
      device_local_account_policy_service_(NULL),
      user_loading_stage_(STAGE_NOT_LOADED),
      active_user_(NULL),
      primary_user_(NULL),
      session_started_(false),
      user_sessions_restored_(false),
      is_current_user_owner_(false),
      is_current_user_new_(false),
      is_current_user_ephemeral_regular_user_(false),
      ephemeral_users_enabled_(false),
      supervised_user_manager_(new SupervisedUserManagerImpl(this)),
      manager_creation_time_(base::TimeTicks::Now()) {
  UpdateNumberOfUsers();
  // UserManager instance should be used only on UI thread.
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  registrar_.Add(this, chrome::NOTIFICATION_OWNERSHIP_STATUS_CHANGED,
      content::NotificationService::AllSources());
  registrar_.Add(this, chrome::NOTIFICATION_LOGIN_USER_PROFILE_PREPARED,
      content::NotificationService::AllSources());
  registrar_.Add(this,
                 chrome::NOTIFICATION_PROFILE_CREATED,
                 content::NotificationService::AllSources());
  RetrieveTrustedDevicePolicies();
  local_accounts_subscription_ = cros_settings_->AddSettingsObserver(
      kAccountsPrefDeviceLocalAccounts,
      base::Bind(&UserManagerImpl::RetrieveTrustedDevicePolicies,
                 base::Unretained(this)));
  supervised_users_subscription_ = cros_settings_->AddSettingsObserver(
      kAccountsPrefSupervisedUsersEnabled,
      base::Bind(&UserManagerImpl::RetrieveTrustedDevicePolicies,
                 base::Unretained(this)));
  multi_profile_user_controller_.reset(new MultiProfileUserController(
      this, g_browser_process->local_state()));

  policy::BrowserPolicyConnectorChromeOS* connector =
      g_browser_process->platform_part()->browser_policy_connector_chromeos();
  avatar_policy_observer_.reset(new policy::CloudExternalDataPolicyObserver(
      cros_settings_,
      this,
      connector->GetDeviceLocalAccountPolicyService(),
      policy::key::kUserAvatarImage,
      this));
  avatar_policy_observer_->Init();

  wallpaper_policy_observer_.reset(new policy::CloudExternalDataPolicyObserver(
      cros_settings_,
      this,
      connector->GetDeviceLocalAccountPolicyService(),
      policy::key::kWallpaperImage,
      this));
  wallpaper_policy_observer_->Init();

  UpdateLoginState();
}

UserManagerImpl::~UserManagerImpl() {
  // Can't use STLDeleteElements because of the private destructor of User.
  for (UserList::iterator it = users_.begin(); it != users_.end();
       it = users_.erase(it)) {
    DeleteUser(*it);
  }
  // These are pointers to the same User instances that were in users_ list.
  logged_in_users_.clear();
  lru_logged_in_users_.clear();

  DeleteUser(active_user_);
}

void UserManagerImpl::Shutdown() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  local_accounts_subscription_.reset();
  supervised_users_subscription_.reset();
  // Stop the session length limiter.
  session_length_limiter_.reset();

  if (device_local_account_policy_service_)
    device_local_account_policy_service_->RemoveObserver(this);

  for (UserImageManagerMap::iterator it = user_image_managers_.begin(),
                                     ie = user_image_managers_.end();
       it != ie; ++it) {
    it->second->Shutdown();
  }
  multi_profile_user_controller_.reset();
  avatar_policy_observer_.reset();
  wallpaper_policy_observer_.reset();
  registrar_.RemoveAll();
}

MultiProfileUserController* UserManagerImpl::GetMultiProfileUserController() {
  return multi_profile_user_controller_.get();
}

UserImageManager* UserManagerImpl::GetUserImageManager(
    const std::string& user_id) {
  UserImageManagerMap::iterator ui = user_image_managers_.find(user_id);
  if (ui != user_image_managers_.end())
    return ui->second.get();
  linked_ptr<UserImageManagerImpl> mgr(new UserImageManagerImpl(user_id, this));
  user_image_managers_[user_id] = mgr;
  return mgr.get();
}

SupervisedUserManager* UserManagerImpl::GetSupervisedUserManager() {
  return supervised_user_manager_.get();
}

const UserList& UserManagerImpl::GetUsers() const {
  const_cast<UserManagerImpl*>(this)->EnsureUsersLoaded();
  return users_;
}

UserList UserManagerImpl::GetUsersAdmittedForMultiProfile() const {
  // Supervised users are not allowed to use multi profile.
  if (logged_in_users_.size() == 1 &&
      GetPrimaryUser()->GetType() != User::USER_TYPE_REGULAR)
    return UserList();

  UserList result;
  int num_users_allowed = 0;
  const UserList& users = GetUsers();
  for (UserList::const_iterator it = users.begin(); it != users.end(); ++it) {
    if ((*it)->GetType() == User::USER_TYPE_REGULAR && !(*it)->is_logged_in()) {
      MultiProfileUserController::UserAllowedInSessionResult check =
          multi_profile_user_controller_->
              IsUserAllowedInSession((*it)->email());
      if (check == MultiProfileUserController::
              NOT_ALLOWED_PRIMARY_USER_POLICY_FORBIDS) {
        return UserList();
      }

      // Users with a policy that prevents them being added to a session will be
      // shown in login UI but will be grayed out.
      if (check == MultiProfileUserController::ALLOWED ||
          check == MultiProfileUserController::NOT_ALLOWED_POLICY_FORBIDS) {
        result.push_back(*it);
        if (check == MultiProfileUserController::ALLOWED)
          num_users_allowed++;
      }
    }
  }

  // We only show multi-profiles sign in UI if there's at least one user that
  // is allowed to be added to the session.
  if (!num_users_allowed)
    result.clear();

  return result;
}

const UserList& UserManagerImpl::GetLoggedInUsers() const {
  return logged_in_users_;
}

const UserList& UserManagerImpl::GetLRULoggedInUsers() {
  // If there is no user logged in, we return the active user as the only one.
  if (lru_logged_in_users_.empty() && active_user_) {
    temp_single_logged_in_users_.clear();
    temp_single_logged_in_users_.insert(temp_single_logged_in_users_.begin(),
                                        active_user_);
    return temp_single_logged_in_users_;
  }
  return lru_logged_in_users_;
}

UserList UserManagerImpl::GetUnlockUsers() const {
  const UserList& logged_in_users = GetLoggedInUsers();
  if (logged_in_users.empty())
    return UserList();

  UserList unlock_users;
  Profile* profile = GetProfileByUser(primary_user_);
  std::string primary_behavior =
      profile->GetPrefs()->GetString(prefs::kMultiProfileUserBehavior);

  // Specific case: only one logged in user or
  // primary user has primary-only multi-profile policy.
  if (logged_in_users.size() == 1 ||
      primary_behavior == MultiProfileUserController::kBehaviorPrimaryOnly) {
    if (primary_user_->can_lock())
      unlock_users.push_back(primary_user_);
  } else {
    // Fill list of potential unlock users based on multi-profile policy state.
    for (UserList::const_iterator it = logged_in_users.begin();
         it != logged_in_users.end(); ++it) {
      User* user = (*it);
      Profile* profile = GetProfileByUser(user);
      const std::string behavior =
          profile->GetPrefs()->GetString(prefs::kMultiProfileUserBehavior);
      if (behavior == MultiProfileUserController::kBehaviorUnrestricted &&
          user->can_lock()) {
        unlock_users.push_back(user);
      } else if (behavior == MultiProfileUserController::kBehaviorPrimaryOnly) {
        NOTREACHED()
            << "Spotted primary-only multi-profile policy for non-primary user";
      }
    }
  }

  return unlock_users;
}

const std::string& UserManagerImpl::GetOwnerEmail() {
  return owner_email_;
}

void UserManagerImpl::UserLoggedIn(const std::string& user_id,
                                   const std::string& username_hash,
                                   bool browser_restart) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  User* user = FindUserInListAndModify(user_id);
  if (active_user_ && user) {
    user->set_is_logged_in(true);
    user->set_username_hash(username_hash);
    logged_in_users_.push_back(user);
    lru_logged_in_users_.push_back(user);
    // Reset the new user flag if the user already exists.
    is_current_user_new_ = false;
    NotifyUserAddedToSession(user);
    // Remember that we need to switch to this user as soon as profile ready.
    pending_user_switch_ = user_id;
    return;
  }

  policy::DeviceLocalAccount::Type device_local_account_type;
  if (user_id == UserManager::kGuestUserName) {
    GuestUserLoggedIn();
  } else if (user_id == UserManager::kRetailModeUserName) {
    RetailModeUserLoggedIn();
  } else if (policy::IsDeviceLocalAccountUser(user_id,
                                              &device_local_account_type) &&
             device_local_account_type ==
                 policy::DeviceLocalAccount::TYPE_KIOSK_APP) {
    KioskAppLoggedIn(user_id);
  } else if (DemoAppLauncher::IsDemoAppSession(user_id)) {
    DemoAccountLoggedIn();
  } else {
    EnsureUsersLoaded();

    if (user && user->GetType() == User::USER_TYPE_PUBLIC_ACCOUNT) {
      PublicAccountUserLoggedIn(user);
    } else if ((user && user->GetType() == User::USER_TYPE_LOCALLY_MANAGED) ||
               (!user && gaia::ExtractDomainName(user_id) ==
                    UserManager::kLocallyManagedUserDomain)) {
      LocallyManagedUserLoggedIn(user_id);
    } else if (browser_restart && user_id == g_browser_process->local_state()->
                   GetString(kPublicAccountPendingDataRemoval)) {
      PublicAccountUserLoggedIn(User::CreatePublicAccountUser(user_id));
    } else if (user_id != owner_email_ && !user &&
               (AreEphemeralUsersEnabled() || browser_restart)) {
      RegularUserLoggedInAsEphemeral(user_id);
    } else {
      RegularUserLoggedIn(user_id);
    }

    // Initialize the session length limiter and start it only if
    // session limit is defined by the policy.
    session_length_limiter_.reset(new SessionLengthLimiter(NULL,
                                                           browser_restart));
  }
  DCHECK(active_user_);
  active_user_->set_is_logged_in(true);
  active_user_->set_is_active(true);
  active_user_->set_username_hash(username_hash);

  // Place user who just signed in to the top of the logged in users.
  logged_in_users_.insert(logged_in_users_.begin(), active_user_);
  SetLRUUser(active_user_);

  if (!primary_user_) {
    primary_user_ = active_user_;
    if (primary_user_->GetType() == User::USER_TYPE_REGULAR)
      SendRegularUserLoginMetrics(user_id);
  }

  UMA_HISTOGRAM_ENUMERATION("UserManager.LoginUserType",
                            active_user_->GetType(), User::NUM_USER_TYPES);

  g_browser_process->local_state()->SetString(kLastLoggedInRegularUser,
    (active_user_->GetType() == User::USER_TYPE_REGULAR) ? user_id : "");

  NotifyOnLogin();
}

void UserManagerImpl::SwitchActiveUser(const std::string& user_id) {
  User* user = FindUserAndModify(user_id);
  if (!user) {
    NOTREACHED() << "Switching to a non-existing user";
    return;
  }
  if (user == active_user_) {
    NOTREACHED() << "Switching to a user who is already active";
    return;
  }
  if (!user->is_logged_in()) {
    NOTREACHED() << "Switching to a user that is not logged in";
    return;
  }
  if (user->GetType() != User::USER_TYPE_REGULAR) {
    NOTREACHED() << "Switching to a non-regular user";
    return;
  }
  if (user->username_hash().empty()) {
    NOTREACHED() << "Switching to a user that doesn't have username_hash set";
    return;
  }

  DCHECK(active_user_);
  active_user_->set_is_active(false);
  user->set_is_active(true);
  active_user_ = user;

  // Move the user to the front.
  SetLRUUser(active_user_);

  NotifyActiveUserHashChanged(active_user_->username_hash());
  NotifyActiveUserChanged(active_user_);
}

void UserManagerImpl::RestoreActiveSessions() {
  DBusThreadManager::Get()->GetSessionManagerClient()->RetrieveActiveSessions(
      base::Bind(&UserManagerImpl::OnRestoreActiveSessions,
                 base::Unretained(this)));
}

void UserManagerImpl::SessionStarted() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  session_started_ = true;
  UpdateLoginState();
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_SESSION_STARTED,
      content::Source<UserManager>(this),
      content::Details<const User>(active_user_));
  if (is_current_user_new_) {
    // Make sure that the new user's data is persisted to Local State.
    g_browser_process->local_state()->CommitPendingWrite();
  }
}

void UserManagerImpl::RemoveUser(const std::string& user_id,
                                 RemoveUserDelegate* delegate) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  const User* user = FindUser(user_id);
  if (!user || (user->GetType() != User::USER_TYPE_REGULAR &&
                user->GetType() != User::USER_TYPE_LOCALLY_MANAGED))
    return;

  // Sanity check: we must not remove single user unless it's an enterprise
  // device. This check may seem redundant at a first sight because
  // this single user must be an owner and we perform special check later
  // in order not to remove an owner. However due to non-instant nature of
  // ownership assignment this later check may sometimes fail.
  // See http://crosbug.com/12723
  policy::BrowserPolicyConnectorChromeOS* connector =
      g_browser_process->platform_part()
          ->browser_policy_connector_chromeos();
  if (users_.size() < 2 && !connector->IsEnterpriseManaged())
    return;

  // Sanity check: do not allow any of the the logged in users to be removed.
  for (UserList::const_iterator it = logged_in_users_.begin();
       it != logged_in_users_.end(); ++it) {
    if ((*it)->email() == user_id)
      return;
  }

  RemoveUserInternal(user_id, delegate);
}

void UserManagerImpl::RemoveUserInternal(const std::string& user_email,
                                         RemoveUserDelegate* delegate) {
  CrosSettings* cros_settings = CrosSettings::Get();

  // Ensure the value of owner email has been fetched.
  if (CrosSettingsProvider::TRUSTED != cros_settings->PrepareTrustedValues(
          base::Bind(&UserManagerImpl::RemoveUserInternal,
                     base::Unretained(this),
                     user_email, delegate))) {
    // Value of owner email is not fetched yet.  RemoveUserInternal will be
    // called again after fetch completion.
    return;
  }
  std::string owner;
  cros_settings->GetString(kDeviceOwner, &owner);
  if (user_email == owner) {
    // Owner is not allowed to be removed from the device.
    return;
  }
  RemoveNonOwnerUserInternal(user_email, delegate);
}

void UserManagerImpl::RemoveNonOwnerUserInternal(const std::string& user_email,
                                                 RemoveUserDelegate* delegate) {
  if (delegate)
    delegate->OnBeforeUserRemoved(user_email);
  RemoveUserFromList(user_email);
  cryptohome::AsyncMethodCaller::GetInstance()->AsyncRemove(
      user_email, base::Bind(&OnRemoveUserComplete, user_email));

  if (delegate)
    delegate->OnUserRemoved(user_email);
}

void UserManagerImpl::RemoveUserFromList(const std::string& user_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  RemoveNonCryptohomeData(user_id);
  if (user_loading_stage_ == STAGE_LOADED) {
    DeleteUser(RemoveRegularOrLocallyManagedUserFromList(user_id));
  } else if (user_loading_stage_ == STAGE_LOADING) {
    DCHECK(gaia::ExtractDomainName(user_id) ==
        UserManager::kLocallyManagedUserDomain);
    // Special case, removing partially-constructed supervised user during user
    // list loading.
    ListPrefUpdate users_update(g_browser_process->local_state(),
                                kRegularUsers);
    users_update->Remove(base::StringValue(user_id), NULL);
  } else {
    NOTREACHED() << "Users are not loaded yet.";
    return;
  }
  // Make sure that new data is persisted to Local State.
  g_browser_process->local_state()->CommitPendingWrite();
}

bool UserManagerImpl::IsKnownUser(const std::string& user_id) const {
  return FindUser(user_id) != NULL;
}

const User* UserManagerImpl::FindUser(const std::string& user_id) const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  if (active_user_ && active_user_->email() == user_id)
    return active_user_;
  return FindUserInList(user_id);
}

User* UserManagerImpl::FindUserAndModify(const std::string& user_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  if (active_user_ && active_user_->email() == user_id)
    return active_user_;
  return FindUserInListAndModify(user_id);
}

const User* UserManagerImpl::GetLoggedInUser() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return active_user_;
}

User* UserManagerImpl::GetLoggedInUser() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return active_user_;
}

const User* UserManagerImpl::GetActiveUser() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return active_user_;
}

User* UserManagerImpl::GetActiveUser() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return active_user_;
}

const User* UserManagerImpl::GetPrimaryUser() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return primary_user_;
}

User* UserManagerImpl::GetUserByProfile(Profile* profile) const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  if (ProfileHelper::IsSigninProfile(profile))
    return NULL;

  // Special case for non-CrOS tests that do create several profiles
  // and don't really care about mapping to the real user.
  // Without multi-profiles on Chrome OS such tests always got active_user_.
  // Now these tests will specify special flag to continue working.
  // In future those tests can get a proper CrOS configuration i.e. register
  // and login several users if they want to work with an additional profile.
  if (CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kIgnoreUserProfileMappingForTests)) {
    return active_user_;
  }

  const std::string username_hash =
      ProfileHelper::GetUserIdHashFromProfile(profile);
  const UserList& users = GetUsers();
  const UserList::const_iterator pos = std::find_if(
      users.begin(), users.end(), UserHashMatcher(username_hash));
  if (pos != users.end())
    return *pos;

  // Many tests do not have their users registered with UserManager and
  // runs here. If |active_user_| matches |profile|, returns it.
  return active_user_ &&
                 ProfileHelper::GetProfilePathByUserIdHash(
                     active_user_->username_hash()) == profile->GetPath()
             ? active_user_
             : NULL;
}

Profile* UserManagerImpl::GetProfileByUser(const User* user) const {
  Profile* profile = NULL;
  if (user->is_profile_created())
    profile = ProfileHelper::GetProfileByUserIdHash(user->username_hash());
  else
    profile = ProfileManager::GetActiveUserProfile();

  // GetActiveUserProfile() or GetProfileByUserIdHash() returns a new instance
  // of ProfileImpl(), but actually its OffTheRecordProfile() should be used.
  if (profile && IsLoggedInAsGuest())
    profile = profile->GetOffTheRecordProfile();
  return profile;
}

void UserManagerImpl::SaveUserOAuthStatus(
    const std::string& user_id,
    User::OAuthTokenStatus oauth_token_status) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  DVLOG(1) << "Saving user OAuth token status in Local State";
  User* user = FindUserAndModify(user_id);
  if (user)
    user->set_oauth_token_status(oauth_token_status);

  GetUserFlow(user_id)->HandleOAuthTokenStatusChange(oauth_token_status);

  // Do not update local state if data stored or cached outside the user's
  // cryptohome is to be treated as ephemeral.
  if (IsUserNonCryptohomeDataEphemeral(user_id))
    return;

  PrefService* local_state = g_browser_process->local_state();

  DictionaryPrefUpdate oauth_status_update(local_state, kUserOAuthTokenStatus);
  oauth_status_update->SetWithoutPathExpansion(user_id,
      new base::FundamentalValue(static_cast<int>(oauth_token_status)));
}

void UserManagerImpl::SaveForceOnlineSignin(const std::string& user_id,
                                            bool force_online_signin) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // Do not update local state if data stored or cached outside the user's
  // cryptohome is to be treated as ephemeral.
  if (IsUserNonCryptohomeDataEphemeral(user_id))
    return;

  DictionaryPrefUpdate force_online_update(g_browser_process->local_state(),
                                           kUserForceOnlineSignin);
  force_online_update->SetBooleanWithoutPathExpansion(user_id,
                                                      force_online_signin);
}

void UserManagerImpl::SaveUserDisplayName(const std::string& user_id,
                                          const base::string16& display_name) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (User* user = FindUserAndModify(user_id)) {
    user->set_display_name(display_name);

    // Do not update local state if data stored or cached outside the user's
    // cryptohome is to be treated as ephemeral.
    if (!IsUserNonCryptohomeDataEphemeral(user_id)) {
      PrefService* local_state = g_browser_process->local_state();

      DictionaryPrefUpdate display_name_update(local_state, kUserDisplayName);
      display_name_update->SetWithoutPathExpansion(
          user_id,
          new base::StringValue(display_name));

      supervised_user_manager_->UpdateManagerName(user_id, display_name);
    }
  }
}

base::string16 UserManagerImpl::GetUserDisplayName(
    const std::string& user_id) const {
  const User* user = FindUser(user_id);
  return user ? user->display_name() : base::string16();
}

void UserManagerImpl::SaveUserDisplayEmail(const std::string& user_id,
                                           const std::string& display_email) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  User* user = FindUserAndModify(user_id);
  if (!user)
    return;  // Ignore if there is no such user.

  user->set_display_email(display_email);

  // Do not update local state if data stored or cached outside the user's
  // cryptohome is to be treated as ephemeral.
  if (IsUserNonCryptohomeDataEphemeral(user_id))
    return;

  PrefService* local_state = g_browser_process->local_state();

  DictionaryPrefUpdate display_email_update(local_state, kUserDisplayEmail);
  display_email_update->SetWithoutPathExpansion(
      user_id,
      new base::StringValue(display_email));
}

std::string UserManagerImpl::GetUserDisplayEmail(
    const std::string& user_id) const {
  const User* user = FindUser(user_id);
  return user ? user->display_email() : user_id;
}

void UserManagerImpl::UpdateUserAccountData(
    const std::string& user_id,
    const UserAccountData& account_data) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  SaveUserDisplayName(user_id, account_data.display_name());

  if (User* user = FindUserAndModify(user_id)) {
    base::string16 given_name = account_data.given_name();
    user->set_given_name(given_name);
    if (!IsUserNonCryptohomeDataEphemeral(user_id)) {
      PrefService* local_state = g_browser_process->local_state();

      DictionaryPrefUpdate given_name_update(local_state, kUserGivenName);
      given_name_update->SetWithoutPathExpansion(
          user_id,
          new base::StringValue(given_name));
    }
  }

  UpdateUserAccountLocale(user_id, account_data.locale());
}

// TODO(alemate): http://crbug.com/288941 : Respect preferred language list in
// the Google user profile.
//
// Returns true if callback will be called.
bool UserManagerImpl::RespectLocalePreference(
    Profile* profile,
    const User* user,
    scoped_ptr<locale_util::SwitchLanguageCallback> callback) const {
  if (g_browser_process == NULL)
    return false;
  if ((user == NULL) || (user != GetPrimaryUser()) ||
      (!user->is_profile_created()))
    return false;

  // In case of Multi Profile mode we don't apply profile locale because it is
  // unsafe.
  if (GetLoggedInUsers().size() != 1)
    return false;
  const PrefService* prefs = profile->GetPrefs();
  if (prefs == NULL)
    return false;

  std::string pref_locale;
  const std::string pref_app_locale =
      prefs->GetString(prefs::kApplicationLocale);
  const std::string pref_bkup_locale =
      prefs->GetString(prefs::kApplicationLocaleBackup);

  pref_locale = pref_app_locale;
  if (pref_locale.empty())
    pref_locale = pref_bkup_locale;

  const std::string* account_locale = NULL;
  if (pref_locale.empty() && user->has_gaia_account()) {
    if (user->GetAccountLocale() == NULL)
      return false;  // wait until Account profile is loaded.
    account_locale = user->GetAccountLocale();
    pref_locale = *account_locale;
  }
  const std::string global_app_locale =
      g_browser_process->GetApplicationLocale();
  if (pref_locale.empty())
    pref_locale = global_app_locale;
  DCHECK(!pref_locale.empty());
  LOG(WARNING) << "RespectLocalePreference: "
               << "app_locale='" << pref_app_locale << "', "
               << "bkup_locale='" << pref_bkup_locale << "', "
               << (account_locale != NULL
                       ? (std::string("account_locale='") + (*account_locale) +
                          "'. ")
                       : (std::string("account_locale - unused. ")))
               << " Selected '" << pref_locale << "'";
  profile->ChangeAppLocale(pref_locale, Profile::APP_LOCALE_CHANGED_VIA_LOGIN);

  // Here we don't enable keyboard layouts for normal users. Input methods
  // are set up when the user first logs in. Then the user may customize the
  // input methods.  Hence changing input methods here, just because the user's
  // UI language is different from the login screen UI language, is not
  // desirable. Note that input method preferences are synced, so users can use
  // their farovite input methods as soon as the preferences are synced.
  //
  // For Guest mode, user locale preferences will never get initialized.
  // So input methods should be enabled somewhere.
  const bool enable_layouts = UserManager::Get()->IsLoggedInAsGuest();
  locale_util::SwitchLanguage(pref_locale,
                              enable_layouts,
                              false /* login_layouts_only */,
                              callback.Pass());

  return true;
}

void UserManagerImpl::StopPolicyObserverForTesting() {
  avatar_policy_observer_.reset();
  wallpaper_policy_observer_.reset();
}

void UserManagerImpl::Observe(int type,
                              const content::NotificationSource& source,
                              const content::NotificationDetails& details) {
  switch (type) {
    case chrome::NOTIFICATION_OWNERSHIP_STATUS_CHANGED:
      if (!device_local_account_policy_service_) {
        policy::BrowserPolicyConnectorChromeOS* connector =
            g_browser_process->platform_part()
                ->browser_policy_connector_chromeos();
        device_local_account_policy_service_ =
            connector->GetDeviceLocalAccountPolicyService();
        if (device_local_account_policy_service_)
          device_local_account_policy_service_->AddObserver(this);
      }
      RetrieveTrustedDevicePolicies();
      UpdateOwnership();
      break;
    case chrome::NOTIFICATION_LOGIN_USER_PROFILE_PREPARED: {
      Profile* profile = content::Details<Profile>(details).ptr();
      if (IsUserLoggedIn() &&
          !IsLoggedInAsGuest() &&
          !IsLoggedInAsKioskApp()) {
        if (IsLoggedInAsLocallyManagedUser())
          ManagedUserPasswordServiceFactory::GetForProfile(profile);
        if (IsLoggedInAsRegularUser())
          ManagerPasswordServiceFactory::GetForProfile(profile);

        if (!profile->IsOffTheRecord()) {
          AuthSyncObserver* sync_observer =
              AuthSyncObserverFactory::GetInstance()->GetForProfile(profile);
          sync_observer->StartObserving();
          multi_profile_user_controller_->StartObserving(profile);
        }
      }

      // Now that the user profile has been initialized and
      // |GetNSSCertDatabaseForProfile| is safe to be used, get the NSS cert
      // database for the primary user and start certificate loader with it.
      if (IsUserLoggedIn() &&
          GetPrimaryUser() &&
          profile == GetProfileByUser(GetPrimaryUser()) &&
          CertLoader::IsInitialized() &&
          base::SysInfo::IsRunningOnChromeOS()) {
        GetNSSCertDatabaseForProfile(profile,
                                     base::Bind(&OnGetNSSCertDatabaseForUser));
      }
      break;
    }
    case chrome::NOTIFICATION_PROFILE_CREATED: {
      Profile* profile = content::Source<Profile>(source).ptr();
      User* user = GetUserByProfile(profile);
      if (user != NULL)
        user->set_profile_is_created();
      // If there is pending user switch, do it now.
      if (!pending_user_switch_.empty()) {
        // Call SwitchActiveUser async because otherwise it may cause
        // ProfileManager::GetProfile before the profile gets registered
        // in ProfileManager. It happens in case of sync profile load when
        // NOTIFICATION_PROFILE_CREATED is called synchronously.
        base::MessageLoop::current()->PostTask(FROM_HERE,
          base::Bind(&UserManagerImpl::SwitchActiveUser,
                     base::Unretained(this),
                     pending_user_switch_));
        pending_user_switch_.clear();
      }
      break;
    }
    default:
      NOTREACHED();
  }
}

void UserManagerImpl::OnExternalDataSet(const std::string& policy,
                                        const std::string& user_id) {
  if (policy == policy::key::kUserAvatarImage)
    GetUserImageManager(user_id)->OnExternalDataSet(policy);
  else if (policy == policy::key::kWallpaperImage)
    WallpaperManager::Get()->OnPolicySet(policy, user_id);
  else
    NOTREACHED();
}

void UserManagerImpl::OnExternalDataCleared(const std::string& policy,
                                            const std::string& user_id) {
  if (policy == policy::key::kUserAvatarImage)
    GetUserImageManager(user_id)->OnExternalDataCleared(policy);
  else if (policy == policy::key::kWallpaperImage)
    WallpaperManager::Get()->OnPolicyCleared(policy, user_id);
  else
    NOTREACHED();
}

void UserManagerImpl::OnExternalDataFetched(const std::string& policy,
                                            const std::string& user_id,
                                            scoped_ptr<std::string> data) {
  if (policy == policy::key::kUserAvatarImage)
    GetUserImageManager(user_id)->OnExternalDataFetched(policy, data.Pass());
  else if (policy == policy::key::kWallpaperImage)
    WallpaperManager::Get()->OnPolicyFetched(policy, user_id, data.Pass());
  else
    NOTREACHED();
}

void UserManagerImpl::OnPolicyUpdated(const std::string& user_id) {
  UpdatePublicAccountDisplayName(user_id);
  NotifyUserListChanged();
}

void UserManagerImpl::OnDeviceLocalAccountsChanged() {
  // No action needed here, changes to the list of device-local accounts get
  // handled via the kAccountsPrefDeviceLocalAccounts device setting observer.
}

bool UserManagerImpl::IsCurrentUserOwner() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  base::AutoLock lk(is_current_user_owner_lock_);
  return is_current_user_owner_;
}

void UserManagerImpl::SetCurrentUserIsOwner(bool is_current_user_owner) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  {
    base::AutoLock lk(is_current_user_owner_lock_);
    is_current_user_owner_ = is_current_user_owner;
  }
  UpdateLoginState();
}

bool UserManagerImpl::IsCurrentUserNew() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return is_current_user_new_;
}

bool UserManagerImpl::IsCurrentUserNonCryptohomeDataEphemeral() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() &&
         IsUserNonCryptohomeDataEphemeral(GetLoggedInUser()->email());
}

bool UserManagerImpl::CanCurrentUserLock() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() && active_user_->can_lock() &&
      GetCurrentUserFlow()->CanLockScreen();
}

bool UserManagerImpl::IsUserLoggedIn() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return active_user_;
}

bool UserManagerImpl::IsLoggedInAsRegularUser() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() &&
         active_user_->GetType() == User::USER_TYPE_REGULAR;
}

bool UserManagerImpl::IsLoggedInAsDemoUser() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() &&
         active_user_->GetType() == User::USER_TYPE_RETAIL_MODE;
}

bool UserManagerImpl::IsLoggedInAsPublicAccount() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() &&
      active_user_->GetType() == User::USER_TYPE_PUBLIC_ACCOUNT;
}

bool UserManagerImpl::IsLoggedInAsGuest() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() &&
         active_user_->GetType() == User::USER_TYPE_GUEST;
}

bool UserManagerImpl::IsLoggedInAsLocallyManagedUser() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() &&
      active_user_->GetType() == User::USER_TYPE_LOCALLY_MANAGED;
}

bool UserManagerImpl::IsLoggedInAsKioskApp() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() &&
      active_user_->GetType() == User::USER_TYPE_KIOSK_APP;
}

bool UserManagerImpl::IsLoggedInAsStub() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return IsUserLoggedIn() && active_user_->email() == kStubUser;
}

bool UserManagerImpl::IsSessionStarted() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return session_started_;
}

bool UserManagerImpl::UserSessionsRestored() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  return user_sessions_restored_;
}

bool UserManagerImpl::HasBrowserRestarted() const {
  CommandLine* command_line = CommandLine::ForCurrentProcess();
  return base::SysInfo::IsRunningOnChromeOS() &&
         command_line->HasSwitch(switches::kLoginUser) &&
         !command_line->HasSwitch(switches::kLoginPassword);
}

bool UserManagerImpl::IsUserNonCryptohomeDataEphemeral(
    const std::string& user_id) const {
  // Data belonging to the guest, retail mode and stub users is always
  // ephemeral.
  if (user_id == UserManager::kGuestUserName ||
      user_id == UserManager::kRetailModeUserName ||
      user_id == kStubUser) {
    return true;
  }

  // Data belonging to the owner, anyone found on the user list and obsolete
  // public accounts whose data has not been removed yet is not ephemeral.
  if (user_id == owner_email_  || UserExistsInList(user_id) ||
      user_id == g_browser_process->local_state()->
          GetString(kPublicAccountPendingDataRemoval)) {
    return false;
  }

  // Data belonging to the currently logged-in user is ephemeral when:
  // a) The user logged into a regular account while the ephemeral users policy
  //    was enabled.
  //    - or -
  // b) The user logged into any other account type.
  if (IsUserLoggedIn() && (user_id == GetLoggedInUser()->email()) &&
      (is_current_user_ephemeral_regular_user_ || !IsLoggedInAsRegularUser())) {
    return true;
  }

  // Data belonging to any other user is ephemeral when:
  // a) Going through the regular login flow and the ephemeral users policy is
  //    enabled.
  //    - or -
  // b) The browser is restarting after a crash.
  return AreEphemeralUsersEnabled() || HasBrowserRestarted();
}

void UserManagerImpl::AddObserver(UserManager::Observer* obs) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  observer_list_.AddObserver(obs);
}

void UserManagerImpl::RemoveObserver(UserManager::Observer* obs) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  observer_list_.RemoveObserver(obs);
}

void UserManagerImpl::AddSessionStateObserver(
    UserManager::UserSessionStateObserver* obs) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  session_state_observer_list_.AddObserver(obs);
}

void UserManagerImpl::RemoveSessionStateObserver(
    UserManager::UserSessionStateObserver* obs) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  session_state_observer_list_.RemoveObserver(obs);
}

void UserManagerImpl::NotifyLocalStateChanged() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  FOR_EACH_OBSERVER(UserManager::Observer, observer_list_,
                    LocalStateChanged(this));
}

void UserManagerImpl::OnProfilePrepared(Profile* profile) {
  LoginUtils::Get()->DoBrowserLaunch(profile,
                                     NULL);     // host_, not needed here

  if (!CommandLine::ForCurrentProcess()->HasSwitch(::switches::kTestName)) {
    // Did not log in (we crashed or are debugging), need to restore Sync.
    // TODO(nkostylev): Make sure that OAuth state is restored correctly for all
    // users once it is fully multi-profile aware. http://crbug.com/238987
    // For now if we have other user pending sessions they'll override OAuth
    // session restore for previous users.
    LoginUtils::Get()->RestoreAuthenticationSession(profile);
  }

  // Restore other user sessions if any.
  RestorePendingUserSessions();
}

void UserManagerImpl::EnsureUsersLoaded() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  if (!g_browser_process || !g_browser_process->local_state())
    return;

  if (user_loading_stage_ != STAGE_NOT_LOADED)
    return;
  user_loading_stage_ = STAGE_LOADING;
  // Clean up user list first. All code down the path should be synchronous,
  // so that local state after transaction rollback is in consistent state.
  // This process also should not trigger EnsureUsersLoaded again.
  if (supervised_user_manager_->HasFailedUserCreationTransaction())
    supervised_user_manager_->RollbackUserCreationTransaction();

  PrefService* local_state = g_browser_process->local_state();
  const base::ListValue* prefs_regular_users =
      local_state->GetList(kRegularUsers);
  const base::ListValue* prefs_public_sessions =
      local_state->GetList(kPublicAccounts);
  const base::DictionaryValue* prefs_display_names =
      local_state->GetDictionary(kUserDisplayName);
  const base::DictionaryValue* prefs_given_names =
      local_state->GetDictionary(kUserGivenName);
  const base::DictionaryValue* prefs_display_emails =
      local_state->GetDictionary(kUserDisplayEmail);

  // Load public sessions first.
  std::vector<std::string> public_sessions;
  std::set<std::string> public_sessions_set;
  ParseUserList(*prefs_public_sessions, std::set<std::string>(),
                &public_sessions, &public_sessions_set);
  for (std::vector<std::string>::const_iterator it = public_sessions.begin();
       it != public_sessions.end(); ++it) {
    users_.push_back(User::CreatePublicAccountUser(*it));
    UpdatePublicAccountDisplayName(*it);
  }

  // Load regular users and locally managed users.
  std::vector<std::string> regular_users;
  std::set<std::string> regular_users_set;
  ParseUserList(*prefs_regular_users, public_sessions_set,
                &regular_users, &regular_users_set);
  for (std::vector<std::string>::const_iterator it = regular_users.begin();
       it != regular_users.end(); ++it) {
    User* user = NULL;
    const std::string domain = gaia::ExtractDomainName(*it);
    if (domain == UserManager::kLocallyManagedUserDomain)
      user = User::CreateLocallyManagedUser(*it);
    else
      user = User::CreateRegularUser(*it);
    user->set_oauth_token_status(LoadUserOAuthStatus(*it));
    user->set_force_online_signin(LoadForceOnlineSignin(*it));
    users_.push_back(user);

    base::string16 display_name;
    if (prefs_display_names->GetStringWithoutPathExpansion(*it,
                                                           &display_name)) {
      user->set_display_name(display_name);
    }

    base::string16 given_name;
    if (prefs_given_names->GetStringWithoutPathExpansion(*it, &given_name)) {
      user->set_given_name(given_name);
    }

    std::string display_email;
    if (prefs_display_emails->GetStringWithoutPathExpansion(*it,
                                                            &display_email)) {
      user->set_display_email(display_email);
    }
  }

  user_loading_stage_ = STAGE_LOADED;

  for (UserList::iterator ui = users_.begin(), ue = users_.end();
       ui != ue; ++ui) {
    GetUserImageManager((*ui)->email())->LoadUserImage();
  }
}

void UserManagerImpl::RetrieveTrustedDevicePolicies() {
  ephemeral_users_enabled_ = false;
  owner_email_ = "";

  // Schedule a callback if device policy has not yet been verified.
  if (CrosSettingsProvider::TRUSTED != cros_settings_->PrepareTrustedValues(
          base::Bind(&UserManagerImpl::RetrieveTrustedDevicePolicies,
                     base::Unretained(this)))) {
    return;
  }

  cros_settings_->GetBoolean(kAccountsPrefEphemeralUsersEnabled,
                             &ephemeral_users_enabled_);
  cros_settings_->GetString(kDeviceOwner, &owner_email_);

  EnsureUsersLoaded();

  bool changed = UpdateAndCleanUpPublicAccounts(
      policy::GetDeviceLocalAccounts(cros_settings_));

  // If ephemeral users are enabled and we are on the login screen, take this
  // opportunity to clean up by removing all regular users except the owner.
  if (ephemeral_users_enabled_ && !IsUserLoggedIn()) {
    ListPrefUpdate prefs_users_update(g_browser_process->local_state(),
                                      kRegularUsers);
    prefs_users_update->Clear();
    for (UserList::iterator it = users_.begin(); it != users_.end(); ) {
      const std::string user_email = (*it)->email();
      if ((*it)->GetType() == User::USER_TYPE_REGULAR &&
          user_email != owner_email_) {
        RemoveNonCryptohomeData(user_email);
        DeleteUser(*it);
        it = users_.erase(it);
        changed = true;
      } else {
        if ((*it)->GetType() != User::USER_TYPE_PUBLIC_ACCOUNT)
          prefs_users_update->Append(new base::StringValue(user_email));
        ++it;
      }
    }
  }

  if (changed)
    NotifyUserListChanged();
}

bool UserManagerImpl::AreEphemeralUsersEnabled() const {
  policy::BrowserPolicyConnectorChromeOS* connector =
      g_browser_process->platform_part()->browser_policy_connector_chromeos();
  return ephemeral_users_enabled_ &&
         (connector->IsEnterpriseManaged() || !owner_email_.empty());
}

UserList& UserManagerImpl::GetUsersAndModify() {
  EnsureUsersLoaded();
  return users_;
}

const User* UserManagerImpl::FindUserInList(const std::string& user_id) const {
  const UserList& users = GetUsers();
  for (UserList::const_iterator it = users.begin(); it != users.end(); ++it) {
    if ((*it)->email() == user_id)
      return *it;
  }
  return NULL;
}

const bool UserManagerImpl::UserExistsInList(const std::string& user_id) const {
  PrefService* local_state = g_browser_process->local_state();
  const base::ListValue* user_list = local_state->GetList(kRegularUsers);
  for (size_t i = 0; i < user_list->GetSize(); ++i) {
    std::string email;
    if (user_list->GetString(i, &email) && (user_id == email))
      return true;
  }
  return false;
}

User* UserManagerImpl::FindUserInListAndModify(const std::string& user_id) {
  UserList& users = GetUsersAndModify();
  for (UserList::iterator it = users.begin(); it != users.end(); ++it) {
    if ((*it)->email() == user_id)
      return *it;
  }
  return NULL;
}

void UserManagerImpl::GuestUserLoggedIn() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  active_user_ = User::CreateGuestUser();
  // TODO(nkostylev): Add support for passing guest session cryptohome
  // mount point. Legacy (--login-profile) value will be used for now.
  // http://crosbug.com/230859
  active_user_->SetStubImage(User::kInvalidImageIndex, false);
  // Initializes wallpaper after active_user_ is set.
  WallpaperManager::Get()->SetUserWallpaperNow(UserManager::kGuestUserName);
}

void UserManagerImpl::AddUserRecord(User* user) {
  // Add the user to the front of the user list.
  ListPrefUpdate prefs_users_update(g_browser_process->local_state(),
                                    kRegularUsers);
  prefs_users_update->Insert(0, new base::StringValue(user->email()));
  users_.insert(users_.begin(), user);
}

void UserManagerImpl::RegularUserLoggedIn(const std::string& user_id) {
  // Remove the user from the user list.
  active_user_ = RemoveRegularOrLocallyManagedUserFromList(user_id);

  // If the user was not found on the user list, create a new user.
  is_current_user_new_ = !active_user_;
  if (!active_user_) {
    active_user_ = User::CreateRegularUser(user_id);
    active_user_->set_oauth_token_status(LoadUserOAuthStatus(user_id));
    SaveUserDisplayName(active_user_->email(),
                        base::UTF8ToUTF16(active_user_->GetAccountName(true)));
    WallpaperManager::Get()->SetUserWallpaperNow(user_id);
  }

  AddUserRecord(active_user_);

  GetUserImageManager(user_id)->UserLoggedIn(is_current_user_new_, false);

  WallpaperManager::Get()->EnsureLoggedInUserWallpaperLoaded();

  // Make sure that new data is persisted to Local State.
  g_browser_process->local_state()->CommitPendingWrite();
}

void UserManagerImpl::RegularUserLoggedInAsEphemeral(
    const std::string& user_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  is_current_user_new_ = true;
  is_current_user_ephemeral_regular_user_ = true;
  active_user_ = User::CreateRegularUser(user_id);
  GetUserImageManager(user_id)->UserLoggedIn(is_current_user_new_, false);
  WallpaperManager::Get()->SetUserWallpaperNow(user_id);
}

void UserManagerImpl::LocallyManagedUserLoggedIn(
    const std::string& user_id) {
  // TODO(nkostylev): Refactor, share code with RegularUserLoggedIn().

  // Remove the user from the user list.
  active_user_ = RemoveRegularOrLocallyManagedUserFromList(user_id);
  // If the user was not found on the user list, create a new user.
  if (!active_user_) {
    is_current_user_new_ = true;
    active_user_ = User::CreateLocallyManagedUser(user_id);
    // Leaving OAuth token status at the default state = unknown.
    WallpaperManager::Get()->SetUserWallpaperNow(user_id);
  } else {
    if (supervised_user_manager_->CheckForFirstRun(user_id)) {
      is_current_user_new_ = true;
      WallpaperManager::Get()->SetUserWallpaperNow(user_id);
    } else {
      is_current_user_new_ = false;
    }
  }

  // Add the user to the front of the user list.
  ListPrefUpdate prefs_users_update(g_browser_process->local_state(),
                                    kRegularUsers);
  prefs_users_update->Insert(0, new base::StringValue(user_id));
  users_.insert(users_.begin(), active_user_);

  // Now that user is in the list, save display name.
  if (is_current_user_new_) {
    SaveUserDisplayName(active_user_->email(),
                        active_user_->GetDisplayName());
  }

  GetUserImageManager(user_id)->UserLoggedIn(is_current_user_new_, true);
  WallpaperManager::Get()->EnsureLoggedInUserWallpaperLoaded();

  // Make sure that new data is persisted to Local State.
  g_browser_process->local_state()->CommitPendingWrite();
}

void UserManagerImpl::PublicAccountUserLoggedIn(User* user) {
  is_current_user_new_ = true;
  active_user_ = user;
  // The UserImageManager chooses a random avatar picture when a user logs in
  // for the first time. Tell the UserImageManager that this user is not new to
  // prevent the avatar from getting changed.
  GetUserImageManager(user->email())->UserLoggedIn(false, true);
  WallpaperManager::Get()->EnsureLoggedInUserWallpaperLoaded();
}

void UserManagerImpl::KioskAppLoggedIn(const std::string& app_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  policy::DeviceLocalAccount::Type device_local_account_type;
  DCHECK(policy::IsDeviceLocalAccountUser(app_id,
                                          &device_local_account_type));
  DCHECK_EQ(policy::DeviceLocalAccount::TYPE_KIOSK_APP,
            device_local_account_type);

  active_user_ = User::CreateKioskAppUser(app_id);
  active_user_->SetStubImage(User::kInvalidImageIndex, false);

  WallpaperManager::Get()->SetUserWallpaperNow(app_id);

  // TODO(bartfab): Add KioskAppUsers to the users_ list and keep metadata like
  // the kiosk_app_id in these objects, removing the need to re-parse the
  // device-local account list here to extract the kiosk_app_id.
  const std::vector<policy::DeviceLocalAccount> device_local_accounts =
      policy::GetDeviceLocalAccounts(cros_settings_);
  const policy::DeviceLocalAccount* account = NULL;
  for (std::vector<policy::DeviceLocalAccount>::const_iterator
           it = device_local_accounts.begin();
       it != device_local_accounts.end(); ++it) {
    if (it->user_id == app_id) {
      account = &*it;
      break;
    }
  }
  std::string kiosk_app_id;
  if (account) {
    kiosk_app_id = account->kiosk_app_id;
  } else {
    LOG(ERROR) << "Logged into nonexistent kiosk-app account: " << app_id;
    NOTREACHED();
  }

  CommandLine* command_line = CommandLine::ForCurrentProcess();
  command_line->AppendSwitch(::switches::kForceAppMode);
  command_line->AppendSwitchASCII(::switches::kAppId, kiosk_app_id);

  // Disable window animation since kiosk app runs in a single full screen
  // window and window animation causes start-up janks.
  command_line->AppendSwitch(
      wm::switches::kWindowAnimationsDisabled);
}

void UserManagerImpl::DemoAccountLoggedIn() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  active_user_ = User::CreateKioskAppUser(DemoAppLauncher::kDemoUserName);
  active_user_->SetStubImage(User::kInvalidImageIndex, false);
  WallpaperManager::Get()->SetUserWallpaperNow(DemoAppLauncher::kDemoUserName);

  CommandLine* command_line = CommandLine::ForCurrentProcess();
  command_line->AppendSwitch(::switches::kForceAppMode);
  command_line->AppendSwitchASCII(::switches::kAppId,
                                  DemoAppLauncher::kDemoAppId);

  // Disable window animation since the demo app runs in a single full screen
  // window and window animation causes start-up janks.
  CommandLine::ForCurrentProcess()->AppendSwitch(
      wm::switches::kWindowAnimationsDisabled);
}

void UserManagerImpl::RetailModeUserLoggedIn() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  is_current_user_new_ = true;
  active_user_ = User::CreateRetailModeUser();
  GetUserImageManager(UserManager::kRetailModeUserName)->UserLoggedIn(
      is_current_user_new_,
      true);
  WallpaperManager::Get()->SetUserWallpaperNow(
      UserManager::kRetailModeUserName);
}

void UserManagerImpl::NotifyOnLogin() {
  CHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  UpdateNumberOfUsers();
  NotifyActiveUserHashChanged(active_user_->username_hash());
  NotifyActiveUserChanged(active_user_);
  UpdateLoginState();

  // TODO(nkostylev): Deprecate this notification in favor of
  // ActiveUserChanged() observer call.
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_LOGIN_USER_CHANGED,
      content::Source<UserManager>(this),
      content::Details<const User>(active_user_));

  // Owner must be first user in session. DeviceSettingsService can't deal with
  // multiple user and will mix up ownership, crbug.com/230018.
  if (GetLoggedInUsers().size() == 1) {
    // Indicate to DeviceSettingsService that the owner key may have become
    // available.
    DeviceSettingsService::Get()->SetUsername(active_user_->email());

    if (NetworkPortalDetector::IsInitialized()) {
      NetworkPortalDetector::Get()->SetStrategy(
          PortalDetectorStrategy::STRATEGY_ID_SESSION);
    }
  }
}

User::OAuthTokenStatus UserManagerImpl::LoadUserOAuthStatus(
    const std::string& user_id) const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  PrefService* local_state = g_browser_process->local_state();
  const base::DictionaryValue* prefs_oauth_status =
      local_state->GetDictionary(kUserOAuthTokenStatus);
  int oauth_token_status = User::OAUTH_TOKEN_STATUS_UNKNOWN;
  if (prefs_oauth_status &&
      prefs_oauth_status->GetIntegerWithoutPathExpansion(
          user_id, &oauth_token_status)) {
    User::OAuthTokenStatus result =
        static_cast<User::OAuthTokenStatus>(oauth_token_status);
    if (result == User::OAUTH2_TOKEN_STATUS_INVALID)
      GetUserFlow(user_id)->HandleOAuthTokenStatusChange(result);
    return result;
  }
  return User::OAUTH_TOKEN_STATUS_UNKNOWN;
}

bool UserManagerImpl::LoadForceOnlineSignin(const std::string& user_id) const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  PrefService* local_state = g_browser_process->local_state();
  const base::DictionaryValue* prefs_force_online =
      local_state->GetDictionary(kUserForceOnlineSignin);
  bool force_online_signin = false;
  if (prefs_force_online) {
    prefs_force_online->GetBooleanWithoutPathExpansion(user_id,
                                                       &force_online_signin);
  }
  return force_online_signin;
}

void UserManagerImpl::UpdateOwnership() {
  bool is_owner = DeviceSettingsService::Get()->HasPrivateOwnerKey();
  VLOG(1) << "Current user " << (is_owner ? "is owner" : "is not owner");

  SetCurrentUserIsOwner(is_owner);
}

void UserManagerImpl::RemoveNonCryptohomeData(const std::string& user_id) {
  WallpaperManager::Get()->RemoveUserWallpaperInfo(user_id);
  GetUserImageManager(user_id)->DeleteUserImage();

  PrefService* prefs = g_browser_process->local_state();
  DictionaryPrefUpdate prefs_display_name_update(prefs, kUserDisplayName);
  prefs_display_name_update->RemoveWithoutPathExpansion(user_id, NULL);

  DictionaryPrefUpdate prefs_given_name_update(prefs, kUserGivenName);
  prefs_given_name_update->RemoveWithoutPathExpansion(user_id, NULL);

  DictionaryPrefUpdate prefs_display_email_update(prefs, kUserDisplayEmail);
  prefs_display_email_update->RemoveWithoutPathExpansion(user_id, NULL);

  DictionaryPrefUpdate prefs_oauth_update(prefs, kUserOAuthTokenStatus);
  prefs_oauth_update->RemoveWithoutPathExpansion(user_id, NULL);

  DictionaryPrefUpdate prefs_force_online_update(prefs, kUserForceOnlineSignin);
  prefs_force_online_update->RemoveWithoutPathExpansion(user_id, NULL);

  supervised_user_manager_->RemoveNonCryptohomeData(user_id);

  multi_profile_user_controller_->RemoveCachedValues(user_id);
}

User* UserManagerImpl::RemoveRegularOrLocallyManagedUserFromList(
    const std::string& user_id) {
  ListPrefUpdate prefs_users_update(g_browser_process->local_state(),
                                    kRegularUsers);
  prefs_users_update->Clear();
  User* user = NULL;
  for (UserList::iterator it = users_.begin(); it != users_.end(); ) {
    const std::string user_email = (*it)->email();
    if (user_email == user_id) {
      user = *it;
      it = users_.erase(it);
    } else {
      if ((*it)->GetType() == User::USER_TYPE_REGULAR ||
          (*it)->GetType() == User::USER_TYPE_LOCALLY_MANAGED) {
        prefs_users_update->Append(new base::StringValue(user_email));
      }
      ++it;
    }
  }
  return user;
}

void UserManagerImpl::CleanUpPublicAccountNonCryptohomeDataPendingRemoval() {
  PrefService* local_state = g_browser_process->local_state();
  const std::string public_account_pending_data_removal =
      local_state->GetString(kPublicAccountPendingDataRemoval);
  if (public_account_pending_data_removal.empty() ||
      (IsUserLoggedIn() &&
       public_account_pending_data_removal == GetActiveUser()->email())) {
    return;
  }

  RemoveNonCryptohomeData(public_account_pending_data_removal);
  local_state->ClearPref(kPublicAccountPendingDataRemoval);
}

void UserManagerImpl::CleanUpPublicAccountNonCryptohomeData(
    const std::vector<std::string>& old_public_accounts) {
  std::set<std::string> users;
  for (UserList::const_iterator it = users_.begin(); it != users_.end(); ++it)
    users.insert((*it)->email());

  // If the user is logged into a public account that has been removed from the
  // user list, mark the account's data as pending removal after logout.
  if (IsLoggedInAsPublicAccount()) {
    const std::string active_user_id = GetActiveUser()->email();
    if (users.find(active_user_id) == users.end()) {
      g_browser_process->local_state()->SetString(
          kPublicAccountPendingDataRemoval, active_user_id);
      users.insert(active_user_id);
    }
  }

  // Remove the data belonging to any other public accounts that are no longer
  // found on the user list.
  for (std::vector<std::string>::const_iterator
           it = old_public_accounts.begin();
       it != old_public_accounts.end(); ++it) {
    if (users.find(*it) == users.end())
      RemoveNonCryptohomeData(*it);
  }
}

bool UserManagerImpl::UpdateAndCleanUpPublicAccounts(
    const std::vector<policy::DeviceLocalAccount>& device_local_accounts) {
  // Try to remove any public account data marked as pending removal.
  CleanUpPublicAccountNonCryptohomeDataPendingRemoval();

  // Get the current list of public accounts.
  std::vector<std::string> old_public_accounts;
  for (UserList::const_iterator it = users_.begin(); it != users_.end(); ++it) {
    if ((*it)->GetType() == User::USER_TYPE_PUBLIC_ACCOUNT)
      old_public_accounts.push_back((*it)->email());
  }

  // Get the new list of public accounts from policy.
  std::vector<std::string> new_public_accounts;
  for (std::vector<policy::DeviceLocalAccount>::const_iterator it =
           device_local_accounts.begin();
       it != device_local_accounts.end(); ++it) {
    // TODO(mnissler, nkostylev, bartfab): Process Kiosk Apps within the
    // standard login framework: http://crbug.com/234694
    if (it->type == policy::DeviceLocalAccount::TYPE_PUBLIC_SESSION)
      new_public_accounts.push_back(it->user_id);
  }

  // If the list of public accounts has not changed, return.
  if (new_public_accounts.size() == old_public_accounts.size()) {
    bool changed = false;
    for (size_t i = 0; i < new_public_accounts.size(); ++i) {
      if (new_public_accounts[i] != old_public_accounts[i]) {
        changed = true;
        break;
      }
    }
    if (!changed)
      return false;
  }

  // Persist the new list of public accounts in a pref.
  ListPrefUpdate prefs_public_accounts_update(g_browser_process->local_state(),
                                              kPublicAccounts);
  prefs_public_accounts_update->Clear();
  for (std::vector<std::string>::const_iterator it =
           new_public_accounts.begin();
       it != new_public_accounts.end(); ++it) {
    prefs_public_accounts_update->AppendString(*it);
  }

  // Remove the old public accounts from the user list.
  for (UserList::iterator it = users_.begin(); it != users_.end();) {
    if ((*it)->GetType() == User::USER_TYPE_PUBLIC_ACCOUNT) {
      if (*it != GetLoggedInUser())
        DeleteUser(*it);
      it = users_.erase(it);
    } else {
      ++it;
    }
  }

  // Add the new public accounts to the front of the user list.
  for (std::vector<std::string>::const_reverse_iterator it =
           new_public_accounts.rbegin();
       it != new_public_accounts.rend(); ++it) {
    if (IsLoggedInAsPublicAccount() && *it == GetActiveUser()->email())
      users_.insert(users_.begin(), GetLoggedInUser());
    else
      users_.insert(users_.begin(), User::CreatePublicAccountUser(*it));
    UpdatePublicAccountDisplayName(*it);
  }

  for (UserList::iterator ui = users_.begin(),
                          ue = users_.begin() + new_public_accounts.size();
       ui != ue; ++ui) {
    GetUserImageManager((*ui)->email())->LoadUserImage();
  }

  // Remove data belonging to public accounts that are no longer found on the
  // user list.
  CleanUpPublicAccountNonCryptohomeData(old_public_accounts);

  return true;
}

void UserManagerImpl::UpdatePublicAccountDisplayName(
    const std::string& user_id) {
  std::string display_name;

  if (device_local_account_policy_service_) {
    policy::DeviceLocalAccountPolicyBroker* broker =
        device_local_account_policy_service_->GetBrokerForUser(user_id);
    if (broker)
      display_name = broker->GetDisplayName();
  }

  // Set or clear the display name.
  SaveUserDisplayName(user_id, base::UTF8ToUTF16(display_name));
}

UserFlow* UserManagerImpl::GetCurrentUserFlow() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  if (!IsUserLoggedIn())
    return GetDefaultUserFlow();
  return GetUserFlow(GetLoggedInUser()->email());
}

UserFlow* UserManagerImpl::GetUserFlow(const std::string& user_id) const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  FlowMap::const_iterator it = specific_flows_.find(user_id);
  if (it != specific_flows_.end())
    return it->second;
  return GetDefaultUserFlow();
}

void UserManagerImpl::SetUserFlow(const std::string& user_id, UserFlow* flow) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  ResetUserFlow(user_id);
  specific_flows_[user_id] = flow;
}

void UserManagerImpl::ResetUserFlow(const std::string& user_id) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  FlowMap::iterator it = specific_flows_.find(user_id);
  if (it != specific_flows_.end()) {
    delete it->second;
    specific_flows_.erase(it);
  }
}

bool UserManagerImpl::GetAppModeChromeClientOAuthInfo(
    std::string* chrome_client_id, std::string* chrome_client_secret) {
  if (!chrome::IsRunningInForcedAppMode() ||
      chrome_client_id_.empty() ||
      chrome_client_secret_.empty()) {
    return false;
  }

  *chrome_client_id = chrome_client_id_;
  *chrome_client_secret = chrome_client_secret_;
  return true;
}

void UserManagerImpl::SetAppModeChromeClientOAuthInfo(
    const std::string& chrome_client_id,
    const std::string& chrome_client_secret) {
  if (!chrome::IsRunningInForcedAppMode())
    return;

  chrome_client_id_ = chrome_client_id;
  chrome_client_secret_ = chrome_client_secret;
}

bool UserManagerImpl::AreLocallyManagedUsersAllowed() const {
  bool locally_managed_users_allowed = false;
  cros_settings_->GetBoolean(kAccountsPrefSupervisedUsersEnabled,
                             &locally_managed_users_allowed);
  policy::BrowserPolicyConnectorChromeOS* connector =
      g_browser_process->platform_part()->browser_policy_connector_chromeos();
  return locally_managed_users_allowed || !connector->IsEnterpriseManaged();
}

base::FilePath UserManagerImpl::GetUserProfileDir(
    const std::string& user_id) const {
  // TODO(dpolukhin): Remove Chrome OS specific profile path logic from
  // ProfileManager and use only this function to construct profile path.
  // TODO(nkostylev): Cleanup profile dir related code paths crbug.com/294233
  base::FilePath profile_dir;
  const User* user = FindUser(user_id);
  if (user && !user->username_hash().empty())
    profile_dir = ProfileHelper::GetUserProfileDir(user->username_hash());

  ProfileManager* profile_manager = g_browser_process->profile_manager();
  profile_dir = profile_manager->user_data_dir().Append(profile_dir);

  return profile_dir;
}

UserFlow* UserManagerImpl::GetDefaultUserFlow() const {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  if (!default_flow_.get())
    default_flow_.reset(new DefaultUserFlow());
  return default_flow_.get();
}

void UserManagerImpl::NotifyUserListChanged() {
  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_USER_LIST_CHANGED,
      content::Source<UserManager>(this),
      content::NotificationService::NoDetails());
}

void UserManagerImpl::NotifyActiveUserChanged(const User* active_user) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  FOR_EACH_OBSERVER(UserManager::UserSessionStateObserver,
                    session_state_observer_list_,
                    ActiveUserChanged(active_user));
}

void UserManagerImpl::NotifyUserAddedToSession(const User* added_user) {
  UpdateNumberOfUsers();
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  FOR_EACH_OBSERVER(UserManager::UserSessionStateObserver,
                    session_state_observer_list_,
                    UserAddedToSession(added_user));
}

void UserManagerImpl::NotifyActiveUserHashChanged(const std::string& hash) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  FOR_EACH_OBSERVER(UserManager::UserSessionStateObserver,
                    session_state_observer_list_,
                    ActiveUserHashChanged(hash));
}

void UserManagerImpl::NotifyPendingUserSessionsRestoreFinished() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  user_sessions_restored_ = true;
  FOR_EACH_OBSERVER(UserManager::UserSessionStateObserver,
                    session_state_observer_list_,
                    PendingUserSessionsRestoreFinished());
}

void UserManagerImpl::UpdateLoginState() {
  if (!LoginState::IsInitialized())
    return;  // LoginState may not be intialized in tests.
  LoginState::LoggedInState logged_in_state;
  logged_in_state = active_user_ ? LoginState::LOGGED_IN_ACTIVE
      : LoginState::LOGGED_IN_NONE;

  LoginState::LoggedInUserType login_user_type;
  if (logged_in_state == LoginState::LOGGED_IN_NONE)
    login_user_type = LoginState::LOGGED_IN_USER_NONE;
  else if (is_current_user_owner_)
    login_user_type = LoginState::LOGGED_IN_USER_OWNER;
  else if (active_user_->GetType() == User::USER_TYPE_GUEST)
    login_user_type = LoginState::LOGGED_IN_USER_GUEST;
  else if (active_user_->GetType() == User::USER_TYPE_RETAIL_MODE)
    login_user_type = LoginState::LOGGED_IN_USER_RETAIL_MODE;
  else if (active_user_->GetType() == User::USER_TYPE_PUBLIC_ACCOUNT)
    login_user_type = LoginState::LOGGED_IN_USER_PUBLIC_ACCOUNT;
  else if (active_user_->GetType() == User::USER_TYPE_LOCALLY_MANAGED)
    login_user_type = LoginState::LOGGED_IN_USER_LOCALLY_MANAGED;
  else if (active_user_->GetType() == User::USER_TYPE_KIOSK_APP)
    login_user_type = LoginState::LOGGED_IN_USER_KIOSK_APP;
  else
    login_user_type = LoginState::LOGGED_IN_USER_REGULAR;

  LoginState::Get()->SetLoggedInState(logged_in_state, login_user_type);
}

void UserManagerImpl::SetLRUUser(User* user) {
  UserList::iterator it = std::find(lru_logged_in_users_.begin(),
                                    lru_logged_in_users_.end(),
                                    user);
  if (it != lru_logged_in_users_.end())
    lru_logged_in_users_.erase(it);
  lru_logged_in_users_.insert(lru_logged_in_users_.begin(), user);
}

void UserManagerImpl::OnRestoreActiveSessions(
    const SessionManagerClient::ActiveSessionsMap& sessions,
    bool success) {
  if (!success) {
    LOG(ERROR) << "Could not get list of active user sessions after crash.";
    // If we could not get list of active user sessions it is safer to just
    // sign out so that we don't get in the inconsistent state.
    DBusThreadManager::Get()->GetSessionManagerClient()->StopSession();
    return;
  }

  // One profile has been already loaded on browser start.
  DCHECK(GetLoggedInUsers().size() == 1);
  DCHECK(GetActiveUser());
  std::string active_user_id = GetActiveUser()->email();

  SessionManagerClient::ActiveSessionsMap::const_iterator it;
  for (it = sessions.begin(); it != sessions.end(); ++it) {
    if (active_user_id == it->first)
      continue;
    pending_user_sessions_[it->first] = it->second;
  }
  RestorePendingUserSessions();
}

void UserManagerImpl::RestorePendingUserSessions() {
  if (pending_user_sessions_.empty()) {
    NotifyPendingUserSessionsRestoreFinished();
    return;
  }

  // Get next user to restore sessions and delete it from list.
  SessionManagerClient::ActiveSessionsMap::const_iterator it =
      pending_user_sessions_.begin();
  std::string user_id = it->first;
  std::string user_id_hash = it->second;
  DCHECK(!user_id.empty());
  DCHECK(!user_id_hash.empty());
  pending_user_sessions_.erase(user_id);

  // Check that this user is not logged in yet.
  UserList logged_in_users = GetLoggedInUsers();
  bool user_already_logged_in = false;
  for (UserList::const_iterator it = logged_in_users.begin();
       it != logged_in_users.end(); ++it) {
    const User* user = (*it);
    if (user->email() == user_id) {
      user_already_logged_in = true;
      break;
    }
  }
  DCHECK(!user_already_logged_in);

  if (!user_already_logged_in) {
    UserContext user_context(user_id);
    user_context.SetUserIDHash(user_id_hash);
    user_context.SetIsUsingOAuth(false);
    // Will call OnProfilePrepared() once profile has been loaded.
    LoginUtils::Get()->PrepareProfile(user_context,
                                      std::string(),  // display_email
                                      false,          // has_cookies
                                      true,           // has_active_session
                                      this);
  } else {
    RestorePendingUserSessions();
  }
}

void UserManagerImpl::SendRegularUserLoginMetrics(const std::string& user_id) {
  // If this isn't the first time Chrome was run after the system booted,
  // assume that Chrome was restarted because a previous session ended.
  if (!CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kFirstExecAfterBoot)) {
    const std::string last_email =
        g_browser_process->local_state()->GetString(kLastLoggedInRegularUser);
    const base::TimeDelta time_to_login =
        base::TimeTicks::Now() - manager_creation_time_;
    if (!last_email.empty() && user_id != last_email &&
        time_to_login.InSeconds() <= kLogoutToLoginDelayMaxSec) {
      UMA_HISTOGRAM_CUSTOM_COUNTS("UserManager.LogoutToLoginDelay",
          time_to_login.InSeconds(), 0, kLogoutToLoginDelayMaxSec, 50);
    }
  }
}

void UserManagerImpl::OnUserNotAllowed(const std::string& user_email) {
  LOG(ERROR) << "Shutdown session because a user is not allowed to be in the "
                "current session";
  chromeos::ShowMultiprofilesSessionAbortedDialog(user_email);
}

void UserManagerImpl::UpdateUserAccountLocale(const std::string& user_id,
                                              const std::string& locale) {
  if (!locale.empty() &&
      locale != g_browser_process->GetApplicationLocale()) {
    BrowserThread::PostBlockingPoolTask(
        FROM_HERE,
        base::Bind(ResolveLocale, locale,
            base::Bind(&UserManagerImpl::DoUpdateAccountLocale,
                       base::Unretained(this),
                       user_id)));
  } else {
    DoUpdateAccountLocale(user_id, locale);
  }
}

void UserManagerImpl::DoUpdateAccountLocale(
    const std::string& user_id,
    const std::string& resolved_locale) {
  if (User* user = FindUserAndModify(user_id))
    user->SetAccountLocale(resolved_locale);
}

void UserManagerImpl::UpdateNumberOfUsers() {
  size_t users = GetLoggedInUsers().size();
  if (users) {
    // Write the user number as UMA stat when a multi user session is possible.
    if ((users + GetUsersAdmittedForMultiProfile().size()) > 1)
      ash::MultiProfileUMA::RecordUserCount(users);
  }

  base::debug::SetCrashKeyValue(crash_keys::kNumberOfUsers,
      base::StringPrintf("%" PRIuS, GetLoggedInUsers().size()));
}

void UserManagerImpl::DeleteUser(User* user) {
  const bool is_active_user = (user == active_user_);
  delete user;
  if (is_active_user)
    active_user_ = NULL;
}

}  // namespace chromeos
