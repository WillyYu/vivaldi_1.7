// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/guest_view/web_view/web_view_guest.h"

#include <stddef.h>

#include <utility>

#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "components/browsing_data/content/storage_partition_http_cache_data_remover.h"
#include "components/guest_view/browser/guest_view_event.h"
#include "components/guest_view/browser/guest_view_manager.h"
#include "components/guest_view/common/guest_view_constants.h"
#include "components/web_cache/browser/web_cache_manager.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/native_web_keyboard_event.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/resource_request_details.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/user_metrics.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/common/browser_side_navigation_policy.h"
#include "content/public/common/media_stream_request.h"
#include "content/public/common/page_zoom.h"
#include "content/public/common/result_codes.h"
#include "content/public/common/stop_find_action.h"
#include "content/public/common/url_constants.h"
#include "extensions/browser/api/declarative/rules_registry_service.h"
#include "extensions/browser/api/extensions_api_client.h"
#include "extensions/browser/api/guest_view/web_view/web_view_internal_api.h"
#include "extensions/browser/api/web_request/web_request_api.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/extensions_browser_client.h"
#include "extensions/browser/guest_view/web_view/web_view_constants.h"
#include "extensions/browser/guest_view/web_view/web_view_content_script_manager.h"
#include "extensions/browser/guest_view/web_view/web_view_permission_helper.h"
#include "extensions/browser/guest_view/web_view/web_view_permission_types.h"
#include "extensions/browser/guest_view/web_view/web_view_renderer_state.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension_messages.h"
#include "extensions/common/manifest_constants.h"
#include "extensions/strings/grit/extensions_strings.h"
#include "ipc/ipc_message_macros.h"
#include "net/base/escape.h"
#include "net/base/net_errors.h"
#include "ui/base/models/simple_menu_model.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "url/url_constants.h"

#ifdef VIVALDI_BUILD
#include "app/vivaldi_apptools.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tab_dialogs.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/profiles/profile.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "content/browser/browser_plugin/browser_plugin_guest.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "extensions/api/extension_action_utils/extension_action_utils_api.h"
#include "prefs/vivaldi_pref_names.h"

using vivaldi::IsVivaldiApp;
using vivaldi::IsVivaldiRunning;
#endif // VIVALDI_BUILD

using base::UserMetricsAction;
using content::GlobalRequestID;
using content::RenderFrameHost;
using content::RenderProcessHost;
using content::ResourceType;
using content::StoragePartition;
using content::WebContents;
using guest_view::GuestViewBase;
using guest_view::GuestViewEvent;
using guest_view::GuestViewManager;
using zoom::ZoomController;

namespace extensions {

namespace {

// Returns storage partition removal mask from web_view clearData mask. Note
// that storage partition mask is a subset of webview's data removal mask.
uint32_t GetStoragePartitionRemovalMask(uint32_t web_view_removal_mask) {
  uint32_t mask = 0;
  if (web_view_removal_mask & webview::WEB_VIEW_REMOVE_DATA_MASK_APPCACHE)
    mask |= StoragePartition::REMOVE_DATA_MASK_APPCACHE;
  if (web_view_removal_mask & webview::WEB_VIEW_REMOVE_DATA_MASK_COOKIES)
    mask |= StoragePartition::REMOVE_DATA_MASK_COOKIES;
  if (web_view_removal_mask & webview::WEB_VIEW_REMOVE_DATA_MASK_FILE_SYSTEMS)
    mask |= StoragePartition::REMOVE_DATA_MASK_FILE_SYSTEMS;
  if (web_view_removal_mask & webview::WEB_VIEW_REMOVE_DATA_MASK_INDEXEDDB)
    mask |= StoragePartition::REMOVE_DATA_MASK_INDEXEDDB;
  if (web_view_removal_mask & webview::WEB_VIEW_REMOVE_DATA_MASK_LOCAL_STORAGE)
    mask |= StoragePartition::REMOVE_DATA_MASK_LOCAL_STORAGE;
  if (web_view_removal_mask & webview::WEB_VIEW_REMOVE_DATA_MASK_WEBSQL)
    mask |= StoragePartition::REMOVE_DATA_MASK_WEBSQL;

  return mask;
}

std::string WindowOpenDispositionToString(
  WindowOpenDisposition window_open_disposition) {
  switch (window_open_disposition) {
    case WindowOpenDisposition::IGNORE_ACTION:
      return "ignore";
    case WindowOpenDisposition::SAVE_TO_DISK:
      return "save_to_disk";
    case WindowOpenDisposition::CURRENT_TAB:
      return "current_tab";
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
      return "new_background_tab";
    case WindowOpenDisposition::NEW_FOREGROUND_TAB:
      return "new_foreground_tab";
    case WindowOpenDisposition::NEW_WINDOW:
      return "new_window";
    case WindowOpenDisposition::NEW_POPUP:
      return "new_popup";
    case WindowOpenDisposition::OFF_THE_RECORD:
      return "off_the_record";
    default:
      NOTREACHED() << "Unknown Window Open Disposition";
      return "ignore";
  }
}

static std::string TerminationStatusToString(base::TerminationStatus status) {
  switch (status) {
    case base::TERMINATION_STATUS_NORMAL_TERMINATION:
      return "normal";
    case base::TERMINATION_STATUS_ABNORMAL_TERMINATION:
    case base::TERMINATION_STATUS_STILL_RUNNING:
      return "abnormal";
#if defined(OS_CHROMEOS)
    case base::TERMINATION_STATUS_PROCESS_WAS_KILLED_BY_OOM:
      return "oom killed";
#endif
    case base::TERMINATION_STATUS_OOM:
      return "oom";
    case base::TERMINATION_STATUS_PROCESS_WAS_KILLED:
      return "killed";
    case base::TERMINATION_STATUS_PROCESS_CRASHED:
      return "crashed";
    case base::TERMINATION_STATUS_LAUNCH_FAILED:
      return "failed to launch";
    case base::TERMINATION_STATUS_MAX_ENUM:
      break;
  }
  NOTREACHED() << "Unknown Termination Status.";
  return "unknown";
}

std::string GetStoragePartitionIdFromSiteURL(const GURL& site_url) {
  const std::string& partition_id = site_url.query();
  bool persist_storage = site_url.path().find("persist") != std::string::npos;
  return (persist_storage ? webview::kPersistPrefix : "") + partition_id;
}

void ParsePartitionParam(const base::DictionaryValue& create_params,
                         std::string* storage_partition_id,
                         bool* persist_storage) {
  std::string partition_str;
  if (!create_params.GetString(webview::kStoragePartitionId, &partition_str)) {
    return;
  }

  // Since the "persist:" prefix is in ASCII, base::StartsWith will work fine on
  // UTF-8 encoded |partition_id|. If the prefix is a match, we can safely
  // remove the prefix without splicing in the middle of a multi-byte codepoint.
  // We can use the rest of the string as UTF-8 encoded one.
  if (base::StartsWith(partition_str, "persist:",
                       base::CompareCase::SENSITIVE)) {
    size_t index = partition_str.find(":");
    CHECK(index != std::string::npos);
    // It is safe to do index + 1, since we tested for the full prefix above.
    *storage_partition_id = partition_str.substr(index + 1);

    if (storage_partition_id->empty()) {
      // TODO(lazyboy): Better way to deal with this error.
      return;
    }
    *persist_storage = true;
  } else {
    *storage_partition_id = partition_str;
    *persist_storage = false;
  }
}

void RemoveWebViewEventListenersOnIOThread(
    void* profile,
    int embedder_process_id,
    int view_instance_id) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::IO);
  ExtensionWebRequestEventRouter::GetInstance()->RemoveWebViewEventListeners(
      profile,
      embedder_process_id,
      view_instance_id);
}

double ConvertZoomLevelToZoomFactor(double zoom_level) {
  double zoom_factor = content::ZoomLevelToZoomFactor(zoom_level);
  // Because the conversion from zoom level to zoom factor isn't perfect, the
  // resulting zoom factor is rounded to the nearest 6th decimal place.
  zoom_factor = round(zoom_factor * 1000000) / 1000000;
  return zoom_factor;
}

using WebViewKey = std::pair<int, int>;
using WebViewKeyToIDMap = std::map<WebViewKey, int>;
static base::LazyInstance<WebViewKeyToIDMap> web_view_key_to_id_map =
    LAZY_INSTANCE_INITIALIZER;

}  // namespace

