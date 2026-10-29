import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/carplay.dart';
import 'package:rewamp/download_cancel.dart';
import 'package:rewamp/l10n/app_localizations_en.dart';
import 'package:rewamp/local_db.dart';
import 'package:rewamp/player_controller.dart';
import 'package:rewamp/rewamp_db.dart';

class Controller extends ChangeNotifier implements PlayerController {
  @override
  List<QueueEntry> queue = [const QueueEntry(id: 1, title: 'Tune')];
  @override
  int queueIdx = 0;
  @override
  int queueRevision = 0;
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class Library implements CarPlayLibrary {
  @override
  Future<List<TrackRecord>> tracks(String source) async => [];
  @override
  Future<List<UserPlaylist>> playlists() async => [];
  @override
  Future<List<Collection>> collections() async => [];
  @override
  Future<List<SearchResult>> online(
          String source, String query, int offset, int limit) async =>
      [];
}

void main() {
  final binding = TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('rewamp/carplay');
  const codec = StandardMethodCodec();
  late Controller controller;
  late CarPlaySession session;
  late List<MethodCall> calls;
  Future<void> Function()? play;

  Future<Object?> incoming(
      String method, Map<String, Object?> arguments) async {
    final reply = Completer<ByteData?>();
    binding.defaultBinaryMessenger.handlePlatformMessage(channel.name,
        codec.encodeMethodCall(MethodCall(method, arguments)), reply.complete);
    return codec.decodeEnvelope((await reply.future)!);
  }

  setUp(() {
    calls = [];
    controller = Controller();
    binding.defaultBinaryMessenger.setMockMethodCallHandler(channel,
        (call) async {
      calls.add(call);
      return call.method == 'ready';
    });
    final browser = CarPlayBrowser(
        library: Library(),
        queue: () => controller.queue,
        queueIndex: () => controller.queueIdx,
        playQueueIndex: (_) async {
          await play?.call();
        },
        playTracks: (tracks, index) async {},
        playOnline: (_) async {});
    session = CarPlaySession(controller, browser, () => AppLocalizationsEn());
    play = null;
  });
  tearDown(() {
    session.dispose();
    controller.dispose();
    RewampDb.downloadStatus.value = null;
    binding.defaultBinaryMessenger.setMockMethodCallHandler(channel, null);
  });

  testWidgets(
      'cold-start handshake returns localized labels then handles native browse',
      (tester) async {
    await session.start();
    expect(session.connected, true);
    expect((calls.first.arguments as Map)['library'], 'Library');
    final page = await incoming('browse', {'source': 'queue'}) as Map;
    expect((page['items'] as List).single['title'], 'Tune');
    await tester.pump(const Duration(milliseconds: 250));
    expect(calls.where((c) => c.method == 'changed').length, 1);
  });

  testWidgets(
      'player ticks do not repeatedly rebuild the car lists; disconnect silences updates',
      (tester) async {
    await session.start();
    controller.notifyListeners();
    await tester.pump(const Duration(milliseconds: 250));
    calls.clear();
    for (var i = 0; i < 10; i++) {
      controller.notifyListeners();
    }
    await tester.pump(const Duration(seconds: 1));
    expect(calls, isEmpty);
    await incoming('setConnected', {'connected': false});
    controller.queueIdx = 1;
    controller.notifyListeners();
    RewampDb.downloadStatus.value =
        const DownloadInfo('Download', progress: .5);
    await tester.pump(const Duration(milliseconds: 250));
    expect(calls, isEmpty);
  });

  testWidgets(
      'cancel aborts only the active CarPlay selection and returns a cancelled result',
      (tester) async {
    await session.start();
    final started = Completer<void>();
    final resume = Completer<void>();
    late DownloadCancelToken token;
    play = () async {
      token = DownloadCancelToken.current!;
      started.complete();
      await resume.future;
      token.throwIfCancelled();
    };
    final page = await incoming('browse', {'source': 'queue'}) as Map;
    final selection = incoming('select',
        {'source': 'queue', 'id': (page['items'] as List).single['id']});
    final expectation = expectLater(
        selection,
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'cancelled')));
    await started.future;
    await incoming('cancel', {});
    expect(token.isCancelled, true);
    resume.complete();
    await expectation;
    await tester.pump(const Duration(milliseconds: 250));
  });
}
