// WindowSmash Mods: the mod manager for the macOS build - GameBanana's Smash Ultimate mods found,
// installed, turned on and off. tools/mods.py does the work (installing, and making the game read
// the mods: see its header); this app shows it, and talks to GameBanana's API for browsing.
// Built by tools/mac/build_mods_app.sh into build-mac/WindowSmash Mods.app.
import AppKit
import SwiftUI

// ---- the port's folder and its Python ------------------------------------------------------------
enum Port {
    static let root: String = {
        if let r = Bundle.main.object(forInfoDictionaryKey: "WindowSmashRoot") as? String, !r.isEmpty { return r }
        var url = Bundle.main.bundleURL
        for _ in 0..<4 {
            url.deleteLastPathComponent()
            if FileManager.default.fileExists(atPath: url.appendingPathComponent("tools/mods.py").path) { return url.path }
        }
        return NSHomeDirectory() + "/WindowSmash"
    }()
    static var python: String { root + "/deps/venv/bin/python" }
    static var script: String { root + "/tools/mods.py" }
    static var modsFolder: String { root + "/mods" }
}

// ---- tools/mods.py's view of the mods -------------------------------------------------------------
struct InstalledMod: Codable, Identifiable, Equatable {
    var id: String { name }
    let name: String
    var enabled: Bool
    let files: Int
    let added: Int?      // new files the game doesn't have, added to it (tools/mods.py)
    let merged: Int?     // patch files (.xmsbt, .prcx, .prcxml) merged into the game's files
    let skipped: Int
    let gamebanana: Int?
    let title: String?
    let image: String?
    let author: String?
    let category: String?
}

struct Listing: Codable {
    let mods: [InstalledMod]
    let applied: [String]?
    let modded: Bool
}

// ---- GameBanana -----------------------------------------------------------------------------------
struct GBMod: Identifiable, Hashable {
    let id: Int
    let name: String
    let image: URL?
    let author: String
    let category: String
    let likes: Int
    let views: Int
}

struct GBFile: Identifiable, Hashable {
    let id: Int
    let name: String
    let size: Int
    let downloads: Int
    let scan: String
}

struct GBCategory: Identifiable, Hashable {
    let id: Int
    let name: String
    let count: Int
}

enum Sort: String, CaseIterable, Identifiable {
    case liked = "Most liked", downloaded = "Most downloaded", newest = "Newest", updated = "Recently updated"
    var id: String { rawValue }
    var api: String {
        switch self {
        case .liked: return "Generic_MostLiked"
        case .downloaded: return "Generic_MostDownloaded"
        case .newest: return "Generic_Newest"
        case .updated: return "Generic_LatestUpdated"
        }
    }
}

enum GameBanana {
    static let game = 6498   // Super Smash Bros. Ultimate
    static let skins = 3330  // its Skins category: one subcategory a fighter

    static func get(_ path: String) async throws -> Any {
        var req = URLRequest(url: URL(string: "https://gamebanana.com/apiv11" + path)!)
        req.setValue("WindowSmash mod manager", forHTTPHeaderField: "User-Agent")
        let (data, _) = try await URLSession.shared.data(for: req)
        return try JSONSerialization.jsonObject(with: data)
    }

    static func mods(from records: [[String: Any]]) -> [GBMod] {
        records.compactMap { r in
            guard (r["_sModelName"] as? String) == "Mod", let id = r["_idRow"] as? Int else { return nil }
            var image: URL?
            if let media = r["_aPreviewMedia"] as? [String: Any], let images = media["_aImages"] as? [[String: Any]], let first = images.first,
               let base = first["_sBaseUrl"] as? String, let file = (first["_sFile530"] ?? first["_sFile"]) as? String {
                image = URL(string: base + "/" + file)
            }
            return GBMod(id: id, name: (r["_sName"] as? String) ?? "?", image: image,
                         author: ((r["_aSubmitter"] as? [String: Any])?["_sName"] as? String) ?? "",
                         category: ((r["_aRootCategory"] as? [String: Any])?["_sName"] as? String) ?? "",
                         likes: (r["_nLikeCount"] as? Int) ?? 0, views: (r["_nViewCount"] as? Int) ?? 0)
        }
    }