// static
void WebViewGuest::CleanUp(content::BrowserContext* browser_context,
                           int embedder_process_id,
                           int view_instance_id) {
  GuestViewBase::CleanUp(browser_context, embedder_process_id,
                         view_instance_id);

  // Clean up rules registries for the WebView.
  WebViewKey key(embedder_process_id, view_instance_id);
  auto it = web_view_key_to_id_map.Get().find(key);
  if (it != web_view_key_to_id_map.Get().end()) {
    auto rules_registry_id = it->second;
    web_view_key_to_id_map.Get().erase(it);
    RulesRegistryService* rrs =
        RulesRegistryService::GetIfExists(browser_context);
    if (rrs)
      rrs->RemoveRulesRegistriesByID(rules_registry_id);
  }

  // Clean up web request event listeners for the WebView.
  content::BrowserThread::PostTask(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(
          &RemoveWebViewEventListenersOnIOThread,
          browser_context,
          embedder_process_id,
          view_instance_id));

  // Clean up content scripts for the WebView.
  auto* csm = WebViewContentScriptManager::Get(browser_context);
  csm->RemoveAllContentScriptsForWebView(embedder_process_id, view_instance_id);

  // Allow an extensions browser client to potentially perform more cleanup.
  ExtensionsBrowserClient::Get()->CleanUpWebView(
      browser_context, embedder_process_id, view_instance_id);
}

// static
GuestViewBase* WebViewGuest::Create(WebContents* owner_web_contents) {
  return new WebViewGuest(owner_web_contents);
}

// static
bool WebViewGuest::GetGuestPartitionConfigForSite(
    const GURL& site,
    std::string* partition_domain,
    std::string* partition_name,
    bool* in_memory) {
  if (!site.SchemeIs(content::kGuestScheme))
    return false;

  // Since guest URLs are only used for packaged apps, there must be an app
  // id in the URL.
  CHECK(site.has_host());
  *partition_domain = site.host();
  // Since persistence is optional, the path must either be empty or the
  // literal string.
  *in_memory = (site.path() != "/persist");
  // The partition name is user supplied value, which we have encoded when the
  // URL was created, so it needs to be decoded.
  *partition_name =
      net::UnescapeURLComponent(site.query(), net::UnescapeRule::NORMAL);
  return true;
}

// static
std::string WebViewGuest::GetPartitionID(
    const RenderProcessHost* render_process_host) {
  WebViewRendererState* renderer_state = WebViewRendererState::GetInstance();
  int process_id = render_process_host->GetID();
  std::string partition_id;
  if (renderer_state->IsGuest(process_id))
    renderer_state->GetPartitionID(process_id, &partition_id);

  return partition_id;
}

// static
const char WebViewGuest::Type[] = "webview";

// static
int WebViewGuest::GetOrGenerateRulesRegistryID(
    int embedder_process_id,
    int webview_instance_id) {
  bool is_web_view = embedder_process_id && webview_instance_id;
  if (!is_web_view)
    return RulesRegistryService::kDefaultRulesRegistryID;

  WebViewKey key = std::make_pair(embedder_process_id, webview_instance_id);
  auto it = web_view_key_to_id_map.Get().find(key);
  if (it != web_view_key_to_id_map.Get().end())
    return it->second;

  auto* rph = RenderProcessHost::FromID(embedder_process_id);
  int rules_registry_id =
      RulesRegistryService::Get(rph->GetBrowserContext())->
          GetNextRulesRegistryID();
  web_view_key_to_id_map.Get()[key] = rules_registry_id;
  return rules_registry_id;
}

bool WebViewGuest::CanRunInDetachedState() const {
  return true;
}

void WebViewGuest::CreateWebContents(
    const base::DictionaryValue& create_params,
    const WebContentsCreatedCallback& callback) {
  RenderProcessHost* owner_render_process_host =
      owner_web_contents()->GetRenderProcessHost();
  std::string storage_partition_id;
  bool persist_storage = false;
  ParsePartitionParam(create_params, &storage_partition_id, &persist_storage);
  // Validate that the partition id coming from the renderer is valid UTF-8,
  // since we depend on this in other parts of the code, such as FilePath
  // creation. If the validation fails, treat it as a bad message and kill the
  // renderer process.
  if (!base::IsStringUTF8(storage_partition_id)) {
    content::RecordAction(
        base::UserMetricsAction("BadMessageTerminate_BPGM"));
    owner_render_process_host->Shutdown(content::RESULT_CODE_KILLED_BAD_MESSAGE,
                                        false);
    callback.Run(nullptr);
    return;
  }
  std::string url_encoded_partition = net::EscapeQueryParamValue(
      storage_partition_id, false);
  std::string partition_domain = GetOwnerSiteURL().host();

  GURL guest_site;
  std::string new_url;
  if (IsVivaldiApp(owner_host()) &&
    create_params.GetString(webview::kNewURL, &new_url)) {
    guest_site = GURL(new_url);
  } else {
     guest_site = GURL(base::StringPrintf("%s://%s/%s?%s",
                                     content::kGuestScheme,
                                     partition_domain.c_str(),
                                     persist_storage ? "persist" : "",
                                     url_encoded_partition.c_str()));
  }

#if !defined(VIVALDI_BUILD)
  // If we already have a webview tag in the same app using the same storage
  // partition, we should use the same SiteInstance so the existing tag and
  // the new tag can script each other.
  auto* guest_view_manager = GuestViewManager::FromBrowserContext(
      owner_render_process_host->GetBrowserContext());
  scoped_refptr<content::SiteInstance> guest_site_instance =
      guest_view_manager->GetGuestSiteInstance(guest_site);
  if (!guest_site_instance) {
    // Create the SiteInstance in a new BrowsingInstance, which will ensure
    // that webview tags are also not allowed to send messages across
    // different partitions.
    guest_site_instance = content::SiteInstance::CreateForURL(
        owner_render_process_host->GetBrowserContext(), guest_site);
  }
  WebContents::CreateParams params(
      owner_render_process_host->GetBrowserContext(),
      std::move(guest_site_instance));
  params.guest_delegate = this;
#endif  // !defined(VIVALDI_BUILD)

  WebContents* new_contents = nullptr;

  // If we created the WebContents through CreateNewWindow and created this
  // guest with InitWithWebContents we cannot delete the tabstrip contents, and
  // we don't need to recreate the webcontents either. Just use the WebContents
  // owned by the tab-strip.

  std::string tab_id_as_string;
  if (create_params.GetString("tab_id", &tab_id_as_string)) {
    int tab_id = atoi(tab_id_as_string.c_str());;
    content::WebContents* tabstrip_contents = NULL;
    bool include_incognito = true;
    Browser* browser;
    Profile* profile =
      Profile::FromBrowserContext(owner_web_contents()->GetBrowserContext());
    int tab_index;

    if (extensions::ExtensionTabUtil::GetTabById(tab_id, profile,
        include_incognito, &browser, NULL, &tabstrip_contents, &tab_index)) {
      // Morph the webcontents into a guest with this webviewguest as delegate.
      new_contents = tabstrip_contents;
      content::WebContentsImpl* contentsimpl =
          static_cast<content::WebContentsImpl*>(new_contents);

      // This is a temporary solution for moving PDF documents between windows.
      // See bug# VB-23855.
      bool replace_webcontents =
          contentsimpl->GetContentsMimeType() == "application/pdf";
      if (replace_webcontents) {
        WebContents::CreateParams params =
            GetWebContentsCreateParams(browser->profile(), guest_site);
        new_contents = WebContents::Create(params);
        new_contents->SetExtData(tabstrip_contents->GetExtData());
        new_contents->GetController().CopyStateFrom(
            tabstrip_contents->GetController());
        TabStripModel* tab_strip = browser->tab_strip_model();
        new_contents->SetDelegate(this);
        tab_strip->ReplaceWebContentsAt(tab_index, new_contents);
        delete tabstrip_contents;
      } else {
        content::BrowserPluginGuest::Create(contentsimpl, this);
        contentsimpl->RecreateViewAsGuest(this);
      }

    } else {
      // NOTE(jarle): Get the |new_contents| using the original Chromium way.
      // This makes Chrome apps work again, ref. VB-22908.

      // If we already have a webview tag in the same app using the same storage
      // partition, we should use the same SiteInstance so the existing tag and
      // the new tag can script each other.
      auto* guest_view_manager = GuestViewManager::FromBrowserContext(
          owner_render_process_host->GetBrowserContext());
      scoped_refptr<content::SiteInstance> guest_site_instance =
          guest_view_manager->GetGuestSiteInstance(guest_site);
      if (!guest_site_instance) {
        // Create the SiteInstance in a new BrowsingInstance, which will ensure
        // that webview tags are also not allowed to send messages across
        // different partitions.
        guest_site_instance = content::SiteInstance::CreateForURL(
            owner_render_process_host->GetBrowserContext(), guest_site);
      }
      WebContents::CreateParams params(
          owner_render_process_host->GetBrowserContext(),
          std::move(guest_site_instance));
      params.guest_delegate = this;
      new_contents = WebContents::Create(params);
    }
  } else {
    // Look up the correct Browser object to use as the Profile owner.
    content::BrowserContext* context =
        owner_render_process_host->GetBrowserContext();
    std::string window_id;
    if (create_params.GetString("window_id", &window_id)) {
      std::string windowId;
      BrowserList* list =
          BrowserList::GetInstance();
      for (size_t i = 0; i < list->size(); i++) {
        if (ExtensionActionUtil::GetWindowIdFromExtData(
                list->get(i)->ext_data(), windowId)) {
          if (windowId == window_id) {
            context = list->get(i)->profile();
            break;
          }
        }
      }
    }
    WebContents::CreateParams params =
        GetWebContentsCreateParams(context, guest_site);
    new_contents = WebContents::Create(params);
  }
  DCHECK(new_contents);
  if (owner_web_contents()->IsAudioMuted()) {
    // NOTE(pettern@vivaldi.com): If the owner is muted it means the webcontents
    // of the AppWindow has been muted due to thumbnail capturing, so we also
    // mute the webview webcontents.
    chrome::SetTabAudioMuted(new_contents, true,
                      TabMutedReason::MEDIA_CAPTURE,
                      chrome::GetExtensionIdForMutedTab(owner_web_contents()));
  }

  // Grant access to the origin of the embedder to the guest process. This
  // allows blob:/filesystem: URLs with the embedder origin to be created
  // inside the guest. It is possible to do this by running embedder code
  // through webview accessible_resources.
  content::ChildProcessSecurityPolicy::GetInstance()->GrantOrigin(
      new_contents->GetMainFrame()->GetProcess()->GetID(),
      url::Origin(GetOwnerSiteURL()));

  callback.Run(new_contents);
}

