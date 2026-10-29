import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/l10n/app_localizations.dart';
import 'package:rewamp/rewamp_db.dart';
import 'package:rewamp/search_screen.dart';

void main() {
  late Directory temp;
  setUp(() async {
    temp = await Directory.systemTemp.createTemp('carplay-download-');
    RewampDb.debugDownloadsBaseOverride = temp;
  });
  tearDown(() async {
    RewampDb.debugDownloadsBaseOverride = null;
    await temp.delete(recursive: true);
  });

  SearchResult row(String path) => SearchResult(
      songId: path,
      collection: 'local',
      filename: 'tune.sid',
      title: 'Tune',
      album: null,
      formatExt: 'sid',
      downloadUrl: null,
      fileSize: 0,
      year: null,
      artistNames: const ['Composer'],
      totalCount: 0,
      localPath: path,
      subsongIdx: 7);

  Future<BuildContext> host(WidgetTester tester) async {
    late BuildContext context;
    await tester.pumpWidget(MaterialApp(
        localizationsDelegates: AppLocalizations.localizationsDelegates,
        supportedLocales: AppLocalizations.supportedLocales,
        home: Builder(builder: (ctx) {
          context = ctx;
          return const Scaffold(body: Text('Phone remains usable'));
        })));
    await tester.pumpAndSettle();
    return context;
  }

  testWidgets(
      'CarPlay cached playback preserves the subsong and never opens/pops a phone route',
      (tester) async {
    final context = await host(tester);
    final file = File('${temp.path}/tune.sid')..writeAsBytesSync([1, 2, 3]);
    String? playedPath;
    int? playedSubsong;
    await tester.runAsync(() => downloadAndPlay(context, row(file.path),
            (path, title,
                {artist,
                album,
                albumId,
                formatExt,
                onlineId,
                artworkUrl,
                artworkTargetDir,
                int subsongIdx = 0,
                durationS,
                subsongCount}) {
          playedPath = path;
          playedSubsong = subsongIdx;
        }, showUI: false));
    await tester.pumpAndSettle();
    expect(playedPath, file.path);
    expect(playedSubsong, 7);
    expect(find.byType(AlertDialog), findsNothing);
    expect(find.text('Phone remains usable'), findsOneWidget);
  });

  testWidgets(
      'CarPlay resolver errors reach the car without a phone dialog or snackbar',
      (tester) async {
    final context = await host(tester);
    var callbackCalled = false;
    await tester.runAsync(() async {
      await expectLater(
          downloadAndPlay(context, row('${temp.path}/missing.sid'),
              (path, title,
                  {artist,
                  album,
                  albumId,
                  formatExt,
                  onlineId,
                  artworkUrl,
                  artworkTargetDir,
                  int subsongIdx = 0,
                  durationS,
                  subsongCount}) {
            callbackCalled = true;
          }, showUI: false),
          throwsA(isA<Exception>()));
    });
    await tester.pumpAndSettle();
    expect(callbackCalled, false);
    expect(find.byType(AlertDialog), findsNothing);
    expect(find.byType(SnackBar), findsNothing);
    expect(find.text('Phone remains usable'), findsOneWidget);
  });
}
