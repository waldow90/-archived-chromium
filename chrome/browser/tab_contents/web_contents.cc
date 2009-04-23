// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/tab_contents/web_contents.h"

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/file_version_info.h"
#include "base/process_util.h"
#include "base/string_util.h"
#include "chrome/browser/autofill_manager.h"
#include "chrome/browser/bookmarks/bookmark_model.h"
#include "chrome/browser/browser.h"
#include "chrome/browser/character_encoding.h"
#include "chrome/browser/debugger/devtools_manager.h"
#include "chrome/browser/dom_operation_notification_details.h"
#include "chrome/browser/dom_ui/dom_ui.h"
#include "chrome/browser/dom_ui/dom_ui_factory.h"
#include "chrome/browser/download/download_manager.h"
#include "chrome/browser/download/download_request_manager.h"
#include "chrome/browser/gears_integration.h"
#include "chrome/browser/google_util.h"
#include "chrome/browser/jsmessage_box_handler.h"
#include "chrome/browser/load_from_memory_cache_details.h"
#include "chrome/browser/load_notification_details.h"
#include "chrome/browser/password_manager/password_manager.h"
#include "chrome/browser/plugin_installer.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/renderer_host/render_process_host.h"
#include "chrome/browser/renderer_host/render_view_host.h"
#include "chrome/browser/renderer_host/render_widget_host_view.h"
#include "chrome/browser/renderer_host/web_cache_manager.h"
#include "chrome/browser/search_engines/template_url_fetcher.h"
#include "chrome/browser/search_engines/template_url_model.h"
#include "chrome/browser/tab_contents/navigation_entry.h"
#include "chrome/browser/tab_contents/provisional_load_details.h"
#include "chrome/browser/tab_contents/tab_contents_view.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/l10n_util.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/pref_service.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/url_constants.h"
#include "grit/locale_settings.h"
#include "net/base/mime_util.h"
#include "net/base/net_errors.h"
#include "net/base/registry_controlled_domain.h"
#include "webkit/glue/feed.h"
#include "webkit/glue/webkit_glue.h"

#if defined(OS_WIN)
// TODO(port): fill these in as we flesh out the implementation of this class
#include "chrome/browser/modal_html_dialog_delegate.h"
#include "chrome/browser/plugin_service.h"
#include "chrome/browser/printing/print_job.h"
#include "chrome/browser/views/hung_renderer_view.h"  // TODO(brettw) delete me.
#include "chrome/common/resource_bundle.h"
#endif

#include "grit/generated_resources.h"

// Cross-Site Navigations
//
// If a WebContents is told to navigate to a different web site (as determined
// by SiteInstance), it will replace its current RenderViewHost with a new
// RenderViewHost dedicated to the new SiteInstance.  This works as follows:
//
// - Navigate determines whether the destination is cross-site, and if so,
//   it creates a pending_render_view_host_ and moves into the PENDING
//   RendererState.
// - The pending RVH is "suspended," so that no navigation messages are sent to
//   its renderer until the onbeforeunload JavaScript handler has a chance to
//   run in the current RVH.
// - The pending RVH tells CrossSiteRequestManager (a thread-safe singleton)
//   that it has a pending cross-site request.  ResourceDispatcherHost will
//   check for this when the response arrives.
// - The current RVH runs its onbeforeunload handler.  If it returns false, we
//   cancel all the pending logic and go back to NORMAL.  Otherwise we allow
//   the pending RVH to send the navigation request to its renderer.
// - ResourceDispatcherHost receives a ResourceRequest on the IO thread.  It
//   checks CrossSiteRequestManager to see that the RVH responsible has a
//   pending cross-site request, and then installs a CrossSiteEventHandler.
// - When RDH receives a response, the BufferedEventHandler determines whether
//   it is a download.  If so, it sends a message to the new renderer causing
//   it to cancel the request, and the download proceeds in the download
//   thread.  For now, we stay in a PENDING state (with a pending RVH) until
//   the next DidNavigate event for this WebContents.  This isn't ideal, but it
//   doesn't affect any functionality.
// - After RDH receives a response and determines that it is safe and not a
//   download, it pauses the response to first run the old page's onunload
//   handler.  It does this by asynchronously calling the OnCrossSiteResponse
//   method of WebContents on the UI thread, which sends a ClosePage message
//   to the current RVH.
// - Once the onunload handler is finished, a ClosePage_ACK message is sent to
//   the ResourceDispatcherHost, who unpauses the response.  Data is then sent
//   to the pending RVH.
// - The pending renderer sends a FrameNavigate message that invokes the
//   DidNavigate method.  This replaces the current RVH with the
//   pending RVH and goes back to the NORMAL RendererState.

using base::TimeDelta;
using base::TimeTicks;

namespace {

// Amount of time we wait between when a key event is received and the renderer
// is queried for its state and pushed to the NavigationEntry.
const int kQueryStateDelay = 5000;

const int kSyncWaitDelay = 40;

// If another javascript message box is displayed within
// kJavascriptMessageExpectedDelay of a previous javascript message box being
// dismissed, display an option to suppress future message boxes from this
// contents.
const int kJavascriptMessageExpectedDelay = 1000;

// Minimum amount of time in ms that has to elapse since the download shelf was
// shown for us to hide it when navigating away from the current page.
const int kDownloadShelfHideDelay = 5000;

const char kLinkDoctorBaseURL[] =
    "http://linkhelp.clients.google.com/tbproxy/lh/fixurl";

// The printer icon in shell32.dll. That's a standard icon user will quickly
// recognize.
const int kShell32PrinterIcon = 17;

// The list of prefs we want to observe.
const wchar_t* kPrefsToObserve[] = {
  prefs::kAlternateErrorPagesEnabled,
  prefs::kWebKitJavaEnabled,
  prefs::kWebKitJavascriptEnabled,
  prefs::kWebKitLoadsImagesAutomatically,
  prefs::kWebKitPluginsEnabled,
  prefs::kWebKitUsesUniversalDetector,
  prefs::kWebKitSerifFontFamily,
  prefs::kWebKitSansSerifFontFamily,
  prefs::kWebKitFixedFontFamily,
  prefs::kWebKitDefaultFontSize,
  prefs::kWebKitDefaultFixedFontSize,
  prefs::kDefaultCharset
  // kWebKitStandardFontIsSerif needs to be added
  // if we let users pick which font to use, serif or sans-serif when
  // no font is specified or a CSS generic family (serif or sans-serif)
  // is not specified.
};

const int kPrefsToObserveLength = arraysize(kPrefsToObserve);

// Limit on the number of suggestions to appear in the pop-up menu under an
// text input element in a form.
const int kMaxAutofillMenuItems = 6;

// Returns true if the entry's transition type is FORM_SUBMIT.
bool IsFormSubmit(const NavigationEntry* entry) {
  return (PageTransition::StripQualifier(entry->transition_type()) ==
          PageTransition::FORM_SUBMIT);
}

#if defined(OS_WIN)

BOOL CALLBACK InvalidateWindow(HWND hwnd, LPARAM lparam) {
  // Note: erase is required to properly paint some widgets borders. This can
  // be seen with textfields.
  InvalidateRect(hwnd, NULL, TRUE);
  return TRUE;
}
#endif

}  // namespace

WebContents::WebContents(Profile* profile,
                         SiteInstance* site_instance,
                         int routing_id,
                         base::WaitableEvent* modal_dialog_event)
    : TabContents(profile) {
  pending_install_.page_id = 0;
  pending_install_.callback_functor = NULL;

  render_manager_.Init(profile, site_instance, routing_id, modal_dialog_event);

  view_->CreateView();

  // Register for notifications about all interested prefs change.
  PrefService* prefs = profile->GetPrefs();
  if (prefs) {
    for (int i = 0; i < kPrefsToObserveLength; ++i)
      prefs->AddPrefObserver(kPrefsToObserve[i], this);
  }

  // Register for notifications about URL starredness changing on any profile.
  registrar_.Add(this, NotificationType::URLS_STARRED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::BOOKMARK_MODEL_LOADED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::RENDER_WIDGET_HOST_DESTROYED,
                 NotificationService::AllSources());

  // Keep a global copy of the previous search string (if any).
  static string16 global_last_search = string16();
  find_prepopulate_text_ = &global_last_search;
}