    /// A page of mods: a search, or a category's (or the whole game's) in an order
    static func page(search: String, category: Int?, sort: Sort, page: Int) async throws -> (mods: [GBMod], more: Bool) {
        let path: String
        if !search.isEmpty {
            let q = search.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? ""
            path = "/Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=\(game)&_sSearchString=\(q)&_nPage=\(page)"
        } else if let category {
            path = "/Mod/Index?_nPage=\(page)&_nPerpage=24&_aFilters%5BGeneric_Category%5D=\(category)&_sSort=\(sort.api)"
        } else {
            path = "/Mod/Index?_nPage=\(page)&_nPerpage=24&_aFilters%5BGeneric_Game%5D=\(game)&_sSort=\(sort.api)"
        }
        let json = try await get(path) as? [String: Any] ?? [:]
        let records = json["_aRecords"] as? [[String: Any]] ?? []
        let complete = ((json["_aMetadata"] as? [String: Any])?["_bIsComplete"] as? Bool) ?? true
        return (mods(from: records), !complete && !records.isEmpty)
    }

    static func categories(parent: Int?) async throws -> [GBCategory] {
        let path = parent.map { "/Mod/Categories?_idCategoryRow=\($0)&_sSort=a_to_z&_bShowEmpty=false" }
            ?? "/Mod/Categories?_idGameRow=\(game)&_sSort=a_to_z&_bShowEmpty=false"
        let list = try await get(path) as? [[String: Any]] ?? []
        return list.compactMap { c in
            guard let id = c["_idRow"] as? Int, let name = c["_sName"] as? String else { return nil }
            return GBCategory(id: id, name: name, count: (c["_nItemCount"] as? Int) ?? 0)
        }
    }

    static func details(_ id: Int) async throws -> (text: String, files: [GBFile], images: [URL], downloads: Int) {
        let json = try await get("/Mod/\(id)?_csvProperties=_sText,_aFiles,_aPreviewMedia,_nDownloadCount") as? [String: Any] ?? [:]
        let files = (json["_aFiles"] as? [[String: Any]] ?? []).compactMap { f -> GBFile? in
            guard let fid = f["_idRow"] as? Int else { return nil }
            return GBFile(id: fid, name: (f["_sFile"] as? String) ?? "?", size: (f["_nFilesize"] as? Int) ?? 0,
                          downloads: (f["_nDownloadCount"] as? Int) ?? 0, scan: (f["_sAvResult"] as? String) ?? "")
        }
        var images: [URL] = []
        if let media = json["_aPreviewMedia"] as? [String: Any], let list = media["_aImages"] as? [[String: Any]] {
            for i in list {
                if let base = i["_sBaseUrl"] as? String, let file = i["_sFile"] as? String, let u = URL(string: base + "/" + file) { images.append(u) }
            }
        }
        return (plain(json["_sText"] as? String ?? ""), files, images, (json["_nDownloadCount"] as? Int) ?? 0)
    }

