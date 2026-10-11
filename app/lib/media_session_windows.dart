import 'dart:async';

import 'package:audio_service_platform_interface/audio_service_platform_interface.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// Implémentation WINDOWS d'`audio_service`: la session média système (SMTC —
/// la vignette du volume et de la barre des tâches, les touches multimédia du
/// clavier et du casque). Même rôle qu'`audio_service_mpris` sous Linux: le
/// MÊME `RewampAudioHandler` alimente la session, qui n'est qu'un miroir
/// d'état — la lecture reste dans le moteur C.
///
/// Le natif vit dans le runner (`windows/runner/media_controls.cpp`, canal
/// `rewamp/smtc`): `audio_service` n'a pas d'implémentation Windows, et une
/// session SMTC s'obtient pour une FENÊTRE, que seul le runner possède.
///
/// Posée par [initMediaSession] AVANT `AudioService.init`.
class AudioServiceSmtc extends AudioServicePlatform {
  static const _channel = MethodChannel('rewamp/smtc');

  AudioHandlerCallbacks? _callbacks;
  bool _playing = false;

  // Ce qui a été envoyé en dernier, pour ne pas réécrire la session à chaque
  // tick de 250 ms: seul un CHANGEMENT d'état, un saut de position (seek) ou
  // une seconde écoulée la réécrivent.
  String? _lastStateKey;
  Duration _lastPosition = Duration.zero;
  DateTime _lastSent = DateTime.fromMillisecondsSinceEpoch(0);

  @override
  Future<void> configure(ConfigureRequest request) async {
    _channel.setMethodCallHandler(_onNativeCall);
  }

  Future<void> _onNativeCall(MethodCall call) async {
    final cb = _callbacks;
    if (cb == null) return;
    switch (call.method) {
      case 'button':
        switch (call.arguments as String?) {
          case 'play':
            await cb.play(const PlayRequest());
          case 'pause':
            await cb.pause(const PauseRequest());
          case 'stop':
            await cb.stop(const StopRequest());
          case 'next':
            await cb.skipToNext(const SkipToNextRequest());
          case 'previous':
            await cb.skipToPrevious(const SkipToPreviousRequest());
        }
      case 'seek':
        final ms = call.arguments as int?;
        if (ms != null) {
          await cb.seek(SeekRequest(position: Duration(milliseconds: ms)));
        }
    }
  }

  @override
  void setHandlerCallbacks(AudioHandlerCallbacks callbacks) {
    _callbacks = callbacks;
  }

  @override
  Future<void> setMediaItem(SetMediaItemRequest request) async {
    final item = request.mediaItem;
    // `audio_service` télécharge lui-même une pochette http et republie la
    // piste avec le chemin obtenu dans `artCacheFile`: un fichier local
    // l'emporte toujours (SMTC n'en lit un que par StorageFile).
    final cached = item.extras?['artCacheFile'] as String?;
    final art = item.artUri;
    String? artPath;
    String? artUrl;
    if (cached != null && cached.isNotEmpty) {
      artPath = cached;
    } else if (art != null && art.scheme == 'file') {
      artPath = art.toFilePath(windows: true);
    } else if (art != null && (art.scheme == 'http' || art.scheme == 'https')) {
      artUrl = art.toString();
    }
    _lastStateKey = null; // la piste a changé: l'état suivant repart
    await _invoke('setMetadata', {
      'title': item.title,
      'artist': item.artist,
      'album': item.album,
      'durationMs': item.duration?.inMilliseconds,
      'artPath': artPath,
      'artUrl': artUrl,
    });
  }

  @override
  Future<void> setState(SetStateRequest request) async {
    final s = request.state;
    _playing = s.playing;
    final actions = {
      for (final c in s.controls) c.action,
      ...s.systemActions,
    };
    final hasFile = s.processingState != AudioProcessingStateMessage.idle;
    final canNext = actions.contains(MediaActionMessage.skipToNext);
    final canPrev = actions.contains(MediaActionMessage.skipToPrevious);
    final canSeek = actions.contains(MediaActionMessage.seek);
    final key = '$hasFile|$_playing|$canNext|$canPrev|$canSeek';

    final now = DateTime.now();
    final elapsed = now.difference(_lastSent);
    final expected =
        _lastPosition + (_playing ? elapsed : Duration.zero);
    final drift = (s.updatePosition - expected).abs();
    if (key == _lastStateKey &&
        drift < const Duration(seconds: 2) &&
        (!_playing || elapsed < const Duration(seconds: 1))) {
      return;
    }
    _lastStateKey = key;
    _lastPosition = s.updatePosition;
    _lastSent = now;
    await _invoke('setState', {
      'hasFile': hasFile,
      'playing': _playing,
      'canNext': canNext,
      'canPrev': canPrev,
      'canSeek': canSeek,
      'positionMs': s.updatePosition.inMilliseconds,
    });
  }

  @override
  Future<void> stopService(StopServiceRequest request) async {
    _lastStateKey = null;
    await _invoke('clear', const {});
  }

  // Sans objet ici; `audio_service` les appelle quand même (la file à chaque
  // changement, les infos Android à l'init) et une exception remonterait dans
  // son flux d'erreurs.
  @override
  Future<void> setQueue(SetQueueRequest request) async {}

  @override
  Future<void> setAndroidPlaybackInfo(
      SetAndroidPlaybackInfoRequest request) async {}

  @override
  Future<void> androidForceEnableMediaButtons(
      AndroidForceEnableMediaButtonsRequest request) async {}

  @override
  Future<void> notifyChildrenChanged(
      NotifyChildrenChangedRequest request) async {}

  Future<void> _invoke(String method, Map<String, Object?> args) async {
    try {
      await _channel.invokeMethod<bool>(method, args);
    } on MissingPluginException {
      // Runner antérieur à la session média: rien à faire.
    } catch (e) {
      debugPrint('[smtc] $method: $e');
    }
  }
}
