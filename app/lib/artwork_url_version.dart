/// Versions d'une url de pochette.
///
/// Le serveur republie une pochette REMPLACÉE sous une url VERSIONNÉE
/// (`…/cover.png?v=<n>`), alors que l'url nue continue d'être servie par
/// l'edge (`max-age=86400`) avec l'ANCIENNE image. Trois endroits gardent une
/// url de pochette et doivent savoir laquelle est la plus récente: la
/// provenance d'un fichier posé sur disque (ArtworkCache), la ligne `tracks`
/// (upsertTrack ne doit pas y remettre l'ancienne qu'une entrée de file
/// porte encore) et le lecteur qui reçoit la réponse du serveur.
library;

/// Numéro de version d'une url de pochette (`?v=<n>`), 0 sans version.
int artworkUrlVersion(String url) {
  final q = Uri.tryParse(url)?.queryParameters['v'];
  return q == null ? 0 : (int.tryParse(q) ?? 0);
}

String _unversioned(String url) {
  final q = url.indexOf('?');
  return q < 0 ? url : url.substring(0, q);
}

/// [url] est une version PLUS RÉCENTE de [storedFrom] — même url à la requête
/// près, numéro de version plus grand.
///
/// ⚠️ « Différente » ne suffit pas, il faut « plus récente »: une ligne de
/// base ou de file qui porte encore l'ancienne url (sans version) ferait
/// sinon remplacer la nouvelle image par l'ancienne, et deux écrans se la
/// disputeraient. Une url d'un autre CHEMIN n'est pas une version: la même
/// pochette est atteinte par l'url du morceau ET par celle de l'album. Rien de
/// connu ([storedFrom] null: fichier posé avant la règle, ou par
/// l'utilisateur) = version 0.
bool artworkUrlSupersedes({required String? storedFrom, required String url}) {
  if (!url.startsWith('http')) return false;
  if (storedFrom != null && _unversioned(storedFrom) != _unversioned(url)) {
    return false;
  }
  final had = storedFrom == null ? 0 : artworkUrlVersion(storedFrom);
  return artworkUrlVersion(url) > had;
}
