// Document / DocumentManager: load, save (atomic, line endings), modified, open-once.

#include "test_main.h"

#include "model/document.h"
#include "text/text_buffer.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <cstdint>

using namespace osef::editor;
namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int count_entries(const fs::path& dir) {
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

} // namespace

TEST(document_load_save_round_trip) {
    const fs::path dir = test::temp_dir("doc_rt");
    const fs::path file = dir / "main.cpp";
    write_file(file, "int main() {\n    return 0;\n}\n");

    std::string error;
    auto doc = Document::load(file, &error);
    REQUIRE(doc != nullptr);
    CHECK_EQ(doc->title(), "main.cpp");
    CHECK_EQ(doc->text(), "int main() {\n    return 0;\n}\n");
    CHECK(!doc->modified());
    CHECK(!doc->read_only());

    const uint64_t v = doc->version();
    doc->buffer().set_cursor({1, 11});
    doc->buffer().insert("42");
    CHECK(doc->modified());
    CHECK(doc->version() > v);
    CHECK_EQ(read_file(file), "int main() {\n    return 0;\n}\n");  // nothing saved yet

    REQUIRE(doc->save(&error));
    CHECK(!doc->modified());
    CHECK_EQ(read_file(file), "int main() {\n    return 420;\n}\n");
    CHECK_EQ(count_entries(dir), 1);  // no temp file left behind

    // Undo past the save point -> modified again.
    doc->buffer().undo();
    CHECK(doc->modified());
}

TEST(document_preserves_crlf_and_bom) {
    const fs::path dir = test::temp_dir("doc_crlf");
    const fs::path file = dir / "win.cpp";
    write_file(file, "\xEF\xBB\xBF" "a\r\nb\r\n");
    std::string error;
    auto doc = Document::load(file, &error);
    REQUIRE(doc != nullptr);
    CHECK_EQ(doc->text(), "a\nb\n");  // "\n" in memory
    doc->buffer().move_doc_end(false);
    doc->buffer().insert("c\nd");
    REQUIRE(doc->save(&error));
    CHECK_EQ(read_file(file), "\xEF\xBB\xBF" "a\r\nb\r\nc\r\nd");

    // LF files stay LF.
    const fs::path lf = dir / "unix.cpp";
    write_file(lf, "x\ny");
    auto d2 = Document::load(lf, &error);
    REQUIRE(d2 != nullptr);
    d2->buffer().insert("1\n");
    REQUIRE(d2->save(&error));
    CHECK_EQ(read_file(lf), "1\nx\ny");
}

TEST(document_atomic_save_replaces_file) {
    const fs::path dir = test::temp_dir("doc_atomic");
    const fs::path file = dir / "f.txt";
    write_file(file, "old");
    std::string error;
    auto doc = Document::load(file, &error);
    REQUIRE(doc != nullptr);
    // Keep the old inode open: an atomic rename leaves its content intact.
    std::ifstream held(file, std::ios::binary);
    doc->replace_all("new content");
    REQUIRE(doc->save(&error));
    CHECK_EQ(read_file(file), "new content");
    std::string old((std::istreambuf_iterator<char>(held)), std::istreambuf_iterator<char>());
#if !defined(_WIN32)
    CHECK_EQ(old, "old");
#endif
    CHECK_EQ(count_entries(dir), 1);

    // Saving into a directory that vanished fails cleanly with an error.
    auto ghost = Document::from_text(dir / "missing" / "g.txt", "x");
    std::string e2;
    CHECK(!ghost->save(&e2));
    CHECK(!e2.empty());

    // from_text docs can be saved to create the file.
    auto fresh = Document::from_text(dir / "new.txt", "hello\n");
    CHECK_EQ(fresh->title(), "new.txt");
    REQUIRE(fresh->save(&error));
    CHECK_EQ(read_file(dir / "new.txt"), "hello\n");
}

