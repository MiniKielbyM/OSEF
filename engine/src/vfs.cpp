#include <osef/vfs.h>

namespace osef {

// ---------------------------------------------------------------- paths

namespace path {

String normalize(StringView cwd, StringView p) {
    String full;
    if (!p.empty() && p[0] == '/') {
        full = String(p);
    } else {
        full = String(cwd.empty() ? StringView("/") : cwd);
        full += '/';
        full += p;
    }
    Vector<String> parts;
    String cur;
    auto flush = [&] {
        if (cur.empty() || cur == ".") {
        } else if (cur == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (char ch : full) {
        if (ch == '/' || ch == '\\') flush();
        else cur += ch;
    }
    flush();
    if (parts.empty()) return String("/");
    String out;
    for (const String& part : parts) {
        out += '/';
        out += part;
    }
    return out;
}

String parent(StringView abs) {
    String n = normalize("/", abs);
    size_t pos = n.rfind('/');
    return pos == 0 ? String("/") : n.substr(0, pos);
}

String basename(StringView abs) {
    String n = normalize("/", abs);
    return n == "/" ? String() : n.substr(n.rfind('/') + 1);
}

} // namespace path

const char* to_string(NodeType type) {
    switch (type) {
        case NodeType::File: return "file";
        case NodeType::Directory: return "dir";
        case NodeType::Device: return "dev";
        default: return "none";
    }
}

// ---------------------------------------------------------------- VFS

void VFS::mount(StringView at, SharedPtr<FileSystem> fs) {
    mounts_.insert_or_assign(path::normalize("/", at), osef::move(fs));
}

bool VFS::unmount(StringView at) { return mounts_.erase(path::normalize("/", at)) != 0; }

bool VFS::has_mount(StringView at) const { return mounts_.contains(path::normalize("/", at)); }

Vector<Pair<String, String>> VFS::mounts() const {
    Vector<Pair<String, String>> out;
    for (const auto& [at, fs] : mounts_) out.push_back(Pair<String, String>{at, String(fs->type())});
    return out;
}

VFS::Resolved VFS::resolve(StringView p) const {
    const String n = path::normalize("/", p);
    const String* best = nullptr;
    FileSystem* fs = nullptr;
    for (const auto& [at, mounted] : mounts_) {
        const bool match = at == "/" || n == at ||
                           (n.size() > at.size() && n.starts_with(at) && n[at.size()] == '/');
        if (match && (!best || at.size() > best->size())) {
            best = &at;
            fs = mounted.get();
        }
    }
    if (!fs) return {};
    String sub = *best == "/" ? n : n.substr(best->size());
    if (sub.empty()) sub = "/";
    return {fs, osef::move(sub)};
}

Stat VFS::stat(StringView p) {
    Resolved r = resolve(p);
    return r.fs ? r.fs->stat(r.sub) : Stat{};
}

bool VFS::read(StringView p, String& out) {
    Resolved r = resolve(p);
    return r.fs && r.fs->read(r.sub, out);
}

bool VFS::write(StringView p, StringView data, bool append) {
    Resolved r = resolve(p);
    return r.fs && r.fs->write(r.sub, data, append);
}

bool VFS::list(StringView p, Vector<DirEntry>& out) {
    Resolved r = resolve(p);
    if (!r.fs || !r.fs->list(r.sub, out)) return false;
    // Show mount points that live directly under this directory.
    const String n = path::normalize("/", p);
    for (const auto& [at, fs] : mounts_) {
        if (at == "/" || path::parent(at) != n) continue;
        String name = path::basename(at);
        bool listed = osef::any_of(out.begin(), out.end(), [&](const DirEntry& e) { return e.name == name; });
        if (!listed) out.push_back(DirEntry{osef::move(name), NodeType::Directory, 0});
    }
    osef::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return true;
}

bool VFS::mkdir(StringView p) {
    Resolved r = resolve(p);
    return r.fs && r.fs->mkdir(r.sub);
}

bool VFS::remove(StringView p) {
    if (has_mount(p)) return false;
    Resolved r = resolve(p);
    return r.fs && r.fs->remove(r.sub);
}

// ---------------------------------------------------------------- RamFS

struct RamFS::Node {
    NodeType type = NodeType::Directory;
    String data;
    Map<String, UniquePtr<Node>> children;
};

RamFS::RamFS() : root_(make_unique<Node>()) {}
RamFS::~RamFS() = default;

RamFS::Node* RamFS::find(StringView p) {
    Node* node = root_.get();
    const String n = path::normalize("/", p);
    if (n == "/") return node;
    size_t start = 1;
    while (start <= n.size()) {
        size_t slash = n.find('/', start);
        if (slash == String::npos) slash = n.size();
        StringView part = StringView(n).substr(start, slash - start);
        if (node->type != NodeType::Directory) return nullptr;
        auto it = node->children.find(part);
        if (it == node->children.end()) return nullptr;
        node = it->second.get();
        start = slash + 1;
    }
    return node;
}

RamFS::Node* RamFS::parent_of(StringView p, String& leaf) {
    const String n = path::normalize("/", p);
    if (n == "/") return nullptr;
    leaf = path::basename(n);
    Node* dir = find(path::parent(n));
    return dir && dir->type == NodeType::Directory ? dir : nullptr;
}

Stat RamFS::stat(StringView p) {
    Node* node = find(p);
    if (!node) return {};
    return {node->type, node->type == NodeType::File ? node->data.size() : node->children.size()};
}

bool RamFS::read(StringView p, String& out) {
    Node* node = find(p);
    if (!node || node->type != NodeType::File) return false;
    out = node->data;
    return true;
}

bool RamFS::write(StringView p, StringView data, bool append) {
    String leaf;
    Node* dir = parent_of(p, leaf);
    if (!dir) return false;
    UniquePtr<Node>& slot = dir->children[leaf];
    if (!slot) {
        slot = make_unique<Node>();
        slot->type = NodeType::File;
    }
    if (slot->type != NodeType::File) return false;
    if (append) slot->data += data;
    else slot->data = String(data);
    return true;
}

bool RamFS::list(StringView p, Vector<DirEntry>& out) {
    Node* node = find(p);
    if (!node || node->type != NodeType::Directory) return false;
    for (const auto& [name, child] : node->children)
        out.push_back(DirEntry{name, child->type,
                               child->type == NodeType::File ? child->data.size() : child->children.size()});
    return true;
}

bool RamFS::mkdir(StringView p) {
    String leaf;
    Node* dir = parent_of(p, leaf);
    if (!dir || dir->children.contains(leaf)) return false;
    dir->children.insert_or_assign(leaf, make_unique<Node>());
    return true;
}

bool RamFS::remove(StringView p) {
    String leaf;
    Node* dir = parent_of(p, leaf);
    if (!dir) return false;
    auto it = dir->children.find(leaf);
    if (it == dir->children.end() || !it->second->children.empty()) return false;
    dir->children.erase(it);
    return true;
}

// ---------------------------------------------------------------- DevFS

Stat DevFS::stat(StringView p) {
    const String n = path::normalize("/", p);
    if (n == "/") return {NodeType::Directory, devices_.size()};
    return devices_.contains(StringView(n).substr(1)) ? Stat{NodeType::Device, 0} : Stat{};
}

bool DevFS::read(StringView p, String& out) {
    const String n = path::normalize("/", p);
    auto it = devices_.find(StringView(n).substr(1));
    return it != devices_.end() && it->second.read && it->second.read(out);
}

bool DevFS::write(StringView p, StringView data, bool) {
    const String n = path::normalize("/", p);
    auto it = devices_.find(StringView(n).substr(1));
    return it != devices_.end() && it->second.write && it->second.write(data);
}

bool DevFS::list(StringView p, Vector<DirEntry>& out) {
    if (path::normalize("/", p) != "/") return false;
    for (const auto& [name, dev] : devices_) out.push_back(DirEntry{name, NodeType::Device, 0});
    return true;
}

} // namespace osef
