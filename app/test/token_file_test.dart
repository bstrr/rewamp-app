// Le jeton en fichier 0600, repli sans service de secrets (voir token_file.dart).
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/token_file.dart';

void main() {
  late Directory tmp;
  setUp(() async {
    tmp = await Directory.systemTemp.createTemp('tok_');
    TokenFile.dirOverride = tmp;
  });
  tearDown(() async {
    TokenFile.dirOverride = null;
    await tmp.delete(recursive: true);
  });

  test('rien écrit ⇒ null', () async {
    expect(await TokenFile.read(), isNull);
  });

  test('écrit, relu à l\'identique, et lisible par le seul propriétaire', () async {
    expect(await TokenFile.write('abc.def.ghi'), isTrue);
    expect(await TokenFile.read(), 'abc.def.ghi');
    if (!Platform.isWindows) {
      // Par dart:io, pas par `stat`: `stat -c %a` est la syntaxe GNU, le stat
      // BSD de macOS la refuse (stdout vide) — ce test est resté rouge sur Mac
      // du 2026-09-26 au 2026-10-18 pour ça, le code testé étant sain.
      final st = await File('${tmp.path}/auth_token').stat();
      final mode = (st.mode & 0x1ff).toRadixString(8);
      expect(mode, '600', reason: 'un secret qui reste doit rester PRIVÉ');
    }
  });

  test('réécrit: le nouveau remplace l\'ancien, mode conservé', () async {
    await TokenFile.write('premier');
    await TokenFile.write('second');
    expect(await TokenFile.read(), 'second');
  });

  test('effacé ⇒ null, et effacer deux fois ne lève pas', () async {
    await TokenFile.write('x');
    await TokenFile.delete();
    await TokenFile.delete();
    expect(await TokenFile.read(), isNull);
  });
}
