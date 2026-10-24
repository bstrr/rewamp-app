import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/artwork_url_version.dart';
import 'package:rewamp/local_db.dart';
import 'package:sqflite_common_ffi/sqflite_ffi.dart';

/// Une pochette REPUBLIÉE arrive sous une url versionnée (`?v=<n>`); l'url
/// nue reste servie par l'edge avec l'ANCIENNE image. « Skate or Die »
/// (hvsc), 2026-10-23: nouvelle image dans le lecteur après
/// « Re-télécharger », l'ancienne dans « Écoutés récemment » et dans le
/// lecteur après relance.
void main() {
  const bare = 'https://files.rewamp.app/artwork/hvsc/X.sid/cover.png';
  const v1 = '$bare?v=1792767717';
  const v2 = '$bare?v=1792999999';

  test('plus récente seulement, jamais « différente »', () {
    expect(artworkUrlSupersedes(storedFrom: bare, url: v1), isTrue);
    expect(artworkUrlSupersedes(storedFrom: v1, url: v2), isTrue);
    expect(artworkUrlSupersedes(storedFrom: v1, url: bare), isFalse);
    expect(artworkUrlSupersedes(storedFrom: v2, url: v1), isFalse);
    expect(artworkUrlSupersedes(storedFrom: v1, url: v1), isFalse);
  });

  test('provenance inconnue: périmée face à une url versionnée seulement', () {
    expect(artworkUrlSupersedes(storedFrom: null, url: v1), isTrue);
    expect(artworkUrlSupersedes(storedFrom: null, url: bare), isFalse);
  });

  test('un autre chemin n\'est pas une version (url d\'album vs de morceau)',
      () {
    expect(
        artworkUrlSupersedes(
            storedFrom: 'https://files.rewamp.app/artwork/album/cover.png',
            url: v1),
        isFalse);
  });

  group('upsertTrack ne remet pas l\'ancienne url', () {
    sqfliteFfiInit();
    databaseFactory = databaseFactoryFfi;
    late Database db;

    setUp(() async {
      db = await databaseFactory.openDatabase(inMemoryDatabasePath);
      await db.execute('''CREATE TABLE tracks (
        id TEXT PRIMARY KEY, file_path TEXT NOT NULL, entry_path TEXT NOT NULL,
        subsong_idx INTEGER NOT NULL, title TEXT, artist TEXT, meta_album TEXT,
        album_id TEXT, position INTEGER, duration_s REAL, format_ext TEXT,
        subsong_count INTEGER, source TEXT NOT NULL, online_id TEXT,
        artwork_url TEXT, collection_slug TEXT, platform_name TEXT,
        year INTEGER, local_rel_path TEXT, local_origin TEXT,
        UNIQUE(file_path, entry_path, subsong_idx))''');
      await db.execute('CREATE TABLE recent_albums (album_key TEXT PRIMARY '
          'KEY, file_path TEXT NOT NULL, artwork_url TEXT)');
      LocalDb.instance.debugUseDatabase(db);
    });
    tearDown(() => db.close());

    Future<String?> art() async =>
        (await db.query('tracks')).single['artwork_url'] as String?;

    test('ancienne url ignorée, nouvelle appliquée', () async {
      await LocalDb.instance.upsertTrack(
          filePath: '/x/a.sid', title: 'A', artworkUrl: v1, source: 'online');
      await LocalDb.instance.upsertTrack(
          filePath: '/x/a.sid', title: 'A', artworkUrl: bare, source: 'online');
      expect(await art(), v1);
      await LocalDb.instance.upsertTrack(
          filePath: '/x/a.sid', title: 'A', artworkUrl: v2, source: 'online');
      expect(await art(), v2);
    });

    test('replaceTrackArtwork remplace, sous-chansons comprises', () async {
      for (final i in [0, 1]) {
        await LocalDb.instance.upsertTrack(
            filePath: '/x/a.sid', subsongIdx: i, title: 'A',
            artworkUrl: bare, source: 'online');
      }
      await LocalDb.instance.replaceTrackArtwork('/x/a.sid', v1);
      await db.insert('recent_albums',
          {'album_key': 'k', 'file_path': '/x/a.sid', 'artwork_url': bare});
      await LocalDb.instance.replaceTrackArtwork('/x/a.sid', v2);
      expect((await db.query('tracks')).map((r) => r['artwork_url']).toSet(),
          {v2});
      expect((await db.query('recent_albums')).single['artwork_url'], v2);
    });
  });
}