    /// GameBanana's description HTML, as text
    static func plain(_ html: String) -> String {
        var s = html.replacingOccurrences(of: "<br>", with: "\n").replacingOccurrences(of: "<br/>", with: "\n")
            .replacingOccurrences(of: "</p>", with: "\n").replacingOccurrences(of: "</li>", with: "\n")
        s = s.replacingOccurrences(of: "<[^>]+>", with: "", options: .regularExpression)
        for (k, v) in [("&amp;", "&"), ("&lt;", "<"), ("&gt;", ">"), ("&quot;", "\""), ("&#39;", "'"), ("&nbsp;", " ")] {
            s = s.replacingOccurrences(of: k, with: v)
        }
        return s.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

// ---- the app's state ------------------------------------------------------------------------------
@MainActor
final class Model: ObservableObject {
    @Published var installed: [InstalledMod] = []
    @Published var applied: [String]? = nil
    @Published var status = ""
    @Published var error: String? = nil
    @Published var busy = false
    @Published var progress: Double? = nil
    @Published var installedIDs: Set<Int> = []

    var gameRunning: Bool {
        NSWorkspace.shared.runningApplications.contains { $0.bundleIdentifier == "io.github.sno2207.windowsmash" }
    }

    /// tools/mods.py with --json: each line it prints (log, progress, error, result) as it goes
    func run(_ args: [String], line: ((([String: Any]) -> Void))? = nil) async -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Port.python)
        p.arguments = [Port.script] + args + ["--json", "--tree", Port.root]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = Pipe()
        do { try p.run() } catch {
            self.error = "Can't run WindowSmash's Python (\(Port.python)): run Install WindowSmash.command first."
            return false
        }
        var failed = false
        let handle = out.fileHandleForReading
        var buffer = Data()
        while true {
            let chunk: Data = await Task.detached { handle.availableData }.value
            if chunk.isEmpty { break }
            buffer.append(chunk)
            while let nl = buffer.firstIndex(of: 10) {
                let lineData = buffer[buffer.startIndex..<nl]
                buffer.removeSubrange(buffer.startIndex...nl)
                guard let obj = try? JSONSerialization.jsonObject(with: lineData) as? [String: Any] else { continue }
                if let e = obj["error"] as? String { self.error = e; failed = true }
                if let l = obj["log"] as? String { self.status = l }
                if let pr = obj["progress"] as? Double { self.progress = pr }
                line?(obj)
            }
        }
        p.waitUntilExit()
        return !failed && p.terminationStatus == 0
    }

    func refresh() async {
        var listing: Listing?
        _ = await run(["list"]) { obj in
            if let d = try? JSONSerialization.data(withJSONObject: obj), let l = try? JSONDecoder().decode(Listing.self, from: d) { listing = l }
        }
        if let listing {
            installed = listing.mods
            applied = listing.modded ? (listing.applied ?? []) : nil
            installedIDs = Set(listing.mods.compactMap { $0.gamebanana })
        }
    }

    /// Make the game read what's turned on now (tools/mods.py apply)
    func apply() async {
        busy = true
        status = "Updating the game's files…"
        _ = await run(["apply"])
        await refresh()
        busy = false
        let on = installed.filter(\.enabled).count
        status = on == 0 ? "No mod is on: the game plays as it is." : "\(on) mod\(on == 1 ? "" : "s") on."
        if gameRunning { status += " The game is open: restart it to see the change." }
    }

    func set(_ mod: InstalledMod, enabled: Bool) async {
        _ = await run([enabled ? "enable" : "disable", mod.name])
        await apply()
    }

    func remove(_ mod: InstalledMod) async {
        _ = await run(["remove", mod.name])
        await apply()
    }

    func move(from: IndexSet, to: Int) async {
        var names = installed.map(\.name)
        names.move(fromOffsets: from, toOffset: to)
        installed.move(fromOffsets: from, toOffset: to)
        _ = await run(["order"] + names)
        await apply()
    }

    func install(file: URL) async {
        busy = true
        status = "Installing \(file.lastPathComponent)…"
        let ok = await run(["install", file.path])
        busy = false
        if ok { await apply() }
    }

    func install(mod: GBMod, file: GBFile) async -> Bool {
        busy = true
        progress = 0
        status = "Downloading \(file.name)…"
        let ok = await run(["gamebanana", String(mod.id), "--file", String(file.id)])
        progress = nil
        busy = false
        if ok { await apply() }
        return ok
    }
}

// ---- the window -----------------------------------------------------------------------------------
enum Pane: Hashable {
    case installed
    case browse(Int?, String)   // a category (nil: every mod) and its name
}

struct ContentView: View {
    @EnvironmentObject var model: Model
    @State private var section: Pane? = .installed
    @State private var top: [GBCategory] = []
    @State private var fighters: [GBCategory] = []

