import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/local_db.dart';
import 'package:sqflite_common_ffi/sqflite_ffi.dart';

/// Un zip DÉPOSÉ (Darius Gaiden: des vgz et un m3u) se jouait avec la bonne
/// file, mais `tracks` ne recevait que les pistes JOUÉES — l'écran d'album du
/// zip et la relance depuis « écoutés récemment », qui se reconstruisent
/// depuis `tracks`, n'en montraient donc qu'une partie. L'ouverture inscrit
/// désormais toute la liste, sans abîmer ce qu'une lecture a déjà mesuré.
void main() {
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
      year INTEGER, is_favorite INTEGER NOT NULL DEFAULT 0,
      in_library INTEGER NOT NULL DEFAULT 0,
      play_count INTEGER NOT NULL DEFAULT 0, last_played_at INTEGER,
      local_rel_path TEXT, local_origin TEXT,
      file_size INTEGER, file_mtime_ms INTEGER, file_crc INTEGER,
      UNIQUE(file_path, entry_path, subsong_idx))''');
    LocalDb.instance.debugUseDatabase(db);
  });
  tearDown(() => db.close());

  TrackRecord rec(String name) => TrackRecord(
        id: '', filePath: '/nonexistent/cache/dg/$name', entryPath: '',
        subsongIdx: 0, title: name, metaAlbum: 'Darius Gaiden.zip',
        formatExt: 'vgz', source: 'local',
        isFavorite: false, inLibrary: false, playCount: 0,
      );

  test('toute la liste de l\'archive, dans l\'ordre de la file', () async {
    // Une piste déjà jouée: sa durée mesurée et son titre restent.
    await db.insert('tracks', {
      'id': 'played', 'file_path': '/nonexistent/cache/dg/02.vgz',
      'entry_path': '', 'subsong_idx': 0, 'title': 'Captain Neo',
      'meta_album': 'Darius Gaiden.zip', 'duration_s': 42.0,
      'source': 'local', 'last_played_at': 1,
    });
    await LocalDb.instance.materializeOpenedArchiveTracks(
        [rec('01.vgz'), rec('02.vgz'), rec('03.vgz')]);

    final rows = await db.query('tracks', orderBy: 'position');
    expect(rows.map((r) => r['file_path']), [
      '/nonexistent/cache/dg/01.vgz',
      '/nonexistent/cache/dg/02.vgz',
      '/nonexistent/cache/dg/03.vgz',
    ]);
    final played = rows[1];
    expect((played['title'], played['duration_s'], played['position']),
        ('Captain Neo', 42.0, 2));
    // Pas un import: aucune origine posée.
    expect(rows.every((r) => r['local_origin'] == null), isTrue);

    final album = await LocalDb.instance.getTracksForAlbum('Darius Gaiden.zip');
    expect(album.length, 3);
  });
}
