import CarPlay
import Flutter
import UIKit

/// One bridge for the process, shared by the phone and CarPlay scenes.
final class CarPlayBridge {
  static let shared = CarPlayBridge()
  private var channel: FlutterMethodChannel?
  private(set) var ready = false
  weak var scene: CarPlaySceneDelegate?
  private var strings: [String: String] = [:]

  func text(_ key: String) -> String {
    strings[key] ?? ["queue": "Queue", "library": "Library",
      "catalogue": "Catalogue", "favorites": "Favorites",
      "recent": "Recently played", "playlists": "Playlists",
      "search": "Search", "empty": "Nothing here yet",
      "error": "Unable to play this item", "retry": "Retry",
      "more": "More", "cancel": "Cancel",
      "downloading": "Downloading…", "ok": "OK"][key] ?? key
  }

  func attach(to messenger: FlutterBinaryMessenger) {
    ready = false
    let channel = FlutterMethodChannel(name: "rewamp/carplay", binaryMessenger: messenger)
    self.channel = channel
    channel.setMethodCallHandler { [weak self] call, result in
      guard let self = self else { result(nil); return }
      switch call.method {
      case "isConnected": result(self.scene != nil)
      case "ready", "changed":
        self.strings = call.arguments as? [String: String] ?? self.strings
        self.ready = true
        self.scene?.refresh(includeOnline: call.method == "ready")
        result(self.scene != nil)
      case "download":
        self.scene?.updateDownload(call.arguments as? [String: Any])
        result(nil)
      case "error":
        self.scene?.showError()
        result(nil)
      default: result(FlutterMethodNotImplemented)
      }
    }
  }

  func connected(_ connected: Bool) {
    guard ready else { return } // Dart's ready handshake covers a cold start.
    channel?.invokeMethod("setConnected", arguments: ["connected": connected])
  }

  func call(_ method: String, _ args: [String: Any], completion: @escaping (Any?) -> Void) {
    guard ready, let channel = channel else {
      completion(FlutterError(code: "starting", message: nil, details: nil)); return
    }
    channel.invokeMethod(method, arguments: args, result: completion)
  }
}

