#ifndef RUNNER_MEDIA_CONTROLS_H_
#define RUNNER_MEDIA_CONTROLS_H_

// Session média système de Windows (SMTC, System Media Transport Controls):
// la vignette du volume / de la barre des tâches, les touches multimédia du
// clavier et du casque. C'est un MIROIR d'état, comme MPRIS sous Linux: la
// lecture reste dans le moteur C, Dart pousse titre / pochette / état par le
// canal `rewamp/smtc` (lib/media_session_windows.dart) et reçoit en retour les
// boutons pressés.

#include <flutter/binary_messenger.h>
#include <windows.h>

#include <memory>

class MediaControls {
 public:
  // Message de fenêtre par lequel un bouton SMTC (rappel sur un fil WinRT
  // quelconque) revient sur le fil de plateforme, le seul d'où un canal
  // Flutter peut être appelé.
  static constexpr UINT kButtonMessage = WM_APP + 0x51;

  // Donne au processus une identité que le shell sait NOMMER, AVANT la
  // création de toute fenêtre. Sans elle, la session média porte l'identifiant
  // que Windows fabrique pour un exe nu (« rewamp.exe ») et la vignette du
  // volume affiche « Application inconnue »: une app non empaquetée doit
  // déclarer un AppUserModelID ET avoir un raccourci du menu Démarrer qui le
  // porte (créé ici s'il manque).
  static void RegisterAppIdentity();

  MediaControls(HWND window, flutter::BinaryMessenger* messenger);
  ~MediaControls();

  // Appelé par FlutterWindow::MessageHandler pour kButtonMessage.
  void DrainPendingEvents();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif  // RUNNER_MEDIA_CONTROLS_H_
