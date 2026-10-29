# CarPlay audio integration

Rewamp uses Apple's CarPlay framework with the existing iOS 15 minimum.
The car and phone share one Flutter engine, library, queue and audio decoder.
The engine starts in `AppDelegate`, so launching from CarPlay does not require
opening a phone window first. `SceneDelegate` attaches the phone UI to that
same engine when needed.

## Features

- **Queue:** select an entry, including an individual subsong. Selection follows
  the entry's identity if the queue is reordered while the list is visible.
- **Library:** favourites, recent tracks and saved playlists. Catalogue entries
  restored from an account remain downloadable even before a local track row
  exists. Missing files without a catalogue identity are omitted.
- **Browse:** select a catalogue collection and browse its tracks, ordered by
  popularity, with server pagination. Archive albums open their track lists.
- **Search on iOS 27+:** the search button in Queue/Library searches the library;
  the button in Browse searches the online catalogue. Older iOS versions keep
  the browsing interface. The car controls when its keyboard is available.
- **Download and play:** the phone's existing resolver handles cached files,
  mirrors, archive albums, auxiliary files and subsongs. Progress, cancellation
  and errors are presented in CarPlay without a phone dialog. Cancellation uses
  a token scoped to the CarPlay selection. Rewamp downloads/cache-fills before
  decoding; this integration does not add progressive network audio streaming.
- **Now Playing:** Apple's shared template uses `audio_service`'s existing
  metadata, artwork, transport commands and interruption handling. Up Next opens
  the queue. Disconnecting the display leaves playback running.

Visualizers and videos are not placed in the CarPlay audio interface. List
pagination replaces the current list, search starts from a top-level tab, and
download UI observes the five-template stack limit.

On iOS the native output device is created on the first playback request;
`audio_session` owns audio-session activation before that request. CarPlay
startup also skips the media-session workaround that briefly advertises a
playing state solely to arm remote controls. Browsing must not activate the
audio session and interrupt the car's other audio source.

## Signing and distribution

`Runner/Runner.entitlements` requests `com.apple.developer.carplay-audio` in all
build configurations. This file is a request, not an Apple approval.

The developer distributing a device build must:

1. Request Apple's CarPlay audio entitlement for their developer account and app.
2. Enable it on the App ID and obtain a matching development/distribution profile.
3. Sign with that team/profile. A fork needs its own bundle identifier and approval;
   a normal wildcard development profile does not grant CarPlay access.

The upstream bundle identifier and signing team are preserved in the project.
Do not commit personal team, profile or bundle identifier changes when preparing
an upstream pull request. Adding CarPlay source does not publish or submit the app.

For vehicle testing, use a Release build signed with a development profile that
includes the test iPhone, so the app can start without a connected Flutter debugger.

See Apple's [entitlement setup](https://developer.apple.com/documentation/carplay/requesting-carplay-entitlements)
and [CarPlay Developer Guide](https://developer.apple.com/download/files/CarPlay-Developer-Guide.pdf).
The guide dated 2026-10-13 adds the search template to audio apps on iOS 27;
using it on earlier systems is not supported by the audio entitlement.

## Build and automated validation

Follow the native-library prerequisites in [README.md](README.md), including
submodule patches and the iOS ANGLE/libopenmpt/vgmstream XCFrameworks, then:

```sh
cd app
flutter pub get
flutter analyze --no-pub lib/carplay.dart lib/app_shell.dart \
  lib/player_controller.dart lib/search_screen.dart lib/media_session.dart
flutter test --no-pub
flutter build ios --simulator --debug --no-codesign
```

`tool/check_carplay_ios.sh` can type-check the native host against the installed
iOS SDK before the decoder libraries are available. It substitutes only the
generated plugin registrant for that check; it does not link or run the app.

For a real-engine transport smoke test, build with the Xcode build-setting
override `FLUTTER_TARGET=tool/carplay_audio_smoke.dart`. It generates a quiet WAV
inside the app's cache, verifies that startup has no output device, then checks
first play, pause, resume and stop through the normal media handler. Look for
`[CarPlay audio smoke] PASSED` in the device log. Rebuild with
`FLUTTER_TARGET=lib/main.dart` afterward; the smoke entrypoint is not the app's
shipping entrypoint.

Focused tests are in `test/carplay_browser_test.dart`,
`test/carplay_download_test.dart` and `test/carplay_session_test.dart`. They
exercise queue mutations, duplicate/subsong identity, restored playlists,
online pagination, redownloadable library rows, offline errors, the native/Dart
handshake, progress subscriptions, cancellation and phone-dialog isolation.

## Manual acceptance checks

Xcode 27's Device Hub does not support CarPlay with virtual iPhones. Its CarPlay
simulator connects to a physical iPhone, which still needs a signed build with
the approved entitlement. Apple confirms this in
[developer forum thread 834440](https://developer.apple.com/forums/thread/834440).
An earlier Xcode simulator with a CarPlay external display can exercise the
older browsing/playback UI; iOS 27 search still needs a supported test setup.
Use a signed device in a CarPlay vehicle for final acceptance.

1. Cold-launch from CarPlay with no phone scene; browse and play, then open the
   phone and confirm that it shows the same track and queue.
2. Open the phone first, connect/disconnect/reconnect CarPlay, and verify that
   there is still one playback session and playback survives disconnection.
3. Test play/pause, next/previous, steering-wheel controls, metadata/artwork,
   Up Next and selecting subsongs. Reorder/remove a queue entry on the phone
   immediately before selecting its displayed car row.
4. Browse a collection past the first page, play a new download, observe
   progress, cancel a slow transfer, and retry after a network failure. Test a
   simple module, a subsong container, and an archive album with auxiliary files.
5. Restore a playlist on a device without its downloads and play several
   consecutive catalogue entries. Verify offline cached playback and missing
   local-only files.
6. On iOS 27+, search locally and online, type quickly, clear the query, and
   test with the car's keyboard disabled. On iOS 15–26 verify that no search
   template is offered and lists still work.
7. Test while the phone is locked, during calls/audio interruptions, at the
   maximum navigation depth, and while another car audio source is playing.
   Opening Rewamp for browsing alone must not interrupt that source.

Automated tests and a headless engine smoke test do not replace these complete
CarPlay and vehicle acceptance checks.

## Validation on 2026-10-29

- Dart analysis: no findings in the changed app code and smoke entrypoint.
- Full Flutter suite: 831 tests passed, including 22 CarPlay regression tests.
- Native host: SDK type-check and complete arm64 iOS simulator build passed
  with Xcode 27.1.
- Real-engine smoke test on iOS 27 Simulator: output absent at startup, created
  on first play; play, pause, resume and stop passed.
- Headless Flutter startup: the widget tree builds without a phone window.
- Signed arm64 device builds: Debug and Release passed. The CarPlay audio
  entitlement was verified in both the development profile and app signature;
  the registered test iPhone is included in the profile.
- CarPlay screen/vehicle acceptance: pending a suitable simulator setup or an
  entitled device build. No physical-phone installation or in-car testing
  was performed during this work.