void WebViewGuest::DidAttachToEmbedder() {
  ApplyAttributes(*attach_params());
  if(delayed_open_url_.get()) {
    NavigateGuest(*delayed_open_url_.get(), false);
    delayed_open_url_.reset(nullptr);
  }

  // Make sure we re-draw the page if needed. This was added because of
  // VB-20658. If the web_contents was already fully loaded when here, we would
  // not get the page painted.
  auto render_widget_host = content::RenderWidgetHostImpl::From(
      web_contents()->GetRenderViewHost()->GetWidget());
  render_widget_host->ScheduleComposite();
}

void WebViewGuest::DidDropLink(const GURL& url) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetString(guest_view::kUrl, url.spec());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(webview::kEventDropLink,
                                                       std::move(args)));
}

void WebViewGuest::DidInitialize(const base::DictionaryValue& create_params) {
  script_executor_.reset(
      new ScriptExecutor(web_contents(), &script_observers_));

  notification_registrar_.Add(this,
                              content::NOTIFICATION_LOAD_COMPLETED_MAIN_FRAME,
                              content::Source<WebContents>(web_contents()));

  notification_registrar_.Add(this,
                              content::NOTIFICATION_RESOURCE_RECEIVED_REDIRECT,
                              content::Source<WebContents>(web_contents()));

  // For Vivaldi the web contents is created through the browser and the
  // helpers are attached there (tab_helpers.cc).

  // Note this is not the case anymore as we replace the |WebContents|

  // Note:  Even though you can attach helpers multiple times there are
  //        some helpers we want to exclude for Vivaldi and we want to control
  //        that in one place).
  if(!IsVivaldiRunning())
  if (web_view_guest_delegate_)
    web_view_guest_delegate_->OnDidInitialize();
  ExtensionsAPIClient::Get()->AttachWebContentsHelpers(web_contents());
  web_view_permission_helper_.reset(new WebViewPermissionHelper(this));

#ifdef VIVALDI_BUILD
  web_modal::WebContentsModalDialogManager::CreateForWebContents(web_contents());

  extensions::AppWindow* app_win = GetAppWindow();

  web_modal::WebContentsModalDialogManager::FromWebContents(web_contents())->
      SetDelegate(app_win);

  InitListeners();

#endif

  rules_registry_id_ = GetOrGenerateRulesRegistryID(
      owner_web_contents()->GetRenderProcessHost()->GetID(),
      view_instance_id());

  // We must install the mapping from guests to WebViews prior to resuming
  // suspended resource loads so that the WebRequest API will catch resource
  // requests.
  PushWebViewStateToIOThread();

  ApplyAttributes(create_params);
}

void WebViewGuest::ClearDataInternal(base::Time remove_since,
                                     uint32_t removal_mask,
                                     const base::Closure& callback) {
  uint32_t storage_partition_removal_mask =
      GetStoragePartitionRemovalMask(removal_mask);
  if (!storage_partition_removal_mask) {
    callback.Run();
    return;
  }
  content::StoragePartition* partition =
      content::BrowserContext::GetStoragePartition(
          web_contents()->GetBrowserContext(),
          web_contents()->GetSiteInstance());
  partition->ClearData(
      storage_partition_removal_mask,
      content::StoragePartition::QUOTA_MANAGED_STORAGE_MASK_ALL, GURL(),
      content::StoragePartition::OriginMatcherFunction(), remove_since,
      base::Time::Now(), callback);
}

void WebViewGuest::GuestViewDidStopLoading() {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(webview::kEventLoadStop,
                                                       std::move(args)));
}

void WebViewGuest::EmbedderFullscreenToggled(bool entered_fullscreen) {
  is_embedder_fullscreen_ = entered_fullscreen;
  // If the embedder has got out of fullscreen, we get out of fullscreen
  // mode as well.
  if (!entered_fullscreen)
    SetFullscreenState(false);
}

bool WebViewGuest::ZoomPropagatesFromEmbedderToGuest() const {
  // If Vivaldi and the webcontents is in a tabstrip we should not sync
  // zoom-level between the embedder and the WebViewGuest.
  if (IsVivaldiRunning() &&
    chrome::FindBrowserWithWebContents(web_contents())) {
    return false;
  }
  // We use the embedder's zoom iff we haven't set a zoom ourselves using
  // e.g. webview.setZoom().
  return !did_set_explicit_zoom_;
}

const char* WebViewGuest::GetAPINamespace() const {
  return webview::kAPINamespace;
}

int WebViewGuest::GetTaskPrefix() const {
  return IDS_EXTENSION_TASK_MANAGER_WEBVIEW_TAG_PREFIX;
}

void WebViewGuest::GuestDestroyed() {

  if (IsVivaldiRunning()) {
    // Make sure the client is informed about the mediastate.
    UpdateMediaState(TabAlertState::NONE);
    // Signal that the <webview> has been destroyed so that the client can do
    // what is needed. For instance re-create the <webview> element.
    std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
    DispatchEventToView(base::WrapUnique(
        new GuestViewEvent(webview::kEventWebContentsDiscarded,
                           std::move(args))));
  }
  RemoveWebViewStateFromIOThread(web_contents());
}

void WebViewGuest::GuestReady() {
  // The guest RenderView should always live in an isolated guest process.

#ifndef VIVALDI_BUILD
  CHECK(web_contents()->GetRenderProcessHost()->IsForGuestsOnly());
#endif // VIVALDI_BUILD

  Send(new ExtensionMsg_SetFrameName(web_contents()->GetRoutingID(), name_));

  if (IsVivaldiRunning() &&
      !web_contents()->GetRenderViewHost()->GetWidget()->GetView()) {
    // While running as Vivaldi we use a WebViewGuest as owner for the
    // background host contents to make them guests.
    return;
  }

  // We don't want to accidentally set the opacity of an interstitial page.
  // WebContents::GetRenderWidgetHostView will return the RWHV of an
  // interstitial page if one is showing at this time. We only want opacity
  // to apply to web pages.
  if (allow_transparency_) {
    web_contents()
        ->GetRenderViewHost()
        ->GetWidget()
        ->GetView()
        ->SetBackgroundColor(SK_ColorTRANSPARENT);
  } else {
    web_contents()
        ->GetRenderViewHost()
        ->GetWidget()
        ->GetView()
        ->SetBackgroundColorToDefault();
  }
}

void WebViewGuest::GuestSizeChangedDueToAutoSize(const gfx::Size& old_size,
                                                 const gfx::Size& new_size) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetInteger(webview::kOldHeight, old_size.height());
  args->SetInteger(webview::kOldWidth, old_size.width());
  // NOTE(andre@vivaldi.com): The value in is not scaled, comes directly from
  // WebContents which always work on scale 1.0, and this event is also used
  // internally in WebViewImpl.prototype.onSizeChanged to update the <WebView>
  // element width and height.
  // Note: We divide with the zoomfactor because the zoom factor is inherited
  // from the embedder. Hence the actual px-value will be reverse.
  args->SetInteger(webview::kNewHeight, new_size.height() / GetZoom());
  args->SetInteger(webview::kNewWidth, new_size.width() / GetZoom());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventSizeChanged, std::move(args)));
}

bool WebViewGuest::IsAutoSizeSupported() const {
  return true;
}

void WebViewGuest::GuestZoomChanged(double old_zoom_level,
                                    double new_zoom_level) {
  // Dispatch the zoomchange event.
  double old_zoom_factor = ConvertZoomLevelToZoomFactor(old_zoom_level);
  double new_zoom_factor = ConvertZoomLevelToZoomFactor(new_zoom_level);
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetDouble(webview::kOldZoomFactor, old_zoom_factor);
  args->SetDouble(webview::kNewZoomFactor, new_zoom_factor);
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventZoomChange, std::move(args)));
}

