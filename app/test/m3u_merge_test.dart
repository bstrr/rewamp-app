// Un dossier peut porter PLUSIEURS playlists qui ne listent pas les mêmes
// fichiers. Memories Off 2nd (jw_dsf) en a trois: `!playlist(InGame).m3u`
// (versions « a » + pistes communes), `!playlist(MusicPlayer).m3u` (versions
// « b » + les mêmes pistes communes) et `!tags.m3u` (tout, avec `@ALBUM`).
// « La plus longue gagne » laissait une moitié de l'album de côté: la playlist
// d'un dossier est la FUSION de toutes, sans doublon.

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/local_import.dart';
import 'package:rewamp/rewamp_db.dart';

List<String> entries(String m3u) => [
      for (final l in m3u.split('\n'))
        if (l.trim().isNotEmpty && !l.trim().startsWith('#')) l.trim()
    ];

const inGame = '01 Ashita.txtp\n'
    '02a Firefly (In-Game).dsf\n'
    '03a Frosted (In-Game).dsf\n'
    '34 Eternity -Ringtone-.txtp\n';
const musicPlayer = '01 Ashita.txtp\n'
    '02b Firefly (Music Player).dsf\n'
    '03b Frosted (Music Player).dsf\n'
    '34 Eternity -Ringtone-.txtp\n';
const tags = '# @ALBUM\tMemories Off 2nd\n'
    '# @DATE\t2001\n'
    '\n'
    '# %TITLE\tAshita tenki ni...\n'
    '01 Ashita.txtp\n'
    '\n'
    '# %TITLE\tFirefly (In-Game)\n'
    '02a Firefly (In-Game).dsf\n'
    '# %TITLE\tFirefly (Music Player)\n'
    '02b Firefly (Music Player).dsf\n'
    '03a Frosted (In-Game).dsf\n'
    '03b Frosted (Music Player).dsf\n'
    '34 Eternity -Ringtone-.txtp\n';

void main() {
  group('mergeM3uTexts', () {
    test('une playlist seule est rendue telle quelle', () {
      expect(RewampDb.mergeM3uTexts([inGame]), inGame);
    });

    test("l'union des deux versions, chaque fichier une fois", () {
      final merged = RewampDb.mergeM3uTexts([inGame, musicPlayer, tags]);
      expect(entries(merged), [
        '01 Ashita.txtp',
        '02a Firefly (In-Game).dsf',
        '03a Frosted (In-Game).dsf',
        '34 Eternity -Ringtone-.txtp',
        '02b Firefly (Music Player).dsf',
        '03b Frosted (Music Player).dsf',
      ]);
    });

    test('deux playlists de tailles différentes sans rien en commun', () {
      final merged = RewampDb.mergeM3uTexts(
          ['a.psf\nb.psf\nc.psf\n', 'x.psf\ny.psf\n']);
      expect(entries(merged), ['a.psf', 'b.psf', 'c.psf', 'x.psf', 'y.psf']);
    });

    test("l'en-tête d'un !tags.m3u fusionné en dernier remonte en tête", () {
      final merged = RewampDb.mergeM3uTexts([inGame, musicPlayer, tags]);
      final lines = merged.split('\n');
      final firstEntry = lines.indexWhere((l) => !l.startsWith('#'));
      expect(lines.indexOf('# @ALBUM\tMemories Off 2nd'),
          inInclusiveRange(0, firstEntry - 1));
    });

    test("le bloc d'une entrée sautée part avec elle", () {
      // Le %TITLE de « 02a » dans !tags.m3u ne doit pas se coller à l'entrée
      // suivante: 02a est déjà là, son bloc est jeté.
      final merged =
          RewampDb.mergeM3uTexts(['02a Firefly (In-Game).dsf\n', tags]);
      expect(merged.contains('# %TITLE\tFirefly (In-Game)'), isFalse);
      final lines = merged.split('\n');
      final b = lines.indexOf('02b Firefly (Music Player).dsf');
      expect(lines[b - 1], '# %TITLE\tFirefly (Music Player)');
    });

    test('une sous-chanson se dédoublonne par son NUMÉRO, pas par son titre',
        () {
      final merged = RewampDb.mergeM3uTexts([
        'game.nsf::NSF,1,Title,1:00,,\ngame.nsf::NSF,2,Field,2:00,,\n',
        'game.nsf::NSF,1,Titre,1:00,,\ngame.nsf::NSF,3,Boss,2:00,,\n',
      ]);
      expect(entries(merged), [
        'game.nsf::NSF,1,Title,1:00,,',
        'game.nsf::NSF,2,Field,2:00,,',
        'game.nsf::NSF,3,Boss,2:00,,',
      ]);
    });
  });

  test('ordre de fusion: ordinaires par nom, !tags.m3u en dernier', () {
    final names = ['!tags.m3u', '!playlist(MusicPlayer).m3u',
        '!playlist(InGame).m3u']..sort(RewampDb.comparePlaylistNames);
    expect(names, ['!playlist(InGame).m3u', '!playlist(MusicPlayer).m3u',
        '!tags.m3u']);
  });

  group('import local', () {
    late Directory tmp;
    setUp(() async {
      tmp = await Directory.systemTemp.createTemp('rewamp_m3u_merge');
    });
    tearDown(() async {
      try {
        await tmp.delete(recursive: true);
      } catch (_) {}
    });

    Future<String> mk(String name, [String content = 'x']) async {
      final f = File(p.join(tmp.path, name));
      await f.writeAsString(content);
      return f.path;
    }

    test('trois playlists du même dossier font UN album, sans démotion',
        () async {
      final names = entries(tags);
      final tracks = <LocalImportPendingTrack>[
        for (final n in names)
          LocalImportPendingTrack(await mk(n), n, 'picker',
              p.extension(n).replaceFirst('.', '')),
      ];
      final m3us = [
        await mk('!playlist(InGame).m3u', inGame),
        await mk('!playlist(MusicPlayer).m3u', musicPlayer),
        await mk('!tags.m3u', tags),
      ];

      final plan = await applyM3uAlbums(tracks, const [], m3us);

      expect(plan.albums.map((a) => a.name), ['Memories Off 2nd']);
      expect(plan.tracks.length, names.length);
      expect(plan.tracks.every((t) => t.album == 'Memories Off 2nd'), isTrue);
      expect([for (final t in plan.tracks) p.basename(t.filePath)], [
        '01 Ashita.txtp',
        '02a Firefly (In-Game).dsf',
        '03a Frosted (In-Game).dsf',
        '34 Eternity -Ringtone-.txtp',
        '02b Firefly (Music Player).dsf',
        '03b Frosted (Music Player).dsf',
      ]);
      expect([for (final t in plan.tracks) t.position], [0, 1, 2, 3, 4, 5]);
    });

    test('des M3U de sous-chansons (un fichier chacun) ne fusionnent pas',
        () async {
      final a = await mk('a.gbs');
      final b = await mk('b.gbs');
      final tracks = [
        LocalImportPendingTrack(a, 'a.gbs', 'picker', 'gbs'),
        LocalImportPendingTrack(b, 'b.gbs', 'picker', 'gbs'),
      ];
      final m3us = [
        await mk('a.m3u', 'a.gbs::GBS,1,One\na.gbs::GBS,2,Two\n'),
        await mk('b.m3u', 'b.gbs::GBS,1,Un\nb.gbs::GBS,2,Deux\n'),
      ];

      final plan = await applyM3uAlbums(tracks, const [], m3us);

      expect(plan.albums, isEmpty);
      expect(plan.tracks.length, 2);
    });
  });
}
