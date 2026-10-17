// HostFS: a host directory as an OSEF file system (hosted builds only).

#include <osef/hostfs.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace osef {

namespace fs = std::filesystem;

namespace {

fs::path to_path(StringView s) { return fs::path(std::string(s.data(), s.size())); }

// Host path of an OSEF path inside the root. normalize() strips "..", so a
// path can never escape the root.
fs::path map(const String& root, StringView p) {
    String n = path::normalize("/", p);
    return to_path(root) / to_path(StringView(n.data() + 1, n.size() - 1));
}

} // namespace

HostFS::HostFS(StringView root) {
    std::error_code ec;
    fs::path r = fs::absolute(to_path(root), ec);
    if (ec) r = to_path(root);
    fs::create_directories(r, ec);
    const std::string s = r.lexically_normal().string();
    root_.assign(s.data(), s.size());
}

Stat HostFS::stat(StringView p) {
    std::error_code ec;
    fs::path target = map(root_, p);
    fs::file_status st = fs::status(target, ec);
    if (ec) return {};
    if (fs::is_directory(st)) return {NodeType::Directory, 0};
    if (fs::is_regular_file(st)) return {NodeType::File, static_cast<uint64_t>(fs::file_size(target, ec))};
    return {};
}

bool HostFS::read(StringView p, String& out) {
    std::ifstream in(map(root_, p), std::ios::binary);
    if (!in) return false;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.assign(data.data(), data.size());
    return true;
}

bool HostFS::write(StringView p, StringView data, bool append) {
    std::ofstream o(map(root_, p), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!o) return false;
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(o);
}

bool HostFS::list(StringView p, Vector<DirEntry>& out) {
    std::error_code ec;
    fs::directory_iterator it(map(root_, p), ec);
    if (ec) return false;
    for (const auto& entry : it) {
        DirEntry e;
        const std::string name = entry.path().filename().string();
        e.name.assign(name.data(), name.size());
        if (entry.is_directory(ec)) e.type = NodeType::Directory;
        else if (entry.is_regular_file(ec)) {
            e.type = NodeType::File;
            e.size = static_cast<uint64_t>(entry.file_size(ec));
        } else continue;
        out.push_back(osef::move(e));
    }
    return true;
}

bool HostFS::mkdir(StringView p) {
    std::error_code ec;
    return fs::create_directory(map(root_, p), ec);
}

bool HostFS::remove(StringView p) {
    if (path::normalize("/", p) == "/") return false;
    std::error_code ec;
    return fs::remove(map(root_, p), ec);
}

} // namespace osef