WebContents::~WebContents() {
  is_being_destroyed_ = true;

  // We don't want any notifications while we're runnign our destructor.
  registrar_.RemoveAll();

  // Unregister the notifications of all observed prefs change.
  PrefService* prefs = profile()->GetPrefs();
  if (prefs) {
    for (int i = 0; i < kPrefsToObserveLength; ++i)
      prefs->RemovePrefObserver(kPrefsToObserve[i], this);
  }

  // Clean up subwindows like plugins and the find in page bar.
  view_->OnContentsDestroy();

  NotifyDisconnected();
  HungRendererWarning::HideForWebContents(this);

  if (pending_install_.callback_functor)
    pending_install_.callback_functor->Cancel();

  // First cleanly close all child windows.
  // TODO(mpcomplete): handle case if MaybeCloseChildWindows() already asked
  // some of these to close.  CloseWindows is async, so it might get called
  // twice before it runs.
  int size = static_cast<int>(child_windows_.size());
  for (int i = size - 1; i >= 0; --i) {
    ConstrainedWindow* window = child_windows_[i];
    if (window)
      window->CloseConstrainedWindow();
  }

  // Notify any lasting InfobarDelegates that have not yet been removed that
  // whatever infobar they were handling in this TabContents has closed,
  // because the TabContents is going away entirely.
  for (int i = 0; i < infobar_delegate_count(); ++i) {
    InfoBarDelegate* delegate = GetInfoBarDelegateAt(i);
    delegate->InfoBarClosed();
  }
  infobar_delegates_.clear();

  // Notify any observer that have a reference on this tab contents.
  NotificationService::current()->Notify(
      NotificationType::TAB_CONTENTS_DESTROYED,
      Source<TabContents>(this),
      NotificationService::NoDetails());

  // TODO(brettw) this should be moved to the view.
#if defined(OS_WIN)
  // If we still have a window handle, destroy it. GetNativeView can return
  // NULL if this contents was part of a window that closed.
  if (GetNativeView())
    ::DestroyWindow(GetNativeView());
#endif
}

// static
void WebContents::RegisterUserPrefs(PrefService* prefs) {
  prefs->RegisterBooleanPref(prefs::kAlternateErrorPagesEnabled, true);

  WebPreferences pref_defaults;
  prefs->RegisterBooleanPref(prefs::kWebKitJavascriptEnabled,
                             pref_defaults.javascript_enabled);
  prefs->RegisterBooleanPref(prefs::kWebKitWebSecurityEnabled,
                             pref_defaults.web_security_enabled);
  prefs->RegisterBooleanPref(
      prefs::kWebKitJavascriptCanOpenWindowsAutomatically, true);
  prefs->RegisterBooleanPref(prefs::kWebKitLoadsImagesAutomatically,
                             pref_defaults.loads_images_automatically);
  prefs->RegisterBooleanPref(prefs::kWebKitPluginsEnabled,
                             pref_defaults.plugins_enabled);
  prefs->RegisterBooleanPref(prefs::kWebKitDomPasteEnabled,
                             pref_defaults.dom_paste_enabled);
  prefs->RegisterBooleanPref(prefs::kWebKitShrinksStandaloneImagesToFit,
                             pref_defaults.shrinks_standalone_images_to_fit);
  prefs->RegisterBooleanPref(prefs::kWebKitDeveloperExtrasEnabled,
                             true);
  prefs->RegisterBooleanPref(prefs::kWebKitTextAreasAreResizable,
                             pref_defaults.text_areas_are_resizable);
  prefs->RegisterBooleanPref(prefs::kWebKitJavaEnabled,
                             pref_defaults.java_enabled);

  prefs->RegisterLocalizedStringPref(prefs::kAcceptLanguages,
                                     IDS_ACCEPT_LANGUAGES);
  prefs->RegisterLocalizedStringPref(prefs::kDefaultCharset,
                                     IDS_DEFAULT_ENCODING);
  prefs->RegisterLocalizedBooleanPref(prefs::kWebKitStandardFontIsSerif,
                                      IDS_STANDARD_FONT_IS_SERIF);
  prefs->RegisterLocalizedStringPref(prefs::kWebKitFixedFontFamily,
                                     IDS_FIXED_FONT_FAMILY);
  prefs->RegisterLocalizedStringPref(prefs::kWebKitSerifFontFamily,
                                     IDS_SERIF_FONT_FAMILY);
  prefs->RegisterLocalizedStringPref(prefs::kWebKitSansSerifFontFamily,
                                     IDS_SANS_SERIF_FONT_FAMILY);
  prefs->RegisterLocalizedStringPref(prefs::kWebKitCursiveFontFamily,
                                     IDS_CURSIVE_FONT_FAMILY);
  prefs->RegisterLocalizedStringPref(prefs::kWebKitFantasyFontFamily,
                                     IDS_FANTASY_FONT_FAMILY);
  prefs->RegisterLocalizedIntegerPref(prefs::kWebKitDefaultFontSize,
                                      IDS_DEFAULT_FONT_SIZE);
  prefs->RegisterLocalizedIntegerPref(prefs::kWebKitDefaultFixedFontSize,
                                      IDS_DEFAULT_FIXED_FONT_SIZE);
  prefs->RegisterLocalizedIntegerPref(prefs::kWebKitMinimumFontSize,
                                      IDS_MINIMUM_FONT_SIZE);
  prefs->RegisterLocalizedIntegerPref(prefs::kWebKitMinimumLogicalFontSize,
                                      IDS_MINIMUM_LOGICAL_FONT_SIZE);
  prefs->RegisterLocalizedBooleanPref(prefs::kWebKitUsesUniversalDetector,
                                      IDS_USES_UNIVERSAL_DETECTOR);
  prefs->RegisterLocalizedStringPref(prefs::kStaticEncodings,
                                     IDS_STATIC_ENCODING_LIST);
}

const string16& WebContents::GetTitle() const {
  DOMUI* our_dom_ui = render_manager_.pending_dom_ui() ?
      render_manager_.pending_dom_ui() : render_manager_.dom_ui();
  if (our_dom_ui) {
    // Give the DOM UI the chance to override our title.
    const string16& title = our_dom_ui->overridden_title();
    if (!title.empty())
      return title;
  }

  // We use the title for the last committed entry rather than a pending
  // navigation entry. For example, when the user types in a URL, we want to
  // keep the old page's title until the new load has committed and we get a new
  // title.
  // The exception is with transient pages, for which we really want to use
  // their title, as they are not committed.
  NavigationEntry* entry = controller_.GetTransientEntry();
  if (entry)
    return entry->GetTitleForDisplay(&controller_);

  entry = controller_.GetLastCommittedEntry();
  if (entry)
    return entry->GetTitleForDisplay(&controller_);
  else if (controller_.LoadingURLLazily())
    return controller_.GetLazyTitle();
  return EmptyString16();
}

SiteInstance* WebContents::GetSiteInstance() const {
  return render_manager_.current_host()->site_instance();
}

bool WebContents::ShouldDisplayURL() {
  DOMUI* dom_ui = GetDOMUIForCurrentState();
  if (dom_ui)
    return !dom_ui->should_hide_url();
  return true;
}

bool WebContents::ShouldDisplayFavIcon() {
  // Always display a throbber during pending loads.
  if (controller_.GetLastCommittedEntry() && controller_.pending_entry())
    return true;

  DOMUI* dom_ui = GetDOMUIForCurrentState();
  if (dom_ui)
    return !dom_ui->hide_favicon();
  return true;
}

std::wstring WebContents::GetStatusText() const {
  if (!is_loading() || load_state_ == net::LOAD_STATE_IDLE)
    return std::wstring();

  switch (load_state_) {
    case net::LOAD_STATE_WAITING_FOR_CACHE:
      return l10n_util::GetString(IDS_LOAD_STATE_WAITING_FOR_CACHE);
    case net::LOAD_STATE_RESOLVING_PROXY_FOR_URL:
      return l10n_util::GetString(IDS_LOAD_STATE_RESOLVING_PROXY_FOR_URL);
    case net::LOAD_STATE_RESOLVING_HOST:
      return l10n_util::GetString(IDS_LOAD_STATE_RESOLVING_HOST);
    case net::LOAD_STATE_CONNECTING:
      return l10n_util::GetString(IDS_LOAD_STATE_CONNECTING);
    case net::LOAD_STATE_SENDING_REQUEST:
      return l10n_util::GetString(IDS_LOAD_STATE_SENDING_REQUEST);
    case net::LOAD_STATE_WAITING_FOR_RESPONSE:
      return l10n_util::GetStringF(IDS_LOAD_STATE_WAITING_FOR_RESPONSE,
                                   load_state_host_);
    // Ignore net::LOAD_STATE_READING_RESPONSE and net::LOAD_STATE_IDLE
    case net::LOAD_STATE_IDLE:
    case net::LOAD_STATE_READING_RESPONSE:
      break;
  }

  return std::wstring();
}

bool WebContents::NavigateToPendingEntry(bool reload) {
  const NavigationEntry& entry = *controller_.pending_entry();

  RenderViewHost* dest_render_view_host = render_manager_.Navigate(entry);
  if (!dest_render_view_host)
    return false;  // Unable to create the desired render view host.

  // Tell DevTools agent that it is attached prior to the navigation.
  DevToolsManager* dev_tools_manager = g_browser_process->devtools_manager();
  if (dev_tools_manager)  // NULL in unit tests.
    dev_tools_manager->SendAttachToAgent(*this, dest_render_view_host);

  // Used for page load time metrics.
  current_load_start_ = TimeTicks::Now();

  // Navigate in the desired RenderViewHost.
  dest_render_view_host->NavigateToEntry(entry, reload);

  if (entry.page_id() == -1) {
    // HACK!!  This code suppresses javascript: URLs from being added to
    // session history, which is what we want to do for javascript: URLs that
    // do not generate content.  What we really need is a message from the
    // renderer telling us that a new page was not created.  The same message
    // could be used for mailto: URLs and the like.
    if (entry.url().SchemeIs(chrome::kJavaScriptScheme))
      return false;
  }

  // Clear any provisional password saves - this stops password infobars
  // showing up on pages the user navigates to while the right page is
  // loading.
  GetPasswordManager()->ClearProvisionalSave();

  if (reload && !profile()->IsOffTheRecord()) {
    HistoryService* history =
        profile()->GetHistoryService(Profile::IMPLICIT_ACCESS);
    if (history)
      history->SetFavIconOutOfDateForPage(entry.url());
  }

  return true;
}

