// NoiseTracker GS (Apple IIGS): les rips n'ont le plus souvent AUCUNE
// extension. L'import local les reconnaît à leur en-tête « FTA MODULEFILE »,
// sinon ils étaient copiés comme des COMPAGNONS (pas des pistes).

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/local_import.dart';

void main() {
  late Directory tmp;
  setUp(() async => tmp = await Directory.systemTemp.createTemp('ntgs'));
  tearDown(() async => tmp.delete(recursive: true));

  File mk(String name, List<int> bytes) =>
      File(p.join(tmp.path, name))..writeAsBytesSync(bytes);

  test('un module sans extension est une piste « ntgs »', () {
    final f = mk('Blood Money', [0, 0, ...'FTA MODULEFILE'.codeUnits, 0x28, 0]);
    expect(hasNtgsMagic(f.path), isTrue);
    expect(localAudioFormatOf(f.path), 'ntgs');
  });

  test('les formats éclatés (MUSICFILE/SOUNDFILE) ne sont PAS des modules', () {
    final m = mk('INTROMUSIC', [0x10, 0, ...'FTA MUSICFILE '.codeUnits]);
    final w = mk('INTROMUSIC.W', [0xe0, 0, ...'FTA SOUNDFILE '.codeUnits]);
    expect(localAudioFormatOf(m.path), isNull);
    expect(localAudioFormatOf(w.path), isNull);
  });

  test('un fichier quelconque sans extension reste un compagnon', () {
    final f = mk('README', 'hello world, nothing here'.codeUnits);
    expect(localAudioFormatOf(f.path), isNull);
    expect(localAudioFormatOf(p.join(tmp.path, 'absent')), isNull);
  });
}
