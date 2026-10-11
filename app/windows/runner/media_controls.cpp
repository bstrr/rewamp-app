#include "media_controls.h"

#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <knownfolders.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace wf  = winrt::Windows::Foundation;
namespace wm  = winrt::Windows::Media;
namespace wst = winrt::Windows::Storage;
namespace wss = winrt::Windows::Storage::Streams;

using flutter::EncodableMap;
using flutter::EncodableValue;

namespace {

// Ce que le système a demandé, en attente du fil de plateforme. Partagé par
// pointeur fort avec les rappels WinRT: un bouton pressé PENDANT la fermeture
// trouve encore une boîte valide (et une fenêtre nulle, donc il ne poste rien).
struct Inbox {
  std::mutex lock;
  HWND window = nullptr;
  // {méthode, argument}: "button" + nom, ou "seek" + position en ms.
  std::deque<std::pair<std::string, EncodableValue>> events;

  void Push(std::string method, EncodableValue arg) {
    std::lock_guard<std::mutex> g(lock);
    events.emplace_back(std::move(method), std::move(arg));
    if (window) ::PostMessage(window, MediaControls::kButtonMessage, 0, 0);
  }
};

const EncodableValue* Find(const EncodableMap& m, const char* key) {
  auto it = m.find(EncodableValue(key));
  if (it == m.end() || it->second.IsNull()) return nullptr;
  return &it->second;
}

std::string GetString(const EncodableMap& m, const char* key) {
  const auto* v = Find(m, key);
  if (!v) return {};
  if (const auto* s = std::get_if<std::string>(v)) return *s;
  return {};
}

bool GetBool(const EncodableMap& m, const char* key) {
  const auto* v = Find(m, key);
  if (!v) return false;
  if (const auto* b = std::get_if<bool>(v)) return *b;
  return false;
}

// Le codec standard rend un int32 ou un int64 selon la valeur.
int64_t GetInt(const EncodableMap& m, const char* key) {
  const auto* v = Find(m, key);
  if (!v) return 0;
  if (const auto* i = std::get_if<int32_t>(v)) return *i;
  if (const auto* l = std::get_if<int64_t>(v)) return *l;
  return 0;
}

wf::TimeSpan Ms(int64_t ms) {
  return std::chrono::duration_cast<wf::TimeSpan>(std::chrono::milliseconds(ms));
}

const char* ButtonName(wm::SystemMediaTransportControlsButton b) {
  using B = wm::SystemMediaTransportControlsButton;
  switch (b) {
    case B::Play:     return "play";
    case B::Pause:    return "pause";
    case B::Stop:     return "stop";
    case B::Next:     return "next";
    case B::Previous: return "previous";
    default:          return nullptr;
  }
}

// L'affichage (titre, pochette) est touché par le fil de plateforme ET par
// l'achèvement asynchrone de la pochette, qui peut survenir APRÈS la
// destruction de MediaControls: il vit donc à part, en pointeur fort.
struct Display {
  wm::SystemMediaTransportControls smtc{nullptr};
  std::mutex lock;
  // Une pochette arrivée après le changement de piste ne doit pas repeindre
  // la piste suivante: chaque setMetadata (et la destruction) change de
  // génération.
  std::atomic<uint64_t> generation{0};
};

}  // namespace

namespace {

// Le même identifiant que le flatpak (docs/FLATPAK.md). ⚠️ Il regroupe aussi
// les fenêtres dans la barre des tâches: le changer après une sortie détache
// les épinglages existants.
constexpr wchar_t kAppUserModelId[] = L"app.rewamp.Rewamp";
// Le NOM du raccourci est ce que la vignette du volume affiche.
constexpr wchar_t kShortcutName[] = L"Rewamp.lnk";

// Le raccourci du menu Démarrer pointe-t-il déjà sur CET exe, avec NOTRE
// identifiant? Un dossier d'app (zip décompressé) se déplace: un raccourci
// resté sur l'ancien chemin est réécrit.
bool ShortcutIsCurrent(const std::wstring& lnk, const std::wstring& exe) {
  if (::GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  try {
    auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
    if (FAILED(link.as<IPersistFile>()->Load(lnk.c_str(), STGM_READ))) return false;
    wchar_t target[MAX_PATH] = {};
    if (FAILED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH))) return false;
    if (_wcsicmp(target, exe.c_str()) != 0) return false;

    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    bool same = false;
    if (SUCCEEDED(link.as<IPropertyStore>()->GetValue(PKEY_AppUserModel_ID, &pv)) &&
        pv.vt == VT_LPWSTR) {
      same = wcscmp(pv.pwszVal, kAppUserModelId) == 0;
    }
    ::PropVariantClear(&pv);
    return same;
  } catch (const winrt::hresult_error&) {
    return false;
  }
}

