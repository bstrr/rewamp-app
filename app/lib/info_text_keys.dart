/// Repère, dans le corps du panneau ⓘ, l'INTITULÉ d'une ligne « Clé: valeur »
/// pour le faire ressortir — « Format: », « Engine: », les tags des moteurs
/// (« GAME: », « SYSTEM: »), les en-têtes de section (« Files: », « M3U: »).
///
/// Le texte vient de dizaines de moteurs et ne porte aucune structure: c'est
/// la FORME de la ligne qui décide. Une clé commence par une lettre (les
/// listes d'instruments « 12: nom » restent du texte, et un temps « 1:23 »
/// aussi), reste courte (une phrase qui finit par « : » n'est pas une clé) et
/// le deux-points est suivi d'un blanc ou de la fin de ligne (une URL
/// « http://… » n'en est pas une).
///
/// Le CONTENU d'un bloc « COMMENT: » (VGM: le commentaire va à la ligne, clos
/// par un séparateur) n'est jamais lu comme des clés: c'est de la prose citée,
/// où « Thanks to: … » n'est pas un champ.
library;

final _keyRe = RegExp(r"^([A-Za-z][A-Za-z0-9 _\-/().#&'+]{0,31}):(?=\s|$)");

/// Séparateur qui clôt un bloc de commentaire (posé par le greffon VGM).
bool isInfoSeparator(String line) =>
    line.length >= 8 && line.runes.every((r) => r == 0x2500);

/// Pour chaque ligne de [text], la longueur de l'intitulé à mettre en avant
/// (deux-points compris), ou 0.
List<int> infoKeyLengths(String text) {
  final out = <int>[];
  var inComment = false;
  for (final line in text.split('\n')) {
    if (inComment) {
      if (isInfoSeparator(line)) inComment = false;
      out.add(0);
      continue;
    }
    final m = _keyRe.firstMatch(line);
    if (m == null) {
      out.add(0);
      continue;
    }
    // « COMMENT: » seul sur sa ligne ouvre un bloc de prose.
    if (m.group(1) == 'COMMENT' && line.trim() == 'COMMENT:') inComment = true;
    out.add(m.end);
  }
  return out;
}