void WebContents::Stop() {
  render_manager_.Stop();
  printing_.Stop();
}

void WebContents::Cut() {
  render_view_host()->Cut();
}

void WebContents::Copy() {
  render_view_host()->Copy();
}

void WebContents::Paste() {
  render_view_host()->Paste();
}

void WebContents::DisassociateFromPopupCount() {
  render_view_host()->DisassociateFromPopupCount();
}

TabContents* WebContents::Clone() {
  // We create a new SiteInstance so that the new tab won't share processes
  // with the old one. This can be changed in the future if we need it to share
  // processes for some reason.
  TabContents* tc = new WebContents(profile(),
                                    SiteInstance::CreateSiteInstance(profile()),
                                    MSG_ROUTING_NONE, NULL);
  tc->controller().CopyStateFrom(controller_);
  return tc;
}

void WebContents::DidBecomeSelected() {
  controller_.SetActive(true);

  if (render_widget_host_view())
    render_widget_host_view()->DidBecomeSelected();

  // If pid() is -1, that means the RenderProcessHost still hasn't been
  // initialized.  It'll register with CacheManagerHost when it is.
  if (process()->pid() != -1)
    WebCacheManager::GetInstance()->ObserveActivity(process()->pid());
}

void WebContents::WasHidden() {
  if (!capturing_contents()) {
    // |render_view_host()| can be NULL if the user middle clicks a link to open
    // a tab in then background, then closes the tab before selecting it.  This
    // is because closing the tab calls WebContents::Destroy(), which removes
    // the |render_view_host()|; then when we actually destroy the window,
    // OnWindowPosChanged() notices and calls HideContents() (which calls us).
    if (render_widget_host_view())
      render_widget_host_view()->WasHidden();

    // Loop through children and send WasHidden to them, too.
    int count = static_cast<int>(child_windows_.size());
    for (int i = count - 1; i >= 0; --i) {
      ConstrainedWindow* window = child_windows_.at(i);
      window->WasHidden();
    }
  }

  NotificationService::current()->Notify(
      NotificationType::TAB_CONTENTS_HIDDEN,
      Source<TabContents>(this),
      NotificationService::NoDetails());
}

void WebContents::ShowContents() {
  if (render_widget_host_view())
    render_widget_host_view()->DidBecomeSelected();

  // Loop through children and send DidBecomeSelected to them, too.
  int count = static_cast<int>(child_windows_.size());
  for (int i = count - 1; i >= 0; --i) {
    ConstrainedWindow* window = child_windows_.at(i);
    window->DidBecomeSelected();
  }
}

void WebContents::HideContents() {
  // TODO(pkasting): http://b/1239839  Right now we purposefully don't call
  // our superclass HideContents(), because some callers want to be very picky
  // about the order in which these get called.  In addition to making the code
  // here practically impossible to understand, this also means we end up
  // calling TabContents::WasHidden() twice if callers call both versions of
  // HideContents() on a WebContents.
  WasHidden();
}

bool WebContents::IsBookmarkBarAlwaysVisible() {
  // See GetDOMUIForCurrentState() comment for more info. This case is very
  // similar, but for non-first loads, we want to use the committed entry. This
  // is so the bookmarks bar disappears at the same time the page does.
  if (controller_.GetLastCommittedEntry()) {
    // Not the first load, always use the committed DOM UI.
    if (render_manager_.dom_ui())
      return render_manager_.dom_ui()->force_bookmark_bar_visible();
    return false;  // Default.
  }

  // When it's the first load, we know either the pending one or the committed
  // one will have the DOM UI in it (see GetDOMUIForCurrentState), and only one
  // of them will be valid, so we can just check both.
  if (render_manager_.pending_dom_ui())
    return render_manager_.pending_dom_ui()->force_bookmark_bar_visible();
  if (render_manager_.dom_ui())
    return render_manager_.dom_ui()->force_bookmark_bar_visible();
  return false;  // Default.
}

void WebContents::SetDownloadShelfVisible(bool visible) {
  if (shelf_visible_ != visible) {
    if (visible) {
      // Invoke GetDownloadShelf to force the shelf to be created.
      GetDownloadShelf();
    }
    shelf_visible_ = visible;

    if (delegate_)
      delegate_->ContentsStateChanged(this);
  }

  // SetShelfVisible can force-close the shelf, so make sure we lay out
  // everything correctly, as if the animation had finished. This doesn't
  // matter for showing the shelf, as the show animation will do it.
  ToolbarSizeChanged(false);

  if (visible) {
    // Always set this value as it reflects the last time the download shelf
    // was made visible (even if it was already visible).
    last_download_shelf_show_ = TimeTicks::Now();
  }
}

void WebContents::PopupNotificationVisibilityChanged(bool visible) {
  render_view_host()->PopupNotificationVisibilityChanged(visible);
}

bool WebContents::FocusLocationBarByDefault() {
  DOMUI* dom_ui = GetDOMUIForCurrentState();
  if (dom_ui)
    return dom_ui->focus_location_bar_by_default();
  return false;
}

gfx::NativeView WebContents::GetNativeView() const {
  return view_->GetNativeView();
}

gfx::NativeView WebContents::GetContentNativeView() {
  return view_->GetContentNativeView();
}

void WebContents::GetContainerBounds(gfx::Rect *out) const {
  view_->GetContainerBounds(out);
}

void WebContents::CreateShortcut() {
  NavigationEntry* entry = controller_.GetLastCommittedEntry();
  if (!entry)
    return;

  // We only allow one pending install request. By resetting the page id we
  // effectively cancel the pending install request.
  pending_install_.page_id = entry->page_id();
  pending_install_.icon = GetFavIcon();
  pending_install_.title = UTF16ToWideHack(GetTitle());
  pending_install_.url = GetURL();
  if (pending_install_.callback_functor) {
    pending_install_.callback_functor->Cancel();
    pending_install_.callback_functor = NULL;
  }
  DCHECK(!pending_install_.icon.isNull()) << "Menu item should be disabled.";
  if (pending_install_.title.empty())
    pending_install_.title = UTF8ToWide(GetURL().spec());

  // Request the application info. When done OnDidGetApplicationInfo is invoked
  // and we'll create the shortcut.
  render_view_host()->GetApplicationInfo(pending_install_.page_id);
}

void WebContents::OnJavaScriptMessageBoxClosed(IPC::Message* reply_msg,
                                               bool success,
                                               const std::wstring& prompt) {
  last_javascript_message_dismissal_ = TimeTicks::Now();
  render_manager_.OnJavaScriptMessageBoxClosed(reply_msg, success, prompt);
}

void WebContents::OnSavePage() {
  // If we can not save the page, try to download it.
  if (!SavePackage::IsSavableContents(contents_mime_type())) {
    DownloadManager* dlm = profile()->GetDownloadManager();
    const GURL& current_page_url = GetURL();
    if (dlm && current_page_url.is_valid())
      dlm->DownloadUrl(current_page_url, GURL(), this);
    return;
  }

  Stop();

  // Create the save package and possibly prompt the user for the name to save
  // the page as. The user prompt is an asynchronous operation that runs on
  // another thread.
  save_package_ = new SavePackage(this);
  save_package_->GetSaveInfo();
}

// Used in automated testing to bypass prompting the user for file names.
// Instead, the names and paths are hard coded rather than running them through
// file name sanitation and extension / mime checking.
void WebContents::SavePage(const std::wstring& main_file,
                           const std::wstring& dir_path,
                           SavePackage::SavePackageType save_type) {
  // Stop the page from navigating.
  Stop();

  save_package_ = new SavePackage(this, save_type,
                                  FilePath::FromWStringHack(main_file),
                                  FilePath::FromWStringHack(dir_path));
  save_package_->Init();
}

void WebContents::PrintPreview() {
  // We don't show the print preview yet, only the print dialog.
  PrintNow();
}

bool WebContents::PrintNow() {
  // We can't print interstitial page for now.
  if (showing_interstitial_page())
    return false;

  return render_view_host()->PrintPages();
}

bool WebContents::IsActiveEntry(int32 page_id) {
  NavigationEntry* active_entry = controller_.GetActiveEntry();
  return (active_entry != NULL &&
          active_entry->site_instance() == GetSiteInstance() &&
          active_entry->page_id() == page_id);
}

void WebContents::Focus() {
  view_->Focus();
}

void WebContents::SetInitialFocus(bool reverse) {
  render_view_host()->SetInitialFocus(reverse);
}

