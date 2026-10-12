import AppKit
import AVFAudio
import Foundation
import NaturalLanguage
import Speech
import SwiftUI
import Translation

public typealias LanguageCallback = @convention(c) (UnsafeMutableRawPointer?, Int64, UnsafePointer<CChar>?) -> Void

private func json(_ object: [String: Any]) -> String {
  guard let data = try? JSONSerialization.data(withJSONObject: object),
        let string = String(data: data, encoding: .utf8) else {
    return "{\"kind\":\"error\",\"error\":\"Invalid native language event\"}"
  }
  return string
}

private func languageID(_ value: String) -> String {
  value.replacingOccurrences(of: "_", with: "-")
}

private func localeName(_ language: Locale.Language) -> [String: String] {
  let id = language.languageCode?.identifier ?? language.minimalIdentifier
  return ["id": id, "name": Locale.current.localizedString(forIdentifier: id) ?? id]
}

private func localeName(_ language: Locale) -> [String: String] {
  let id = languageID(language.identifier)
  return ["id": id, "name": Locale.current.localizedString(forIdentifier: id) ?? id]
}

@available(macOS 15.0, *)
private struct PreparationView: View {
  let configuration: TranslationSession.Configuration
  let finished: (String?) -> Void

  var body: some View {
    VStack(spacing: 14) {
      Text(configuration.source.map { localeName($0)["name"] ?? "" } ?? "")
      Text(configuration.target.map { localeName($0)["name"] ?? "" } ?? "")
      ProgressView()
    }
    .padding(28)
    .frame(width: 360)
    .translationTask(configuration) { session in
      do {
        try await session.prepareTranslation()
        await MainActor.run { finished(nil) }
      } catch {
        await MainActor.run { finished(error.localizedDescription) }
      }
    }
  }
}

private final class PreparationWindowDelegate: NSObject, NSWindowDelegate {
  let closed: () -> Void
  init(closed: @escaping () -> Void) { self.closed = closed }
  func windowWillClose(_ notification: Notification) { closed() }
}

private final class LanguageBridge {
  private var callback: LanguageCallback?
  private var context: UnsafeMutableRawPointer?
  private var alive = true
  private var lastCapabilities = "{\"available\":false,\"languages\":[],\"speechLanguages\":[],\"error\":\"Unavailable\"}"
  private var nextToken: UInt64 = 0
  private var activeTokens: [Int64: UInt64] = [:]
  private var translationTasks: [Int64: Task<Void, Never>] = [:]
  private var speechTasks: [Int64: Task<Void, Never>] = [:]
  private var speechContinuations: [Int64: Any] = [:]
  private var speechConverters: [Int64: AVAudioConverter] = [:]
  private var speechFormats: [Int64: AVAudioFormat] = [:]
  private var capabilitiesCString: UnsafeMutablePointer<CChar>?
  private var preparationPanel: NSPanel?
  private var preparationDelegate: PreparationWindowDelegate?
  private var preparationRequest: Int64?
  private var preparationCompletion: ((String?) -> Void)?
  private var preparationTask: Task<Void, Never>?

  init(callback: LanguageCallback?, context: UnsafeMutableRawPointer?) {
    self.callback = callback
    self.context = context
    refreshCapabilities()
  }

  deinit {
    invalidate()
    if let capabilitiesCString { free(capabilitiesCString) }
  }

