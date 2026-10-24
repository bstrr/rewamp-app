import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/local_db.dart';
import 'package:sqflite_common_ffi/sqflite_ffi.dart';

/// Une archive locale REMPLACÉE sous le même chemin change de clé de cache
/// (`<radical>_<taille>`): l'ancienne extraction cède sa place, et ce que la
/// base en savait suit — sans quoi « Écoutés récemment » montrait l'album
/// deux fois (Elevator Action Returns, zip régénéré entre deux dépôts).
void main() {
  sqfliteFfiInit();
  databaseFactory = databaseFactoryFfi;

  late Database db;
  final oldDir = p.join('/c', 'local_archives', 'Album_100');
  final newDir = p.join('/c', 'local_archives', 'Album_200');

  setUp(() async {
    db = await databaseFactory.openDatabase(inMemoryDatabasePath);
    await db.execute('''CREATE TABLE tracks (
      id TEXT PRIMARY KEY, file_path TEXT NOT NULL, entry_path TEXT NOT NULL,
      subsong_idx INTEGER NOT NULL, play_count INTEGER NOT NULL DEFAULT 0,
      local_rel_path TEXT, UNIQUE(file_path, entry_path, subsong_idx))''');
    await db.execute('CREATE TABLE playlist_tracks (id INTEGER PRIMARY KEY, '
        'file_path TEXT, rel_path TEXT)');
    await db.execute('CREATE TABLE recent_albums (album_key TEXT PRIMARY KEY, '
        'meta_album TEXT, file_path TEXT NOT NULL, last_played_at INTEGER)');
    await db.execute('CREATE TABLE library_items (id INTEGER PRIMARY KEY, '
        'type TEXT, ref_id TEXT)');
    LocalDb.instance.debugUseDatabase(db);
  });

  tearDown(() => db.close());

  Future<void> track(String id, String dir, String name, {int plays = 0}) =>
      db.insert('tracks', {
        'id': id, 'file_path': p.join(dir, name), 'entry_path': '',
        'subsong_idx': 0, 'play_count': plays,
      });

  test('l\'historique suit les membres; un membre déjà connu garde la ligne '
      'neuve', () async {
    await track('a-old', oldDir, '01 A.vgz', plays: 3);
    await track('b-old', oldDir, '02 B.vgz', plays: 1);
    await track('b-new', newDir, '02 B.vgz');
    await db.insert('recent_albums', {
      'album_key': 'Album.zip|$oldDir', 'meta_album': 'Album.zip',
      'file_path': p.join(oldDir, '01 A.vgz'), 'last_played_at': 1,
    });
    await db.insert('library_items',
        {'type': 'track', 'ref_id': p.join(oldDir, '01 A.vgz')});

    await LocalDb.instance.supersedeArchiveExtraction(oldDir, newDir);

    final rows = {
      for (final r in await db.query('tracks'))
        r['id']: (r['file_path'], r['play_count'])
    };
    expect(rows, {
      'a-old': (p.join(newDir, '01 A.vgz'), 3),
      'b-new': (p.join(newDir, '02 B.vgz'), 0),
    });
    final recents = await db.query('recent_albums');
    expect(recents.single['file_path'], p.join(newDir, '01 A.vgz'));
    expect((await db.query('library_items')).single['ref_id'],
        p.join(newDir, '01 A.vgz'));
  });

  test('la nouvelle version a déjà son entrée récente: l\'ancienne part',
      () async {
    await db.insert('recent_albums', {
      'album_key': 'old', 'meta_album': 'Album.zip',
      'file_path': p.join(oldDir, '01 A.vgz'), 'last_played_at': 1,
    });
    await db.insert('recent_albums', {
      'album_key': 'new', 'meta_album': 'Album.zip',
      'file_path': p.join(newDir, '01 A.vgz'), 'last_played_at': 2,
    });
    await LocalDb.instance.supersedeArchiveExtraction(oldDir, newDir);
    expect((await db.query('recent_albums')).single['album_key'], 'new');
  });

  test('un dossier voisin au préfixe proche n\'est pas touché', () async {
    await track('x', '${oldDir}0', '01 A.vgz', plays: 2);
    await LocalDb.instance.supersedeArchiveExtraction(oldDir, newDir);
    expect((await db.query('tracks')).single['file_path'],
        p.join('${oldDir}0', '01 A.vgz'));
  });

  group('reconcileArchiveRows: noms EXACTS du disque', () {
    test('casse changée: la ligne rejoint le vrai nom, ou part si prise',
        () async {
      // Le nouveau zip écrit « mission » en minuscules.
      await track('a-old', newDir, '04 Red (Mission 1-1).vgz', plays: 4);
      await track('b-old', newDir, '05 Red (Mission 1-2).vgz', plays: 1);
      await track('b-new', newDir, '05 Red (mission 1-2).vgz');
      await track('gone', newDir, '27 Bonus.vgz', plays: 2);
      await track('ok', newDir, '01 Prologue.vgz');
      await db.insert('library_items',
          {'type': 'track', 'ref_id': p.join(newDir, '04 Red (Mission 1-1).vgz')});

      await LocalDb.instance.reconcileArchiveRows(newDir, {
        '01 Prologue.vgz',
        '04 Red (mission 1-1).vgz',
        '05 Red (mission 1-2).vgz',
      });

      final rows = {
        for (final r in await db.query('tracks'))
          r['id']: (p.basename(r['file_path'] as String), r['play_count'])
      };
      expect(rows, {
        'a-old': ('04 Red (mission 1-1).vgz', 4),
        'b-new': ('05 Red (mission 1-2).vgz', 0),
        'ok': ('01 Prologue.vgz', 0),
      });
      expect((await db.query('library_items')).single['ref_id'],
          p.join(newDir, '04 Red (mission 1-1).vgz'));
    });
  });
}
