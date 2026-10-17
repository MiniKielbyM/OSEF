#pragma once
// Virtual file system: a mount table over pluggable FileSystem drivers.
// Built-in drivers: RamFS (in memory) and DevFS (devices); hosted builds
// also have HostFS (a host folder, see osef/hostfs.h).

#include <osef/core.h>

namespace osef {

namespace path {
// Resolves `p` against `cwd` and collapses ".", ".." and duplicate slashes.
// Always returns an absolute path ("/" at minimum).
String normalize(StringView cwd, StringView p);
String parent(StringView abs);
String basename(StringView abs);
} // namespace path

enum class NodeType { None, File, Directory, Device };
const char* to_string(NodeType type);

struct Stat {
    NodeType type = NodeType::None;
    uint64_t size = 0;
};

struct DirEntry {
    String name;
    NodeType type = NodeType::None;
    uint64_t size = 0;
};

// A file system driver. Paths passed in are absolute *within the mount*.
class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual const char* type() const = 0;
    virtual Stat stat(StringView path) = 0;
    virtual bool read(StringView path, String& out) = 0;
    virtual bool write(StringView path, StringView data, bool append) = 0;
    virtual bool list(StringView path, Vector<DirEntry>& out) = 0;
    virtual bool mkdir(StringView path) = 0;
    virtual bool remove(StringView path) = 0;
};

class VFS {
public:
    void mount(StringView at, SharedPtr<FileSystem> fs);
    bool unmount(StringView at);
    bool has_mount(StringView at) const;
    Vector<Pair<String, String>> mounts() const; // (path, type)

    Stat stat(StringView path);
    bool exists(StringView path) { return stat(path).type != NodeType::None; }
    bool read(StringView path, String& out);
    bool write(StringView path, StringView data, bool append = false);
    bool list(StringView path, Vector<DirEntry>& out);
    bool mkdir(StringView path);
    bool remove(StringView path);

private:
    struct Resolved {
        FileSystem* fs = nullptr;
        String sub;
    };
    Resolved resolve(StringView path) const;

    Map<String, SharedPtr<FileSystem>> mounts_;
};

// ---------------------------------------------------------------- RamFS
class RamFS final : public FileSystem {
public:
    RamFS();
    ~RamFS() override;
    const char* type() const override { return "ramfs"; }
    Stat stat(StringView path) override;
    bool read(StringView path, String& out) override;
    bool write(StringView path, StringView data, bool append) override;
    bool list(StringView path, Vector<DirEntry>& out) override;
    bool mkdir(StringView path) override;
    bool remove(StringView path) override;

private:
    struct Node;
    Node* find(StringView path);
    Node* parent_of(StringView path, String& leaf);
    UniquePtr<Node> root_;
};

// ---------------------------------------------------------------- DevFS
// A device is just a pair of callbacks. Leave one empty to make the device
// write-only or read-only.
struct Device {
    String description;
    Function<bool(String& out)> read;
    Function<bool(StringView data)> write;
};

class DevFS final : public FileSystem {
public:
    void add(StringView name, Device dev) { devices_.insert_or_assign(String(name), osef::move(dev)); }
    const Map<String, Device>& devices() const { return devices_; }

    const char* type() const override { return "devfs"; }
    Stat stat(StringView path) override;
    bool read(StringView path, String& out) override;
    bool write(StringView path, StringView data, bool append) override;
    bool list(StringView path, Vector<DirEntry>& out) override;
    bool mkdir(StringView) override { return false; }
    bool remove(StringView) override { return false; }

private:
    Map<String, Device> devices_;
};

} // namespace osef
