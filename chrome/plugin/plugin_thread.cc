// Copyright 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <windows.h>
#include <objbase.h>

#include "chrome/plugin/plugin_thread.h"

#include "chrome/common/chrome_plugin_lib.h"
#include "chrome/common/ipc_logging.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/plugin_messages.h"
#include "chrome/plugin/chrome_plugin_host.h"
#include "chrome/plugin/npobject_util.h"
#include "chrome/plugin/plugin_process.h"
#include "webkit/glue/plugins/plugin_lib.h"
#include "webkit/glue/webkit_glue.h"

PluginThread* PluginThread::plugin_thread_;

PluginThread::PluginThread(PluginProcess* process,
                           const std::wstring& channel_name)
    : plugin_process_(process),
      channel_name_(channel_name),
      owner_loop_(MessageLoop::current()),
      preloaded_plugin_module_(NULL),
      Thread("Chrome_PluginThread") {
  DCHECK(plugin_process_);
  DCHECK(owner_loop_);
  DCHECK(!plugin_thread_);
  plugin_thread_ = this;

  Start();
}

PluginThread::~PluginThread() {
  Stop();
  plugin_thread_ = NULL;
}

void PluginThread::OnChannelError() {
  owner_loop_->Quit();
}

bool PluginThread::Send(IPC::Message* msg) {
  return channel_->Send(msg);
}

void PluginThread::OnMessageReceived(const IPC::Message& msg) {
  if (msg.routing_id() == MSG_ROUTING_CONTROL) {
    // Resource responses are sent to the resource dispatcher.
    if (resource_dispatcher_->OnMessageReceived(msg))
      return;
    IPC_BEGIN_MESSAGE_MAP(PluginThread, msg)
      IPC_MESSAGE_HANDLER(PluginProcessMsg_CreateChannel, OnCreateChannel)
      IPC_MESSAGE_HANDLER(PluginProcessMsg_ShutdownResponse, OnShutdownResponse)
      IPC_MESSAGE_HANDLER(PluginProcessMsg_PluginMessage, OnPluginMessage)
      IPC_MESSAGE_HANDLER(PluginProcessMsg_BrowserShutdown, OnBrowserShutdown)
    IPC_END_MESSAGE_MAP()
  } else {
    NOTREACHED() << "Only control messages should reach PluginThread.";
  }
}

void PluginThread::Init() {
  PatchNPNFunctions();
  CoInitialize(NULL);
  channel_.reset(new IPC::SyncChannel(channel_name_,
      IPC::Channel::MODE_CLIENT, this, NULL, owner_loop_, true,
      PluginProcess::GetShutDownEvent()));
  notification_service_.reset(new NotificationService);
  resource_dispatcher_ = new ResourceDispatcher(this);

  // Preload the dll to avoid loading, unloading then reloading
  preloaded_plugin_module_ = NPAPI::PluginLib::LoadPluginHelper(
      plugin_process_->plugin_path().c_str());

  ChromePluginLib::Create(plugin_process_->plugin_path(),
                          GetCPBrowserFuncsForPlugin());

  scoped_refptr<NPAPI::PluginLib> plugin =
      NPAPI::PluginLib::CreatePluginLib(plugin_process_->plugin_path());
  if (plugin.get()) {
    plugin->NP_Initialize();
  }

  // Certain plugins, such as flash, steal the unhandled exception filter
  // thus we never get crash reports when they fault. This call fixes it.
  message_loop()->set_exception_restoration(true);

#ifdef IPC_MESSAGE_LOG_ENABLED
  IPC::Logging::current()->SetIPCSender(this);
#endif
}

void PluginThread::CleanUp() {
#ifdef IPC_MESSAGE_LOG_ENABLED
  IPC::Logging::current()->SetIPCSender(NULL);
#endif
  if (preloaded_plugin_module_) {
    FreeLibrary(preloaded_plugin_module_);
    preloaded_plugin_module_ = NULL;
  }
  PluginChannelBase::CleanupChannels();
  NPAPI::PluginLib::UnloadAllPlugins();
  ChromePluginLib::UnloadAllPlugins();
  notification_service_.reset();
  resource_dispatcher_ = NULL;
  CoUninitialize();
}

void PluginThread::OnCreateChannel(int process_id, HANDLE renderer_handle) {
  std::wstring channel_name;
  scoped_refptr<PluginChannel> channel =
      PluginChannel::GetPluginChannel(process_id, renderer_handle, owner_loop_);
  if (channel.get())
    channel_name = channel->channel_name();

  Send(new PluginProcessHostMsg_ChannelCreated(process_id, channel_name));
}

void PluginThread::OnShutdownResponse(bool ok_to_shutdown) {
  PluginProcess::ShutdownProcessResponse(ok_to_shutdown);
}

void PluginThread::OnBrowserShutdown() {
  PluginProcess::BrowserShutdown();
}

void PluginThread::OnPluginMessage(const std::vector<unsigned char> &data) {
  // We Add/Release ref here to ensure that something will trigger the
  // shutdown mechanism for processes started in the absence of renderer's
  // opening a plugin channel.
  PluginProcess::AddRefProcess();
  ChromePluginLib *chrome_plugin =
      ChromePluginLib::Find(plugin_process_->plugin_path());
  if (chrome_plugin) {
    void *data_ptr = const_cast<void*>(reinterpret_cast<const void*>(&data[0]));
    uint32 data_len = static_cast<uint32>(data.size());
    chrome_plugin->functions().on_message(data_ptr, data_len);
  }
  PluginProcess::ReleaseProcess();
}

namespace webkit_glue {

bool DownloadUrl(const std::string& url, HWND caller_window) {
  PluginThread* plugin_thread = PluginThread::GetPluginThread();
  if (!plugin_thread) {
    return false;
  }

  IPC::Message* message =
      new PluginProcessHostMsg_DownloadUrl(MSG_ROUTING_NONE, url,
                                           ::GetCurrentProcessId(),
                                           caller_window);
  return plugin_thread->Send(message);
}

bool GetPluginFinderURL(std::string* plugin_finder_url) {
  if (!plugin_finder_url) {
    NOTREACHED();
    return false;
  }

  PluginThread* plugin_thread = PluginThread::GetPluginThread();
  if (!plugin_thread) {
    return false;
  }

  plugin_thread->Send(
      new PluginProcessHostMsg_GetPluginFinderUrl(plugin_finder_url));
  DCHECK(!plugin_finder_url->empty());
  return true;
}

bool IsDefaultPluginEnabled() {
  return true;
}

} // namespace webkit_glue