  func invalidate() {
    guard alive else { return }
    alive = false
    callback = nil
    context = nil
    for task in translationTasks.values { task.cancel() }
    for task in speechTasks.values { task.cancel() }
    if #available(macOS 26.0, *) {
      for value in speechContinuations.values {
        (value as? AsyncStream<AnalyzerInput>.Continuation)?.finish()
      }
    }
    translationTasks.removeAll()
    speechTasks.removeAll()
    speechContinuations.removeAll()
    speechConverters.removeAll()
    speechFormats.removeAll()
    activeTokens.removeAll()
    preparationCompletion?("Language model preparation was cancelled")
    preparationCompletion = nil
    preparationRequest = nil
    preparationTask?.cancel()
    preparationTask = nil
    preparationPanel?.orderOut(nil)
    preparationPanel = nil
    preparationDelegate = nil
  }

  private func emit(_ request: Int64, _ event: [String: Any]) {
    guard alive, let callback else { return }
    let value = json(event)
    value.withCString { callback(context, request, $0) }
  }

  func capabilities() -> UnsafePointer<CChar> {
    if let capabilitiesCString { free(capabilitiesCString) }
    capabilitiesCString = strdup(lastCapabilities)
    return UnsafePointer(capabilitiesCString!)
  }

  private func refreshCapabilities() {
    guard #available(macOS 15.0, *) else { return }
    Task { @MainActor [weak self] in
      guard let self else { return }
      var seen = Set<String>()
      let languages = await LanguageAvailability().supportedLanguages.map(localeName)
        .filter { seen.insert($0["id"] ?? "").inserted }
        .sorted { ($0["name"] ?? "") < ($1["name"] ?? "") }
      var speechLanguages: [[String: String]] = []
      var available = false
      var error = "Text and speech translation requires macOS 26"
      if #available(macOS 26.0, *) {
        speechLanguages = await SpeechTranscriber.supportedLocales.map(localeName)
        available = SpeechTranscriber.isAvailable
        error = available ? "" : "Speech transcription is unavailable"
      }
      let event: [String: Any] = ["kind": "capabilities", "available": available,
                                   "languages": languages, "speechLanguages": speechLanguages,
                                   "error": error]
      self.lastCapabilities = json(event)
      self.emit(0, event)
    }
  }

  private func beginToken(_ request: Int64) -> UInt64 {
    nextToken &+= 1
    activeTokens[request] = nextToken
    return nextToken
  }

  private func isCurrent(_ request: Int64, _ token: UInt64) -> Bool {
    activeTokens[request] == token
  }

  private func clear(_ request: Int64, _ token: UInt64) {
    if activeTokens[request] == token { activeTokens.removeValue(forKey: request) }
  }

  private func showTranslationPreparation(_ request: Int64, source: String, target: String, completion: @escaping (String?) -> Void) {
    guard #available(macOS 15.0, *) else {
      completion("Translation model preparation requires macOS 15")
      return
    }
    preparationPanel?.orderOut(nil)
    preparationRequest = request
    preparationCompletion = completion
    let configuration = TranslationSession.Configuration(
      source: Locale.Language(identifier: source), target: Locale.Language(identifier: target))
    let view = PreparationView(configuration: configuration) { [weak self] error in
      guard let self, self.preparationRequest == request else { return }
      let finish = self.preparationCompletion
      self.preparationCompletion = nil
      self.preparationRequest = nil
      self.preparationPanel?.orderOut(nil)
      self.preparationPanel = nil
      self.preparationDelegate = nil
      finish?(error)
    }
    let controller = NSHostingController(rootView: view)
    let panel = NSPanel(contentViewController: controller)
    panel.styleMask = [.titled, .closable]
    panel.title = Locale.current.localizedString(forIdentifier: target) ?? target
    let delegate = PreparationWindowDelegate { [weak self] in
      guard let self, let finish = self.preparationCompletion else { return }
      self.preparationCompletion = nil
      self.preparationRequest = nil
      self.preparationPanel = nil
      self.preparationDelegate = nil
      finish("Language model preparation was cancelled")
    }
    preparationDelegate = delegate
    panel.delegate = delegate
    panel.center()
    preparationPanel = panel
    panel.makeKeyAndOrderFront(nil)
  }

  func prepare(_ request: Int64, source: String, target: String, speech: Bool) {
    guard !source.isEmpty, !target.isEmpty else {
      emit(request, ["kind": "error", "error": "Model preparation requires explicit language identifiers"])
      return
    }
    guard #available(macOS 26.0, *) else {
      emit(request, ["kind": "error", "error": "Language model preparation requires macOS 26"])
      return
    }
    if preparationTask != nil || preparationCompletion != nil {
      emit(request, ["kind": "error", "error": "Another language model preparation is already running"])
      return
    }
    preparationRequest = request
    preparationTask = Task { @MainActor [weak self] in
      guard let self else { return }
      do {
        if speech {
          guard let locale = await SpeechTranscriber.supportedLocale(equivalentTo: Locale(identifier: source)) else {
            throw NSError(domain: "SquadLanguage", code: 3, userInfo: [NSLocalizedDescriptionKey: "Speech locale is unavailable"])
          }
          let transcriber = SpeechTranscriber(locale: locale, preset: .progressiveTranscription)
          if let installRequest = try await AssetInventory.assetInstallationRequest(supporting: [transcriber]) {
            try await installRequest.downloadAndInstall()
          }
        }
        try Task.checkCancellation()
        guard self.alive, self.preparationRequest == request else { return }
        if Locale.Language(identifier: source).languageCode == Locale.Language(identifier: target).languageCode {
          self.preparationTask = nil
          self.preparationRequest = nil
          self.emit(request, ["kind": "ready", "text": "", "final": true, "error": ""])
          return
        }
        self.showTranslationPreparation(request, source: source, target: target) { [weak self] error in
          guard let self else { return }
          self.preparationTask = nil
          if let error { self.emit(request, ["kind": "error", "error": error]) }
          else { self.emit(request, ["kind": "ready", "text": "", "final": true, "error": ""]) }
        }
      } catch {
        guard self.alive, self.preparationRequest == request else { return }
        self.preparationRequest = nil
        self.preparationTask = nil
        self.emit(request, ["kind": "error", "error": error.localizedDescription])
      }
    }
  }

  func translate(_ request: Int64, source: String, target: String, text: String) {
    guard #available(macOS 26.0, *) else {
      emit(request, ["kind": "error", "error": "Text translation requires macOS 26 in this experimental bridge"])
      return
    }
    guard !target.isEmpty else {
      emit(request, ["kind": "error", "error": "Text translation requires an explicit target language"])
      return
    }
    if translationTasks.count >= 4 && translationTasks[request] == nil {
      emit(request, ["kind": "error", "error": "Too many translation requests"])
      return
    }
    cancel(request)
    let token = beginToken(request)
    translationTasks[request] = Task { @MainActor [weak self] in
      var detected = source
      do {
        if detected.isEmpty {
          guard let language = NLLanguageRecognizer.dominantLanguage(for: text), language.rawValue != "und" else {
            throw NSError(domain: "SquadLanguage", code: 5, userInfo: [NSLocalizedDescriptionKey: "The source language could not be detected"])
          }
          detected = languageID(language.rawValue)
        }
        let sourceLanguage = Locale.Language(identifier: detected)
        let targetLanguage = Locale.Language(identifier: target)
        let output: String
        if sourceLanguage.languageCode == targetLanguage.languageCode {
          output = text
        } else {
          let session = TranslationSession(installedSource: sourceLanguage, target: targetLanguage)
          output = try await session.translate(text).targetText
        }
        guard let self, self.isCurrent(request, token) else { return }
        self.translationTasks.removeValue(forKey: request)
        self.clear(request, token)
        self.emit(request, ["kind": "text", "text": output, "source": detected, "final": true, "error": ""])
      } catch {
        guard let self, self.isCurrent(request, token) else { return }
        self.translationTasks.removeValue(forKey: request)
        self.clear(request, token)
        self.emit(request, ["kind": "error", "source": detected, "error": error.localizedDescription])
      }
    }
  }

  func listen(_ request: Int64, source: String) {
    cancel(request)
    guard #available(macOS 26.0, *) else {
      emit(request, ["kind": "error", "error": "Live speech translation requires macOS 26"])
      return
    }
    guard SpeechTranscriber.isAvailable else {
      emit(request, ["kind": "error", "error": "Speech transcription is unavailable"])
      return
    }
    guard speechTasks.count < 4 else {
      emit(request, ["kind": "error", "error": "Too many speech sessions"])
      return
    }
    let (stream, continuation) = AsyncStream.makeStream(of: AnalyzerInput.self, bufferingPolicy: .bufferingNewest(50))
    speechContinuations[request] = continuation
    let token = beginToken(request)
    speechTasks[request] = Task { @MainActor [weak self] in
      do {
        guard let locale = await SpeechTranscriber.supportedLocale(equivalentTo: Locale(identifier: source)) else {
          throw NSError(domain: "SquadLanguage", code: 1, userInfo: [NSLocalizedDescriptionKey: "Speech locale is unavailable"])
        }
        let transcriber = SpeechTranscriber(locale: locale, preset: .progressiveTranscription)
        guard await AssetInventory.status(forModules: [transcriber]) == .installed else {
          throw NSError(domain: "SquadLanguage", code: 6, userInfo: [NSLocalizedDescriptionKey: "Prepare local speech models first"])
        }
        let analyzer = SpeechAnalyzer(modules: [transcriber])
        guard let format = await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [transcriber]),
              let inputFormat = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 1),
              let converter = AVAudioConverter(from: inputFormat, to: format) else {
          throw NSError(domain: "SquadLanguage", code: 2, userInfo: [NSLocalizedDescriptionKey: "Speech audio format is unavailable"])
        }
        guard let self, self.isCurrent(request, token) else { return }
        self.speechConverters[request] = converter
        self.speechFormats[request] = format
        try await analyzer.prepareToAnalyze(in: format)
        guard self.isCurrent(request, token) else { return }
        self.emit(request, ["kind": "ready", "text": "", "final": true, "error": ""])
        try await withTaskCancellationHandler(operation: {
          try await analyzer.start(inputSequence: stream)
          for try await result in transcriber.results {
            guard self.isCurrent(request, token) else { break }
            self.emit(request, ["kind": "text", "text": String(result.text.characters),
                                "final": result.isFinal, "error": ""])
          }
          await analyzer.cancelAndFinishNow()
        }, onCancel: {
          Task { await analyzer.cancelAndFinishNow() }
        })
        guard self.isCurrent(request, token) else { return }
        self.speechContinuations.removeValue(forKey: request)
        self.speechTasks.removeValue(forKey: request)
        self.speechConverters.removeValue(forKey: request)
        self.speechFormats.removeValue(forKey: request)
        self.clear(request, token)
      } catch {
        guard let self, self.isCurrent(request, token) else { return }
        self.speechContinuations.removeValue(forKey: request)
        self.speechTasks.removeValue(forKey: request)
        self.speechConverters.removeValue(forKey: request)
        self.speechFormats.removeValue(forKey: request)
        self.clear(request, token)
        self.emit(request, ["kind": "error", "error": error.localizedDescription])
      }
    }
  }

  func audio(_ request: Int64, samples: UnsafePointer<Float>?, count: Int) {
    guard #available(macOS 26.0, *), count > 0, count <= 48_000, let samples,
          let continuation = speechContinuations[request] as? AsyncStream<AnalyzerInput>.Continuation else { return }
    guard let format = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 1),
          let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(count)),
          let channel = buffer.floatChannelData?[0] else { return }
    buffer.frameLength = AVAudioFrameCount(count)
    channel.update(from: samples, count: count)
    if let converter = speechConverters[request], let format = speechFormats[request],
       let converted = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(Double(count) * format.sampleRate / 48_000 + 2)) {
      var conversionError: NSError?
      var supplied = false
      converter.convert(to: converted, error: &conversionError) { _, status in
        guard !supplied else { status.pointee = .noDataNow; return nil }
        supplied = true
        status.pointee = .haveData
        return buffer
      }
      if let conversionError {
        cancel(request)
        emit(request, ["kind": "error", "error": conversionError.localizedDescription])
        return
      }
      if converted.frameLength > 0, case .dropped = continuation.yield(AnalyzerInput(buffer: converted)) {
        cancel(request)
        emit(request, ["kind": "error", "error": "Speech audio input buffer overflow"])
      }
    } else if case .dropped = continuation.yield(AnalyzerInput(buffer: buffer)) {
      cancel(request)
      emit(request, ["kind": "error", "error": "Speech audio input buffer overflow"])
    }
  }

  func cancel(_ request: Int64) {
    activeTokens.removeValue(forKey: request)
    translationTasks.removeValue(forKey: request)?.cancel()
    if #available(macOS 26.0, *) {
      (speechContinuations.removeValue(forKey: request) as? AsyncStream<AnalyzerInput>.Continuation)?.finish()
    }
    speechTasks.removeValue(forKey: request)?.cancel()
    speechConverters.removeValue(forKey: request)
    speechFormats.removeValue(forKey: request)
    if preparationRequest == request {
      preparationTask?.cancel()
      preparationTask = nil
      preparationCompletion?("Language model preparation was cancelled")
      preparationCompletion = nil
      preparationRequest = nil
      preparationPanel?.orderOut(nil)
      preparationPanel = nil
      preparationDelegate = nil
    }
  }
}

