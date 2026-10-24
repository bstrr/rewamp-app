import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/info_text_keys.dart';

void main() {
  List<String> keys(String t) {
    final lines = t.split('\n');
    final l = infoKeyLengths(t);
    return [for (var i = 0; i < lines.length; i++) lines[i].substring(0, l[i])];
  }

  test('intitulés techniques, tags et sections', () {
    expect(
        keys('Format: VGM\nEngine: libvgm\nChannels: 6 (YM2612 ×6)\n'
            'GAME: Darius\nTITLE-JPN: x\nFiles:\na.vgz  (3 KB)'),
        ['Format:', 'Engine:', 'Channels:', 'GAME:', 'TITLE-JPN:', 'Files:', '']);
  });

  test('ni numéro, ni temps, ni URL, ni phrase', () {
    expect(
        keys('12: Bass\n1:23 intro\nhttps://vgmrips.net\n'
            'This is a really long sentence that happens to end with:'),
        ['', '', '', '']);
  });

  test('le contenu d\'un COMMENT reste de la prose jusqu\'au séparateur', () {
    expect(
        keys('COMMENT:\nThanks to: Foo\nSecond line\n'
            '────────────────────────\nTITLE: X'),
        ['COMMENT:', '', '', '', 'TITLE:']);
  });
}