void WebViewGuest::WillDestroy() {
  if (!attached() && GetOpener())
    GetOpener()->pending_new_windows_.erase(this);

  // Vivaldi: Need to remove notifications since webcontents might be changed.
  notification_registrar_.RemoveAll();

  web_contents()->GetRenderViewHost()->GetWidget()->RemoveMouseEventCallback(
      mouseevent_callback_);
}

bool WebViewGuest::DidAddMessageToConsole(WebContents* source,
                                          int32_t level,
                                          const base::string16& message,
                                          int32_t line_no,
                                          const base::string16& source_id) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  // Log levels are from base/logging.h: LogSeverity.
  args->SetInteger(webview::kLevel, level);
  args->SetString(webview::kMessage, message);
  args->SetInteger(webview::kLine, line_no);
  args->SetString(webview::kSourceId, source_id);
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventConsoleMessage, std::move(args)));
  return false;
}

void WebViewGuest::CloseContents(WebContents* source) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  DispatchEventToView(
      base::MakeUnique<GuestViewEvent>(webview::kEventClose, std::move(args)));
}

void WebViewGuest::FindReply(WebContents* source,
                             int request_id,
                             int number_of_matches,
                             const gfx::Rect& selection_rect,
                             int active_match_ordinal,
                             bool final_update) {
  GuestViewBase::FindReply(source, request_id, number_of_matches,
                           selection_rect, active_match_ordinal, final_update);
  find_helper_.FindReply(request_id, number_of_matches, selection_rect,
                         active_match_ordinal, final_update);
}

double WebViewGuest::GetZoom() const {
  double zoom_level =
      ZoomController::FromWebContents(web_contents())->GetZoomLevel();
  return ConvertZoomLevelToZoomFactor(zoom_level);
}

ZoomController::ZoomMode WebViewGuest::GetZoomMode() {
  return ZoomController::FromWebContents(web_contents())->zoom_mode();
}

bool WebViewGuest::HandleContextMenu(
    const content::ContextMenuParams& params) {
  if (!web_view_guest_delegate_)
    return false;
  return web_view_guest_delegate_->HandleContextMenu(params);
}

void WebViewGuest::HandleKeyboardEvent(
    WebContents* source,
    const content::NativeWebKeyboardEvent& event) {
  if (HandleKeyboardShortcuts(event))
    return;

  GuestViewBase::HandleKeyboardEvent(source, event);
}

bool WebViewGuest::PreHandleGestureEvent(WebContents* source,
                                         const blink::WebGestureEvent& event) {
  return !allow_scaling_ && GuestViewBase::PreHandleGestureEvent(source, event);
}

void WebViewGuest::LoadProgressChanged(WebContents* source, double progress) {
#if 0  // NOTE(pettern): Ignore as we get the extended event handled by 
       // WebViewGuest::ExtendedLoadProgressChanged
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetString(guest_view::kUrl, web_contents()->GetURL().spec());
  args->SetDouble(webview::kProgress, progress);
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventLoadProgress, std::move(args)));
#endif
}

void WebViewGuest::LoadAbort(bool is_top_level,
                             const GURL& url,
                             int error_code) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetBoolean(guest_view::kIsTopLevel, is_top_level);
  args->SetString(guest_view::kUrl, url.possibly_invalid_spec());
  args->SetInteger(guest_view::kCode, error_code);
  args->SetString(guest_view::kReason, net::ErrorToShortString(error_code));
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(webview::kEventLoadAbort,
                                                       std::move(args)));
}

void WebViewGuest::SetContextMenuPosition(const gfx::Point& position) {
  if (web_view_guest_delegate_)
    web_view_guest_delegate_->SetContextMenuPosition(position);
}

void WebViewGuest::CreateNewGuestWebViewWindow(
    const content::OpenURLParams& params) {
  GuestViewManager* guest_manager =
      GuestViewManager::FromBrowserContext(browser_context());
  // Set the attach params to use the same partition as the opener.
  // We pull the partition information from the site's URL, which is of the
  // form guest://site/{persist}?{partition_name}.
  const GURL& site_url = web_contents()->GetSiteInstance()->GetSiteURL();
  const std::string storage_partition_id =
      GetStoragePartitionIdFromSiteURL(site_url);
  base::DictionaryValue create_params;
  create_params.SetString(webview::kStoragePartitionId, storage_partition_id);
  // gisli@vivaldi.com:  Add the url so we can create the right site instance
  // when creating the webcontents.
  create_params.SetString(webview::kNewURL, params.url.spec());

  // Vivaldi; in private mode we need to make sure the correct Browser is used
  // as the Profile follows this object correctly.
  if (params.source_site_instance &&
      params.source_site_instance->GetBrowserContext()->IsOffTheRecord()) {
    Profile* current_profile = Profile::FromBrowserContext(
        params.source_site_instance->GetBrowserContext());
    Browser* current_browser = chrome::FindLastActiveWithProfile(
        current_profile);
    std::string window_id;
    current_browser->ext_data();
    if (ExtensionActionUtil::GetWindowIdFromExtData(current_browser->ext_data(),
                                                    window_id)) {
      create_params.SetString("window_id", window_id);
    }
  }

  guest_manager->CreateGuest(WebViewGuest::Type,
                             embedder_web_contents(),
                             create_params,
                             base::Bind(&WebViewGuest::NewGuestWebViewCallback,
                                        weak_ptr_factory_.GetWeakPtr(),
                                        params));
}

void WebViewGuest::NewGuestWebViewCallback(const content::OpenURLParams& params,
                                           WebContents* guest_web_contents) {
  WebViewGuest* new_guest = WebViewGuest::FromWebContents(guest_web_contents);
  new_guest->SetOpener(this);

  // Take ownership of |new_guest|.
  pending_new_windows_.insert(
      std::make_pair(new_guest, NewWindowInfo(params.url, std::string())));
  // NOTE(espen@vivaldi.com): We must send along the refrerrer as well.
  pending_new_windows_.at(new_guest).referrer =
      new content::Referrer(params.referrer.url, params.referrer.policy);

  // NOTE(andre@vivaldi.com): If we open a new window from an incognito window,
  // this new window should be the same type.
  if (IsVivaldiRunning() &&
      params.disposition == WindowOpenDisposition::NEW_WINDOW &&
      params.source_site_instance->GetBrowserContext()->IsOffTheRecord()) {
    DCHECK(guest_web_contents->GetBrowserContext()->IsOffTheRecord());
    RequestNewWindowPermission(WindowOpenDisposition::OFF_THE_RECORD,
                               gfx::Rect(), params.user_gesture,
                               new_guest->web_contents());
  } else  // if Vivaldi and new window from private window.
  // Request permission to show the new window.
  RequestNewWindowPermission(params.disposition,
                             gfx::Rect(),
                             params.user_gesture,
                             new_guest->web_contents());
}

// TODO(fsamuel): Find a reliable way to test the 'responsive' and
// 'unresponsive' events.
void WebViewGuest::RendererResponsive(WebContents* source) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetInteger(webview::kProcessId,
                   web_contents()->GetRenderProcessHost()->GetID());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventResponsive, std::move(args)));
}

void WebViewGuest::RendererUnresponsive(
    WebContents* source,
    const content::WebContentsUnresponsiveState& unresponsive_state) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetInteger(webview::kProcessId,
                   web_contents()->GetRenderProcessHost()->GetID());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventUnresponsive, std::move(args)));
}

void WebViewGuest::Observe(int type,
                           const content::NotificationSource& source,
                           const content::NotificationDetails& details) {
  switch (type) {
    case content::NOTIFICATION_LOAD_COMPLETED_MAIN_FRAME: {
      DCHECK_EQ(content::Source<WebContents>(source).ptr(), web_contents());
      if (content::Source<WebContents>(source).ptr() == web_contents())
        LoadHandlerCalled();
      break;
    }
    case content::NOTIFICATION_RESOURCE_RECEIVED_REDIRECT: {
      DCHECK_EQ(content::Source<WebContents>(source).ptr(), web_contents());
      content::ResourceRedirectDetails* resource_redirect_details =
          content::Details<content::ResourceRedirectDetails>(details).ptr();
      bool is_top_level = resource_redirect_details->resource_type ==
                          content::RESOURCE_TYPE_MAIN_FRAME;
      LoadRedirect(resource_redirect_details->url,
                   resource_redirect_details->new_url,
                   is_top_level);
      break;
    }
    default:
      NOTREACHED() << "Unexpected notification sent.";
      break;
  }
}

void WebViewGuest::StartFind(
    const base::string16& search_text,
    const blink::WebFindOptions& options,
    scoped_refptr<WebViewInternalFindFunction> find_function) {
  find_helper_.Find(web_contents(), search_text, options, find_function);
}

