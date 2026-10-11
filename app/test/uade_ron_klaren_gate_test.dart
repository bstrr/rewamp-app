// Ron Klaren: un `.rk` qui porte « RON_KLAREN_SOUNDMODULE! » est joué par le
// moteur NATIF (portage NostalgicPlayer), pas par UADE — même règle que
// `rk_probe` côté natif. La songdb (calculée par le player CustomMade d'UADE)
// ne garde alors que ses métadonnées, jamais sa liste de sous-chansons.

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/uade_info.dart';

void main() {
  late Directory tmp;
  setUp(() async => tmp = await Directory.systemTemp.createTemp('ronklaren'));
  tearDown(() async => tmp.delete(recursive: true));

  /// Un en-tête de module: hunk exécutable + la marque à l'octet 40.
  String module(String name, {bool marker = true, bool hunk = true}) {
    final b = Uint8List(0xa40);
    if (hunk) b.setAll(0, const [0x00, 0x00, 0x03, 0xf3]);
    if (marker) b.setAll(40, 'RON_KLAREN_SOUNDMODULE!'.codeUnits);
    final f = File(p.join(tmp.path, name))..writeAsBytesSync(b);
    return f.path;
  }

  test('forme suffixe, marque présente', () {
    expect(UadeInfoService.isRonKlarenAt(module('electricity.rk')), isTrue);
  });

  test('forme préfixe Amiga, casse comprise', () {
    expect(UadeInfoService.isRonKlarenAt(module('RK.electricity')), isTrue);
  });

  test('sans la marque: reste à UADE', () {
    final path = module('other.rk', marker: false);
    expect(UadeInfoService.isRonKlarenAt(path), isFalse);
    expect(UadeInfoService.isUadeFileAt(path), isTrue);
  });

  test('sans le hunk exécutable: reste à UADE', () {
    expect(UadeInfoService.isRonKlarenAt(module('nohunk.rk', hunk: false)),
        isFalse);
  });

  test('.cm et .rkb restent à UADE même avec la marque', () {
    expect(UadeInfoService.isRonKlarenAt(module('a.cm')), isFalse);
    expect(UadeInfoService.isRonKlarenAt(module('a.rkb')), isFalse);
  });

  test('la songdb garde ses métadonnées, pas ses sous-chansons', () {
    final info = UadeInfo.fromJson({
      'min_subsong': 0,
      'subsong_count': 1,
      'authors': ['Master Blaster'],
      'album': 'Colorado',
      'year': 1990,
      'subsongs': [
        {'idx': 0, 'songend': 'e', 'length_ms': 0},
      ],
    }).withoutSubsongs();
    expect(info.authors, ['Master Blaster']);
    expect(info.album, 'Colorado');
    expect(info.year, 1990);
    expect(info.isKnown, isFalse);
    expect(info.playableSubsongs, isEmpty);
    expect(info.durationMsFor(0), isNull);
  });
}
