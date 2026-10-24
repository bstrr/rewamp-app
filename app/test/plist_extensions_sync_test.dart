// Les deux Info.plist Apple listent les extensions que l'app ouvre (UTI
// exportée + type de document « Chiptune (by extension) »). Ces listes avaient
// été écrites une fois à la main et ne suivaient pas lib/engines.dart: le
// 2026-10-18 le registre comptait 1249 extensions et les plists 1242 — tout
// format ajouté depuis la beta 6 jouait mais « Ouvrir avec », le panneau
// d'ouverture et le Dock l'ignoraient.
//
// Ce test fait les DEUX moitiés du travail, comme third_party_notices_test:
//   REWAMP_WRITE_PLIST=1 flutter test test/plist_extensions_sync_test.dart
//     → réécrit les deux plists;
//   flutter test (normal)
//     → échoue si une extension du registre manque à l'un des deux.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import '../tool/plist_extensions.dart';

/// Depuis `app/`, où `flutter test` s'exécute.
const _plists = ['macos/Runner/Info.plist', 'ios/Runner/Info.plist'];

void main() {
  test('rewritePlistExtensions ne touche que les deux tableaux', () {
    const xml = '<dict>\n'
        '\t<key>public.filename-extension</key>\n'
        '\t<array>\n'
        '\t\t<string>old</string>\n'
        '\t</array>\n'
        '\t<key>Other</key>\n'
        '\t<array>\n'
        '\t\t<string>keep</string>\n'
        '\t</array>\n'
        '\t<key>CFBundleTypeExtensions</key>\n'
        '\t<array>\n'
        '\t\t\t\t<string>old</string>\n'
        '\t\t\t\t<string>older</string>\n'
        '\t</array>\n'
        '</dict>\n';
    final out = rewritePlistExtensions(xml, extensions: ['a', 'b']);
    expect(out, '<dict>\n'
        '\t<key>public.filename-extension</key>\n'
        '\t<array>\n'
        '\t\t<string>a</string>\n'
        '\t\t<string>b</string>\n'
        '\t</array>\n'
        '\t<key>Other</key>\n'
        '\t<array>\n'
        '\t\t<string>keep</string>\n'
        '\t</array>\n'
        '\t<key>CFBundleTypeExtensions</key>\n'
        '\t<array>\n'
        '\t\t<string>a</string>\n'
        '\t\t<string>b</string>\n'
        '\t</array>\n'
        '</dict>\n');
    expect(plistExtensionArrayCount(xml), 2);
  });

  test('le registre ne produit que des extensions plist-sûres', () {
    for (final e in bundledExtensions()) {
      expect(e, isNotEmpty);
      expect(e, e.toLowerCase());
      // Un caractère XML dans une extension casserait le plist entier.
      expect(e, isNot(matches(RegExp(r'[<>&"\s]'))));
    }
  });

  test('les Info.plist Apple suivent le registre des moteurs', () {
    final write = Platform.environment['REWAMP_WRITE_PLIST'] == '1';
    for (final path in _plists) {
      final file = File(path);
      expect(file.existsSync(), isTrue, reason: '$path absent');
      final current = file.readAsStringSync();
      // macOS: UTI + CFBundleTypeExtensions; iOS: UTI seule (le type de
      // document par extension y est ignoré, donc pas déclaré).
      expect(plistExtensionArrayCount(current),
          path.startsWith('macos') ? 2 : 1,
          reason: '$path n\'a pas la forme attendue — le tableau d\'extensions '
              'a bougé ou a été dupliqué, le générateur ne sait plus où écrire');
      final expected = rewritePlistExtensions(current);
      if (write) {
        if (expected != current) {
          file.writeAsStringSync(expected);
          // ignore: avoid_print
          print('$path réécrit (${bundledExtensions().length} extensions)');
        }
        continue;
      }
      expect(current, expected,
          reason: '$path liste d\'autres extensions que engines.dart — un '
              'format ajouté n\'apparaît pas dans « Ouvrir avec » ni dans le '
              'panneau d\'ouverture. Régénérer: REWAMP_WRITE_PLIST=1 flutter '
              'test test/plist_extensions_sync_test.dart');
    }
  });
}
