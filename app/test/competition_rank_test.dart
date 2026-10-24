// La coupe d'une entrée de compo se colore d'après `ranking`, la chaîne que
// demozoo affiche. Elle doit se lire COMME le serveur la lit (`podium_rank`,
// mig 282): un ex-aequo s'écrit « 2= » mais aussi « =2 » ou « #2 », et un rang
// se lit en entier — « 10 » n'est pas un or. Le serveur a compté les rangs
// 10-19 comme des ors pendant des mois pour avoir lu le premier caractère.
import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/rewamp_db.dart';

int? rankOf(String ranking) =>
    CompetitionEntry(position: 1, title: 't', ranking: ranking).rankNumber;

void main() {
  test('les formes de podium de demozoo donnent le rang', () {
    expect(rankOf('1'), 1);
    expect(rankOf('2='), 2);
    expect(rankOf('=2'), 2);
    expect(rankOf('#3'), 3);
    expect(rankOf('1st'), 1);
    expect(rankOf('1 (2)'), 1);
  });

  test('un rang se lit en entier, pas sur son premier caractère', () {
    expect(rankOf('10'), 10);
    expect(rankOf('12='), 12);
    expect(rankOf('=23'), 23);
  });

  test('ce qui n\'est pas un rang ne donne rien', () {
    expect(rankOf(''), isNull);
    expect(rankOf('Block 3'), isNull);
    expect(rankOf('Split 14'), isNull);
    expect(rankOf('-'), isNull);
  });
}
