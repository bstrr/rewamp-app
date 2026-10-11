// Paul Robotham: « NOM.dat » + « NOM.ssd » (ou « dat.NOM » + « ssd.NOM »).
// `.dat` est d'abord une extension SID; seul le VOISIN dit que c'est de
// l'Amiga — même règle que `uade_has_robotham_companion` côté natif.

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/uade_info.dart';

void main() {
  late Directory tmp;
  setUp(() async => tmp = await Directory.systemTemp.createTemp('robotham'));
  tearDown(() async => tmp.delete(recursive: true));

  String touch(String n) {
    final f = File(p.join(tmp.path, n))..writeAsBytesSync(const [0]);
    return f.path;
  }

  test('forme suffixe, avec son .ssd', () {
    final dat = touch('space 1889.dat');
    touch('space 1889.ssd');
    expect(UadeInfoService.isUadeFileAt(dat), isTrue);
  });

  test('forme préfixe, casse comprise', () {
    final dat = touch('dat.foo');
    touch('SSD.foo');
    expect(UadeInfoService.isUadeFileAt(dat), isTrue);
  });

  test('le kit par défaut du replay: mdtest.ssd', () {
    final dat = touch('ashes of empire-auto.dat');
    touch('mdtest.ssd');
    expect(UadeInfoService.isUadeFileAt(dat), isTrue);
  });

  test('un .dat seul reste un SID', () {
    expect(UadeInfoService.hasRobothamCompanion(touch('lonely.dat')), isFalse);
  });

  test('.sqt: Quartet PSG (68000) vs SQ-Tracker ZX', () {
    final st = File(p.join(tmp.path, 'time machine.sqt'))
      ..writeAsBytesSync([0x60, 0, 2, 0x64, 0x60, 0, 3, 0x0c]);
    final zx = File(p.join(tmp.path, 'zx.sqt'))
      ..writeAsBytesSync([0x1c, 0x0b, 0x10, 0x00, 0x22, 0x00, 0, 0]);
    expect(UadeInfoService.isUadeFileAt(st.path), isTrue);
    expect(UadeInfoService.isUadeFileAt(zx.path), isFalse);
  });

  test('.psf: SoundFactory (Amiga) vs PSF PlayStation', () {
    final sf = File(p.join(tmp.path, 'axelf.psf'))
      ..writeAsBytesSync([0, 1, 0x3d, 0x67, 0x0f, 0]);
    final ps = File(p.join(tmp.path, 'ff7.psf'))
      ..writeAsBytesSync([0x50, 0x53, 0x46, 1, 0, 0]);
    expect(UadeInfoService.isUadeFileAt(sf.path), isTrue);
    expect(UadeInfoService.isUadeFileAt(ps.path), isFalse);
  });
}
