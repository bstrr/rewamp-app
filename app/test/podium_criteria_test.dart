// Navigateur « Podiums » (migs serveur 283-289): les critères au-delà du
// rang, leurs paramètres RPC, l'agrégation des années de compo en décennies
// et la lecture d'une ligne de palmarès. Contrat: docs/podium_browser_
// server_review.md.
import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/l10n/app_localizations_en.dart';
import 'package:rewamp/podium_filter.dart';
import 'package:rewamp/rewamp_db.dart';

void main() {
  final l10n = AppLocalizationsEn();

  group('PodiumCriteria.toParams', () {
    test('vide: aucun paramètre (le serveur impliquerait p_podium = 0)', () {
      expect(PodiumCriteria.empty.isEmpty, isTrue);
      expect(PodiumCriteria.empty.toParams(), isEmpty);
    });

    test('chaque critère posé part sous son nom serveur', () {
      const c = PodiumCriteria(
          via: 'production',
          kinds: {'chip', 'tracked'},
          series: {'Assembly'},
          yearMin: 1990,
          yearMax: 1999);
      final p = c.toParams();
      expect(p['p_podium_via'], 'production');
      expect(p['p_compo_kind'], unorderedEquals(['chip', 'tracked']));
      expect(p['p_party_series'], ['Assembly']);
      expect(p['p_compo_year_min'], 1990);
      expect(p['p_compo_year_max'], 1999);
      expect(p.keys.toSet(), PodiumCriteria.kParams);
    });

    test('une valeur hors enum ne part JAMAIS (le serveur répondrait 400)', () {
      const c = PodiumCriteria(via: 'song', kinds: {'themed', 'chip'});
      final p = c.toParams();
      expect(p.containsKey('p_podium_via'), isFalse);
      expect(p['p_compo_kind'], ['chip']);
    });

    test('copyWith distingue « non touché » de « remis à null »', () {
      const c = PodiumCriteria(via: 'self', yearMin: 1990, yearMax: 1999);
      expect(c.copyWith(kinds: {'chip'}).via, 'self');
      expect(c.copyWith(via: null).via, isNull);
      expect(c.copyWith(yearMin: null).yearMax, 1999);
      expect(c.copyWith(yearMin: null).yearMin, isNull);
    });

    test('matches: repli client sur une page non filtrée', () {
      CompoPodium pod(Map<String, dynamic> j) => CompoPodium.fromJson({'rank': 1, ...j})!;
      const c = PodiumCriteria(via: 'self', kinds: {'chip'}, series: {'Revision'}, yearMin: 2010);
      expect(c.matches(pod({'via': 'self', 'compo_kind': 'chip', 'party': 'Revision', 'year': 2015})), isTrue);
      // `party` est le NOM de la party; la série s'y lit en préfixe.
      expect(c.matches(pod({'via': 'self', 'compo_kind': 'chip', 'party': 'Revision 2015', 'year': 2015})), isTrue);
      expect(c.matches(pod({'via': 'self', 'compo_kind': 'chip', 'party': 'Revisionist 2015', 'year': 2015})), isFalse);
      expect(c.matches(pod({'via': 'production', 'compo_kind': 'chip', 'party': 'Revision', 'year': 2015})), isFalse);
      expect(c.matches(pod({'via': 'self', 'compo_kind': 'tracked', 'party': 'Revision', 'year': 2015})), isFalse);
      expect(c.matches(pod({'via': 'self', 'compo_kind': 'chip', 'party': 'Assembly', 'year': 2015})), isFalse);
      expect(c.matches(pod({'via': 'self', 'compo_kind': 'chip', 'party': 'Revision', 'year': 2009})), isFalse);
      expect(c.matches(null), isFalse);
      expect(PodiumCriteria.empty.matches(pod({'via': 'self'})), isTrue);
    });
  });

  test('CompoPodium lit compo_kind (mig 283), absent sur un vieux serveur', () {
    expect(CompoPodium.fromJson({'rank': 2, 'compo_kind': 'oldskool'})!.compoKind, 'oldskool');
    expect(CompoPodium.fromJson({'rank': 2})!.compoKind, isNull);
  });

  group('facettes de dimension', () {
    const facets = [
      FacetCount(kind: 'compo_year', value: '1993', count: 300),
      FacetCount(kind: 'compo_year', value: '1999', count: 100),
      FacetCount(kind: 'compo_year', value: '1989', count: 20),
      FacetCount(kind: 'compo_year', value: '2024', count: 50),
      FacetCount(kind: 'compo_year', value: 'n/a', count: 7),
      FacetCount(kind: 'compo_kind', value: 'combined', count: 2477),
      FacetCount(kind: 'compo_kind', value: 'tracked', count: 5293),
      FacetCount(kind: 'compo_kind', value: '', count: 9),
      FacetCount(kind: 'podium', value: '1', count: 7406),
    ];

    test('les années se regroupent en décennies, ordre chronologique', () {
      final d = decadeCounts(facets);
      expect(d.map((e) => e.decade), [1980, 1990, 2020]);
      expect(d.map((e) => e.count), [20, 400, 50]);
    });

    test('une facette de dimension est triée par compte, valeur vide écartée', () {
      final k = podiumDimFacets(facets, 'compo_kind');
      expect(k.map((f) => f.value), ['tracked', 'combined']);
      expect(podiumDimFacets(facets, 'party_series'), isEmpty);
    });

    test('libellés de décennie et bornes de critère', () {
      expect(decadeLabel(l10n, 1990), '1990s');
      final c = decadeCriteria(PodiumCriteria.empty, 1990);
      expect((c.yearMin, c.yearMax), (1990, 1999));
      final y = yearCriteria(PodiumCriteria.empty, 1993);
      expect((y.yearMin, y.yearMax), (1993, 1993));
      expect(yearCounts(facets).map((e) => e.year), [1989, 1993, 1999, 2024]);
    });

    test('eraLabel relit une tranche posée, décennie ou non', () {
      expect(eraLabel(l10n, PodiumCriteria.empty), isNull);
      expect(eraLabel(l10n, const PodiumCriteria(yearMin: 1990, yearMax: 1999)), '1990s');
      expect(eraLabel(l10n, const PodiumCriteria(yearMax: 1989)), '– 1989');
      expect(eraLabel(l10n, const PodiumCriteria(yearMin: 1995, yearMax: 1995)), '1995');
      expect(eraLabel(l10n, const PodiumCriteria(yearMin: 1995, yearMax: 2003)), '1995 – 2003');
      expect(eraLabel(l10n, const PodiumCriteria(yearMin: 2025)), '2025 –');
    });
  });

  test('les sept genres ont un libellé et une explication; un inconnu garde sa valeur', () {
    for (final k in PodiumCriteria.kCompoKinds) {
      expect(compoKindLabel(l10n, k), isNot(k == 'tracked' ? 'x' : k.isEmpty));
      expect(compoKindHint(l10n, k), isNotNull, reason: k);
    }
    expect(compoKindLabel(l10n, 'themed'), 'themed');
    expect(compoKindHint(l10n, 'themed'), isNull);
    expect(podiumViaLabel(l10n, 'production'), l10n.podiumViaProduction);
    expect(podiumViaLabel(l10n, 'self'), l10n.podiumViaSelf);
  });

  test('HallOfFameRow: key_id est la clé, label le libellé, total_count −1 hors offset 0', () {
    final r = HallOfFameRow.fromJson({
      'key_id': 'a1b2', 'label': 'Purple Motion', 'gold': 7, 'silver': 2,
      'bronze': 0, 'total': 9, 'sample_song_id': 's1', 'artwork_url': null,
      'total_count': 5357,
    });
    expect((r.keyId, r.label, r.gold, r.silver, r.bronze, r.total), ('a1b2', 'Purple Motion', 7, 2, 0, 9));
    expect(r.totalCount, 5357);
    expect(HallOfFameRow.fromJson({'key_id': '1993', 'gold': 1}).label, '1993');
    expect(HallOfFameRow.fromJson({'key_id': 'x'}).totalCount, -1);
  });

  test('podiumParamOf borne p_podium à 0-3', () {
    expect([for (final v in [-1, 0, 2, 3, 4]) RewampDb.podiumParamOf(v)], [0, 0, 2, 3, 0]);
  });
}