void WebViewGuest::StopFinding(content::StopFindAction action) {
  find_helper_.CancelAllFindSessions();
  web_contents()->StopFinding(action);
}

bool WebViewGuest::Go(int relative_index) {
  content::NavigationController& controller = web_contents()->GetController();
  if (!controller.CanGoToOffset(relative_index))
    return false;

  controller.GoToOffset(relative_index);
  return true;
}

void WebViewGuest::Reload() {
  // TODO(fsamuel): Don't check for repost because we don't want to show
  // Chromium's repost warning. We might want to implement a separate API
  // for registering a callback if a repost is about to happen.
  web_contents()->GetController().Reload(false);
}

void WebViewGuest::SetUserAgentOverride(
    const std::string& user_agent_override) {
  is_overriding_user_agent_ = !user_agent_override.empty();
  if (is_overriding_user_agent_) {
    content::RecordAction(UserMetricsAction("WebView.Guest.OverrideUA"));
  }
  web_contents()->SetUserAgentOverride(user_agent_override);
}

void WebViewGuest::Stop() {
  web_contents()->Stop();
}

void WebViewGuest::Terminate() {
  content::RecordAction(UserMetricsAction("WebView.Guest.Terminate"));
  base::ProcessHandle process_handle =
      web_contents()->GetRenderProcessHost()->GetHandle();
  if (process_handle)
    web_contents()->GetRenderProcessHost()->Shutdown(
        content::RESULT_CODE_KILLED, false);
}

bool WebViewGuest::ClearData(base::Time remove_since,
                             uint32_t removal_mask,
                             const base::Closure& callback) {
  content::RecordAction(UserMetricsAction("WebView.Guest.ClearData"));
  content::StoragePartition* partition =
      content::BrowserContext::GetStoragePartition(
          web_contents()->GetBrowserContext(),
          web_contents()->GetSiteInstance());

  if (!partition)
    return false;

  if (removal_mask & webview::WEB_VIEW_REMOVE_DATA_MASK_CACHE) {
    // First clear http cache data and then clear the rest in
    // |ClearDataInternal|.
    int render_process_id = web_contents()->GetRenderProcessHost()->GetID();
    // We need to clear renderer cache separately for our process because
    // StoragePartitionHttpCacheDataRemover::ClearData() does not clear that.
    web_cache::WebCacheManager::GetInstance()->ClearCacheForProcess(
        render_process_id);

    base::Closure cache_removal_done_callback = base::Bind(
        &WebViewGuest::ClearDataInternal, weak_ptr_factory_.GetWeakPtr(),
        remove_since, removal_mask, callback);
    // StoragePartitionHttpCacheDataRemover removes itself when it is done.
    // components/, move |ClearCache| to WebViewGuest: http//crbug.com/471287.
    browsing_data::StoragePartitionHttpCacheDataRemover::CreateForRange(
        partition, remove_since, base::Time::Now())
        ->Remove(cache_removal_done_callback);

    return true;
  }

  ClearDataInternal(remove_since, removal_mask, callback);
  return true;
}

WebViewGuest::WebViewGuest(WebContents* owner_web_contents)
    : GuestView<WebViewGuest>(owner_web_contents),
      rules_registry_id_(RulesRegistryService::kInvalidRulesRegistryID),
      find_helper_(this),
      is_overriding_user_agent_(false),
      allow_transparency_(false),
      javascript_dialog_helper_(this),
      allow_scaling_(false),
      is_guest_fullscreen_(false),
      is_embedder_fullscreen_(false),
      last_fullscreen_permission_was_allowed_by_embedder_(false),
      pending_zoom_factor_(0.0),
      did_set_explicit_zoom_(false),
#ifdef VIVALDI_BUILD
// Vivaldi specific member initialization
#include "extensions/api/guest_view/vivaldi_web_view_guest_construct.inc"
#endif // VIVALDI_BUILD
      weak_ptr_factory_(this) {
  web_view_guest_delegate_.reset(
      ExtensionsAPIClient::Get()->CreateWebViewGuestDelegate(this));
}

WebViewGuest::~WebViewGuest() {
}

void WebViewGuest::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (navigation_handle->IsErrorPage() || !navigation_handle->HasCommitted()) {
    // Suppress loadabort for "mailto" URLs.
    // Also during destruction, owner_web_contents() is null so there's no point
    // trying to send the event.
    if (!navigation_handle->GetURL().SchemeIs(url::kMailToScheme) &&
        owner_web_contents()) {
      // If a load is blocked, either by WebRequest or security checks, the
      // navigation may or may not have committed. So if we don't see an error
      // code, mark it as blocked.
      int error_code = navigation_handle->GetNetErrorCode();
      if (error_code == net::OK)
        error_code = net::ERR_BLOCKED_BY_CLIENT;
      LoadAbort(navigation_handle->IsInMainFrame(), navigation_handle->GetURL(),
                error_code);
    }
    return;
  }

  if (navigation_handle->IsInMainFrame()) {
    // For LoadDataWithBaseURL loads, |url| contains the data URL, but the
    // virtual URL is needed in that case. So use WebContents::GetURL instead.
    src_ = web_contents()->GetURL();
    // Handle a pending zoom if one exists.
    if (pending_zoom_factor_) {
      SetZoom(pending_zoom_factor_);
      pending_zoom_factor_ = 0.0;
    }
  }
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetString(guest_view::kUrl, src_.spec());
  args->SetBoolean(guest_view::kIsTopLevel, navigation_handle->IsInMainFrame());
  args->SetString(webview::kInternalBaseURLForDataURL,
                  web_contents()
                      ->GetController()
                      .GetLastCommittedEntry()
                      ->GetBaseURLForDataURL()
                      .spec());
  args->SetInteger(webview::kInternalCurrentEntryIndex,
                   web_contents()->GetController().GetCurrentEntryIndex());
  args->SetInteger(webview::kInternalEntryCount,
                   web_contents()->GetController().GetEntryCount());
  args->SetInteger(webview::kInternalProcessId,
                   web_contents()->GetRenderProcessHost()->GetID());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventLoadCommit, std::move(args)));

  find_helper_.CancelAllFindSessions();
}

void WebViewGuest::DidStartNavigation(
    content::NavigationHandle* navigation_handle) {
  // loadStart shouldn't be sent for same page navigations.
  if (navigation_handle->IsSamePage())
    return;

  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetString(guest_view::kUrl, navigation_handle->GetURL().spec());
  args->SetBoolean(guest_view::kIsTopLevel, navigation_handle->IsInMainFrame());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(webview::kEventLoadStart,
                                                       std::move(args)));
}

void WebViewGuest::RenderProcessGone(base::TerminationStatus status) {
  // Cancel all find sessions in progress.
  find_helper_.CancelAllFindSessions();

  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetInteger(webview::kProcessId,
                   web_contents()->GetRenderProcessHost()->GetID());
  args->SetString(webview::kReason, TerminationStatusToString(status));
  DispatchEventToView(
      base::MakeUnique<GuestViewEvent>(webview::kEventExit, std::move(args)));
}

void WebViewGuest::UserAgentOverrideSet(const std::string& user_agent) {
  content::NavigationController& controller = web_contents()->GetController();
  content::NavigationEntry* entry = controller.GetVisibleEntry();
  if (!entry)
    return;
  entry->SetIsOverridingUserAgent(!user_agent.empty());
  web_contents()->GetController().Reload(false);
}

void WebViewGuest::FrameNameChanged(RenderFrameHost* render_frame_host,
                                    const std::string& name) {
  if (render_frame_host->GetParent())
    return;

  if (name_ == name)
    return;

  ReportFrameNameChange(name);
}

void WebViewGuest::ReportFrameNameChange(const std::string& name) {
  name_ = name;
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetString(webview::kName, name);
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventFrameNameChanged, std::move(args)));
}

void WebViewGuest::LoadHandlerCalled() {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventContentLoad, std::move(args)));
}

void WebViewGuest::LoadRedirect(const GURL& old_url,
                                const GURL& new_url,
                                bool is_top_level) {
  std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
  args->SetBoolean(guest_view::kIsTopLevel, is_top_level);
  args->SetString(webview::kNewURL, new_url.spec());
  args->SetString(webview::kOldURL, old_url.spec());
  DispatchEventToView(base::MakeUnique<GuestViewEvent>(
      webview::kEventLoadRedirect, std::move(args)));
}