    var body: some View {
        NavigationSplitView {
            List(selection: $section) {
                Label("Installed", systemImage: "shippingbox").badge(model.installed.count).tag(Pane.installed)
                Section("GameBanana") {
                    Label("All mods", systemImage: "globe").tag(Pane.browse(nil, "All mods"))
                    ForEach(top.filter { $0.id != GameBanana.skins }) { c in
                        Label(c.name, systemImage: icon(c.name)).tag(Pane.browse(c.id, c.name))
                    }
                }
                Section("Skins by fighter") {
                    Label("All skins", systemImage: "tshirt").tag(Pane.browse(GameBanana.skins, "Skins"))
                    ForEach(fighters) { c in Text(c.name).tag(Pane.browse(c.id, c.name)) }
                }
            }
            .navigationSplitViewColumnWidth(min: 200, ideal: 230)
        } detail: {
            switch section {
            case .browse(let id, let name): BrowseView(category: id, title: name).id(id ?? 0)
            default: InstalledView()
            }
        }
        .task {
            await model.refresh()
            top = (try? await GameBanana.categories(parent: nil)) ?? []
            fighters = (try? await GameBanana.categories(parent: GameBanana.skins)) ?? []
        }
        .safeAreaInset(edge: .bottom) { StatusBar() }
        .alert("Something went wrong", isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
            Button("OK") { model.error = nil }
        } message: { Text(model.error ?? "") }
    }

    func icon(_ name: String) -> String {
        switch name {
        case "Music Packs": return "music.note"
        case "Effects": return "sparkles"
        case "Animation": return "figure.run"
        case "Gameplay": return "gamecontroller"
        case "Modpacks": return "square.stack.3d.up"
        case "Skyline": return "exclamationmark.triangle"
        default: return "folder"
        }
    }
}

struct StatusBar: View {
    @EnvironmentObject var model: Model
    var body: some View {
        HStack(spacing: 10) {
            if model.busy { ProgressView().controlSize(.small) }
            if let p = model.progress { ProgressView(value: p).frame(width: 140) }
            Text(model.status.isEmpty ? summary : model.status).font(.callout).foregroundStyle(.secondary).lineLimit(1)
            Spacer()
        }
        .padding(.horizontal, 14).padding(.vertical, 8)
        .background(.bar)
    }
    var summary: String {
        guard let applied = model.applied, !applied.isEmpty else { return "No mod is on: the game plays as it is." }
        return "In the game: " + applied.joined(separator: ", ")
    }
}

// ---- installed mods ---------------------------------------------------------------------------------
struct InstalledView: View {
    @EnvironmentObject var model: Model
    @State private var removing: InstalledMod?

    var body: some View {
        Group {
            if model.installed.isEmpty {
                ContentUnavailableView("No mods yet", systemImage: "shippingbox",
                                       description: Text("Find some on GameBanana (on the left), or install a mod's .zip, .7z or .rar with Install from File."))
            } else {
                List {
                    ForEach(model.installed) { mod in
                        InstalledRow(mod: mod, removing: $removing)
                    }
                    .onMove { from, to in Task { await model.move(from: from, to: to) } }
                }
            }
        }
        .navigationTitle("Installed")
        .navigationSubtitle("A mod lower in the list wins where two change the same file")
        .toolbar {
            ToolbarItemGroup {
                Button { openPanel() } label: { Label("Install from File…", systemImage: "plus") }
                Button { NSWorkspace.shared.open(URL(fileURLWithPath: Port.modsFolder)) } label: { Label("Show Mods Folder", systemImage: "folder") }
                Button { Task { await model.apply() } } label: { Label("Apply Again", systemImage: "arrow.clockwise") }.disabled(model.busy)
            }
        }
        .confirmationDialog("Remove \(removing?.title ?? removing?.name ?? "")?", isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } })) {
            Button("Remove", role: .destructive) { if let m = removing { Task { await model.remove(m) } }; removing = nil }
        } message: { Text("Its files are deleted from the Mods folder.") }
    }

    func openPanel() {
        let panel = NSOpenPanel()
        panel.title = "Install a mod"
        panel.allowedContentTypes = []
        panel.allowsOtherFileTypes = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = true
        guard panel.runModal() == .OK else { return }
        Task { for url in panel.urls { await model.install(file: url) } }
    }
}

struct InstalledRow: View {
    @EnvironmentObject var model: Model
    let mod: InstalledMod
    @Binding var removing: InstalledMod?

