// L'anneau d'attente du bouton lecture/pause: il n'apparaît qu'APRÈS le délai
// (un chargement ordinaire ne montre rien) et disparaît dès le résultat.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:rewamp/loading_ring.dart';

Widget _host(ValueNotifier<bool> loading) => MaterialApp(
      home: Scaffold(
        body: Center(
          child: LoadingRing(
            loading: loading,
            size: 40,
            showAfter: const Duration(milliseconds: 300),
            child: const Icon(Icons.play_arrow),
          ),
        ),
      ),
    );

void main() {
  testWidgets('un chargement plus court que le délai ne montre rien', (t) async {
    final loading = ValueNotifier<bool>(false);
    await t.pumpWidget(_host(loading));
    loading.value = true;
    await t.pump(const Duration(milliseconds: 100));
    expect(find.byType(CircularProgressIndicator), findsNothing);
    loading.value = false;
    await t.pump(const Duration(milliseconds: 400));
    expect(find.byType(CircularProgressIndicator), findsNothing,
        reason: 'le minuteur armé doit être annulé par la retombée');
    expect(find.byIcon(Icons.play_arrow), findsOneWidget);
  });

  testWidgets('un chargement lent montre l\'anneau, qui part au résultat', (t) async {
    final loading = ValueNotifier<bool>(true);
    await t.pumpWidget(_host(loading));
    await t.pump(const Duration(milliseconds: 100));
    expect(find.byType(CircularProgressIndicator), findsNothing);
    await t.pump(const Duration(milliseconds: 250));
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    expect(find.byIcon(Icons.play_arrow), findsOneWidget,
        reason: 'le glyphe reste sous l\'anneau');
    loading.value = false;
    await t.pump();
    expect(find.byType(CircularProgressIndicator), findsNothing);
  });

  testWidgets('outlined dessine le trait noir en plus', (t) async {
    final loading = ValueNotifier<bool>(true);
    await t.pumpWidget(MaterialApp(
      home: LoadingRing(
        loading: loading,
        size: 40,
        outlined: true,
        showAfter: Duration.zero,
        child: const Icon(Icons.pause),
      ),
    ));
    await t.pump(const Duration(milliseconds: 10));
    expect(find.byType(CircularProgressIndicator), findsNWidgets(2));
  });
}
