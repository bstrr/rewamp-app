import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;
import 'package:rewamp/drop_order.dart';

void main() {
  test('a block grabbed by its middle file plays in display order', () {
    final dropped = [
      '/m/05 Boss.vgz', '/m/01 Intro.vgz', '/m/02 Stage.vgz',
      '/m/10 Ending.vgz', '/m/03 Shop.vgz',
    ];
    expect(displayOrderForDrop(dropped, context: p.posix), [
      '/m/01 Intro.vgz', '/m/02 Stage.vgz', '/m/03 Shop.vgz',
      '/m/05 Boss.vgz', '/m/10 Ending.vgz',
    ]);
  });

  test('digits compare by value, case is ignored', () {
    final names = ['track10', 'Track2', 'track1', 'track02b'];
    names.sort(finderNameCompare);
    expect(names, ['track1', 'Track2', 'track02b', 'track10']);
  });

  test('folders stay blocks, in order of first appearance', () {
    final dropped = ['/b/2.mod', '/a/9.mod', '/b/1.mod', '/a/3.mod'];
    expect(displayOrderForDrop(dropped, context: p.posix),
        ['/b/1.mod', '/b/2.mod', '/a/3.mod', '/a/9.mod']);
  });

  test('windows paths group by folder too', () {
    final dropped = [r'C:\m\b.vgm', r'C:\m\a.vgm'];
    expect(displayOrderForDrop(dropped, context: p.windows),
        [r'C:\m\a.vgm', r'C:\m\b.vgm']);
  });
}