void WebViewGuest::PushWebViewStateToIOThread() {
  const GURL& site_url = web_contents()->GetSiteInstance()->GetSiteURL();
  std::string partition_domain;
  std::string partition_id;
  bool in_memory;
  if (!GetGuestPartitionConfigForSite(
          site_url, &partition_domain, &partition_id, &in_memory)) {

    // Vivaldi - geir: This check started kicking in when we started swithcing
    // instances for the guest view (VB-2455) - see VB-2539 for a TODO
    //NOTREACHED();
    return;
  }

  WebViewRendererState::WebViewInfo web_view_info;
  web_view_info.embedder_process_id =
      owner_web_contents()->GetRenderProcessHost()->GetID();
  web_view_info.instance_id = view_instance_id();
  web_view_info.partition_id = partition_id;
  web_view_info.owner_host = owner_host();
  web_view_info.rules_registry_id = rules_registry_id_;

  // Get content scripts IDs added by the guest.
  WebViewContentScriptManager* manager =
      WebViewContentScriptManager::Get(browser_context());
  DCHECK(manager);
  web_view_info.content_script_ids = manager->GetContentScriptIDSet(
      web_view_info.embedder_process_id, web_view_info.instance_id);

  content::BrowserThread::PostTask(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(&WebViewRendererState::AddGuest,
                 base::Unretained(WebViewRendererState::GetInstance()),
                 web_contents()->GetRenderProcessHost()->GetID(),
                 web_contents()->GetRoutingID(),
                 web_view_info));
}

// static
void WebViewGuest::RemoveWebViewStateFromIOThread(
    WebContents* web_contents) {
  content::BrowserThread::PostTask(
      content::BrowserThread::IO, FROM_HERE,
      base::Bind(
          &WebViewRendererState::RemoveGuest,
          base::Unretained(WebViewRendererState::GetInstance()),
          web_contents->GetRenderProcessHost()->GetID(),
          web_contents->GetRoutingID()));
}

void WebViewGuest::RequestMediaAccessPermission(
    WebContents* source,
    const content::MediaStreamRequest& request,
    const content::MediaResponseCallback& callback) {
  web_view_permission_helper_->RequestMediaAccessPermission(source,
                                                            request,
                                                            callback);
}

bool WebViewGuest::CheckMediaAccessPermission(WebContents* source,
                                              const GURL& security_origin,
                                              content::MediaStreamType type) {
  return web_view_permission_helper_->CheckMediaAccessPermission(
      source, security_origin, type);
}

void WebViewGuest::CanDownload(
    const GURL& url,
    const std::string& request_method,
    const content::DownloadInformation& info,
    const base::Callback<void(const content::DownloadItemAction&)>& callback) {
  web_view_permission_helper_->CanDownload(url, request_method,
                                           info,
                                           callback);
}

void WebViewGuest::RequestPointerLockPermission(
    bool user_gesture,
    bool last_unlocked_by_target,
    const base::Callback<void(bool)>& callback) {
  web_view_permission_helper_->RequestPointerLockPermission(
      user_gesture,
      last_unlocked_by_target,
      callback);
}

void WebViewGuest::SignalWhenReady(const base::Closure& callback) {
  auto* manager = WebViewContentScriptManager::Get(browser_context());
  manager->SignalOnScriptsLoaded(callback);
}

bool WebViewGuest::ShouldHandleFindRequestsForEmbedder() const {
  if (web_view_guest_delegate_)
    return web_view_guest_delegate_->ShouldHandleFindRequestsForEmbedder();
  return false;
}

void WebViewGuest::WillAttachToEmbedder() {
  rules_registry_id_ = GetOrGenerateRulesRegistryID(
      owner_web_contents()->GetRenderProcessHost()->GetID(),
      view_instance_id());

  // We must install the mapping from guests to WebViews prior to resuming
  // suspended resource loads so that the WebRequest API will catch resource
  // requests.
  PushWebViewStateToIOThread();
}

content::JavaScriptDialogManager* WebViewGuest::GetJavaScriptDialogManager(
    WebContents* source) {
  return &javascript_dialog_helper_;
}

void WebViewGuest::NavigateGuest(const std::string& src,
                                 bool force_navigation,
                                 bool wasTyped,
                                 content::Referrer* referrer) {
  if (src.empty())
    return;

  GURL url = ResolveURL(src);

  ui::PageTransition transition_type = wasTyped ?
                                       ui::PAGE_TRANSITION_TYPED :
                                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL;

  // We wait for all the content scripts to load and then navigate the guest
  // if the navigation is embedder-initiated. For browser-initiated navigations,
  // content scripts will be ready.
  if (force_navigation) {
    SignalWhenReady(
        base::Bind(&WebViewGuest::LoadURLWithParams,
                   weak_ptr_factory_.GetWeakPtr(), url,
                   referrer ? *referrer : content::Referrer(),
                   transition_type, force_navigation));
    return;
  }
  LoadURLWithParams(url, referrer ? *referrer : content::Referrer(),
                    transition_type, force_navigation);
}

bool WebViewGuest::HandleKeyboardShortcuts(
    const content::NativeWebKeyboardEvent& event) {
  // For Vivaldi we want this triggered regardless inside an extension or not.
  // Note andre@vivaldi.com; this should maybe be fixed by setting
  // |owner_extension_id_| to Vivaldi.
  // <webview> outside of Chrome Apps do not handle keyboard shortcuts.
  if (!GuestViewManager::FromBrowserContext(browser_context())->
          IsOwnedByExtension(this) &&
          !IsVivaldiRunning()) {
    return false;
  }

  if (event.type != blink::WebInputEvent::RawKeyDown)
    return false;

  // We need to go out of fullscreen mode here since the window is forced out of
  // fullscreen and we want the document as well. "fullscreen pseudo"
  if (event.windowsKeyCode == ui::VKEY_ESCAPE) {
    ExitFullscreenModeForTab(web_contents());
  }

  // If the user hits the escape key without any modifiers then unlock the
  // mouse if necessary.
  if ((event.windowsKeyCode == ui::VKEY_ESCAPE) &&
      !(event.modifiers & blink::WebInputEvent::InputModifiers)) {
    return web_contents()->GotResponseToLockMouseRequest(false);
  }

#if defined(OS_MACOSX)
  if (event.modifiers != blink::WebInputEvent::MetaKey)
    return false;

  if (event.windowsKeyCode == ui::VKEY_OEM_4) {
    Go(-1);
    return true;
  }

  if (event.windowsKeyCode == ui::VKEY_OEM_6) {
    Go(1);
    return true;
  }
#else
  if (event.windowsKeyCode == ui::VKEY_BROWSER_BACK) {
    Go(-1);
    return true;
  }

  if (event.windowsKeyCode == ui::VKEY_BROWSER_FORWARD) {
    Go(1);
    return true;
  }
#endif

  return false;
}

void WebViewGuest::ApplyAttributes(const base::DictionaryValue& params) {
  std::string name;
  if (params.GetString(webview::kAttributeName, &name)) {
    // If the guest window's name is empty, then the WebView tag's name is
    // assigned. Otherwise, the guest window's name takes precedence over the
    // WebView tag's name.
    if (name_.empty())
      SetName(name);
  }
  if (attached())
    ReportFrameNameChange(name_);

  std::string user_agent_override;
  params.GetString(webview::kParameterUserAgentOverride, &user_agent_override);
  SetUserAgentOverride(user_agent_override);

  bool allow_transparency = false;
  if (params.GetBoolean(webview::kAttributeAllowTransparency,
      &allow_transparency)) {
    // We need to set the background opaque flag after navigation to ensure that
    // there is a RenderWidgetHostView available.
    SetAllowTransparency(allow_transparency);
  }

  bool allow_scaling = false;
  if (params.GetBoolean(webview::kAttributeAllowScaling, &allow_scaling))
    SetAllowScaling(allow_scaling);

  // Check for a pending zoom from before the first navigation.
  params.GetDouble(webview::kInitialZoomFactor, &pending_zoom_factor_);

  bool is_pending_new_window = false;
  if (GetOpener()) {
    // We need to do a navigation here if the target URL has changed between
    // the time the WebContents was created and the time it was attached.
    // We also need to do an initial navigation if a RenderView was never
    // created for the new window in cases where there is no referrer.
    auto it = GetOpener()->pending_new_windows_.find(this);
    if (it != GetOpener()->pending_new_windows_.end()) {
      const NewWindowInfo& new_window_info = it->second;
      if (new_window_info.changed || !web_contents()->HasOpener()) {
        NavigateGuest(new_window_info.url.spec(),
                      false /* force_navigation */,
                      false /*  was_typed */,
                      new_window_info.referrer);
      }

      // Once a new guest is attached to the DOM of the embedder page, then the
      // lifetime of the new guest is no longer managed by the opener guest.
      GetOpener()->pending_new_windows_.erase(this);

      is_pending_new_window = true;
    }
  }

#ifdef VIVALDI_BUILD
  std::string host_extension;
  if (params.GetString(webview::kAttributeExtensionHost, &host_extension)) {
    CreateExtensionHost(host_extension);
  }
#endif //VIVALDI_BUILD

  // Only read the src attribute if this is not a New Window API flow.
  if (!is_pending_new_window) {
    std::string src;
    if (params.GetString(webview::kAttributeSrc, &src))
      NavigateGuest(src, true /* force_navigation */);
  }

  if (IsVivaldiRunning()) {
    // A web panel is identified by its name ('_name' member) and that is set in
    // this function. Web panels need a set of functionality to work propely so
    // we use this place as a hook to set up what is needed.
    if (IsVivaldiWebPanel()) {
      if (!TabDialogs::FromWebContents(web_contents()))
        TabDialogs::CreateForWebContents(web_contents());
    }
  }

}