void WriteShortcut(const std::wstring& lnk, const std::wstring& exe) {
  try {
    auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
    winrt::check_hresult(link->SetPath(exe.c_str()));
    winrt::check_hresult(link->SetWorkingDirectory(
        exe.substr(0, exe.find_last_of(L'\\')).c_str()));
    winrt::check_hresult(link->SetIconLocation(exe.c_str(), 0));

    auto store = link.as<IPropertyStore>();
    PROPVARIANT pv;
    winrt::check_hresult(::InitPropVariantFromString(kAppUserModelId, &pv));
    const HRESULT hr = store->SetValue(PKEY_AppUserModel_ID, pv);
    ::PropVariantClear(&pv);
    winrt::check_hresult(hr);
    winrt::check_hresult(store->Commit());
    winrt::check_hresult(link.as<IPersistFile>()->Save(lnk.c_str(), TRUE));
  } catch (const winrt::hresult_error& e) {
    fwprintf(stderr, L"[smtc] raccourci du menu Démarrer non écrit: 0x%08X\n",
             static_cast<unsigned>(e.code()));
  }
}

}  // namespace

void MediaControls::RegisterAppIdentity() {
  ::SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);

  // L'identifiant seul ne suffit pas: la vignette du volume ne NOMME une app
  // non empaquetée que par un raccourci du menu Démarrer portant le même
  // AppUserModelID (mesuré: l'enregistrement HKCU\…\AppUserModelId, qui sert
  // aux notifications, y est ignoré — « Application inconnue »). D'où un
  // raccourci créé, ou remis sur le bon exe, à chaque lancement.
  wchar_t exe_buf[MAX_PATH];
  const DWORD n = ::GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
  if (n == 0 || n == MAX_PATH) return;
  const std::wstring exe(exe_buf, n);

  PWSTR programs = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return;
  const std::wstring lnk = std::wstring(programs) + L"\\" + kShortcutName;
  ::CoTaskMemFree(programs);

  if (!ShortcutIsCurrent(lnk, exe)) WriteShortcut(lnk, exe);
}

struct MediaControls::Impl {
  std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
  std::unique_ptr<flutter::MethodChannel<EncodableValue>> channel;
  std::shared_ptr<Display> display = std::make_shared<Display>();
  wm::SystemMediaTransportControls smtc{nullptr};
  winrt::event_token button_token{};
  winrt::event_token seek_token{};
  int64_t duration_ms = 0;

  void HandleCall(const flutter::MethodCall<EncodableValue>& call,
                  std::unique_ptr<flutter::MethodResult<EncodableValue>> result);
  void SetMetadata(const EncodableMap& args);
  void SetState(const EncodableMap& args);
  void Clear();
  void LoadThumbnail(const std::string& path, const std::string& url, uint64_t gen);
};

