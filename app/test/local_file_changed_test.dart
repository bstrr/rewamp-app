import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/local_db.dart';
import 'package:sqflite_common_ffi/sqflite_ffi.dart';

/// Un fichier local déjà joué, puis REMPLACÉ sous le même chemin (un `.vgm`
/// régénéré): la ligne `tracks` gardait la durée et le nombre de sous-chansons
/// de l'ancienne version, et comme la ligne en base l'emporte sur le fichier,
/// la nouvelle version était jouée avec la longueur d'hier — au dépôt comme à
/// la relance depuis les récents. Changé depuis sa dernière lecture ⇒ ces
/// faits sont oubliés (rendu ET base); une ligne du catalogue n'est pas
/// touchée. Le témoin est l'empreinte posée à la lecture (taille, date, CRC32
/// du début et de la fin — mig 78), « modifié après la lecture » en repli
/// pour une ligne qui n'en a pas.
void main() {
  sqfliteFfiInit();
  databaseFactory = databaseFactoryFfi;

  late Database db;
  late Directory tmp;

  setUp(() async {
    tmp = await Directory.systemTemp.createTemp('rewamp_changed_');
    db = await databaseFactory.openDatabase(inMemoryDatabasePath);
    await db.execute('''CREATE TABLE tracks (
      id TEXT PRIMARY KEY, file_path TEXT NOT NULL, entry_path TEXT NOT NULL,
      subsong_idx INTEGER NOT NULL, title TEXT, artist TEXT, meta_album TEXT,
      album_id TEXT, position INTEGER, duration_s REAL, format_ext TEXT,
      subsong_count INTEGER, source TEXT NOT NULL, online_id TEXT,
      artwork_url TEXT, collection_slug TEXT, platform_name TEXT,
      year INTEGER, is_favorite INTEGER NOT NULL DEFAULT 0,
      in_library INTEGER NOT NULL DEFAULT 0,
      play_count INTEGER NOT NULL DEFAULT 0, last_played_at INTEGER,
      local_rel_path TEXT,
      file_size INTEGER, file_mtime_ms INTEGER, file_crc INTEGER,
      UNIQUE(file_path, entry_path, subsong_idx))''');
    LocalDb.instance.debugUseDatabase(db);
  });

  tearDown(() async {
    await db.close();
    await tmp.delete(recursive: true);
  });

  final playedAt = DateTime(2026, 10, 1, 12);
  int sec(DateTime t) => t.millisecondsSinceEpoch ~/ 1000;

  Future<String> file(String name, DateTime modified,
      {List<int> bytes = const [1, 2, 3]}) async {
    final f = File(p.join(tmp.path, name));
    await f.writeAsBytes(bytes);
    await f.setLastModified(modified);
    return f.path;
  }

  Future<void> add(String path,
          {String? onlineId, int? lastPlayed, String album = 'A',
          String? relPath}) =>
      db.insert('tracks', {
        'id': path, 'file_path': path, 'entry_path': '', 'subsong_idx': 0,
        'title': 'T', 'artist': 'Old', 'meta_album': album, 'duration_s': 42.0,
        'subsong_count': 3, 'source': 'local', 'online_id': onlineId,
        'last_played_at': lastPlayed, 'local_rel_path': relPath,
      });

  test('modifié après la dernière lecture: durée, compte, artiste et album '
      'oubliés, en base aussi', () async {
    final f = await file('a.vgm', playedAt.add(const Duration(hours: 1)));
    await add(f, lastPlayed: sec(playedAt));

    // Par l'album d'abord: la requête trouve la ligne par son meta_album
    // AVANT qu'il ne soit oublié.
    expect((await LocalDb.instance.getTracksForAlbum('A')).single.durationS,
        isNull);
    final t = (await LocalDb.instance.getTracksForFile(f)).single;
    expect((t.durationS, t.subsongCount, t.artist, t.metaAlbum, t.title),
        (null, null, null, null, 'T'));
    final row = (await db.query('tracks')).single;
    expect((row['duration_s'], row['subsong_count'], row['artist'],
        row['meta_album']), (null, null, null, null));
    expect((await LocalDb.instance.getTracksForFiles([f]))[f]!.single.durationS,
        isNull);
  });

  test('fichier IMPORTÉ modifié: l\'album (son dossier) reste', () async {
    final f = await file('i.vgm', playedAt.add(const Duration(hours: 1)));
    await add(f, lastPlayed: sec(playedAt), album: 'Dossier', relPath: 'Dossier/i.vgm');
    final t = (await LocalDb.instance.getTracksForFile(f)).single;
    expect((t.artist, t.metaAlbum, t.durationS), (null, 'Dossier', null));
    expect((await db.query('tracks')).single['meta_album'], 'Dossier');
  });

  test('pas modifié depuis: la ligne garde ce qu\'elle sait', () async {
    final f = await file('b.vgm', playedAt.subtract(const Duration(days: 1)));
    await add(f, lastPlayed: sec(playedAt));
    final t = (await LocalDb.instance.getTracksForFile(f)).single;
    expect((t.durationS, t.subsongCount), (42.0, 3));
  });

  test('jamais jouée: aucun témoin, rien d\'oublié', () async {
    final f = await file('c.vgm', playedAt);
    await add(f);
    expect((await LocalDb.instance.getTracksForFile(f)).single.durationS, 42.0);
  });

  test('ligne du catalogue: la durée vient du serveur, jamais oubliée', () async {
    final f = await file('d.vgm', playedAt.add(const Duration(hours: 1)));
    await add(f, onlineId: 'uuid-1', lastPlayed: sec(playedAt));
    expect((await LocalDb.instance.getTracksForFile(f)).single.durationS, 42.0);
    expect((await db.query('tracks')).single['duration_s'], 42.0);
  });

  test('fichier absent: rien d\'oublié (autre appareil)', () async {
    final f = p.join(tmp.path, 'absent.vgm');
    await add(f, lastPlayed: sec(playedAt));
    expect((await LocalDb.instance.getTracksForFile(f)).single.durationS, 42.0);
  });

  group('empreinte (mig 78)', () {
    Future<double?> durationAfter(String f) async =>
        (await LocalDb.instance.getTracksForFile(f)).single.durationS;

    test('posée à la lecture puis fichier intact: rien d\'oublié', () async {
      final f = await file('e.vgm', playedAt);
      await add(f, lastPlayed: sec(playedAt));
      await LocalDb.instance.stampFileFingerprint(f);
      final row = (await db.query('tracks')).single;
      expect(row['file_size'], 3);
      expect(row['file_crc'], isNotNull);
      expect(await durationAfter(f), 42.0);
    });

    test('remplacé par une copie plus ANCIENNE: vu (égalité, pas « plus récent »)',
        () async {
      final f = await file('g.vgm', playedAt);
      await add(f, lastPlayed: sec(playedAt));
      await LocalDb.instance.stampFileFingerprint(f);
      await file('g.vgm', playedAt.subtract(const Duration(days: 30)),
          bytes: [9, 9, 9, 9]);
      expect(await durationAfter(f), isNull);
    });

    test('même taille, même date, contenu différent: le CRC tranche', () async {
      final f = await file('h.vgm', playedAt, bytes: [1, 2, 3]);
      await add(f, lastPlayed: sec(playedAt));
      await LocalDb.instance.stampFileFingerprint(f);
      await file('h.vgm', playedAt, bytes: [3, 2, 1]);
      expect(await durationAfter(f), isNull);
    });

    test('gros fichier: la FIN compte (tags), échantillonnée', () async {
      final big = List<int>.filled(3 * FileFingerprint.kFingerprintSpan, 7);
      final f = await file('big.vgm', playedAt, bytes: big);
      await add(f, lastPlayed: sec(playedAt));
      await LocalDb.instance.stampFileFingerprint(f);
      // Intact: rien d'oublié.
      expect(await durationAfter(f), 42.0);
      // Dernier octet changé, taille et date conservées.
      await file('big.vgm', playedAt, bytes: [...big.take(big.length - 1), 8]);
      expect(await durationAfter(f), isNull);
    });
  });

  group('currentLocalFileFacts (loadFile)', () {
    test('changé: la ligne vidée est rendue, l\'entrée de file ne fait plus foi',
        () async {
      final f = await file('q.vgm', playedAt);
      await add(f, lastPlayed: sec(playedAt));
      await LocalDb.instance.stampFileFingerprint(f);
      await file('q.vgm', playedAt, bytes: [4, 5, 6, 7]);
      final r = await LocalDb.instance.currentLocalFileFacts(f);
      expect((r?.durationS, r?.artist), (null, null));
    });

    test('empreinte de la version présente: la base fait foi', () async {
      final f = await file('r.vgm', playedAt);
      await add(f, lastPlayed: sec(playedAt));
      await LocalDb.instance.stampFileFingerprint(f);
      expect((await LocalDb.instance.currentLocalFileFacts(f))?.durationS, 42.0);
    });

    test('sans empreinte et inchangé: null, la file garde la parole', () async {
      final f = await file('s.vgm', playedAt.subtract(const Duration(days: 1)));
      await add(f, lastPlayed: sec(playedAt));
      expect(await LocalDb.instance.currentLocalFileFacts(f), isNull);
    });

    test('catalogue: jamais', () async {
      final f = await file('t.vgm', playedAt);
      await add(f, onlineId: 'uuid-2', lastPlayed: sec(playedAt));
      await LocalDb.instance.stampFileFingerprint(f);
      expect(await LocalDb.instance.currentLocalFileFacts(f), isNull);
    });
  });
}