void WebViewGuest::ShowContextMenu(int request_id) {
  if (web_view_guest_delegate_)
    web_view_guest_delegate_->OnShowContextMenu(request_id);
}

void WebViewGuest::SetName(const std::string& name) {
  if (name_ == name)
    return;
  name_ = name;

  Send(new ExtensionMsg_SetFrameName(routing_id(), name_));
}

void WebViewGuest::SetZoom(double zoom_factor) {
  if (IsVivaldiRunning()) {
    // NOTE(espen@vivaldi.com): By default the WebViewGuest sets itself up
    // as an observer to the embedder's zoom controller
    // (in GuestViewBase::WillAttach) After setting did_set_explicit_zoom_to
    // true below (this is the only place it is modified) it is no longer
    // possible to remove this observer. That can lead to a crash if the view is
    // later removed. This typically happens in web panels which sets zoom when
    // created. StopTrackingEmbedderZoomLevel() is private so we have a wrapper.
    VivaldiStopTrackingEmbedderZoomLevel();
  }

  did_set_explicit_zoom_ = true;
  auto* zoom_controller = ZoomController::FromWebContents(web_contents());
  DCHECK(zoom_controller);
  double zoom_level = content::ZoomFactorToZoomLevel(zoom_factor);
  zoom_controller->SetZoomLevel(zoom_level);
}

void WebViewGuest::SetZoomMode(ZoomController::ZoomMode zoom_mode) {
  ZoomController::FromWebContents(web_contents())->SetZoomMode(zoom_mode);
}

void WebViewGuest::SetAllowTransparency(bool allow) {
  if (allow_transparency_ == allow)
    return;

  allow_transparency_ = allow;
  if (!web_contents()->GetRenderViewHost()->GetWidget()->GetView())
    return;

  if (allow_transparency_) {
    web_contents()
        ->GetRenderViewHost()
        ->GetWidget()
        ->GetView()
        ->SetBackgroundColor(SK_ColorTRANSPARENT);
  } else {
    web_contents()
        ->GetRenderViewHost()
        ->GetWidget()
        ->GetView()
        ->SetBackgroundColorToDefault();
  }
}

void WebViewGuest::SetAllowScaling(bool allow) {
  allow_scaling_ = allow;
}

bool WebViewGuest::LoadDataWithBaseURL(const std::string& data_url,
                                       const std::string& base_url,
                                       const std::string& virtual_url,
                                       std::string* error) {
  // Make GURLs from URLs.
  const GURL data_gurl = GURL(data_url);
  const GURL base_gurl = GURL(base_url);
  const GURL virtual_gurl = GURL(virtual_url);

  // Check that the provided URLs are valid.
  // |data_url| must be a valid data URL.
  if (!data_gurl.is_valid() || !data_gurl.SchemeIs(url::kDataScheme)) {
    base::SStringPrintf(
        error, webview::kAPILoadDataInvalidDataURL, data_url.c_str());
    return false;
  }
  // |base_url| must be a valid URL.
  if (!base_gurl.is_valid()) {
    base::SStringPrintf(
        error, webview::kAPILoadDataInvalidBaseURL, base_url.c_str());
    return false;
  }
  // |virtual_url| must be a valid URL.
  if (!virtual_gurl.is_valid()) {
    base::SStringPrintf(
        error, webview::kAPILoadDataInvalidVirtualURL, virtual_url.c_str());
    return false;
  }

  // Set up the parameters to load |data_url| with the specified |base_url|.
  content::NavigationController::LoadURLParams load_params(data_gurl);
  load_params.load_type = content::NavigationController::LOAD_TYPE_DATA;
  load_params.base_url_for_data_url = base_gurl;
  load_params.virtual_url_for_data_url = virtual_gurl;
  load_params.override_user_agent =
      content::NavigationController::UA_OVERRIDE_INHERIT;

  // Navigate to the data URL.
  GuestViewBase::LoadURLWithParams(load_params);

  return true;
}

void WebViewGuest::AddNewContents(WebContents* source,
                                  WebContents* new_contents,
                                  WindowOpenDisposition disposition,
                                  const gfx::Rect& initial_rect,
                                  bool user_gesture,
                                  bool* was_blocked) {
  if (was_blocked)
    *was_blocked = false;
  RequestNewWindowPermission(disposition,
                             initial_rect,
                             user_gesture,
                             new_contents);
}

WebContents* WebViewGuest::OpenURLFromTab(
    WebContents* source,
    const content::OpenURLParams& params) {
  // Most navigations should be handled by WebViewGuest::LoadURLWithParams,
  // which takes care of blocking chrome:// URLs and other web-unsafe schemes.
  // (NavigateGuest and CreateNewGuestWebViewWindow also go through
  // LoadURLWithParams.)
  //
  // We make an exception here for context menu items, since the Language
  // Settings item uses a browser-initiated navigation to a chrome:// URL.
  // These can be passed to the embedder's WebContentsDelegate so that the
  // browser performs the action for the <webview>. Navigations to a new
  // tab, etc., are also handled by the WebContentsDelegate.
  if (!IsVivaldiRunning()) {
  if (!params.is_renderer_initiated &&
      (!content::ChildProcessSecurityPolicy::GetInstance()->IsWebSafeScheme(
           params.url.scheme()) ||
       params.disposition != WindowOpenDisposition::CURRENT_TAB)) {
    if (!owner_web_contents()->GetDelegate())
      return nullptr;
    return owner_web_contents()->GetDelegate()->OpenURLFromTab(
        owner_web_contents(), params);
  }
  }

  if (!attached()) {
    WebViewGuest* opener = GetOpener();
    // If the guest wishes to navigate away prior to attachment then we save the
    // navigation to perform upon attachment. Navigation initializes a lot of
    // state that assumes an embedder exists, such as RenderWidgetHostViewGuest.
    // Navigation also resumes resource loading. If we were created using
    // newwindow (i.e. we have an opener), we don't allow navigation until
    // attachment.
    if (opener) {
      auto it = opener->pending_new_windows_.find(this);
      if (it == opener->pending_new_windows_.end())
        return nullptr;
      const NewWindowInfo& info = it->second;
      NewWindowInfo new_window_info(params.url, info.name);
      new_window_info.changed = new_window_info.url != info.url;
      it->second = new_window_info;

      if (IsVivaldiRunning() && !new_window_info.changed) {
        // NOTE(andre@vivaldi.com) : We need to navigate new windows opened via
        // OpenURLFromTab on attaching. See VB-12256 and friends.
        delayed_open_url_.reset(new std::string(params.url.spec()));
      }
      return nullptr;
    }
  }

  // This code path is taken if RenderFrameImpl::DecidePolicyForNavigation
  // decides that a fork should happen. At the time of writing this comment,
  // the only way a well behaving guest could hit this code path is if it
  // navigates to a URL that's associated with the default search engine.
  // This list of URLs is generated by search::GetSearchURLs. Validity checks
  // are performed inside LoadURLWithParams such that if the guest attempts
  // to navigate to a URL that it is not allowed to navigate to, a 'loadabort'
  // event will fire in the embedder, and the guest will be navigated to
  // about:blank.
  if (params.disposition == WindowOpenDisposition::CURRENT_TAB) {
    LoadURLWithParams(params.url, params.referrer, params.transition,
                      true /* force_navigation */);
    return web_contents();
  }

  // This code path is taken if Ctrl+Click, middle click or any of the
  // keyboard/mouse combinations are used to open a link in a new tab/window.
  // This code path is also taken on client-side redirects from about:blank.
  CreateNewGuestWebViewWindow(params);
  return nullptr;
}

void WebViewGuest::WebContentsCreated(WebContents* source_contents,
                                      int opener_render_process_id,
                                      int opener_render_frame_id,
                                      const std::string& frame_name,
                                      const GURL& target_url,
                                      WebContents* new_contents) {
  auto* guest = WebViewGuest::FromWebContents(new_contents);
  CHECK(guest);
  guest->SetOpener(this);
  guest->name_ = frame_name;
  pending_new_windows_.insert(
      std::make_pair(guest, NewWindowInfo(target_url, frame_name)));
}