MediaControls::MediaControls(HWND window, flutter::BinaryMessenger* messenger)
    : impl_(std::make_unique<Impl>()) {
  impl_->inbox->window = window;
  impl_->channel = std::make_unique<flutter::MethodChannel<EncodableValue>>(
      messenger, "rewamp/smtc", &flutter::StandardMethodCodec::GetInstance());

  // Une application de bureau (non empaquetée) n'a pas de vue « courante »:
  // la session s'obtient POUR UNE FENÊTRE, par l'interface d'interop.
  try {
    auto interop = winrt::get_activation_factory<wm::SystemMediaTransportControls,
                                                 ISystemMediaTransportControlsInterop>();
    winrt::check_hresult(interop->GetForWindow(
        window, winrt::guid_of<wm::SystemMediaTransportControls>(),
        winrt::put_abi(impl_->smtc)));
  } catch (const winrt::hresult_error& e) {
    fwprintf(stderr, L"[smtc] GetForWindow a échoué: 0x%08X %ls\n",
             static_cast<unsigned>(e.code()), e.message().c_str());
    impl_->smtc = nullptr;
  }

  if (impl_->smtc) {
    impl_->display->smtc = impl_->smtc;
    auto& smtc = impl_->smtc;
    // Rien n'est annoncé avant la première piste: une session vide ferait
    // apparaître une vignette « rewamp » sans titre au lancement.
    smtc.IsEnabled(false);
    smtc.IsPlayEnabled(true);
    smtc.IsPauseEnabled(true);
    smtc.IsStopEnabled(true);
    smtc.IsNextEnabled(false);
    smtc.IsPreviousEnabled(false);

    std::weak_ptr<Inbox> weak = impl_->inbox;
    impl_->button_token = smtc.ButtonPressed(
        [weak](const wm::SystemMediaTransportControls&,
               const wm::SystemMediaTransportControlsButtonPressedEventArgs& args) {
          const char* name = ButtonName(args.Button());
          if (!name) return;
          if (auto inbox = weak.lock()) inbox->Push("button", EncodableValue(name));
        });
    impl_->seek_token = smtc.PlaybackPositionChangeRequested(
        [weak](const wm::SystemMediaTransportControls&,
               const wm::PlaybackPositionChangeRequestedEventArgs& args) {
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              args.RequestedPlaybackPosition())
                              .count();
          if (auto inbox = weak.lock()) {
            inbox->Push("seek", EncodableValue(static_cast<int64_t>(ms)));
          }
        });
  }

  Impl* impl = impl_.get();
  impl_->channel->SetMethodCallHandler(
      [impl](const flutter::MethodCall<EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
        impl->HandleCall(call, std::move(result));
      });
}

MediaControls::~MediaControls() {
  {
    std::lock_guard<std::mutex> g(impl_->inbox->lock);
    impl_->inbox->window = nullptr;
    impl_->inbox->events.clear();
  }
  impl_->channel->SetMethodCallHandler(nullptr);
  ++impl_->display->generation;
  if (impl_->smtc) {
    try {
      impl_->smtc.ButtonPressed(impl_->button_token);
      impl_->smtc.PlaybackPositionChangeRequested(impl_->seek_token);
      impl_->smtc.IsEnabled(false);
    } catch (const winrt::hresult_error&) {
    }
  }
}

void MediaControls::DrainPendingEvents() {
  std::deque<std::pair<std::string, EncodableValue>> events;
  {
    std::lock_guard<std::mutex> g(impl_->inbox->lock);
    events.swap(impl_->inbox->events);
  }
  for (auto& [method, arg] : events) {
    impl_->channel->InvokeMethod(method, std::make_unique<EncodableValue>(std::move(arg)));
  }
}

void MediaControls::Impl::HandleCall(
    const flutter::MethodCall<EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
  if (!smtc) {
    // Pas de session (GetForWindow refusé): le Dart ne doit pas lever pour ça.
    result->Success(EncodableValue(false));
    return;
  }
  const auto* args = std::get_if<EncodableMap>(call.arguments());
  static const EncodableMap kEmpty;
  const EncodableMap& m = args ? *args : kEmpty;
  try {
    const auto& name = call.method_name();
    if (name == "setMetadata") {
      SetMetadata(m);
    } else if (name == "setState") {
      SetState(m);
    } else if (name == "clear") {
      Clear();
    } else {
      result->NotImplemented();
      return;
    }
  } catch (const winrt::hresult_error& e) {
    result->Error("smtc", winrt::to_string(e.message()));
    return;
  }
  result->Success(EncodableValue(true));
}

