// SidMon 1: modland nomme ces modules Amiga `NOM.sid`, l'extension du C64.
// Sans magie PSID/RSID, libsidplayfp ne peut pas les ouvrir et c'est UADE qui
// joue (`uade_probe_path`, fichier entier) — le portillon Dart doit suivre,
// sinon un SidMon passerait par le STIL et la liste de sous-chansons du C64.

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/uade_info.dart';

void main() {
  late Directory tmp;
  setUp(() async => tmp = await Directory.systemTemp.createTemp('sidmon'));
  tearDown(() async => tmp.delete(recursive: true));

  String file(String name, List<int> head) {
    final b = Uint8List(0x400)..setAll(0, head);
    return (File(p.join(tmp.path, name))..writeAsBytesSync(b)).path;
  }

  test('un SidMon 1 (code 68000 en tête) va à UADE', () {
    // « synthetic age.sid »: 08 F9 00 01 00 BF E0 01…
    final path = file('synthetic age.sid', const [0x08, 0xf9, 0x00, 0x01]);
    expect(UadeInfoService.isSidMon1At(path), isTrue);
    expect(UadeInfoService.isUadeFileAt(path), isTrue);
  });

  test('un vrai SID C64 reste à libsidplayfp', () {
    for (final magic in ['PSID', 'RSID']) {
      final path = file('c64_$magic.sid', magic.codeUnits);
      expect(UadeInfoService.isSidMon1At(path), isFalse, reason: magic);
      expect(UadeInfoService.isUadeFileAt(path), isFalse, reason: magic);
    }
  });

  test('autre extension: hors sujet', () {
    expect(UadeInfoService.isSidMon1At(file('x.prg', const [1, 8, 0, 0])),
        isFalse);
  });
}