// Notifies the RenderWidgetHost instance about the fact that the page is
// loading, or done loading and calls the base implementation.
void WebContents::SetIsLoading(bool is_loading,
                               LoadNotificationDetails* details) {
  if (is_loading == is_loading_)
    return;

  if (!is_loading) {
    load_state_ = net::LOAD_STATE_IDLE;
    load_state_host_.clear();
  }

  render_manager_.SetIsLoading(is_loading);

  is_loading_ = is_loading;
  waiting_for_response_ = is_loading;

  if (delegate_)
    delegate_->LoadingStateChanged(this);

  NotificationType type = is_loading ? NotificationType::LOAD_START :
      NotificationType::LOAD_STOP;
  NotificationDetails det = NotificationService::NoDetails();;
  if (details)
      det = Details<LoadNotificationDetails>(details);
  NotificationService::current()->Notify(type,
      Source<NavigationController>(&controller_),
      det);
}

RenderViewHostDelegate::View* WebContents::GetViewDelegate() const {
  return view_.get();
}

RenderViewHostDelegate::Save* WebContents::GetSaveDelegate() const {
  return save_package_.get();  // May be NULL, but we can return NULL.
}

Profile* WebContents::GetProfile() const {
  return profile();
}

void WebContents::RenderViewCreated(RenderViewHost* render_view_host) {
  NavigationEntry* entry = controller_.GetActiveEntry();
  if (!entry)
    return;

  // When we're creating views, we're still doing initial setup, so we always
  // use the pending DOM UI rather than any possibly existing committed one.
  if (render_manager_.pending_dom_ui()) {
    render_manager_.pending_dom_ui()->RenderViewCreated(render_view_host);
  } else if (entry->IsViewSourceMode()) {
    // Put the renderer in view source mode.
    render_view_host->Send(
        new ViewMsg_EnableViewSourceMode(render_view_host->routing_id()));
  }
}

void WebContents::RenderViewReady(RenderViewHost* rvh) {
  if (rvh != render_view_host()) {
    // Don't notify the world, since this came from a renderer in the
    // background.
    return;
  }

  NotifyConnected();
  SetIsCrashed(false);
}

void WebContents::RenderViewGone(RenderViewHost* rvh) {
  // Ask the print preview if this renderer was valuable.
  if (!printing_.OnRenderViewGone(rvh))
    return;
  if (rvh != render_view_host()) {
    // The pending page's RenderViewHost is gone.
    return;
  }

  SetIsLoading(false, NULL);
  NotifyDisconnected();
  SetIsCrashed(true);

  // Force an invalidation to render sad tab. The view will notice we crashed
  // when it paints.
  view_->Invalidate();

  // Hide any visible hung renderer warning for this web contents' process.
  HungRendererWarning::HideForWebContents(this);
}

void WebContents::DidNavigate(RenderViewHost* rvh,
                              const ViewHostMsg_FrameNavigate_Params& params) {
  if (PageTransition::IsMainFrame(params.transition))
    render_manager_.DidNavigateMainFrame(rvh);

  // Update the site of the SiteInstance if it doesn't have one yet.
  if (!GetSiteInstance()->has_site())
    GetSiteInstance()->SetSite(params.url);

  // Need to update MIME type here because it's referred to in
  // UpdateNavigationCommands() called by RendererDidNavigate() to
  // determine whether or not to enable the encoding menu.
  // It's updated only for the main frame. For a subframe,
  // RenderView::UpdateURL does not set params.contents_mime_type.
  // (see http://code.google.com/p/chromium/issues/detail?id=2929 )
  // TODO(jungshik): Add a test for the encoding menu to avoid
  // regressing it again.
  if (PageTransition::IsMainFrame(params.transition))
    contents_mime_type_ = params.contents_mime_type;

  NavigationController::LoadCommittedDetails details;
  if (!controller_.RendererDidNavigate(params, &details))
    return;  // No navigation happened.

  // DO NOT ADD MORE STUFF TO THIS FUNCTION! Your component should either listen
  // for the appropriate notification (best) or you can add it to
  // DidNavigateMainFramePostCommit / DidNavigateAnyFramePostCommit (only if
  // necessary, please).

  // Run post-commit tasks.
  if (details.is_main_frame)
    DidNavigateMainFramePostCommit(details, params);
  DidNavigateAnyFramePostCommit(rvh, details, params);
}

void WebContents::UpdateState(RenderViewHost* rvh,
                              int32 page_id,
                              const std::string& state) {
  DCHECK(rvh == render_view_host());

  // We must be prepared to handle state updates for any page, these occur
  // when the user is scrolling and entering form data, as well as when we're
  // leaving a page, in which case our state may have already been moved to
  // the next page. The navigation controller will look up the appropriate
  // NavigationEntry and update it when it is notified via the delegate.

  int entry_index = controller_.GetEntryIndexWithPageID(
      GetSiteInstance(), page_id);
  if (entry_index < 0)
    return;
  NavigationEntry* entry = controller_.GetEntryAtIndex(entry_index);

  if (state == entry->content_state())
    return;  // Nothing to update.
  entry->set_content_state(state);
  controller_.NotifyEntryChanged(entry, entry_index);
}

void WebContents::UpdateTitle(RenderViewHost* rvh,
                              int32 page_id, const std::wstring& title) {
  // If we have a title, that's a pretty good indication that we've started
  // getting useful data.
  SetNotWaitingForResponse();

  DCHECK(rvh == render_view_host());
  NavigationEntry* entry = controller_.GetEntryWithPageID(GetSiteInstance(),
                                                            page_id);
  if (!entry || !UpdateTitleForEntry(entry, title))
    return;

  // Broadcast notifications when the UI should be updated.
  if (entry == controller_.GetEntryAtOffset(0))
    NotifyNavigationStateChanged(INVALIDATE_TITLE);
}

void WebContents::UpdateFeedList(
    RenderViewHost* rvh, const ViewHostMsg_UpdateFeedList_Params& params) {
  // We might have an old RenderViewHost sending messages, and we should ignore
  // those messages.
  if (rvh != render_view_host())
    return;

  NavigationEntry* entry = controller_.GetEntryWithPageID(GetSiteInstance(),
                                                            params.page_id);
  if (!entry)
    return;

  entry->set_feedlist(params.feedlist);

  // Broadcast notifications when the UI should be updated.
  if (entry == controller_.GetEntryAtOffset(0))
    NotifyNavigationStateChanged(INVALIDATE_FEEDLIST);
}

void WebContents::UpdateEncoding(RenderViewHost* render_view_host,
                                 const std::wstring& encoding) {
  set_encoding(encoding);
}

void WebContents::UpdateTargetURL(int32 page_id, const GURL& url) {
  if (delegate())
    delegate()->UpdateTargetURL(this, url);
}

void WebContents::UpdateThumbnail(const GURL& url,
                                  const SkBitmap& bitmap,
                                  const ThumbnailScore& score) {
  // Tell History about this thumbnail
  HistoryService* hs;
  if (!profile()->IsOffTheRecord() &&
      (hs = profile()->GetHistoryService(Profile::IMPLICIT_ACCESS))) {
    hs->SetPageThumbnail(url, bitmap, score);
  }
}

void WebContents::Close(RenderViewHost* rvh) {
  // Ignore this if it comes from a RenderViewHost that we aren't showing.
  if (delegate() && rvh == render_view_host())
    delegate()->CloseContents(this);
}

void WebContents::RequestMove(const gfx::Rect& new_bounds) {
  if (delegate() && delegate()->IsPopup(this))
    delegate()->MoveContents(this, new_bounds);
}

void WebContents::DidStartLoading(RenderViewHost* rvh, int32 page_id) {
  SetIsLoading(true, NULL);
}

void WebContents::DidStopLoading(RenderViewHost* rvh, int32 page_id) {
  scoped_ptr<LoadNotificationDetails> details;

  NavigationEntry* entry = controller_.GetActiveEntry();
  // An entry may not exist for a stop when loading an initial blank page or
  // if an iframe injected by script into a blank page finishes loading.
  if (entry) {
    scoped_ptr<base::ProcessMetrics> metrics(
        base::ProcessMetrics::CreateProcessMetrics(
            process()->process().handle()));

    TimeDelta elapsed = TimeTicks::Now() - current_load_start_;

    details.reset(new LoadNotificationDetails(
        entry->display_url(),
        entry->transition_type(),
        elapsed,
        &controller_,
        controller_.GetCurrentEntryIndex()));
  }

  // Tell PasswordManager we've finished a page load, which serves as a
  // green light to save pending passwords and reset itself.
  GetPasswordManager()->DidStopLoading();

  SetIsLoading(false, details.get());
}

void WebContents::DidStartProvisionalLoadForFrame(
    RenderViewHost* render_view_host,
    bool is_main_frame,
    const GURL& url) {
  ProvisionalLoadDetails details(is_main_frame,
                                 controller_.IsURLInPageNavigation(url),
                                 url, std::string(), false);
  NotificationService::current()->Notify(
      NotificationType::FRAME_PROVISIONAL_LOAD_START,
      Source<NavigationController>(&controller_),
      Details<ProvisionalLoadDetails>(&details));
}