@_cdecl("squad_language_create")
public func squad_language_create(_ callback: LanguageCallback?, _ context: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer {
  Unmanaged.passRetained(LanguageBridge(callback: callback, context: context)).toOpaque()
}

@_cdecl("squad_language_destroy")
public func squad_language_destroy(_ handle: UnsafeMutableRawPointer?) {
  guard let handle else { return }
  let bridge = Unmanaged<LanguageBridge>.fromOpaque(handle).takeUnretainedValue()
  bridge.invalidate()
  Unmanaged<LanguageBridge>.fromOpaque(handle).release()
}

@_cdecl("squad_language_capabilities")
public func squad_language_capabilities(_ handle: UnsafeMutableRawPointer?) -> UnsafePointer<CChar>? {
  guard let handle else { return nil }
  return Unmanaged<LanguageBridge>.fromOpaque(handle).takeUnretainedValue().capabilities()
}

@_cdecl("squad_language_prepare")
public func squad_language_prepare(_ handle: UnsafeMutableRawPointer?, _ request: Int64, _ source: UnsafePointer<CChar>?, _ target: UnsafePointer<CChar>?, _ speech: Int32) {
  guard let handle else { return }
  let bridge = Unmanaged<LanguageBridge>.fromOpaque(handle).takeUnretainedValue()
  bridge.prepare(request, source: source.map(String.init) ?? "", target: target.map(String.init) ?? "", speech: speech != 0)
}

@_cdecl("squad_language_translate")
public func squad_language_translate(_ handle: UnsafeMutableRawPointer?, _ request: Int64, _ source: UnsafePointer<CChar>?, _ target: UnsafePointer<CChar>?, _ text: UnsafePointer<CChar>?) {
  guard let handle else { return }
  let bridge = Unmanaged<LanguageBridge>.fromOpaque(handle).takeUnretainedValue()
  bridge.translate(request, source: source.map(String.init) ?? "", target: target.map(String.init) ?? "", text: text.map(String.init) ?? "")
}

@_cdecl("squad_language_listen")
public func squad_language_listen(_ handle: UnsafeMutableRawPointer?, _ request: Int64, _ source: UnsafePointer<CChar>?) {
  guard let handle else { return }
  Unmanaged<LanguageBridge>.fromOpaque(handle).takeUnretainedValue().listen(request, source: source.map(String.init) ?? "")
}

@_cdecl("squad_language_audio")
public func squad_language_audio(_ handle: UnsafeMutableRawPointer?, _ request: Int64, _ samples: UnsafePointer<Float>?, _ count: Int32) {
  guard let handle else { return }
  Unmanaged<LanguageBridge>.fromOpaque(handle).takeUnretainedValue().audio(request, samples: samples, count: Int(count))
}

@_cdecl("squad_language_cancel")
public func squad_language_cancel(_ handle: UnsafeMutableRawPointer?, _ request: Int64) {
  guard let handle else { return }
  Unmanaged<LanguageBridge>.fromOpaque(handle).takeUnretainedValue().cancel(request)
}