void WebViewGuest::EnterFullscreenModeForTab(WebContents* web_contents,
                                             const GURL& origin) {
  // Ask the embedder for permission.
  base::DictionaryValue request_info;
  request_info.SetString(webview::kOrigin, origin.spec());
  web_view_permission_helper_->RequestPermission(
      WEB_VIEW_PERMISSION_TYPE_FULLSCREEN, request_info,
      base::Bind(&WebViewGuest::OnFullscreenPermissionDecided,
                 weak_ptr_factory_.GetWeakPtr()),
      false /* allowed_by_default */);

  // TODO(lazyboy): Right now the guest immediately goes fullscreen within its
  // bounds. If the embedder denies the permission then we will see a flicker.
  // Once we have the ability to "cancel" a renderer/ fullscreen request:
  // http://crbug.com/466854 this won't be necessary and we should be
  // Calling SetFullscreenState(true) once the embedder allowed the request.
  // Otherwise we would cancel renderer/ fullscreen if the embedder denied.
  SetFullscreenState(true);
  ToggleFullscreenModeForTab(web_contents, true);
}

void WebViewGuest::ExitFullscreenModeForTab(WebContents* web_contents) {
  SetFullscreenState(false);
  ToggleFullscreenModeForTab(web_contents, false);
}

bool WebViewGuest::IsFullscreenForTabOrPending(
    const WebContents* web_contents) const {
  return is_guest_fullscreen_;
}

void WebViewGuest::LoadURLWithParams(
    const GURL& url,
    const content::Referrer& referrer,
    ui::PageTransition transition_type,
    bool force_navigation) {
  if (!url.is_valid()) {
    LoadAbort(true /* is_top_level */, url, net::ERR_INVALID_URL);
    NavigateGuest(url::kAboutBlankURL, false /* force_navigation */);
    return;
  }

  bool scheme_is_blocked =
      (!content::ChildProcessSecurityPolicy::GetInstance()->IsWebSafeScheme(
           url.scheme()) &&
       !url.SchemeIs(url::kAboutScheme)) ||
      url.SchemeIs(url::kJavaScriptScheme);

  if (IsVivaldiRunning() &&
      scheme_is_blocked &&
      (url.SchemeIs(url::kJavaScriptScheme) || url.SchemeIsFile() ||
       url.SchemeIs(content::kViewSourceScheme))) {
    // We allow some extra schemes when running as Vivaldi, but block
    // if the src is an internal page of ours or extension and the new scheme
    // is javascript.
    scheme_is_blocked = url.SchemeIs(url::kJavaScriptScheme) &&
                        src_.SchemeIs(extensions::kExtensionScheme);
  }
  // Do not allow navigating a guest to schemes other than known safe schemes.
  // This will block the embedder trying to load unwanted schemes, e.g.
  // chrome://.
  if (scheme_is_blocked) {
    LoadAbort(true /* is_top_level */, url, net::ERR_DISALLOWED_URL_SCHEME);
    NavigateGuest(url::kAboutBlankURL, false /* force_navigation */);
    return;
  }

  if (!force_navigation && (src_ == url))
    return;

  GURL validated_url(url);
  web_contents()->GetRenderProcessHost()->FilterURL(false, &validated_url);
  // As guests do not swap processes on navigation, only navigations to
  // normal web URLs are supported.  No protocol handlers are installed for
  // other schemes (e.g., WebUI or extensions), and no permissions or bindings
  // can be granted to the guest process.
  content::NavigationController::LoadURLParams load_url_params(validated_url);
  load_url_params.referrer = referrer;
  load_url_params.transition_type = transition_type;
  load_url_params.extra_headers = std::string();
  load_url_params.transferred_global_request_id = GlobalRequestID();
  if (is_overriding_user_agent_) {
    load_url_params.override_user_agent =
        content::NavigationController::UA_OVERRIDE_TRUE;
  }
  GuestViewBase::LoadURLWithParams(load_url_params);

  src_ = validated_url;
}

void WebViewGuest::RequestNewWindowPermission(WindowOpenDisposition disposition,
                                              const gfx::Rect &initial_bounds,
                                              bool user_gesture,
                                              WebContents* new_contents) {
  auto* guest = WebViewGuest::FromWebContents(new_contents);
  if (!guest)
    return;
  auto it = pending_new_windows_.find(guest);
  // NOTE(andre@vivaldi.com): We do not get events for new windows created in
  // background-scripts so we handle this here directly.

  if (IsVivaldiRunning()) {
    if (it == pending_new_windows_.end()) {

      // We need to find the browser window id, and since it is only opening via
      // user gestures we can trust the last active browser.

      Profile *profile =
          Profile::FromBrowserContext(new_contents->GetBrowserContext());
      Browser *browser =
          chrome::FindTabbedBrowser(profile, false);
      if (browser) {
        int foreground =
            (disposition == WindowOpenDisposition::NEW_BACKGROUND_TAB) ? 0 : 1;
        std::string user_input =
            base::StringPrintf("%d:%d", browser->session_id().id(), foreground);
        OnWebViewNewWindowResponse(guest->guest_instance_id(), true,
                                   user_input.c_str());
        return;
      }
    }
  } else {
    if (it == pending_new_windows_.end())
      return;
  }
  const NewWindowInfo &new_window_info = it->second;

  // Retrieve the opener partition info if we have it.
  const GURL& site_url = new_contents->GetSiteInstance()->GetSiteURL();
  std::string storage_partition_id = GetStoragePartitionIdFromSiteURL(site_url);

  base::DictionaryValue request_info;
  request_info.SetInteger(webview::kInitialHeight, initial_bounds.height());
  request_info.SetInteger(webview::kInitialWidth, initial_bounds.width());
  request_info.Set(webview::kTargetURL,
                   new base::StringValue(new_window_info.url.spec()));
  request_info.Set(webview::kName, new base::StringValue(new_window_info.name));
  request_info.SetInteger(webview::kWindowID, guest->guest_instance_id());
  // We pass in partition info so that window-s created through newwindow
  // API can use it to set their partition attribute.
  request_info.Set(webview::kStoragePartitionId,
                   new base::StringValue(storage_partition_id));
  request_info.Set(
      webview::kWindowOpenDisposition,
      new base::StringValue(WindowOpenDispositionToString(disposition)));
  request_info.SetBoolean(guest_view::kUserGesture, user_gesture);

  web_view_permission_helper_->
      RequestPermission(WEB_VIEW_PERMISSION_TYPE_NEW_WINDOW,
                        request_info,
                        base::Bind(&WebViewGuest::OnWebViewNewWindowResponse,
                                   weak_ptr_factory_.GetWeakPtr(),
                                   guest->guest_instance_id()),
                                   false /* allowed_by_default */);
}

GURL WebViewGuest::ResolveURL(const std::string& src) {
  if (!GuestViewManager::FromBrowserContext(browser_context())->
          IsOwnedByExtension(this)) {
    return GURL(src);
  }

  GURL default_url(base::StringPrintf("%s://%s/",
                                      kExtensionScheme,
                                      owner_host().c_str()));
  return default_url.Resolve(src);
}

void WebViewGuest::OnWebViewNewWindowResponse(
    int new_window_instance_id,
    bool allow,
    const std::string& user_input) {
  auto* guest =
      WebViewGuest::From(owner_web_contents()->GetRenderProcessHost()->GetID(),
                         new_window_instance_id);
  if (!guest)
    return;

  if (allow) {
#ifdef VIVALDI_BUILD
    if (IsVivaldiRunning()) {
      bool foreground = true;
      bool incognito = false;
      int window_id = -1;
      ParseNewWindowUserInput(user_input, window_id, foreground, incognito);
      // If we are a new incognito window, don't open the tab here. Let the
      // tabs API (WindowsCreateFunction) handle that. Otherwise we would get a
      // second tab with the same URL.
      if (!incognito)
        AddGuestToTabStripModel(guest, window_id, foreground);
      else
        guest->Destroy();
    }
#endif //VIVALDI_BUILD
  } else {
    guest->Destroy();
  }
}

void WebViewGuest::OnFullscreenPermissionDecided(
    bool allowed,
    const std::string& user_input) {
  last_fullscreen_permission_was_allowed_by_embedder_ = allowed;
  SetFullscreenState(allowed);
}

bool WebViewGuest::GuestMadeEmbedderFullscreen() const {
  return last_fullscreen_permission_was_allowed_by_embedder_ &&
         is_embedder_fullscreen_;
}

void WebViewGuest::SetFullscreenState(bool is_fullscreen) {
  if (is_fullscreen == is_guest_fullscreen_)
    return;

  bool was_fullscreen = is_guest_fullscreen_;
  is_guest_fullscreen_ = is_fullscreen;
  // If the embedder entered fullscreen because of us, it should exit fullscreen
  // when we exit fullscreen.
  if (was_fullscreen && GuestMadeEmbedderFullscreen()) {
    // Dispatch a message so we can call document.webkitCancelFullscreen()
    // on the embedder.
    std::unique_ptr<base::DictionaryValue> args(new base::DictionaryValue());
    DispatchEventToView(base::MakeUnique<GuestViewEvent>(
        webview::kEventExitFullscreen, std::move(args)));
  }
  // Since we changed fullscreen state, sending a Resize message ensures that
  // renderer/ sees the change.
  web_contents()->GetRenderViewHost()->GetWidget()->WasResized();
}

}  // namespace extensions