void WebContents::DidRedirectProvisionalLoad(int32 page_id,
                                             const GURL& source_url,
                                             const GURL& target_url) {
  NavigationEntry* entry;
  if (page_id == -1)
    entry = controller_.pending_entry();
  else
    entry = controller_.GetEntryWithPageID(GetSiteInstance(), page_id);
  if (!entry || entry->url() != source_url)
    return;
  entry->set_url(target_url);
}

void WebContents::DidLoadResourceFromMemoryCache(
    const GURL& url,
    const std::string& frame_origin,
    const std::string& main_frame_origin,
    const std::string& security_info) {
  // Send out a notification that we loaded a resource from our memory cache.
  int cert_id = 0, cert_status = 0, security_bits = 0;
  SSLManager::DeserializeSecurityInfo(security_info,
                                      &cert_id, &cert_status,
                                      &security_bits);
  LoadFromMemoryCacheDetails details(url, frame_origin, main_frame_origin,
                                     cert_id, cert_status);

  NotificationService::current()->Notify(
      NotificationType::LOAD_FROM_MEMORY_CACHE,
      Source<NavigationController>(&controller_),
      Details<LoadFromMemoryCacheDetails>(&details));
}

void WebContents::DidFailProvisionalLoadWithError(
    RenderViewHost* render_view_host,
    bool is_main_frame,
    int error_code,
    const GURL& url,
    bool showing_repost_interstitial) {
  if (net::ERR_ABORTED == error_code) {
    // EVIL HACK ALERT! Ignore failed loads when we're showing interstitials.
    // This means that the interstitial won't be torn down properly, which is
    // bad. But if we have an interstitial, go back to another tab type, and
    // then load the same interstitial again, we could end up getting the first
    // interstitial's "failed" message (as a result of the cancel) when we're on
    // the second one.
    //
    // We can't tell this apart, so we think we're tearing down the current page
    // which will cause a crash later one. There is also some code in
    // RenderViewHostManager::RendererAbortedProvisionalLoad that is commented
    // out because of this problem.
    //
    // http://code.google.com/p/chromium/issues/detail?id=2855
    // Because this will not tear down the interstitial properly, if "back" is
    // back to another tab type, the interstitial will still be somewhat alive
    // in the previous tab type. If you navigate somewhere that activates the
    // tab with the interstitial again, you'll see a flash before the new load
    // commits of the interstitial page.
    if (showing_interstitial_page()) {
      LOG(WARNING) << "Discarding message during interstitial.";
      return;
    }

    // This will discard our pending entry if we cancelled the load (e.g., if we
    // decided to download the file instead of load it). Only discard the
    // pending entry if the URLs match, otherwise the user initiated a navigate
    // before the page loaded so that the discard would discard the wrong entry.
    NavigationEntry* pending_entry = controller_.pending_entry();
    if (pending_entry && pending_entry->url() == url)
      controller_.DiscardNonCommittedEntries();

    render_manager_.RendererAbortedProvisionalLoad(render_view_host);
  }

  // Send out a notification that we failed a provisional load with an error.
  ProvisionalLoadDetails details(is_main_frame,
                                 controller_.IsURLInPageNavigation(url),
                                 url, std::string(), false);
  details.set_error_code(error_code);

  NotificationService::current()->Notify(
      NotificationType::FAIL_PROVISIONAL_LOAD_WITH_ERROR,
      Source<NavigationController>(&controller_),
      Details<ProvisionalLoadDetails>(&details));
}

void WebContents::UpdateFavIconURL(RenderViewHost* render_view_host,
                                   int32 page_id,
                                   const GURL& icon_url) {
  fav_icon_helper_.SetFavIconURL(icon_url);
}

void WebContents::DidDownloadImage(
    RenderViewHost* render_view_host,
    int id,
    const GURL& image_url,
    bool errored,
    const SkBitmap& image) {
  // A notification for downloading would be more flexible, but for now I'm
  // forwarding to the two places that could possibly have initiated the
  // request. If we end up with another place invoking DownloadImage, probably
  // best to refactor out into notification service, or something similar.
  if (errored)
    fav_icon_helper_.FavIconDownloadFailed(id);
  else
    fav_icon_helper_.SetFavIcon(id, image_url, image);
}

void WebContents::RequestOpenURL(const GURL& url, const GURL& referrer,
                                 WindowOpenDisposition disposition) {
  if (render_manager_.dom_ui()) {
    // When we're a DOM UI, it will provide a page transition type for us (this
    // is so the new tab page can specify AUTO_BOOKMARK for automatically
    // generated suggestions).
    //
    // Note also that we hide the referrer for DOM UI pages. We don't really
    // want web sites to see a referrer of "chrome-ui://blah" (and some
    // chrome-ui URLs might have search terms or other stuff we don't want to
    // send to the site), so we send no referrer.
    OpenURL(url, GURL(), disposition,
            render_manager_.dom_ui()->link_transition_type());
  } else {
    OpenURL(url, referrer, disposition, PageTransition::LINK);
  }
}

void WebContents::DomOperationResponse(const std::string& json_string,
                                       int automation_id) {
  DomOperationNotificationDetails details(json_string, automation_id);
  NotificationService::current()->Notify(
      NotificationType::DOM_OPERATION_RESPONSE, Source<WebContents>(this),
      Details<DomOperationNotificationDetails>(&details));
}

void WebContents::ProcessDOMUIMessage(const std::string& message,
                                      const std::string& content) {
  if (!render_manager_.dom_ui()) {
    // We shouldn't get a DOM UI message when we haven't enabled the DOM UI.
    // Because the renderer might be owned and sending random messages, we need
    // to ignore these inproper ones.
    NOTREACHED();
    return;
  }
  render_manager_.dom_ui()->ProcessDOMUIMessage(message, content);
}

void WebContents::ProcessExternalHostMessage(const std::string& message,
                                             const std::string& origin,
                                             const std::string& target) {
  if (delegate())
    delegate()->ForwardMessageToExternalHost(message, origin, target);
}

void WebContents::GoToEntryAtOffset(int offset) {
  controller_.GoToOffset(offset);
}

void WebContents::GetHistoryListCount(int* back_list_count,
                                      int* forward_list_count) {
  int current_index = controller_.last_committed_entry_index();
  *back_list_count = current_index;
  *forward_list_count = controller_.entry_count() - current_index - 1;
}

void WebContents::RunFileChooser(bool multiple_files,
                                 const string16& title,
                                 const FilePath& default_file) {
  if (!select_file_dialog_.get())
    select_file_dialog_ = SelectFileDialog::Create(this);
  SelectFileDialog::Type dialog_type =
    multiple_files ? SelectFileDialog::SELECT_OPEN_MULTI_FILE :
                     SelectFileDialog::SELECT_OPEN_FILE;
  select_file_dialog_->SelectFile(dialog_type, title, default_file,
                                  NULL, 0, FILE_PATH_LITERAL(""),
                                  view_->GetTopLevelNativeWindow(), NULL);
}

void WebContents::RunJavaScriptMessage(
    const std::wstring& message,
    const std::wstring& default_prompt,
    const GURL& frame_url,
    const int flags,
    IPC::Message* reply_msg,
    bool* did_suppress_message) {
  // Suppress javascript messages when requested and when inside a constrained
  // popup window (because that activates them and breaks them out of the
  // constrained window jail).
  bool suppress_this_message = suppress_javascript_messages_;
  if (delegate())
    suppress_this_message |=
        (delegate()->GetConstrainingContents(this) != NULL);

  *did_suppress_message = suppress_this_message;

  if (!suppress_this_message) {
    TimeDelta time_since_last_message(
        TimeTicks::Now() - last_javascript_message_dismissal_);
    bool show_suppress_checkbox = false;
    // Show a checkbox offering to suppress further messages if this message is
    // being displayed within kJavascriptMessageExpectedDelay of the last one.
    if (time_since_last_message <
        TimeDelta::FromMilliseconds(kJavascriptMessageExpectedDelay))
      show_suppress_checkbox = true;

    RunJavascriptMessageBox(this, frame_url, flags, message, default_prompt,
                            show_suppress_checkbox, reply_msg);
  } else {
    // If we are suppressing messages, just reply as is if the user immediately
    // pressed "Cancel".
    OnJavaScriptMessageBoxClosed(reply_msg, false, std::wstring());
  }
}

void WebContents::RunBeforeUnloadConfirm(const std::wstring& message,
                                         IPC::Message* reply_msg) {
  RunBeforeUnloadDialog(this, message, reply_msg);
}

void WebContents::ShowModalHTMLDialog(const GURL& url, int width, int height,
                                      const std::string& json_arguments,
                                      IPC::Message* reply_msg) {
  if (delegate()) {
    HtmlDialogUIDelegate* dialog_delegate =
        new ModalHtmlDialogDelegate(url, width, height, json_arguments,
                                    reply_msg, this);
    delegate()->ShowHtmlDialog(dialog_delegate, NULL);
  }
}

