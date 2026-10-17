#pragma once
// HostFS: exposes a real directory on the host as an OSEF file system. Use
// it for persistent "disks" while developing an OS as a hosted app.
//
// Hosted builds only; guard uses with `#if OSEF_HOSTED`. osef/osef.h
// includes this header automatically in hosted builds. Like every public
// OSEF header it uses only osef types, so it never makes the C++ standard
// library visible to OS code (which would compile hosted but not in the
// freestanding ISO kernel).

#include <osef/vfs.h>

#if OSEF_HOSTED

namespace osef {

class HostFS final : public FileSystem {
public:
    // `root` is a host directory, created if missing. A relative path is
    // resolved against the working directory at construction.
    explicit HostFS(StringView root);
    const char* type() const override { return "hostfs"; }
    Stat stat(StringView path) override;
    bool read(StringView path, String& out) override;
    bool write(StringView path, StringView data, bool append) override;
    bool list(StringView path, Vector<DirEntry>& out) override;
    bool mkdir(StringView path) override;
    bool remove(StringView path) override;

    // Absolute host path of the root directory.
    const String& root() const { return root_; }

private:
    String root_;
};

} // namespace osef

#endif // OSEF_HOSTED