    var body: some View {
        HStack(spacing: 12) {
            Thumbnail(url: mod.image.flatMap(URL.init(string:))).frame(width: 96, height: 54)
            VStack(alignment: .leading, spacing: 3) {
                Text(mod.title?.isEmpty == false ? mod.title! : mod.name).font(.headline)
                Text([mod.category, mod.author.map { "by \($0)" }, "\(mod.files) file\(mod.files == 1 ? "" : "s")"]
                        .compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption).foregroundStyle(.secondary)
                if mod.enabled && (mod.added ?? 0) + (mod.merged ?? 0) > 0 {
                    Label([(mod.added ?? 0) > 0 ? "\(mod.added!) new file\(mod.added! == 1 ? "" : "s") added to the game" : nil,
                           (mod.merged ?? 0) > 0 ? "\(mod.merged!) patch\(mod.merged! == 1 ? "" : "es") merged (names, params)" : nil]
                            .compactMap { $0 }.joined(separator: ", "),
                          systemImage: "plus.circle.fill")
                        .font(.caption).foregroundStyle(.green)
                }
                if mod.enabled && mod.skipped > 0 {
                    Label("\(mod.skipped) file\(mod.skipped == 1 ? "" : "s") not loaded: a new costume slot (c08 and up) or a file costume slots share - not supported yet, so it may look wrong",
                          systemImage: "exclamationmark.triangle.fill")
                        .font(.caption).foregroundStyle(.orange)
                }
            }
            Spacer()
            if let id = mod.gamebanana {
                Link(destination: URL(string: "https://gamebanana.com/mods/\(id)")!) { Image(systemName: "safari") }.help("Its GameBanana page")
            }
            Button { removing = mod } label: { Image(systemName: "trash") }.buttonStyle(.borderless).help("Remove")
            Toggle("", isOn: Binding(get: { mod.enabled }, set: { on in Task { await model.set(mod, enabled: on) } }))
                .toggleStyle(.switch).labelsHidden().disabled(model.busy)
        }
        .padding(.vertical, 4)
    }
}

struct Thumbnail: View {
    let url: URL?
    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 6).fill(.quaternary)
            if let url {
                AsyncImage(url: url) { img in img.resizable().scaledToFill() } placeholder: { ProgressView().controlSize(.small) }
            } else {
                Image(systemName: "shippingbox").foregroundStyle(.secondary)
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 6))
    }
}

// ---- GameBanana --------------------------------------------------------------------------------------
struct BrowseView: View {
    @EnvironmentObject var model: Model
    let category: Int?
    let title: String
    @State private var mods: [GBMod] = []
    @State private var page = 1
    @State private var more = true
    @State private var loading = false
    @State private var search = ""
    @State private var sort: Sort = .liked
    @State private var chosen: GBMod?
    @State private var failed: String?

    var body: some View {
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 230), spacing: 16)], spacing: 16) {
                ForEach(mods) { mod in
                    Card(mod: mod, installed: model.installedIDs.contains(mod.id)).onTapGesture { chosen = mod }
                        .onAppear { if mod == mods.last { Task { await load() } } }
                }
            }
            .padding(16)
            if loading { ProgressView().padding() }
            if let failed { Text(failed).foregroundStyle(.secondary).padding() }
        }
        .navigationTitle(title)
        .searchable(text: $search, prompt: "Search GameBanana")
        .onSubmit(of: .search) { Task { await reload() } }
        .onChange(of: search) { _, s in if s.isEmpty { Task { await reload() } } }
        .toolbar {
            Picker("Sort", selection: $sort) { ForEach(Sort.allCases) { Text($0.rawValue).tag($0) } }
                .pickerStyle(.menu).disabled(!search.isEmpty)
        }
        .onChange(of: sort) { _, _ in Task { await reload() } }
        .task { await reload() }
        .sheet(item: $chosen) { mod in ModSheet(mod: mod) }
    }

    func reload() async {
        mods = []
        page = 1
        more = true
        await load()
    }

    func load() async {
        guard more, !loading else { return }
        loading = true
        defer { loading = false }
        do {
            let (found, hasMore) = try await GameBanana.page(search: search, category: search.isEmpty ? category : nil, sort: sort, page: page)
            let seen = Set(mods.map(\.id))
            mods += found.filter { !seen.contains($0.id) }
            more = hasMore
            page += 1
            failed = mods.isEmpty ? "Nothing found." : nil
        } catch {
            failed = "Can't reach GameBanana (\(error.localizedDescription))."
            more = false
        }
    }
}