void WebContents::PasswordFormsSeen(
    const std::vector<PasswordForm>& forms) {
  GetPasswordManager()->PasswordFormsSeen(forms);
}

void WebContents::AutofillFormSubmitted(
    const AutofillForm& form) {
  GetAutofillManager()->AutofillFormSubmitted(form);
}

void WebContents::GetAutofillSuggestions(const std::wstring& field_name,
    const std::wstring& user_text, int64 node_id, int request_id) {
  GetAutofillManager()->FetchValuesForName(field_name, user_text,
      kMaxAutofillMenuItems, node_id, request_id);
}

void WebContents::RemoveAutofillEntry(const std::wstring& field_name,
                                      const std::wstring& value) {
  GetAutofillManager()->RemoveValueForName(field_name, value);
}

// Checks to see if we should generate a keyword based on the OSDD, and if
// necessary uses TemplateURLFetcher to download the OSDD and create a keyword.
void WebContents::PageHasOSDD(RenderViewHost* render_view_host,
                              int32 page_id, const GURL& url,
                              bool autodetected) {
  // Make sure page_id is the current page, and the TemplateURLModel is loaded.
  DCHECK(url.is_valid());
  if (!IsActiveEntry(page_id))
    return;
  TemplateURLModel* url_model = profile()->GetTemplateURLModel();
  if (!url_model)
    return;
  if (!url_model->loaded()) {
    url_model->Load();
    return;
  }
  if (!profile()->GetTemplateURLFetcher())
    return;

  if (profile()->IsOffTheRecord())
    return;

  const NavigationEntry* entry = controller_.GetLastCommittedEntry();
  DCHECK(entry);

  const NavigationEntry* base_entry = entry;
  if (IsFormSubmit(base_entry)) {
    // If the current page is a form submit, find the last page that was not
    // a form submit and use its url to generate the keyword from.
    int index = controller_.last_committed_entry_index() - 1;
    while (index >= 0 && IsFormSubmit(controller_.GetEntryAtIndex(index)))
      index--;
    if (index >= 0)
      base_entry = controller_.GetEntryAtIndex(index);
    else
      base_entry = NULL;
  }

  // We want to use the user typed URL if available since that represents what
  // the user typed to get here, and fall back on the regular URL if not.
  if (!base_entry)
    return;
  GURL keyword_url = base_entry->user_typed_url().is_valid() ?
          base_entry->user_typed_url() : base_entry->url();
  if (!keyword_url.is_valid())
    return;
  std::wstring keyword = TemplateURLModel::GenerateKeyword(keyword_url,
                                                           autodetected);
  if (keyword.empty())
    return;
  const TemplateURL* template_url =
      url_model->GetTemplateURLForKeyword(keyword);
  if (template_url && (!template_url->safe_for_autoreplace() ||
                       template_url->originating_url() == url)) {
    // Either there is a user created TemplateURL for this keyword, or the
    // keyword has the same OSDD url and we've parsed it.
    return;
  }

  // Download the OpenSearch description document. If this is successful a
  // new keyword will be created when done.
#if defined(OS_WIN)
  gfx::NativeView ancestor = GetAncestor(view_->GetNativeView(), GA_ROOT);
#else
  gfx::NativeView ancestor = NULL;
#endif
  profile()->GetTemplateURLFetcher()->ScheduleDownload(
      keyword,
      url,
      base_entry->favicon().url(),
      ancestor,
      autodetected);
}

void WebContents::InspectElementReply(int num_resources) {
  // We have received reply from inspect element request. Notify the
  // automation provider in case we need to notify automation client.
  NotificationService::current()->Notify(
      NotificationType::DOM_INSPECT_ELEMENT_RESPONSE, Source<WebContents>(this),
      Details<int>(&num_resources));
}

void WebContents::DidGetPrintedPagesCount(int cookie, int number_pages) {
  printing_.DidGetPrintedPagesCount(cookie, number_pages);
}

void WebContents::DidPrintPage(const ViewHostMsg_DidPrintPage_Params& params) {
  printing_.DidPrintPage(params);
}

GURL WebContents::GetAlternateErrorPageURL() const {
  GURL url;
  // Disable alternate error pages when in OffTheRecord/Incognito mode.
  if (profile()->IsOffTheRecord())
    return url;

  PrefService* prefs = profile()->GetPrefs();
  DCHECK(prefs);
  if (prefs->GetBoolean(prefs::kAlternateErrorPagesEnabled)) {
    url = google_util::AppendGoogleLocaleParam(GURL(kLinkDoctorBaseURL));
    url = google_util::AppendGoogleTLDParam(url);
  }
  return url;
}

WebPreferences WebContents::GetWebkitPrefs() {
  PrefService* prefs = render_view_host()->process()->profile()->GetPrefs();
  bool isDomUI = false;
  return RenderViewHostDelegateHelper::GetWebkitPrefs(prefs, isDomUI);
}

void WebContents::OnMissingPluginStatus(int status) {
#if defined(OS_WIN)
// TODO(PORT): pull in when plug-ins work
  GetPluginInstaller()->OnMissingPluginStatus(status);
#endif
}

void WebContents::OnCrashedPlugin(const FilePath& plugin_path) {
#if defined(OS_WIN)
// TODO(PORT): pull in when plug-ins work
  DCHECK(!plugin_path.value().empty());

  std::wstring plugin_name = plugin_path.ToWStringHack();
  scoped_ptr<FileVersionInfo> version_info(
      FileVersionInfo::CreateFileVersionInfo(plugin_path));
  if (version_info.get()) {
    const std::wstring& product_name = version_info->product_name();
    if (!product_name.empty())
      plugin_name = product_name;
  }
  AddInfoBar(new SimpleAlertInfoBarDelegate(
      this, l10n_util::GetStringF(IDS_PLUGIN_CRASHED_PROMPT, plugin_name),
      NULL));
#endif
}

void WebContents::OnCrashedWorker() {
  AddInfoBar(new SimpleAlertInfoBarDelegate(
      this, l10n_util::GetString(IDS_WEBWORKER_CRASHED_PROMPT),
      NULL));
}

void WebContents::OnJSOutOfMemory() {
  AddInfoBar(new SimpleAlertInfoBarDelegate(
      this, l10n_util::GetString(IDS_JS_OUT_OF_MEMORY_PROMPT), NULL));
}

bool WebContents::CanBlur() const {
  return delegate() ? delegate()->CanBlur() : true;
}

gfx::Rect WebContents::GetRootWindowResizerRect() const {
  if (delegate())
    return delegate()->GetRootWindowResizerRect();
  return gfx::Rect();
}

void WebContents::RendererUnresponsive(RenderViewHost* rvh,
                                       bool is_during_unload) {
  if (is_during_unload) {
    // Hang occurred while firing the beforeunload/unload handler.
    // Pretend the handler fired so tab closing continues as if it had.
    rvh->UnloadListenerHasFired();

    if (!render_manager_.ShouldCloseTabOnUnresponsiveRenderer())
      return;

    // If the tab hangs in the beforeunload/unload handler there's really
    // nothing we can do to recover. Pretend the unload listeners have
    // all fired and close the tab. If the hang is in the beforeunload handler
    // then the user will not have the option of cancelling the close.
    Close(rvh);
    return;
  }

  if (render_view_host() && render_view_host()->IsRenderViewLive())
    HungRendererWarning::ShowForWebContents(this);
}

void WebContents::RendererResponsive(RenderViewHost* render_view_host) {
  HungRendererWarning::HideForWebContents(this);
}

void WebContents::LoadStateChanged(const GURL& url,
                                   net::LoadState load_state) {
  load_state_ = load_state;
  load_state_host_ = UTF8ToWide(url.host());
  if (load_state_ == net::LOAD_STATE_READING_RESPONSE)
    SetNotWaitingForResponse();
  if (is_loading())
    NotifyNavigationStateChanged(INVALIDATE_LOAD | INVALIDATE_FAVICON);
}

void WebContents::OnDidGetApplicationInfo(
    int32 page_id,
    const webkit_glue::WebApplicationInfo& info) {
  if (pending_install_.page_id != page_id)
    return;  // The user clicked create on a separate page. Ignore this.

  pending_install_.callback_functor =
      new GearsCreateShortcutCallbackFunctor(this);
  GearsCreateShortcut(
      info, pending_install_.title, pending_install_.url, pending_install_.icon,
      NewCallback(pending_install_.callback_functor,
                  &GearsCreateShortcutCallbackFunctor::Run));
}

void WebContents::OnEnterOrSpace() {
  // See comment in RenderViewHostDelegate::OnEnterOrSpace as to why we do this.
#if defined(OS_WIN)
  // TODO(port): this is stubbed in BrowserProcess
  DownloadRequestManager* drm = g_browser_process->download_request_manager();
  if (drm)
    drm->OnUserGesture(this);
#endif
}

