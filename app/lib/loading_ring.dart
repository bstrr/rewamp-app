// Anneau d'attente autour du glyphe lecture/pause pendant qu'une piste s'ouvre.
//
// `rewamp_load_file` tourne hors du fil UI depuis le 2026-10-18: l'interface
// reste vivante pendant une ouverture lente, mais rien ne disait à l'oreille ni
// à l'œil qu'un chargement était en cours. D'où cet anneau — et RIEN d'autre:
// le bouton garde sa place, sa taille et son glyphe (celui de l'état VOULU:
// pause si la piste va démarrer, lecture si l'utilisateur l'a arrêtée pendant
// l'ouverture), le bouton reste actif. Deux garde-fous pour rester discret:
//
//   - un DÉLAI d'apparition (300 ms): un chargement ordinaire dure 5 à 50 ms,
//     aucun anneau, aucun clignotement à chaque piste. Seuls les chargements
//     lents le montrent — exactement ceux qu'on veut signaler;
//   - un fondu d'entrée (150 ms), disparition immédiate au résultat.
//
// Posé aux cinq sites qui dessinent lecture/pause (lecteur plein, transport
// plein écran, mini-lecteur, fenêtre mini compacte et viz). Il ÉCOUTE
// `PlayerController.loadingTrack` lui-même, pour que le lecteur entier ne se
// reconstruise pas deux fois par piste. `outlined` reprend le trait noir des
// glyphes posés sur le visualiseur (voir OutlinedGlyph).
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

class LoadingRing extends StatefulWidget {
  const LoadingRing({
    super.key,
    required this.loading,
    required this.size,
    required this.child,
    this.outlined = false,
    this.showAfter = const Duration(milliseconds: 300),
  });

  /// Vrai tant que l'ouverture native est en vol.
  final ValueListenable<bool> loading;

  /// Diamètre de l'anneau = taille du glyphe qu'il entoure.
  final double size;

  /// Le glyphe (Icon ou OutlinedGlyph).
  final Widget child;

  /// Trait noir sous l'anneau, pour rester lisible sur un preset blanc.
  final bool outlined;

  /// Délai avant d'apparaître — un chargement plus court ne montre rien.
  final Duration showAfter;

  @override
  State<LoadingRing> createState() => _LoadingRingState();
}

class _LoadingRingState extends State<LoadingRing> {
  Timer? _delay;
  bool _visible = false;

  @override
  void initState() {
    super.initState();
    widget.loading.addListener(_onLoading);
    _onLoading();
  }

  @override
  void didUpdateWidget(LoadingRing old) {
    super.didUpdateWidget(old);
    if (old.loading != widget.loading) {
      old.loading.removeListener(_onLoading);
      widget.loading.addListener(_onLoading);
      _onLoading();
    }
  }

  @override
  void dispose() {
    widget.loading.removeListener(_onLoading);
    _delay?.cancel();
    super.dispose();
  }

  void _onLoading() {
    if (widget.loading.value) {
      if (_visible || _delay != null) return;
      _delay = Timer(widget.showAfter, () {
        _delay = null;
        if (!mounted || !widget.loading.value) return;
        setState(() => _visible = true);
      });
    } else {
      _delay?.cancel();
      _delay = null;
      if (_visible) setState(() => _visible = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    // Un peu plus large que le glyphe: l'anneau l'entoure, il ne le barre pas.
    final d = widget.size * 1.15;
    final color = widget.outlined
        ? Colors.white.withValues(alpha: 0.85)
        : IconTheme.of(context).color?.withValues(alpha: 0.6) ??
            Theme.of(context).colorScheme.onSurface.withValues(alpha: 0.6);
    return Stack(
      alignment: Alignment.center,
      clipBehavior: Clip.none,
      children: [
        if (_visible)
          Positioned.fill(
            child: OverflowBox(
              // Contraintes SERRÉES au diamètre, quel que soit le parent: un
              // IconButton donne la taille du glyphe (plus petit que d), un
              // hôte de test donne l'écran entier (plus grand) — avec le seul
              // max, le second cas donnait min > max, contrainte invalide.
              minWidth: d,
              maxWidth: d,
              minHeight: d,
              maxHeight: d,
              child: TweenAnimationBuilder<double>(
                tween: Tween(begin: 0.0, end: 1.0),
                duration: const Duration(milliseconds: 150),
                builder: (_, v, child) => Opacity(opacity: v, child: child),
                child: SizedBox(
                  width: d,
                  height: d,
                  child: Stack(
                    fit: StackFit.expand,
                    children: [
                      if (widget.outlined)
                        CircularProgressIndicator(
                          strokeWidth: 4,
                          color: Colors.black.withValues(alpha: 0.85),
                        ),
                      CircularProgressIndicator(strokeWidth: 2, color: color),
                    ],
                  ),
                ),
              ),
            ),
          ),
        widget.child,
      ],
    );
  }
}