final class CarPlaySceneDelegate: UIResponder, CPTemplateApplicationSceneDelegate,
    CPNowPlayingTemplateObserver, CPSearchTemplateDelegate {
  private var interfaceController: CPInterfaceController?
  private var tabs: [CPListTemplate] = []
  private var lists: [ObjectIdentifier: (CPListTemplate, String, String, Int)] = [:]
  private var requests: [ObjectIdentifier: UUID] = [:]
  private var searchGeneration = UUID()
  private var searchSource = "search"
  private var searchQuery = ""
  private var latestSearchItems: [CPListItem] = []
  private var selecting = false
  private var progressTemplate: CPListTemplate?
  private var progressAlert: CPAlertTemplate?
  private let bridge = CarPlayBridge.shared

  func templateApplicationScene(_ templateApplicationScene: CPTemplateApplicationScene,
                               didConnect interfaceController: CPInterfaceController) {
    self.interfaceController = interfaceController
    bridge.scene = self
    let sources = ["queue", "library", "catalogue"]
    let symbols = ["list.bullet", "music.note.house", "globe"]
    tabs = zip(sources, symbols).map { source, symbol in
      let template = CPListTemplate(title: bridge.text(source), sections: [])
      template.tabTitle = bridge.text(source)
      template.tabImage = UIImage(systemName: symbol)
      lists[ObjectIdentifier(template)] = (template, source, "", 0)
      return template
    }
    interfaceController.setRootTemplate(CPTabBarTemplate(templates: tabs), animated: false,
                                       completion: nil)
    CPNowPlayingTemplate.shared.add(self)
    CPNowPlayingTemplate.shared.isUpNextButtonEnabled = true
    bridge.connected(true)
    refresh(includeOnline: true)
  }

  func templateApplicationScene(_ templateApplicationScene: CPTemplateApplicationScene,
                               didDisconnectInterfaceController interfaceController: CPInterfaceController) {
    CPNowPlayingTemplate.shared.remove(self)
    if bridge.scene === self { bridge.scene = nil }
    bridge.connected(false)
    self.interfaceController = nil
    lists.removeAll()
    requests.removeAll()
    tabs.removeAll()
    progressTemplate = nil
    progressAlert = nil
    searchGeneration = UUID()
    latestSearchItems = []
    // Disconnecting the display never stops audio or destroys the engine.
  }

  func refresh(includeOnline: Bool = false) {
    guard let controller = interfaceController else { return }
    let live = controller.templates
    lists = lists.filter { _, value in
      tabs.contains(where: { $0 === value.0 }) || live.contains(where: { $0 === value.0 })
    }
    for (_, (template, source, query, offset)) in lists {
      if source == "queue" || source == "library" || source == "catalogue" {
        template.tabTitle = bridge.text(source)
      }
      template.emptyViewTitleVariants = [bridge.text("empty")]
      // Search is an audio-app template only from iOS 27 (Apple's guide).
      if #available(iOS 27.0, *), tabs.contains(where: { $0 === template }) {
        template.trailingNavigationBarButtons = [CPBarButton(
          image: UIImage(systemName: "magnifyingglass")!) { [weak self] _ in
            self?.showSearch(online: source == "catalogue" || source.hasPrefix("collection:"))
          }]
      }
      let online = source == "catalogue" || source == "onlineSearch" ||
          source.hasPrefix("collection:") || source.hasPrefix("album:")
      if bridge.ready && (includeOnline || !online) {
        load(template, source: source, query: query, offset: offset)
      }
    }
    CPNowPlayingTemplate.shared.upNextTitle = bridge.text("queue")
  }

  private var pageSize: Int { max(1, min(100, CPListTemplate.maximumItemCount - 2)) }

  private func load(_ template: CPListTemplate, source: String, query: String = "", offset: Int = 0) {
    lists[ObjectIdentifier(template)] = (template, source, query, offset)
    let request = UUID()
    requests[ObjectIdentifier(template)] = request
    bridge.call("browse", ["source": source, "query": query,
                           "offset": offset, "limit": pageSize]) { [weak self, weak template] value in
      guard let self = self, let template = template, self.interfaceController != nil,
            self.requests[ObjectIdentifier(template)] == request else { return }
      guard let page = value as? [String: Any] else {
        let retry = CPListItem(text: self.bridge.text("retry"), detailText: self.bridge.text("error"))
        retry.handler = { [weak self, weak template] _, done in
          if let template = template { self?.load(template, source: source, query: query, offset: offset) }
          done()
        }
        template.updateSections([CPListSection(items: [retry])]); return
      }
      var items = self.items(page, source: source)
      if offset > 0 {
        let previous = CPListItem(text: "1–\(offset)", detailText: nil,
                                  image: UIImage(systemName: "chevron.left"))
        previous.handler = { [weak self, weak template] _, done in
          if let self = self, let template = template {
            self.load(template, source: source, query: query, offset: max(0, offset - self.pageSize))
          }
          done()
        }
        items.insert(previous, at: 0)
      }
      if let next = page["nextOffset"] as? Int {
        let more = CPListItem(text: self.bridge.text("more"), detailText: nil)
        more.handler = { [weak self, weak template] _, done in
          if let template = template { self?.load(template, source: source, query: query, offset: next) }
          done()
        }
        items.append(more)
      }
      template.updateSections(items.isEmpty ? [] : [CPListSection(items: items)])
    }
  }

  private func items(_ page: [String: Any], source: String) -> [CPListItem] {
    (page["items"] as? [[String: Any]] ?? []).map { data in
      let title = data["title"] as? String ?? bridge.text(data["label"] as? String ?? "empty")
      let item = CPListItem(text: title, detailText: data["subtitle"] as? String)
      item.isPlaying = data["playing"] as? Bool ?? false
      if let child = data["source"] as? String {
        item.accessoryType = .disclosureIndicator
        item.handler = { [weak self] _, done in
          guard let self = self else { done(); return }
          let template = CPListTemplate(title: title, sections: [])
          template.emptyViewTitleVariants = [self.bridge.text("empty")]
          self.interfaceController?.pushTemplate(template, animated: true, completion: nil)
          self.load(template, source: child)
          done()
        }
      } else if let id = data["id"] as? String {
        item.handler = { [weak self] _, done in self?.select(source, id: id, done: done) ?? done() }
      }
      return item
    }
  }

  private func select(_ source: String, id: String, done: @escaping () -> Void) {
    guard !selecting else { done(); return }
    selecting = true
    bridge.call("select", ["source": source, "id": id]) { [weak self] result in
      done() // Every CarPlay selection is completed, including a failed download.
      guard let self = self else { return }
      self.selecting = false
      guard let controller = self.interfaceController else { return }
      self.closeProgress {
        if let error = result as? FlutterError {
          if error.code != "cancelled" { self.showError() }
          return
        }
        if controller.templates.contains(where: { $0 === CPNowPlayingTemplate.shared }) {
          controller.pop(to: CPNowPlayingTemplate.shared, animated: true, completion: nil)
        } else {
          controller.pushTemplate(CPNowPlayingTemplate.shared, animated: true, completion: nil)
        }
      }
    }
  }

  func showError() {
    guard let controller = interfaceController, controller.presentedTemplate == nil else { return }
    let action = CPAlertAction(title: bridge.text("ok"), style: .default) { [weak self] _ in
      self?.interfaceController?.dismissTemplate(animated: true, completion: nil)
    }
    controller.presentTemplate(CPAlertTemplate(titleVariants: [bridge.text("error")],
                                              actions: [action]), animated: true, completion: nil)
  }

  func updateDownload(_ info: [String: Any]?) {
    // Only a CarPlay selection owns this progress screen; phone-initiated and
    // speculative prefetch downloads must not interrupt the car's interface.
    guard selecting, let info = info, let controller = interfaceController else { return }
    let progress: String? = (info["progress"] as? Double).map { "\(Int($0 * 100))%" }
    let item = CPListItem(text: info["title"] as? String ?? bridge.text("downloading"),
                          detailText: progress ?? bridge.text("downloading"))
    item.isEnabled = false
    let cancel = CPListItem(text: bridge.text("cancel"), detailText: nil)
    cancel.handler = { [weak self] _, done in
      guard let self = self else { done(); return }
      self.bridge.call("cancel", [:]) { _ in done() }
    }
    let sections = [CPListSection(items: [item, cancel])]
    if let template = progressTemplate { template.updateSections(sections) }
    else if controller.templates.count < 5 {
      let template = CPListTemplate(title: bridge.text("downloading"), sections: sections)
      progressTemplate = template
      controller.pushTemplate(template, animated: true, completion: nil)
    } else if progressAlert == nil && controller.presentedTemplate == nil {
      let action = CPAlertAction(title: bridge.text("cancel"), style: .cancel) { [weak self] _ in
        self?.bridge.call("cancel", [:]) { _ in }
      }
      let alert = CPAlertTemplate(titleVariants: [bridge.text("downloading")], actions: [action])
      progressAlert = alert
      controller.presentTemplate(alert, animated: true, completion: nil)
    }
  }

  private func closeProgress(_ completion: @escaping () -> Void) {
    if let alert = progressAlert, interfaceController?.presentedTemplate === alert {
      progressAlert = nil
      interfaceController?.dismissTemplate(animated: true) { _, _ in completion() }
      return
    }
    if let template = progressTemplate, interfaceController?.topTemplate === template {
      progressTemplate = nil
      interfaceController?.popTemplate(animated: true) { _, _ in completion() }
    } else { progressTemplate = nil; completion() }
  }

  func nowPlayingTemplateUpNextButtonTapped(_ nowPlayingTemplate: CPNowPlayingTemplate) {
    let template = CPListTemplate(title: bridge.text("queue"), sections: [])
    guard let controller = interfaceController else { return }
    let show = { [weak self] in
      controller.pushTemplate(template, animated: true, completion: nil)
      self?.load(template, source: "queue")
    }
    if controller.templates.count >= 5 {
      controller.popToRootTemplate(animated: false) { _, _ in show() }
    } else { show() }
  }

  private func showSearch(online: Bool) {
    guard #available(iOS 27.0, *) else { return }
    searchSource = online ? "onlineSearch" : "search"
    searchQuery = ""
    latestSearchItems = []
    let template = CPSearchTemplate()
    template.delegate = self
    interfaceController?.pushTemplate(template, animated: true, completion: nil)
  }

  func searchTemplate(_ searchTemplate: CPSearchTemplate, updatedSearchText searchText: String,
                      completionHandler: @escaping ([CPListItem]) -> Void) {
    let generation = UUID()
    searchGeneration = generation
    searchQuery = searchText
    latestSearchItems = []
    guard !searchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
      completionHandler([]); return
    }
    let source = searchSource
    // Debounce keystrokes and reject late results from previous queries.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
      guard let self = self else { completionHandler([]); return }
      guard self.searchGeneration == generation, self.interfaceController != nil else {
        completionHandler(self.latestSearchItems); return
      }
      self.bridge.call("browse", ["source": source, "query": searchText,
                                   "offset": 0, "limit": self.pageSize]) { [weak self] value in
        guard let self = self else { completionHandler([]); return }
        guard self.searchGeneration == generation else {
          completionHandler(self.latestSearchItems); return
        }
        guard let page = value as? [String: Any] else {
          completionHandler([]); self.showError(); return
        }
        var items = self.items(page, source: source)
        if let next = page["nextOffset"] as? Int {
          let more = CPListItem(text: self.bridge.text("more"), detailText: nil)
          more.handler = { [weak self] _, done in
            guard let self = self else { done(); return }
            let template = CPListTemplate(title: self.bridge.text("search"), sections: [])
            self.interfaceController?.pushTemplate(template, animated: true, completion: nil)
            self.load(template, source: source, query: searchText, offset: next)
            done()
          }
          items.append(more)
        }
        self.latestSearchItems = items
        completionHandler(items)
      }
    }
  }

  func searchTemplate(_ searchTemplate: CPSearchTemplate, selectedResult item: CPListItem,
                      completionHandler: @escaping () -> Void) {
    if let handler = item.handler { handler(item, completionHandler) }
    else { completionHandler() }
  }

  func searchTemplateSearchButtonPressed(_ searchTemplate: CPSearchTemplate) {
    let template = CPListTemplate(title: bridge.text("search"), sections: [])
    interfaceController?.pushTemplate(template, animated: true, completion: nil)
    load(template, source: searchSource, query: searchQuery)
  }
}