void WebContents::OnFindReply(int request_id,
                              int number_of_matches,
                              const gfx::Rect& selection_rect,
                              int active_match_ordinal,
                              bool final_update) {
  // Ignore responses for requests other than the one we have most recently
  // issued. That way we won't act on stale results when the user has
  // already typed in another query.
  if (request_id != current_find_request_id_)
    return;

  if (number_of_matches == -1)
    number_of_matches = find_result_.number_of_matches();
  if (active_match_ordinal == -1)
    active_match_ordinal = find_result_.active_match_ordinal();

  // Notify the UI, automation and any other observers that a find result was
  // found.
  find_result_ = FindNotificationDetails(request_id, number_of_matches,
                                         selection_rect, active_match_ordinal,
                                         final_update);
  NotificationService::current()->Notify(
      NotificationType::FIND_RESULT_AVAILABLE,
      Source<TabContents>(this),
      Details<FindNotificationDetails>(&find_result_));
}

bool WebContents::CanTerminate() const {
  if (!delegate())
    return true;

  return !delegate()->IsExternalTabContainer();
}

void WebContents::FileSelected(const FilePath& path,
                               int index, void* params) {
  render_view_host()->FileSelected(path);
}

void WebContents::MultiFilesSelected(const std::vector<FilePath>& files,
                                     void* params) {
  render_view_host()->MultiFilesSelected(files);
}

void WebContents::FileSelectionCanceled(void* params) {
  // If the user cancels choosing a file to upload we pass back an
  // empty vector.
  render_view_host()->MultiFilesSelected(std::vector<FilePath>());
}

void WebContents::BeforeUnloadFiredFromRenderManager(
    bool proceed,
    bool* proceed_to_fire_unload) {
  if (delegate())
    delegate()->BeforeUnloadFired(this, proceed, proceed_to_fire_unload);
}

void WebContents::UpdateRenderViewSizeForRenderManager() {
  // TODO(brettw) this is a hack. See TabContentsView::SizeContents.
  view_->SizeContents(view_->GetContainerSize());
}

DOMUI* WebContents::CreateDOMUIForRenderManager(const GURL& url) {
  return DOMUIFactory::CreateDOMUIForURL(this, url);
}

NavigationEntry*
WebContents::GetLastCommittedNavigationEntryForRenderManager() {
  return controller_.GetLastCommittedEntry();
}

bool WebContents::CreateRenderViewForRenderManager(
    RenderViewHost* render_view_host) {
  // When we're running a DOM UI, the RenderViewHost needs to be put in DOM UI
  // mode before CreateRenderView is called. When we're asked to create a
  // RenderView, that means it's for the pending entry, so we have to use the
  // pending DOM UI.
  if (render_manager_.pending_dom_ui())
    render_view_host->AllowDOMUIBindings();

  RenderWidgetHostView* rwh_view = view_->CreateViewForWidget(render_view_host);
  if (!render_view_host->CreateRenderView())
    return false;

  // Now that the RenderView has been created, we need to tell it its size.
  rwh_view->SetSize(view_->GetContainerSize());

  UpdateMaxPageIDIfNecessary(render_view_host->site_instance(),
                             render_view_host);
  return true;
}

void WebContents::Observe(NotificationType type,
                          const NotificationSource& source,
                          const NotificationDetails& details) {
  switch (type.value) {
    case NotificationType::BOOKMARK_MODEL_LOADED:
      // BookmarkModel finished loading, fall through to update starred state.
    case NotificationType::URLS_STARRED: {
      // Somewhere, a URL has been starred.
      // Ignore notifications for profiles other than our current one.
      Profile* source_profile = Source<Profile>(source).ptr();
      if (!source_profile || !source_profile->IsSameProfile(profile()))
        return;

      UpdateStarredStateForCurrentURL();
      break;
    }
    case NotificationType::PREF_CHANGED: {
      std::wstring* pref_name_in = Details<std::wstring>(details).ptr();
      DCHECK(Source<PrefService>(source).ptr() == profile()->GetPrefs());
      if (*pref_name_in == prefs::kAlternateErrorPagesEnabled) {
        UpdateAlternateErrorPageURL();
      } else if (*pref_name_in == prefs::kDefaultCharset ||
          StartsWithASCII(WideToUTF8(*pref_name_in), "webkit.webprefs.", true)
          ) {
        UpdateWebPreferences();
      } else {
        NOTREACHED() << "unexpected pref change notification" << *pref_name_in;
      }
      break;
    }
    case NotificationType::RENDER_WIDGET_HOST_DESTROYED:
      view_->RenderWidgetHostDestroyed(Source<RenderWidgetHost>(source).ptr());
      break;

    case NotificationType::NAV_ENTRY_COMMITTED: {
      DCHECK(&controller_ == Source<NavigationController>(source).ptr());

      NavigationController::LoadCommittedDetails& committed_details =
          *(Details<NavigationController::LoadCommittedDetails>(details).ptr());
      ExpireInfoBars(committed_details);
      break;
    }

    default:
      NOTREACHED();
  }
}

void WebContents::DidNavigateMainFramePostCommit(
    const NavigationController::LoadCommittedDetails& details,
    const ViewHostMsg_FrameNavigate_Params& params) {
  // Hide the download shelf if all the following conditions are true:
  // - there are no active downloads.
  // - this is a navigation to a different TLD.
  // - at least 5 seconds have elapsed since the download shelf was shown.
  // TODO(jcampan): bug 1156075 when user gestures are reliable, they should
  //                 be used to ensure we are hiding only on user initiated
  //                 navigations.
  DownloadManager* download_manager = profile()->GetDownloadManager();
  // download_manager can be NULL in unit test context.
  if (download_manager && download_manager->in_progress_count() == 0 &&
      !details.previous_url.is_empty() &&
      !net::RegistryControlledDomainService::SameDomainOrHost(
          details.previous_url, details.entry->url())) {
    TimeDelta time_delta(
        TimeTicks::Now() - last_download_shelf_show_);
    if (time_delta >
        TimeDelta::FromMilliseconds(kDownloadShelfHideDelay)) {
      SetDownloadShelfVisible(false);
    }
  }

  if (details.is_user_initiated_main_frame_load()) {
    // Clear the status bubble. This is a workaround for a bug where WebKit
    // doesn't let us know that the cursor left an element during a
    // transition (this is also why the mouse cursor remains as a hand after
    // clicking on a link); see bugs 1184641 and 980803. We don't want to
    // clear the bubble when a user navigates to a named anchor in the same
    // page.
    UpdateTargetURL(details.entry->page_id(), GURL());

    // UpdateHelpersForDidNavigate will handle the case where the password_form
    // origin is valid.
    // TODO(brettw) bug 1343111: Password manager stuff in here needs to be
    // cleaned up and covered by tests.
    if (!params.password_form.origin.is_valid())
      GetPasswordManager()->DidNavigate();
  }

  // The keyword generator uses the navigation entries, so must be called after
  // the commit.
  GenerateKeywordIfNecessary(params);

  // Allow the new page to set the title again.
  received_page_title_ = false;

  // Get the favicon, either from history or request it from the net.
  fav_icon_helper_.FetchFavIcon(details.entry->url());

  // Close constrained popups if necessary.
  MaybeCloseChildWindows(details.previous_url, details.entry->url());

  // Update the starred state.
  UpdateStarredStateForCurrentURL();
}

void WebContents::DidNavigateAnyFramePostCommit(
    RenderViewHost* render_view_host,
    const NavigationController::LoadCommittedDetails& details,
    const ViewHostMsg_FrameNavigate_Params& params) {
  // If we navigate, start showing messages again. This does nothing to prevent
  // a malicious script from spamming messages, since the script could just
  // reload the page to stop blocking.
  suppress_javascript_messages_ = false;

  // Update history. Note that this needs to happen after the entry is complete,
  // which WillNavigate[Main,Sub]Frame will do before this function is called.
  if (params.should_update_history) {
    // Most of the time, the displayURL matches the loaded URL, but for about:
    // URLs, we use a data: URL as the real value.  We actually want to save
    // the about: URL to the history db and keep the data: URL hidden. This is
    // what the TabContents' URL getter does.
    UpdateHistoryForNavigation(GetURL(), params);
  }

  // Notify the password manager of the navigation or form submit.
  // TODO(brettw) bug 1343111: Password manager stuff in here needs to be
  // cleaned up and covered by tests.
  if (params.password_form.origin.is_valid())
    GetPasswordManager()->ProvisionallySavePassword(params.password_form);
}

void WebContents::MaybeCloseChildWindows(const GURL& previous_url,
                                         const GURL& current_url) {
  if (net::RegistryControlledDomainService::SameDomainOrHost(
          previous_url, current_url))
    return;

  // Clear out any child windows since we are leaving this page entirely.
  // We use indices instead of iterators in case CloseWindow does something
  // that may invalidate an iterator.
  int size = static_cast<int>(child_windows_.size());
  for (int i = size - 1; i >= 0; --i) {
    ConstrainedWindow* window = child_windows_[i];
    if (window)
      window->CloseConstrainedWindow();
  }
}

void WebContents::UpdateStarredStateForCurrentURL() {
  BookmarkModel* model = profile()->GetBookmarkModel();
  const bool old_state = is_starred_;
  is_starred_ = (model && model->IsBookmarked(GetURL()));

  if (is_starred_ != old_state && delegate())
    delegate()->URLStarredChanged(this, is_starred_);
}