void MediaControls::Impl::SetMetadata(const EncodableMap& m) {
  const uint64_t gen = ++display->generation;
  duration_ms = GetInt(m, "durationMs");
  {
    std::lock_guard<std::mutex> g(display->lock);
    auto updater = smtc.DisplayUpdater();
    updater.ClearAll();
    updater.Type(wm::MediaPlaybackType::Music);
    auto music = updater.MusicProperties();
    music.Title(winrt::to_hstring(GetString(m, "title")));
    music.Artist(winrt::to_hstring(GetString(m, "artist")));
    music.AlbumTitle(winrt::to_hstring(GetString(m, "album")));
    updater.Update();
  }
  smtc.IsEnabled(true);
  LoadThumbnail(GetString(m, "artPath"), GetString(m, "artUrl"), gen);
}

void MediaControls::Impl::LoadThumbnail(const std::string& path,
                                        const std::string& url, uint64_t gen) {
  std::shared_ptr<Display> d = display;
  auto apply = [d, gen](const wss::RandomAccessStreamReference& ref) {
    std::lock_guard<std::mutex> g(d->lock);
    if (d->generation.load() != gen) return;
    auto updater = d->smtc.DisplayUpdater();
    updater.Thumbnail(ref);
    updater.Update();
  };

  if (!path.empty()) {
    // Un fichier local ne passe PAS par CreateFromUri (file:/// n'y est pas
    // accepté hors paquet): il faut un StorageFile, qui ne s'obtient
    // qu'en asynchrone — et le fil de plateforme est un STA, où l'on ne
    // bloque jamais sur une opération WinRT.
    try {
      auto op = wst::StorageFile::GetFileFromPathAsync(winrt::to_hstring(path));
      op.Completed([apply](const wf::IAsyncOperation<wst::StorageFile>& done,
                           wf::AsyncStatus status) {
        if (status != wf::AsyncStatus::Completed) return;
        try {
          apply(wss::RandomAccessStreamReference::CreateFromFile(done.GetResults()));
        } catch (const winrt::hresult_error&) {
        }
      });
    } catch (const winrt::hresult_error& e) {
      fwprintf(stderr, L"[smtc] pochette illisible: %ls\n", e.message().c_str());
    }
  } else if (!url.empty()) {
    try {
      apply(wss::RandomAccessStreamReference::CreateFromUri(
          wf::Uri(winrt::to_hstring(url))));
    } catch (const winrt::hresult_error&) {
    }
  }
}

void MediaControls::Impl::SetState(const EncodableMap& m) {
  const bool has_file = GetBool(m, "hasFile");
  const bool playing = GetBool(m, "playing");
  smtc.IsPlayEnabled(has_file);
  smtc.IsPauseEnabled(has_file);
  smtc.IsStopEnabled(has_file);
  smtc.IsNextEnabled(GetBool(m, "canNext"));
  smtc.IsPreviousEnabled(GetBool(m, "canPrev"));
  smtc.PlaybackStatus(!has_file ? wm::MediaPlaybackStatus::Stopped
                      : playing ? wm::MediaPlaybackStatus::Playing
                                : wm::MediaPlaybackStatus::Paused);

  // Durée inconnue: pas de barre plutôt qu'une barre fausse.
  wm::SystemMediaTransportControlsTimelineProperties tl;
  const int64_t end = has_file ? duration_ms : 0;
  const int64_t pos = end > 0 ? std::min<int64_t>(GetInt(m, "positionMs"), end) : 0;
  tl.StartTime(Ms(0));
  tl.MinSeekTime(Ms(0));
  tl.EndTime(Ms(end));
  tl.MaxSeekTime(Ms(GetBool(m, "canSeek") ? end : 0));
  tl.Position(Ms(pos));
  smtc.UpdateTimelineProperties(tl);
}

void MediaControls::Impl::Clear() {
  ++display->generation;
  {
    std::lock_guard<std::mutex> g(display->lock);
    smtc.DisplayUpdater().ClearAll();
    smtc.DisplayUpdater().Update();
  }
  smtc.PlaybackStatus(wm::MediaPlaybackStatus::Closed);
  smtc.IsEnabled(false);
}