TEST(document_load_errors_and_reload) {
    const fs::path dir = test::temp_dir("doc_reload");
    std::string error;
    CHECK(Document::load(dir / "nope.cpp", &error) == nullptr);
    CHECK(!error.empty());
    CHECK(Document::load(dir, &error) == nullptr);  // a directory

    const fs::path file = dir / "r.cpp";
    write_file(file, "one\ntwo\n");
    auto doc = Document::load(file, &error);
    REQUIRE(doc != nullptr);
    doc->buffer().insert("edit ");
    CHECK(doc->modified());
    write_file(file, "changed on disk\n");
    REQUIRE(doc->reload(&error));
    CHECK_EQ(doc->text(), "changed on disk\n");
    CHECK(!doc->modified());
    CHECK(!doc->buffer().can_undo());
}

TEST(document_replace_all_reveal_and_read_only) {
    auto doc = Document::from_text("/tmp/osef-doc-test/x.cpp", "a\nb\nc");
    doc->buffer().set_cursor({2, 1});
    const uint64_t v = doc->version();
    doc->replace_all("a\nb\nc");  // identical: no-op
    CHECK_EQ(doc->version(), v);
    CHECK(!doc->modified());
    doc->replace_all("a\nB\nc");
    CHECK(doc->modified());
    CHECK_EQ(doc->buffer().cursor().line, 2);
    doc->buffer().undo();
    CHECK(!doc->modified());

    CHECK_EQ(doc->take_reveal_request(), 0);
    doc->request_reveal_line(12);
    CHECK_EQ(doc->take_reveal_request(), 12);
    CHECK_EQ(doc->take_reveal_request(), 0);

    doc->set_read_only(true);
    CHECK(doc->read_only());
}

TEST(document_manager_open_twice_returns_same_doc) {
    const fs::path dir = test::temp_dir("docmgr");
    fs::create_directories(dir / "sub");
    write_file(dir / "a.cpp", "a");
    write_file(dir / "sub" / "b.cpp", "b");

    DocumentManager docs;
    CHECK(docs.active() == nullptr);
    std::string error;
    Document* a = docs.open(dir / "a.cpp", &error);
    REQUIRE(a != nullptr);
    CHECK(docs.active() == a);  // the first document becomes active
    Document* again = docs.open(dir / "sub" / ".." / "a.cpp", &error);
    CHECK(again == a);
    CHECK(docs.find(dir / "./a.cpp") == a);
    CHECK_EQ(docs.documents().size(), 1);
#if !defined(_WIN32)
    std::error_code ec;
    fs::create_symlink(dir / "a.cpp", dir / "link.cpp", ec);
    if (!ec) CHECK(docs.open(dir / "link.cpp", &error) == a);
#endif

    Document* b = docs.open(dir / "sub" / "b.cpp", &error);
    REQUIRE(b != nullptr);
    CHECK(b != a);
    CHECK_EQ(docs.documents().size(), 2);
    CHECK(docs.open(dir / "missing.cpp", &error) == nullptr);
    CHECK_EQ(docs.documents().size(), 2);

    docs.set_active(b);
    CHECK(docs.active() == b);
    CHECK(!docs.any_modified());
    b->buffer().insert("x");
    CHECK(docs.any_modified());
    REQUIRE(docs.save_all(&error));
    CHECK(!docs.any_modified());
    CHECK_EQ(read_file(dir / "sub" / "b.cpp"), "xb");

    // adopt() and close(); closing the active doc focuses a neighbor.
    Document* c = docs.adopt(Document::from_text(dir / "c.cpp", "c"));
    REQUIRE(c != nullptr);
    CHECK_EQ(docs.documents().size(), 3);
    CHECK(docs.adopt(Document::from_text(dir / "c.cpp", "dup")) == c);
    docs.set_active(b);
    CHECK(docs.close(b));
    CHECK(docs.active() == c);
    CHECK(!docs.close(b));
    CHECK(docs.close(c));
    CHECK(docs.active() == a);
    CHECK(docs.close(a));
    CHECK(docs.active() == nullptr);
    CHECK(docs.documents().empty());
}