void WebContents::UpdateAlternateErrorPageURL() {
  GURL url = GetAlternateErrorPageURL();
  render_view_host()->SetAlternateErrorPageURL(url);
}

void WebContents::UpdateWebPreferences() {
  render_view_host()->UpdateWebPreferences(GetWebkitPrefs());
}

void WebContents::UpdateMaxPageIDIfNecessary(SiteInstance* site_instance,
                                             RenderViewHost* rvh) {
  // If we are creating a RVH for a restored controller, then we might
  // have more page IDs than the SiteInstance's current max page ID.  We must
  // make sure that the max page ID is larger than any restored page ID.
  // Note that it is ok for conflicting page IDs to exist in another tab
  // (i.e., NavigationController), but if any page ID is larger than the max,
  // the back/forward list will get confused.
  int max_restored_page_id = controller_.max_restored_page_id();
  if (max_restored_page_id > 0) {
    int curr_max_page_id = site_instance->max_page_id();
    if (max_restored_page_id > curr_max_page_id) {
      // Need to update the site instance immediately.
      site_instance->UpdateMaxPageID(max_restored_page_id);

      // Also tell the renderer to update its internal representation.  We
      // need to reserve enough IDs to make all restored page IDs less than
      // the max.
      if (curr_max_page_id < 0)
        curr_max_page_id = 0;
      rvh->ReservePageIDRange(max_restored_page_id - curr_max_page_id);
    }
  }
}

void WebContents::UpdateHistoryForNavigation(
    const GURL& display_url,
    const ViewHostMsg_FrameNavigate_Params& params) {
  if (profile()->IsOffTheRecord())
    return;

  // Add to history service.
  HistoryService* hs = profile()->GetHistoryService(Profile::IMPLICIT_ACCESS);
  if (hs) {
    if (PageTransition::IsMainFrame(params.transition) &&
        display_url != params.url) {
      // Hack on the "display" URL so that it will appear in history. For some
      // types of URLs, we will display a magic URL that is different from where
      // the page is actually navigated. We want the user to see in history
      // what they saw in the URL bar, so we add the display URL as a redirect.
      // This only applies to the main frame, as the display URL doesn't apply
      // to sub-frames.
      std::vector<GURL> redirects = params.redirects;
      if (!redirects.empty())
        redirects.back() = display_url;
      hs->AddPage(display_url, this, params.page_id, params.referrer,
                  params.transition, redirects);
    } else {
      hs->AddPage(params.url, this, params.page_id, params.referrer,
                  params.transition, params.redirects);
    }
  }
}

bool WebContents::UpdateTitleForEntry(NavigationEntry* entry,
                                      const std::wstring& title) {
  // For file URLs without a title, use the pathname instead. In the case of a
  // synthesized title, we don't want the update to count toward the "one set
  // per page of the title to history."
  std::wstring final_title;
  bool explicit_set;
  if (entry->url().SchemeIsFile() && title.empty()) {
    final_title = UTF8ToWide(entry->url().ExtractFileName());
    explicit_set = false;  // Don't count synthetic titles toward the set limit.
  } else {
    TrimWhitespace(title, TRIM_ALL, &final_title);
    explicit_set = true;
  }

  if (final_title == UTF16ToWideHack(entry->title()))
    return false;  // Nothing changed, don't bother.

  entry->set_title(WideToUTF16Hack(final_title));

  // Update the history system for this page.
  if (!profile()->IsOffTheRecord() && !received_page_title_) {
    HistoryService* hs =
        profile()->GetHistoryService(Profile::IMPLICIT_ACCESS);
    if (hs)
      hs->SetPageTitle(entry->display_url(), final_title);

    // Don't allow the title to be saved again for explicitly set ones.
    received_page_title_ = explicit_set;
  }

  // Lastly, set the title for the view.
  view_->SetPageTitle(final_title);

  return true;
}

void WebContents::NotifySwapped() {
  // After sending out a swap notification, we need to send a disconnect
  // notification so that clients that pick up a pointer to |this| can NULL the
  // pointer.  See Bug 1230284.
  notify_disconnection_ = true;
  NotificationService::current()->Notify(
      NotificationType::WEB_CONTENTS_SWAPPED,
      Source<WebContents>(this),
      NotificationService::NoDetails());
}

void WebContents::NotifyConnected() {
  notify_disconnection_ = true;
  NotificationService::current()->Notify(
      NotificationType::WEB_CONTENTS_CONNECTED,
      Source<WebContents>(this),
      NotificationService::NoDetails());
}

void WebContents::NotifyDisconnected() {
  if (!notify_disconnection_)
    return;

  notify_disconnection_ = false;
  NotificationService::current()->Notify(
      NotificationType::WEB_CONTENTS_DISCONNECTED,
      Source<WebContents>(this),
      NotificationService::NoDetails());
}

void WebContents::GenerateKeywordIfNecessary(
    const ViewHostMsg_FrameNavigate_Params& params) {
  if (!params.searchable_form_url.is_valid())
    return;

  if (profile()->IsOffTheRecord())
    return;

  int last_index = controller_.last_committed_entry_index();
  // When there was no previous page, the last index will be 0. This is
  // normally due to a form submit that opened in a new tab.
  // TODO(brettw) bug 916126: we should support keywords when form submits
  //              happen in new tabs.
  if (last_index <= 0)
    return;
  const NavigationEntry* previous_entry =
      controller_.GetEntryAtIndex(last_index - 1);
  if (IsFormSubmit(previous_entry)) {
    // Only generate a keyword if the previous page wasn't itself a form
    // submit.
    return;
  }

  GURL keyword_url = previous_entry->user_typed_url().is_valid() ?
          previous_entry->user_typed_url() : previous_entry->url();
  std::wstring keyword =
      TemplateURLModel::GenerateKeyword(keyword_url, true);  // autodetected
  if (keyword.empty())
    return;

  TemplateURLModel* url_model = profile()->GetTemplateURLModel();
  if (!url_model)
    return;

  if (!url_model->loaded()) {
    url_model->Load();
    return;
  }

  const TemplateURL* current_url;
  std::wstring url = UTF8ToWide(params.searchable_form_url.spec());
  if (!url_model->CanReplaceKeyword(keyword, url, &current_url))
    return;

  if (current_url) {
    if (current_url->originating_url().is_valid()) {
      // The existing keyword was generated from an OpenSearch description
      // document, don't regenerate.
      return;
    }
    url_model->Remove(current_url);
  }
  TemplateURL* new_url = new TemplateURL();
  new_url->set_keyword(keyword);
  new_url->set_short_name(keyword);
  new_url->SetURL(url, 0, 0);
  new_url->add_input_encoding(params.searchable_form_encoding);
  DCHECK(controller_.GetLastCommittedEntry());
  const GURL& favicon_url =
      controller_.GetLastCommittedEntry()->favicon().url();
  if (favicon_url.is_valid()) {
    new_url->SetFavIconURL(favicon_url);
  } else {
    // The favicon url isn't valid. This means there really isn't a favicon,
    // or the favicon url wasn't obtained before the load started. This assumes
    // the later.
    // TODO(sky): Need a way to set the favicon that doesn't involve generating
    // its url.
    new_url->SetFavIconURL(TemplateURL::GenerateFaviconURL(params.referrer));
  }
  new_url->set_safe_for_autoreplace(true);
  url_model->Add(new_url);
}

DOMUI* WebContents::GetDOMUIForCurrentState() {
  // When there is a pending navigation entry, we want to use the pending DOMUI
  // that goes along with it to control the basic flags. For example, we want to
  // show the pending URL in the URL bar, so we want the display_url flag to
  // be from the pending entry.
  //
  // The confusion comes because there are multiple possibilities for the
  // initial load in a tab as a side effect of the way the RenderViewHostManager
  // works.
  //
  //  - For the very first tab the load looks "normal". The new tab DOM UI is
  //    the pending one, and we want it to apply here.
  //
  //  - For subsequent new tabs, they'll get a new SiteInstance which will then
  //    get switched to the one previously associated with the new tab pages.
  //    This switching will cause the manager to commit the RVH/DOMUI. So we'll
  //    have a committed DOM UI in this case.
  //
  // This condition handles all of these cases:
  //
  //  - First load in first tab: no committed nav entry + pending nav entry +
  //    pending dom ui:
  //    -> Use pending DOM UI if any.
  //
  //  - First load in second tab: no committed nav entry + pending nav entry +
  //    no pending DOM UI:
  //    -> Use the committed DOM UI if any.
  //
  //  - Second navigation in any tab: committed nav entry + pending nav entry:
  //    -> Use pending DOM UI if any.
  //
  //  - Normal state with no load: committed nav entry + no pending nav entry:
  //    -> Use committed DOM UI.
  if (controller_.pending_entry() &&
      (controller_.GetLastCommittedEntry() ||
       render_manager_.pending_dom_ui()))
    return render_manager_.pending_dom_ui();
  return render_manager_.dom_ui();
}
