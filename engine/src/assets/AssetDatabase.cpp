// Project asset database (see Phisiks/Assets/AssetDatabase.h). Uses nlohmann/json (MIT, vendor/).
#include "Phisiks/Assets/AssetDatabase.h"
#include "Phisiks/Assets/Assets.h"
#include "Phisiks/Animation/Animation.h"
#include "Phisiks/Core/Log.h"
#include "Phisiks/Render/Renderer.h"
#include "Phisiks/Scene/Scene.h"
#include "Phisiks/Script/Script.h"
#include "Phisiks/World/Terrain.h"
#include "Phisiks/Physics/Advanced.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Phisiks {
    namespace {
        using json = nlohmann::json;
        constexpr int META_VERSION = 1;
        AssetDatabase* g_active = nullptr;

        std::string Lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); }); return s; }
        std::string Generic(const fs::path& p) { return p.generic_string(); }
        std::string Hex(uint64_t v) { char t[20]; std::snprintf(t, sizeof(t), "%016llx", static_cast<unsigned long long>(v)); return t; }
        bool ParseHex(const std::string& s, uint64_t& out) {
            if (s.empty() || s.size() > 16 || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
            out = std::stoull(s, nullptr, 16);
            return true;
        }
        uint64_t NewGuid() {
            static std::mt19937_64 rng([] { std::random_device d; return (uint64_t(d()) << 32) ^ uint64_t(d()) ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }());
            uint64_t g = 0;
            while (g == 0) g = rng();
            return g;
        }
        std::string NowUtc() {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return text;
        }
        uint64_t Stamp(const fs::path& p) {
            std::error_code ec;
            const auto t = fs::last_write_time(p, ec);
            return ec ? 0 : uint64_t(t.time_since_epoch().count());
        }
        bool HashFile(const fs::path& p, uint64_t& hash, uint64_t& size) {
            std::ifstream in(p, std::ios::binary);
            if (!in) return false;
            uint64_t h = 1469598103934665603ull;
            size = 0;
            char buffer[1 << 16];
            while (in) {
                in.read(buffer, sizeof(buffer));
                const std::streamsize n = in.gcount();
                for (std::streamsize i = 0; i < n; ++i) { h ^= uint8_t(buffer[i]); h *= 1099511628211ull; }
                size += uint64_t(n);
            }
            hash = h;
            return true;
        }
        bool WriteFileAtomic(const fs::path& p, const std::string& text) {
            std::error_code ec;
            const fs::path tmp = fs::path(p.string() + ".tmp");
            { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); if (!out) return false; out << text; out.flush(); if (!out) return false; }
            fs::rename(tmp, p, ec);
            if (ec) { fs::remove(tmp, ec); return false; }
            return true;
        }
        // Relative path inside the root, or "" when it would leave it.
        std::string SafeRelative(const std::string& rel) {
            const fs::path p = fs::path(rel).lexically_normal();
            if (p.empty() || p.is_absolute() || p.has_root_name()) return {};
            for (const fs::path& part : p) if (part == "..") return {};
            return Generic(p);
        }
    }

    const char* ToString(AssetType type) {
        switch (type) {
            case AssetType::Mesh: return "mesh";
            case AssetType::Model: return "model";
            case AssetType::Texture: return "texture";
            case AssetType::Audio: return "audio";
            case AssetType::Scene: return "scene";
            case AssetType::Script: return "script";
            case AssetType::Cad: return "cad";
            case AssetType::CadDocument: return "cad-document";
            case AssetType::Unknown: break;
        }
        return "unknown";
    }

    AssetType AssetTypeOf(const std::string& path) {
        const std::string p = Lower(path);
        auto ends = [&](const char* suffix) { const size_t n = std::char_traits<char>::length(suffix); return p.size() > n && p.compare(p.size() - n, n, suffix) == 0; };
        if (ends(".meta") || ends(".tmp")) return AssetType::Unknown;
        if (ends(".scene.json")) return AssetType::Scene;
        if (ends(".obj") || ends(".stl")) return AssetType::Mesh;
        if (ends(".gltf") || ends(".glb")) return AssetType::Model;
        if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".tga") || ends(".bmp")) return AssetType::Texture;
        if (ends(".wav")) return AssetType::Audio;
        if (ends(".lua")) return AssetType::Script;
        if (ends(".step") || ends(".stp") || ends(".iges") || ends(".igs")) return AssetType::Cad;
        if (ends(".phcad")) return AssetType::CadDocument;
        return AssetType::Unknown;
    }

    struct AssetDatabase::Impl {
        mutable std::mutex mutex;
        fs::path root;
        std::string rootText;
        bool open = false;
        bool active = false;
        std::unordered_map<uint64_t, AssetInfo> assets;
        std::unordered_map<uint64_t, uint64_t> stamps;   // guid -> file time at the last hash

        fs::path Abs(const std::string& rel) const { return root / fs::path(rel); }

        json MetaJson(const AssetInfo& a, uint64_t stamp) const {
            return { { "format", "phisiks-asset" }, { "version", META_VERSION }, { "guid", Hex(a.guid) }, { "type", ToString(a.type) },
                     { "hash", Hex(a.hash) }, { "size", a.size }, { "stamp", stamp }, { "source", a.source }, { "imported", a.imported }, { "revision", a.revision } };
        }
        bool WriteMeta(const AssetInfo& a, uint64_t stamp) const { return WriteFileAtomic(Abs(a.path + ".meta"), MetaJson(a, stamp).dump(2)); }

        // Parses "<file>.meta". False when it is missing or not a phisiks-asset file.
        static bool ReadMeta(const fs::path& metaPath, AssetInfo& a, uint64_t& stamp, std::string* problem) {
            std::ifstream in(metaPath, std::ios::binary);
            if (!in) return false;
            try {
                const json j = json::parse(in);
                if (!j.is_object() || j.value("format", std::string()) != "phisiks-asset") { if (problem) *problem = "not a phisiks-asset .meta"; return false; }
                if (j.value("version", 0) > META_VERSION) { if (problem) *problem = "newer .meta version"; return false; }
                if (!ParseHex(j.value("guid", std::string()), a.guid) || a.guid == 0) { if (problem) *problem = "invalid guid"; return false; }
                ParseHex(j.value("hash", std::string()), a.hash);
                a.size = j.value("size", uint64_t(0));
                stamp = j.value("stamp", uint64_t(0));
                a.source = j.value("source", std::string());
                a.imported = j.value("imported", std::string());
                a.revision = std::max(1, j.value("revision", 1));
                return true;
            }
            catch (const std::exception& e) { if (problem) *problem = std::string("broken .meta: ") + e.what(); return false; }
        }

        std::string UniqueName(const fs::path& folder, const std::string& name) const {
            std::error_code ec;
            if (!fs::exists(folder / name, ec)) return name;
            const fs::path p(name);
            std::string stem = p.stem().string(), ext = p.extension().string();
            if (Lower(name).size() > 11 && Lower(name).compare(name.size() - 11, 11, ".scene.json") == 0) { stem = name.substr(0, name.size() - 11); ext = ".scene.json"; }
            for (int i = 1;; ++i) {
                const std::string candidate = stem + "_" + std::to_string(i) + ext;
                if (!fs::exists(folder / candidate, ec)) return candidate;
            }
        }
    };

    AssetDatabase::AssetDatabase() : m(std::make_unique<Impl>()) {}
    AssetDatabase::~AssetDatabase() { Close(); }

    AssetDatabase* AssetDatabase::Active() { return g_active; }
    bool AssetDatabase::IsOpen() const { std::lock_guard<std::mutex> lock(m->mutex); return m->open; }
    const std::string& AssetDatabase::Root() const { return m->rootText; }

    bool AssetDatabase::Open(const std::string& root, std::string* error, bool activate) {
        Close();
        std::error_code ec;
        fs::path p = fs::absolute(fs::path(root), ec).lexically_normal();
        fs::create_directories(p, ec);
        if (!fs::is_directory(p, ec)) { if (error) *error = "cannot create or open the folder " + p.string(); return false; }
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            m->root = p;
            m->rootText = Generic(p);
            m->open = true;
            m->assets.clear();
            m->stamps.clear();
        }
        const AssetRefreshReport r = Refresh();
        Log::Info() << "Assets: project database " << m->rootText << " (" << r.assets << " assets, " << r.added << " new, " << r.missing << " missing, " << r.milliseconds << " ms)";
        if (activate) {
            if (g_active && g_active != this) g_active->Close();
            g_active = this;
            m->active = true;
            Assets::AddSearchPath(m->rootText);
            Assets::SetReferenceResolver([this](const std::string& reference) { return Resolve(reference); });
        }
        return true;
    }

    void AssetDatabase::Close() {
        if (!m) return;
        bool wasActive = false;
        std::string root;
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            if (!m->open) return;
            m->open = false;
            m->assets.clear();
            m->stamps.clear();
            wasActive = m->active;
            m->active = false;
            root = m->rootText;
        }
        if (wasActive) {
            Assets::SetReferenceResolver({});
            Assets::RemoveSearchPath(root);
            if (g_active == this) g_active = nullptr;
        }
    }

    AssetRefreshReport AssetDatabase::Refresh() {
        const auto t0 = std::chrono::steady_clock::now();
        AssetRefreshReport report;
        std::vector<std::string> reload;
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            if (!m->open) return report;
            std::error_code ec;
            std::vector<fs::path> files, metas;
            for (fs::recursive_directory_iterator it(m->root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::path& p = it->path();
                const std::string name = p.filename().string();
                if (!name.empty() && name[0] == '.') { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
                if (!it->is_regular_file(ec)) continue;
                if (Lower(p.extension().string()) == ".meta") metas.push_back(p);
                else if (AssetTypeOf(name) != AssetType::Unknown) files.push_back(p);
            }
            std::unordered_map<uint64_t, AssetInfo> previous = std::move(m->assets);
            std::unordered_map<uint64_t, uint64_t> previousStamps = std::move(m->stamps);
            m->assets.clear();
            m->stamps.clear();
            std::set<std::string> metaUsed;
            // files without a usable .meta, matched to vanished assets by hash after the first pass
            std::vector<std::pair<fs::path, AssetInfo>> orphans;
            for (const fs::path& file : files) {
                AssetInfo a;
                a.type = AssetTypeOf(file.filename().string());
                a.path = Generic(file.lexically_relative(m->root));
                a.name = file.filename().string();
                const fs::path metaPath = fs::path(file.string() + ".meta");
                uint64_t stamp = 0;
                std::string problem;
                const bool hasMeta = Impl::ReadMeta(metaPath, a, stamp, &problem);
                const uint64_t now = Stamp(file);
                if (hasMeta) {
                    metaUsed.insert(Generic(metaPath));
                    if (m->assets.count(a.guid)) {   // a copied file with a copied .meta: the copy gets a new identity
                        const auto known = previous.find(a.guid);
                        const bool thisIsOriginal = known != previous.end() && known->second.path == a.path;
                        if (thisIsOriginal) {        // the one found first is the copy: renumber it instead
                            AssetInfo copy = m->assets[a.guid];
                            copy.guid = NewGuid();
                            while (m->assets.count(copy.guid)) copy.guid = NewGuid();
                            m->WriteMeta(copy, m->stamps[a.guid]);
                            m->stamps[copy.guid] = m->stamps[a.guid];
                            m->assets.erase(a.guid);
                            m->assets[copy.guid] = copy;
                        }
                        else {
                            a.guid = NewGuid();
                            stamp = 0;
                        }
                        ++report.duplicatesRenumbered;
                    }
                    const uint64_t oldHash = a.hash;
                    uint64_t size = a.size;
                    if (stamp != now || stamp == 0) {
                        if (!HashFile(file, a.hash, size)) a.error = "unreadable";
                        a.size = size;
                        if (oldHash != 0 && a.hash != oldHash) { ++a.revision; ++report.changed; reload.push_back("asset:" + Hex(a.guid) + "/" + a.name); reload.push_back(a.path); }
                        m->WriteMeta(a, now);
                    }
                    if (auto it = previous.find(a.guid); it != previous.end() && !it->second.missing && it->second.path != a.path) ++report.moved;
                    else if (!previous.count(a.guid) && !previous.empty()) ++report.added;
                    m->assets[a.guid] = a;
                    m->stamps[a.guid] = now;
                }
                else {
                    if (!problem.empty()) Log::Warning() << "Assets: " << Generic(metaPath) << ": " << problem << " (a new .meta is written)";
                    uint64_t size = 0;
                    if (!HashFile(file, a.hash, size)) a.error = "unreadable";
                    a.size = size;
                    orphans.emplace_back(file, a);
                }
            }
            // .meta files whose file is gone: missing assets (unless a file without .meta has their content: moved)
            std::unordered_map<uint64_t, AssetInfo> vanished;
            for (const fs::path& metaPath : metas) {
                if (metaUsed.count(Generic(metaPath))) continue;
                AssetInfo a;
                uint64_t stamp = 0;
                if (!Impl::ReadMeta(metaPath, a, stamp, nullptr) || m->assets.count(a.guid)) continue;
                std::string target = metaPath.string();
                target.resize(target.size() - 5);   // strip ".meta"
                a.path = Generic(fs::path(target).lexically_relative(m->root));
                a.name = fs::path(target).filename().string();
                a.type = AssetTypeOf(a.name);
                a.missing = true;
                vanished[a.guid] = a;
                previousStamps.erase(a.guid);
                m->stamps[a.guid] = 0;
                // remember where the orphan .meta is so a hash match can remove it
                m->assets[a.guid] = a;
            }
            for (auto& [file, a] : orphans) {
                uint64_t adopted = 0;
                for (auto& [guid, v] : vanished) if (v.missing && v.hash == a.hash && v.type == a.type && a.hash != 0) { adopted = guid; break; }
                if (!adopted) for (auto& [guid, p] : previous)   // known in memory but its .meta went with it
                    if (!m->assets.count(guid) && p.hash == a.hash && p.type == a.type && a.hash != 0) { adopted = guid; break; }
                if (adopted) {
                    AssetInfo old = m->assets.count(adopted) ? m->assets[adopted] : previous[adopted];
                    if (old.missing) fs::remove(m->Abs(old.path + ".meta"), ec);   // the orphan .meta moves with the identity
                    a.guid = adopted;
                    a.source = old.source;
                    a.imported = old.imported;
                    a.revision = old.revision;
                    vanished.erase(adopted);
                    ++report.moved;
                }
                else {
                    a.guid = NewGuid();
                    while (m->assets.count(a.guid)) a.guid = NewGuid();
                    a.imported = NowUtc();
                    ++report.added;
                }
                a.missing = false;
                const uint64_t now = Stamp(file);
                m->WriteMeta(a, now);
                m->assets[a.guid] = a;
                m->stamps[a.guid] = now;
            }
            // assets known before whose file and .meta are both gone are forgotten; the rest are reported
            for (const auto& [guid, a] : m->assets) if (a.missing) ++report.missing;
            report.assets = m->assets.size();
        }
        for (const std::string& name : reload) Render::ReloadAsset(name);
        report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return report;
    }

    std::vector<AssetInfo> AssetDatabase::List(const std::string& folder, bool recursive) const {
        std::lock_guard<std::mutex> lock(m->mutex);
        std::vector<AssetInfo> out;
        const std::string prefix = folder.empty() ? std::string() : Generic(fs::path(folder).lexically_normal()) + "/";
        for (const auto& [guid, a] : m->assets) {
            if (!prefix.empty() && a.path.rfind(prefix, 0) != 0) continue;
            if (!recursive && a.path.find('/', prefix.size()) != std::string::npos) continue;
            out.push_back(a);
        }
        std::sort(out.begin(), out.end(), [](const AssetInfo& a, const AssetInfo& b) { return Lower(a.path) < Lower(b.path); });
        return out;
    }

    std::vector<std::string> AssetDatabase::Folders() const {
        std::lock_guard<std::mutex> lock(m->mutex);
        std::set<std::string> folders;
        std::error_code ec;
        if (m->open) for (fs::recursive_directory_iterator it(m->root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (!name.empty() && name[0] == '.') { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
            if (it->is_directory(ec)) folders.insert(Generic(it->path().lexically_relative(m->root)));
        }
        return { folders.begin(), folders.end() };
    }

    bool AssetDatabase::Find(uint64_t guid, AssetInfo& out) const {
        std::lock_guard<std::mutex> lock(m->mutex);
        auto it = m->assets.find(guid);
        if (it == m->assets.end()) return false;
        out = it->second;
        return true;
    }

    bool AssetDatabase::FindByPath(const std::string& relativePath, AssetInfo& out) const {
        const std::string want = Lower(Generic(fs::path(relativePath).lexically_normal()));
        std::lock_guard<std::mutex> lock(m->mutex);
        for (const auto& [guid, a] : m->assets) if (Lower(a.path) == want) { out = a; return true; }
        return false;
    }

    std::string AssetDatabase::AbsolutePath(uint64_t guid) const {
        std::lock_guard<std::mutex> lock(m->mutex);
        auto it = m->assets.find(guid);
        if (it == m->assets.end() || it->second.missing) return {};
        return Generic(m->Abs(it->second.path));
    }

    std::string AssetDatabase::Reference(uint64_t guid) const {
        std::lock_guard<std::mutex> lock(m->mutex);
        auto it = m->assets.find(guid);
        return it == m->assets.end() ? std::string() : "asset:" + Hex(guid) + "/" + it->second.name;
    }

    bool AssetDatabase::ParseReference(const std::string& reference, uint64_t& guid) {
        if (reference.rfind("asset:", 0) != 0) return false;
        const size_t slash = reference.find('/', 6);
        return ParseHex(reference.substr(6, slash == std::string::npos ? std::string::npos : slash - 6), guid) && guid != 0;
    }

    std::string AssetDatabase::Resolve(const std::string& reference) const {
        uint64_t guid = 0;
        if (!ParseReference(reference, guid)) return {};
        return AbsolutePath(guid);
    }

    AssetImportResult AssetDatabase::Import(const std::string& sourceFile, const std::string& folder) {
        AssetImportResult r;
        std::error_code ec;
        const fs::path source = fs::absolute(fs::path(sourceFile), ec).lexically_normal();
        const AssetType type = AssetTypeOf(source.filename().string());
        if (!IsOpen()) { r.error = "no project is open"; return r; }
        if (type == AssetType::Unknown) { r.error = "unsupported file type '" + source.extension().string() + "'"; return r; }
        if (!fs::is_regular_file(source, ec)) { r.error = "no such file: " + Generic(source); return r; }
        const std::string relFolder = folder.empty() ? std::string() : SafeRelative(folder);
        if (!folder.empty() && relFolder.empty()) { r.error = "the folder must be inside the project: " + folder; return r; }
        uint64_t hash = 0, size = 0;
        if (!HashFile(source, hash, size)) { r.error = "cannot read " + Generic(source); return r; }
        // the file is already part of the project (importing it in place) or its content already is
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            for (const auto& [guid, a] : m->assets) {
                if (a.missing) continue;
                if (fs::equivalent(m->Abs(a.path), source, ec) || (a.hash == hash && a.size == size && a.type == type)) {
                    r.ok = true; r.guid = guid; r.duplicate = true;
                    return r;
                }
            }
        }
        const fs::path destFolder = m->root / fs::path(relFolder);
        fs::create_directories(destFolder, ec);
        const std::string name = m->UniqueName(destFolder, source.filename().string());
        const fs::path dest = destFolder / name;
        if (!fs::copy_file(source, dest, fs::copy_options::none, ec)) { r.error = "copy failed: " + ec.message(); return r; }
        r.copied.push_back(Generic(dest.lexically_relative(m->root)));
        // glTF: external buffers and images next to the .gltf (relative URIs inside the source folder only)
        if (Lower(source.extension().string()) == ".gltf") {
            try {
                std::ifstream in(source, std::ios::binary);
                const json j = json::parse(in);
                for (const char* list : { "buffers", "images" }) {
                    if (!j.contains(list)) continue;
                    for (const json& item : j.at(list)) {
                        const std::string uri = item.value("uri", std::string());
                        if (uri.empty() || uri.rfind("data:", 0) == 0) continue;
                        const std::string rel = SafeRelative(uri);
                        if (rel.empty()) { Log::Warning() << "Assets: " << source.filename().string() << ": skipped URI outside its folder: " << uri; continue; }
                        const fs::path from = source.parent_path() / fs::path(rel), to = destFolder / fs::path(rel);
                        fs::create_directories(to.parent_path(), ec);
                        if (fs::is_regular_file(from, ec) && !fs::exists(to, ec) && fs::copy_file(from, to, ec)) r.copied.push_back(Generic(to.lexically_relative(m->root)));
                        else if (!fs::exists(to, ec)) Log::Warning() << "Assets: " << source.filename().string() << ": referenced file not found: " << uri;
                    }
                }
            }
            catch (const std::exception& e) { Log::Warning() << "Assets: " << source.filename().string() << ": could not read its URIs (" << e.what() << ")"; }
        }
        AssetInfo a;
        a.guid = NewGuid();
        a.type = type;
        a.path = Generic(dest.lexically_relative(m->root));
        a.name = name;
        a.size = size;
        a.hash = hash;
        a.source = Generic(source);
        a.imported = NowUtc();
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            while (m->assets.count(a.guid)) a.guid = NewGuid();
            const uint64_t now = Stamp(dest);
            m->WriteMeta(a, now);
            m->assets[a.guid] = a;
            m->stamps[a.guid] = now;
        }
        Refresh();   // images / buffers copied with a glTF become assets too
        Log::Info() << "Assets: imported " << Generic(source) << " as " << a.path << " (" << ToString(type) << ", " << Hex(a.guid) << ")";
        r.ok = true;
        r.guid = a.guid;
        return r;
    }

    AssetImportResult AssetDatabase::Reimport(uint64_t guid) {
        AssetImportResult r;
        AssetInfo a;
        if (!Find(guid, a)) { r.error = "unknown asset"; return r; }
        std::error_code ec;
        if (a.source.empty() || !fs::is_regular_file(a.source, ec)) { r.error = "the source file is not available: " + (a.source.empty() ? std::string("(created in place)") : a.source); return r; }
        uint64_t hash = 0, size = 0;
        if (!HashFile(a.source, hash, size)) { r.error = "cannot read " + a.source; return r; }
        r.ok = true;
        r.guid = guid;
        if (hash == a.hash && !a.missing) return r;   // unchanged
        const fs::path dest = m->Abs(a.path);
        fs::create_directories(dest.parent_path(), ec);
        if (!fs::copy_file(a.source, dest, fs::copy_options::overwrite_existing, ec)) { r.ok = false; r.error = "copy failed: " + ec.message(); return r; }
        r.copied.push_back(a.path);
        {
            std::lock_guard<std::mutex> lock(m->mutex);
            AssetInfo& live = m->assets[guid];
            live.hash = hash;
            live.size = size;
            live.missing = false;
            ++live.revision;
            const uint64_t now = Stamp(dest);
            m->WriteMeta(live, now);
            m->stamps[guid] = now;
            a = live;
        }
        Render::ReloadAsset("asset:" + Hex(guid) + "/" + a.name);
        Render::ReloadAsset(a.path);
        Log::Info() << "Assets: reimported " << a.path << " (revision " << a.revision << ")";
        return r;
    }

    bool AssetDatabase::Move(uint64_t guid, const std::string& newRelativePath, std::string* error) {
        const std::string rel = SafeRelative(newRelativePath);
        if (rel.empty()) { if (error) *error = "the destination must be inside the project"; return false; }
        if (AssetTypeOf(rel) != AssetTypeOf(newRelativePath) || AssetTypeOf(rel) == AssetType::Unknown) { if (error) *error = "unsupported file name"; return false; }
        std::lock_guard<std::mutex> lock(m->mutex);
        auto it = m->assets.find(guid);
        if (it == m->assets.end()) { if (error) *error = "unknown asset"; return false; }
        AssetInfo& a = it->second;
        if (a.missing) { if (error) *error = "the asset's file is missing"; return false; }
        if (AssetTypeOf(rel) != a.type) { if (error) *error = "the file type cannot change"; return false; }
        std::error_code ec;
        const fs::path from = m->Abs(a.path), to = m->Abs(rel);
        if (fs::exists(to, ec)) { if (error) *error = "a file with that name exists"; return false; }
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);
        if (ec) { if (error) *error = "move failed: " + ec.message(); return false; }
        fs::rename(fs::path(from.string() + ".meta"), fs::path(to.string() + ".meta"), ec);
        if (ec) m->WriteMeta(a, Stamp(to));
        a.path = rel;
        a.name = to.filename().string();
        return true;
    }

    bool AssetDatabase::CreateFolder(const std::string& relativePath, std::string* error) {
        const std::string rel = SafeRelative(relativePath);
        if (rel.empty()) { if (error) *error = "the folder must be inside the project"; return false; }
        std::error_code ec;
        fs::create_directories(m->Abs(rel), ec);
        if (ec) { if (error) *error = ec.message(); return false; }
        return true;
    }

    std::vector<AssetReference> AssetDatabase::CollectReferences(Scene& scene) {
        std::vector<AssetReference> out;
        std::set<std::string> materials;
        auto add = [&](Entity e, const char* component, const char* field, const std::string& value) {
            if (value.empty()) return;
            AssetReference r;
            r.entity = e ? e.GetGuid() : 0;
            r.entityName = e ? e.GetName() : std::string();
            r.component = component;
            r.field = field;
            r.value = value;
            ParseReference(value, r.asset);
            r.resolved = !Assets::Resolve(value).empty();
            out.push_back(std::move(r));
        };
        scene.Each<MeshRenderer>([&](Entity e, MeshRenderer& m) {
            if (Assets::IsMeshFile(m.mesh) || Assets::IsReference(m.mesh)) add(e, "meshRenderer", "mesh", m.mesh);
            if (!m.material.empty()) materials.insert(m.material);
        });
        scene.Each<AudioSource>([&](Entity e, AudioSource& a) { add(e, "audioSource", "clip", a.clip); });
        scene.Each<CadModel>([&](Entity e, CadModel& c) {   // CAD Import features (the part keeps their exact geometry)
            if (c.document.find("\"import\"") == std::string::npos) return;
            try {
                const json d = json::parse(c.document);
                for (const json& f : d.value("features", json::array()))
                    if (f.value("type", std::string()) == "import") add(e, "cadModel", "import", f.value("asset", std::string()));
            }
            catch (...) {}
        });
        scene.Each<SkinnedModel>([&](Entity e, SkinnedModel& m) { add(e, "skinnedModel", "path", m.path); });
        scene.Each<Script>([&](Entity e, Script& s) { add(e, "script", "file", s.file); });
        scene.Each<Terrain>([&](Entity e, Terrain& t) { add(e, "terrain", "heightmapImage", t.heightmapImage); });
        scene.Each<FoliageScatter>([&](Entity e, FoliageScatter& f) {
            if (Assets::IsMeshFile(f.mesh) || Assets::IsReference(f.mesh)) add(e, "foliageScatter", "mesh", f.mesh);
            if (!f.material.empty()) materials.insert(f.material);
        });
        scene.Each<Physics::Advanced::SoftBody>([&](Entity, Physics::Advanced::SoftBody& s) { if (!s.material.empty()) materials.insert(s.material); });
        for (const std::string& name : materials)
            if (const Render::MaterialDesc* m = Render::FindMaterial(name)) add(Entity(), ("material:" + name).c_str(), "albedoTexture", m->albedoTexture);
        return out;
    }
}
