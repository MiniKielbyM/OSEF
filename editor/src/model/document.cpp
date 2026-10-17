// Document (one open file) and DocumentManager (the set of open files).

#include "model/document.h"

#include "text/text_buffer.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#define OSEF_GETPID _getpid
#else
#include <unistd.h>
#define OSEF_GETPID getpid
#endif

namespace fs = std::filesystem;

namespace osef::editor {

namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

// Absolute, symlink-resolved where possible; works for files that don't exist yet.
fs::path canonical_path(const fs::path& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    if (ec) {
        p = fs::absolute(path, ec);
        if (ec) p = path;
    }
    return p.lexically_normal();
}

bool read_file(const fs::path& path, std::string& out, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        if (error) *error = "not a file: " + path.string();
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    return true;
}

} // namespace

struct Document::Impl {
    fs::path path;
    TextBuffer buffer;
    bool crlf = false;      // the file used "\r\n" line endings
    bool bom = false;       // the file started with a UTF-8 byte order mark
    bool read_only = false;
    int reveal_line = 0;

    // Loads raw file bytes into the buffer, remembering line ending + BOM.
    void set_file_content(std::string_view raw) {
        bom = raw.starts_with(kUtf8Bom);
        if (bom) raw.remove_prefix(kUtf8Bom.size());
        const size_t nl = raw.find('\n');
        crlf = nl != std::string_view::npos && nl > 0 && raw[nl - 1] == '\r';
        buffer.set_text(raw);
    }
};

Document::Document() : impl_(std::make_unique<Impl>()) {}
Document::~Document() = default;

std::unique_ptr<Document> Document::load(const fs::path& path, std::string* error) {
    std::string raw;
    const fs::path canon = canonical_path(path);
    if (!read_file(canon, raw, error)) return nullptr;
    std::unique_ptr<Document> doc(new Document());
    doc->impl_->path = canon;
    doc->impl_->set_file_content(raw);
    return doc;
}

std::unique_ptr<Document> Document::from_text(fs::path path, std::string text) {
    std::unique_ptr<Document> doc(new Document());
    doc->impl_->path = canonical_path(path);
    doc->impl_->buffer.set_text(text);
    return doc;
}

const fs::path& Document::path() const { return impl_->path; }
std::string Document::title() const { return impl_->path.filename().string(); }
std::string Document::text() const { return impl_->buffer.text(); }

void Document::replace_all(const std::string& text) {
    if (text == impl_->buffer.text()) return;
    impl_->buffer.replace_all(text);
}

bool Document::modified() const { return impl_->buffer.modified(); }

bool Document::save(std::string* error) {
    std::string body = impl_->buffer.text();
    if (impl_->crlf) {
        std::string out;
        out.reserve(body.size() + body.size() / 32);
        for (char c : body) {
            if (c == '\n') out.push_back('\r');
            out.push_back(c);
        }
        body = std::move(out);
    }
    if (impl_->bom) body.insert(0, kUtf8Bom);

    // Write next to the target (same file system) and rename over it, so a
    // crash never leaves a half-written file. Symlinks are resolved first.
    std::error_code ec;
    const fs::path target = canonical_path(impl_->path);
    const fs::path tmp = target.parent_path() /
                         ("." + target.filename().string() + ".osef-save-" + std::to_string(OSEF_GETPID()) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
    }
    if (fs::exists(target, ec)) {
        const fs::perms perms = fs::status(target, ec).permissions();
        if (!ec) fs::permissions(tmp, perms, fs::perm_options::replace, ec);
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignore;
        fs::remove(tmp, ignore);
        if (error) *error = "cannot save " + target.string() + ": " + ec.message();
        return false;
    }
    impl_->buffer.mark_saved();
    return true;
}

bool Document::reload(std::string* error) {
    std::string raw;
    if (!read_file(impl_->path, raw, error)) return false;
    const TextPos keep = impl_->buffer.cursor();
    impl_->set_file_content(raw);
    impl_->buffer.set_cursor(keep);
    return true;
}

bool Document::read_only() const { return impl_->read_only; }
void Document::set_read_only(bool read_only) { impl_->read_only = read_only; }
uint64_t Document::version() const { return impl_->buffer.version(); }
TextBuffer& Document::buffer() { return impl_->buffer; }
const TextBuffer& Document::buffer() const { return impl_->buffer; }

void Document::request_reveal_line(int line) { impl_->reveal_line = std::max(0, line); }

int Document::take_reveal_request() {
    const int line = impl_->reveal_line;
    impl_->reveal_line = 0;
    return line;
}

// ---- DocumentManager -------------------------------------------------------------

DocumentManager::DocumentManager() = default;
DocumentManager::~DocumentManager() = default;

Document* DocumentManager::open(const fs::path& path, std::string* error) {
    if (Document* doc = find(path)) return doc;
    std::unique_ptr<Document> doc = Document::load(path, error);
    if (!doc) return nullptr;
    return adopt(std::move(doc));
}

Document* DocumentManager::find(const fs::path& path) const {
    const fs::path key = canonical_path(path);
    for (const auto& doc : docs_)
        if (doc->path() == key) return doc.get();
    return nullptr;
}

Document* DocumentManager::adopt(std::unique_ptr<Document> doc) {
    if (!doc) return nullptr;
    if (Document* existing = find(doc->path())) return existing;
    docs_.push_back(std::move(doc));
    Document* added = docs_.back().get();
    if (!active_) active_ = added;
    return added;
}

bool DocumentManager::close(Document* doc) {
    const auto it = std::find_if(docs_.begin(), docs_.end(), [&](const auto& d) { return d.get() == doc; });
    if (it == docs_.end()) return false;
    const size_t index = static_cast<size_t>(it - docs_.begin());
    docs_.erase(it);
    if (active_ == doc) {
        // Focus the neighbor, like closing a tab in most editors.
        active_ = docs_.empty() ? nullptr : docs_[std::min(index, docs_.size() - 1)].get();
    }
    return true;
}

const std::vector<std::unique_ptr<Document>>& DocumentManager::documents() const { return docs_; }
Document* DocumentManager::active() const { return active_; }

void DocumentManager::set_active(Document* doc) {
    if (!doc) {
        active_ = nullptr;
        return;
    }
    for (const auto& d : docs_)
        if (d.get() == doc) active_ = doc;
}

bool DocumentManager::any_modified() const {
    return std::any_of(docs_.begin(), docs_.end(), [](const auto& d) { return d->modified(); });
}

bool DocumentManager::save_all(std::string* error) {
    bool ok = true;
    std::string errors;
    for (const auto& doc : docs_) {
        if (!doc->modified()) continue;
        std::string e;
        if (!doc->save(&e)) {
            ok = false;
            if (!errors.empty()) errors += "\n";
            errors += e;
        }
    }
    if (!ok && error) *error = errors;
    return ok;
}

} // namespace osef::editor
