// Un « pas dans la songdb » ne se garde pas pour toujours: la songdb du
// serveur peut être complétée (« eezy going »), et un vide figé en cache
// privait le fichier de durée et d'auteur sur l'appareil, sans retour.
import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/uade_info.dart';

void main() {
  const now = 2000000000;
  final ttl = kUadeEmptyCacheTtl.inSeconds;

  test('une réponse vide récente est servie depuis le cache', () {
    expect(
        uadeCacheEntryUsable(
            subsongCount: 0, storedSubsongs: 0,
            fetchedAtSec: now - ttl + 60, nowSec: now),
        isTrue);
  });

  test('une réponse vide périmée fait redemander le serveur', () {
    expect(
        uadeCacheEntryUsable(
            subsongCount: 0, storedSubsongs: 0,
            fetchedAtSec: now - ttl, nowSec: now),
        isFalse);
  });

  test('une réponse pleine ne périme jamais', () {
    expect(
        uadeCacheEntryUsable(
            subsongCount: 3, storedSubsongs: 3,
            fetchedAtSec: 0, nowSec: now),
        isTrue);
  });

  test('un compte sans sous-chants stockés n\'est pas vide', () {
    expect(
        uadeCacheEntryUsable(
            subsongCount: 2, storedSubsongs: 0,
            fetchedAtSec: 0, nowSec: now),
        isTrue);
  });

  test('une ligne sans date (fetched_at absent) est périmée', () {
    expect(
        uadeCacheEntryUsable(
            subsongCount: 0, storedSubsongs: 0,
            fetchedAtSec: 0, nowSec: now),
        isFalse);
  });
}
