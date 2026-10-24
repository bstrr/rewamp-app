import 'package:path/path.dart' as p;

/// Order of the files a drop hands us, made to match what the user SAW.
///
/// The drag pasteboard is NOT in display order, and nothing documents its
/// order (the usual explanation is that Finder puts the grabbed item first,
/// then the rest of the selection). Seen here: a block of sorted tracks
/// dropped "sometimes" landed out of order in the queue.
///
/// Within one folder, entries are sorted the way Finder lists them by name
/// ([finderNameCompare]). Folders keep the order in which they first appear,
/// so dropping two folders' worth of files still plays them as two blocks.
List<String> displayOrderForDrop(List<String> paths, {p.Context? context}) {
  final ctx = context ?? p.context;
  final groups = <String, List<String>>{};
  for (final path in paths) {
    (groups[ctx.dirname(path)] ??= []).add(path);
  }
  return [
    for (final g in groups.values)
      ...(g..sort((a, b) =>
          finderNameCompare(ctx.basename(a), ctx.basename(b)))),
  ];
}

/// Name order as Finder shows it (`localizedStandardCompare`): case-blind,
/// and digit runs compared by VALUE, so « 2 Intro » comes before « 10 Boss ».
/// Equal under that rule ⇒ plain comparison, so the order stays total.
int finderNameCompare(String a, String b) {
  final x = a.toLowerCase(), y = b.toLowerCase();
  var i = 0, j = 0;
  while (i < x.length && j < y.length) {
    final cx = x.codeUnitAt(i), cy = y.codeUnitAt(j);
    if (_isDigit(cx) && _isDigit(cy)) {
      final si = i, sj = j;
      while (i < x.length && _isDigit(x.codeUnitAt(i))) {
        i++;
      }
      while (j < y.length && _isDigit(y.codeUnitAt(j))) {
        j++;
      }
      final na = x.substring(si, i).replaceFirst(RegExp(r'^0+(?=\d)'), '');
      final nb = y.substring(sj, j).replaceFirst(RegExp(r'^0+(?=\d)'), '');
      if (na.length != nb.length) return na.length - nb.length;
      final c = na.compareTo(nb);
      if (c != 0) return c;
      continue;
    }
    if (cx != cy) return cx - cy;
    i++;
    j++;
  }
  final rest = (x.length - i) - (y.length - j);
  return rest != 0 ? rest : a.compareTo(b);
}

bool _isDigit(int c) => c >= 0x30 && c <= 0x39;
