import AppKit
import SwiftUI
import Combine

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var state: AppState!
    private var window: NSWindow!
    private var statusItem: NSStatusItem!
    private var hud: RecordingHUD!
    private var cancellables = Set<AnyCancellable>()

    func applicationDidFinishLaunching(_ notification: Notification) {
        state = AppState()
        hud = RecordingHUD(dictation: state.dictation)
        state.hotkey.onReviewAccept = { [weak self] in self?.state.dictation.acceptReview() }
        state.hotkey.onReviewDiscard = { [weak self] in self?.state.dictation.discardReview() }

        setUpMenu()
        setUpStatusItem()
        setUpWindow()
        observeState()

        Notifier.requestPermissionIfNeeded()
        Notifier.fallback = { [weak self] title, body in
            self?.showFallbackAlertIfVisible(title: title, body: body)
        }

        state.refreshPermissions()
        if state.accessibilityGranted {
            state.hotkey.start()
        }
        showWindow()

        // Quiet update check for released builds (dev builds check manually).
        if !UpdateService.isDevBuild {
            Task {
                if let info = try? await UpdateService.fetchLatest() {
                    Notifier.show(title: "VoiceVector \(info.version) is available",
                                  body: "One-click update in Settings → General.")
                }
            }
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { showWindow() }
        return true
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false // keep running in the status bar; dictation works with the window closed
    }

    // MARK: Window

    private func setUpWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 560),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable,
                                      .fullSizeContentView],
                          backing: .buffered, defer: false)
        window.titlebarAppearsTransparent = true
        window.title = "VoiceVector"
        window.titleVisibility = .hidden
        window.isReleasedWhenClosed = false
        window.center()
        window.setFrameAutosaveName("VoiceVectorMain")
        rebuildContent()
    }

    /// Swap between wizard and main view when wizardCompleted flips.
    private func rebuildContent() {
        let root: AnyView
        if state.config.wizardCompleted {
            root = AnyView(MainView(dictation: state.dictation).environmentObject(state))
        } else {
            root = AnyView(WizardView().environmentObject(state))
        }
        window.contentView = NSHostingView(rootView: root)
    }

    private func showWindow() {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func observeState() {
        var lastWizard = state.config.wizardCompleted
        state.$config
            .receive(on: DispatchQueue.main)
            .sink { [weak self] config in
                guard let self else { return }
                if config.wizardCompleted != lastWizard {
                    lastWizard = config.wizardCompleted
                    self.rebuildContent()
                    if config.wizardCompleted { self.state.refreshPermissions() }
                }
            }
            .store(in: &cancellables)

        state.dictation.$state
            .receive(on: DispatchQueue.main)
            .sink { [weak self] dictationState in
                guard let self else { return }
                switch dictationState {
                case .recording, .processing, .reviewing:
                    self.hud.show()
                default:
                    self.hud.hide()
                }
                self.state.hotkey.recordingActive = (dictationState == .recording)
                self.state.hotkey.reviewActive = (dictationState == .reviewing)
                self.updateStatusIcon(for: dictationState)
            }
            .store(in: &cancellables)
    }

    private func showFallbackAlertIfVisible(title: String, body: String) {
        Log.error("\(title): \(body)")
    }

    // MARK: Status item

    private func setUpStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "waveform.circle",
                                   accessibilityDescription: "VoiceVector")
        }
        let menu = NSMenu()
        let toggleItem = NSMenuItem(title: "Start Dictation", action: #selector(toggleDictation), keyEquivalent: "")
        toggleItem.target = self
        menu.addItem(toggleItem)
        menu.addItem(.separator())
        let openItem = NSMenuItem(title: "Open VoiceVector", action: #selector(openMain), keyEquivalent: "")
        openItem.target = self
        menu.addItem(openItem)
        let settingsItem = NSMenuItem(title: "Settings…", action: #selector(openSettings), keyEquivalent: ",")
        settingsItem.target = self
        menu.addItem(settingsItem)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit VoiceVector", action: #selector(NSApplication.terminate(_:)),
                                keyEquivalent: "q"))
        menu.delegate = self
        statusItem.menu = menu
    }

    private func updateStatusIcon(for dictationState: DictationController.State) {
        guard let button = statusItem.button else { return }
        switch dictationState {
        case .recording:
            button.image = NSImage(systemSymbolName: "record.circle.fill",
                                   accessibilityDescription: "Recording")
            button.contentTintColor = .systemRed
        case .processing:
            button.image = NSImage(systemSymbolName: "waveform.circle.fill",
                                   accessibilityDescription: "Processing")
            button.contentTintColor = .controlAccentColor
        default:
            button.image = NSImage(systemSymbolName: "waveform.circle",
                                   accessibilityDescription: "VoiceVector")
            button.contentTintColor = nil
        }
    }

    @objc private func toggleDictation() {
        if state.dictation.isRecording {
            state.dictation.finishRecording()
        } else {
            state.dictation.startRecording()
        }
    }

    @objc private func openMain() { showWindow() }

    @objc private func openSettings() {
        showWindow()
        state.showSettings = true
    }

    // MARK: App menu (so ⌘C/⌘V/⌘Q work in our own windows)

    private func setUpMenu() {
        let mainMenu = NSMenu()

        let appMenuItem = NSMenuItem()
        mainMenu.addItem(appMenuItem)
        let appMenu = NSMenu()
        appMenu.addItem(NSMenuItem(title: "About VoiceVector",
                                   action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                                   keyEquivalent: ""))
        appMenu.addItem(.separator())
        appMenu.addItem(NSMenuItem(title: "Hide VoiceVector", action: #selector(NSApplication.hide(_:)),
                                   keyEquivalent: "h"))
        appMenu.addItem(.separator())
        appMenu.addItem(NSMenuItem(title: "Quit VoiceVector", action: #selector(NSApplication.terminate(_:)),
                                   keyEquivalent: "q"))
        appMenuItem.submenu = appMenu

        let editMenuItem = NSMenuItem()
        mainMenu.addItem(editMenuItem)
        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(NSMenuItem(title: "Undo", action: Selector(("undo:")), keyEquivalent: "z"))
        editMenu.addItem(NSMenuItem(title: "Redo", action: Selector(("redo:")), keyEquivalent: "Z"))
        editMenu.addItem(.separator())
        editMenu.addItem(NSMenuItem(title: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x"))
        editMenu.addItem(NSMenuItem(title: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c"))
        editMenu.addItem(NSMenuItem(title: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v"))
        editMenu.addItem(NSMenuItem(title: "Select All", action: #selector(NSText.selectAll(_:)),
                                    keyEquivalent: "a"))
        editMenuItem.submenu = editMenu

        let windowMenuItem = NSMenuItem()
        mainMenu.addItem(windowMenuItem)
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(NSMenuItem(title: "Close", action: #selector(NSWindow.performClose(_:)),
                                      keyEquivalent: "w"))
        windowMenu.addItem(NSMenuItem(title: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)),
                                      keyEquivalent: "m"))
        windowMenuItem.submenu = windowMenu
        NSApp.windowsMenu = windowMenu

        NSApp.mainMenu = mainMenu
    }
}

extension AppDelegate: NSMenuDelegate {
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.items.first?.title = state.dictation.isRecording ? "Stop Dictation" : "Start Dictation"
    }
}