struct Card: View {
    let mod: GBMod
    let installed: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Thumbnail(url: mod.image).aspectRatio(16 / 9, contentMode: .fit)
                .overlay(alignment: .topTrailing) {
                    if installed {
                        Label("Installed", systemImage: "checkmark.circle.fill").font(.caption2.bold()).padding(5)
                            .background(.green, in: Capsule()).foregroundStyle(.white).padding(6)
                    }
                }
            Text(mod.name).font(.headline).lineLimit(2)
            HStack(spacing: 8) {
                Text(mod.author).lineLimit(1)
                Spacer()
                Label("\(mod.likes)", systemImage: "heart")
                Label(short(mod.views), systemImage: "eye")
            }
            .font(.caption).foregroundStyle(.secondary)
        }
        .padding(8)
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 10))
        .contentShape(Rectangle())
    }
    func short(_ n: Int) -> String { n >= 1000 ? String(format: "%.1fk", Double(n) / 1000) : "\(n)" }
}

struct ModSheet: View {
    @EnvironmentObject var model: Model
    @Environment(\.dismiss) var dismiss
    let mod: GBMod
    @State private var text = ""
    @State private var files: [GBFile] = []
    @State private var images: [URL] = []
    @State private var downloads = 0
    @State private var installing: Int?
    @State private var done: Set<Int> = []

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading) {
                    Text(mod.name).font(.title2.bold())
                    Text("by \(mod.author) · \(mod.category) · \(mod.likes) likes · \(downloads) downloads").foregroundStyle(.secondary)
                }
                Spacer()
                Link("Open on GameBanana", destination: URL(string: "https://gamebanana.com/mods/\(mod.id)")!)
            }
            ScrollView(.horizontal) {
                HStack { ForEach(images.isEmpty ? [mod.image].compactMap { $0 } : images, id: \.self) { u in Thumbnail(url: u).frame(width: 320, height: 180) } }
            }
            if mod.category == "Skyline" {
                Label("Skyline plugins are code made for a Switch: WindowSmash can't run them.", systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            }
            ScrollView { Text(text.isEmpty ? "…" : text).frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled) }
                .frame(maxHeight: 160)
            Text("Files").font(.headline)
            ForEach(files) { f in
                HStack {
                    Image(systemName: "doc.zipper")
                    VStack(alignment: .leading) {
                        Text(f.name)
                        Text("\(ByteCountFormatter.string(fromByteCount: Int64(f.size), countStyle: .file)) · \(f.downloads) downloads"
                             + (f.scan.isEmpty || f.scan == "clean" ? "" : " · virus scan: \(f.scan)"))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    if done.contains(f.id) {
                        Label("Installed and on", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    } else if installing == f.id {
                        if let p = model.progress { ProgressView(value: p).frame(width: 120) } else { ProgressView().controlSize(.small) }
                    } else {
                        Button("Install") {
                            installing = f.id
                            Task {
                                if await model.install(mod: mod, file: f) { done.insert(f.id) }
                                installing = nil
                            }
                        }
                        .buttonStyle(.borderedProminent).disabled(installing != nil || !(f.scan.isEmpty || f.scan == "clean"))
                    }
                }
            }
            HStack { Spacer(); Button("Done") { dismiss() }.keyboardShortcut(.defaultAction) }
        }
        .padding(20)
        .frame(width: 720)
        .frame(minHeight: 520)
        .task {
            if let d = try? await GameBanana.details(mod.id) {
                text = d.text; files = d.files; images = d.images; downloads = d.downloads
            }
        }
    }
}

// ---- the app ------------------------------------------------------------------------------------------
@main
struct ModsApp: App {
    @StateObject private var model = Model()
    var body: some Scene {
        WindowGroup("WindowSmash Mods") {
            ContentView().environmentObject(model).frame(minWidth: 960, minHeight: 620)
        }
        .windowToolbarStyle(.unified)
    }
}
