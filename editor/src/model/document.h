#pragma once
// Open text files. A Document owns a TextBuffer (text/text_buffer.h) that the
// code editor widget edits; the model edits it too (inspector -> code sync).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace osef::editor {

class TextBuffer;

class Document {
public:
    static std::unique_ptr<Document> load(const std::filesystem::path& path, std::string* error);
    static std::unique_ptr<Document> from_text(std::filesystem::path path, std::string text); // not yet on disk
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    const std::filesystem::path& path() const;
    std::string title() const;                   // file name, e.g. "main.cpp"
    std::string text() const;                    // full content ("\n" line endings)

    // Replaces the whole content as ONE undoable edit, keeping the cursor on
    // (about) the same line. No-op when the text is identical.
    void replace_all(const std::string& text);

    bool modified() const;                       // content differs from the last load/save
    bool save(std::string* error);               // atomic write (temp file + rename); keeps the file's original line endings
    bool reload(std::string* error);             // discards unsaved changes
    bool read_only() const;
    void set_read_only(bool read_only);
    uint64_t version() const;                    // increments on every content change

    TextBuffer& buffer();
    const TextBuffer& buffer() const;

    // Asks the code view to scroll to and place the cursor on `line` (1-based).
    void request_reveal_line(int line);
    int take_reveal_request();                   // the pending line, or 0; clears it

private:
    Document();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class DocumentManager {
public:
    DocumentManager();
    ~DocumentManager();

    // Opens a file, or returns the already-open Document for it (paths are
    // compared after canonicalization). Returns nullptr and sets *error on failure.
    Document* open(const std::filesystem::path& path, std::string* error);
    Document* find(const std::filesystem::path& path) const;
    Document* adopt(std::unique_ptr<Document> doc);  // takes ownership of a from_text() doc
    bool close(Document* doc);                       // false if not open
    const std::vector<std::unique_ptr<Document>>& documents() const;

    Document* active() const;                        // focused tab, may be nullptr
    void set_active(Document* doc);
    bool any_modified() const;
    bool save_all(std::string* error);

private:
    std::vector<std::unique_ptr<Document>> docs_;
    Document* active_ = nullptr;
};

} // namespace osef::editor
