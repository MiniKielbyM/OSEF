// OS settings (model/os_settings.h): storage, validation, code generation.

#include "test_main.h"

#include "model/os_settings.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <chrono>
#include <vector>

using namespace osef::editor;
namespace fs = std::filesystem;

namespace {

OsSettings sample() {
    OsSettings s;
    s.welcome = "Welcome to \"Test\"!\nSecond line\twith tab \\ and backslash";
    s.prompt = "  {os} [{cwd}] % ";  // leading and trailing spaces must survive
    s.text_color = "bright-white";
    s.background = "blue";
    s.prompt_name_color = "bright-magenta";
    s.prompt_path_color = "default";
    s.text_size = 3;
    s.shell = true;
    s.start_program = "sh";
    s.programs = {{"message", "hello", "says hi", "Hi = there [x]", 0},
                  {"countdown", "launch", "", "Lift off!", 3},
                  {"clock", "clock", "", "", 2},
                  {"guess", "guess-game", "", "", 50},
                  {"ask", "ask_me", "", "Who are you?", 0}};
    s.files = {{"/home/readme.txt", "line 1\nline 2"}, {"/etc/motd", ""}};
    return s;
}

bool has_problem(const std::vector<std::string>& problems, const std::string& needle) {
    for (const auto& p : problems)
        if (p.find(needle) != std::string::npos) return true;
    return false;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST(settings_round_trip) {
    const OsSettings s = sample();
    std::vector<std::string> warnings;
    const OsSettings back = parse_settings(serialize_settings(s), &warnings);
    CHECK(back == s);
    CHECK(warnings.empty());
    CHECK(parse_settings(serialize_settings(OsSettings{})) == OsSettings{});
}

TEST(settings_parse_is_forgiving) {
    std::vector<std::string> warnings;
    OsSettings s = parse_settings("# comment\r\n[appearance]\r\n  text_size = 9x \r\nbogus = 1\n[weird]\n"
                                  "[features]\nshell = no\nnoequals\n[program]\nname = a\nnumber = 7\n",
                                  &warnings);
    CHECK_EQ(s.text_size, 0);  // unparsable -> default
    CHECK(!s.shell);
    REQUIRE_EQ(s.programs.size(), size_t(1));
    CHECK_EQ(s.programs[0].name, std::string("a"));
    CHECK_EQ(s.programs[0].number, 7);
    CHECK(has_problem(warnings, "unknown setting 'bogus'"));
    CHECK(has_problem(warnings, "unknown section [weird]"));
    CHECK(has_problem(warnings, "expected key = value"));
}

TEST(settings_validation) {
    CHECK(validate_settings(sample()).empty());
    CHECK(validate_settings(OsSettings{}).empty());

    OsSettings s = sample();
    s.programs.push_back({"message", "Bad Name", "", "", 0});
    s.programs.push_back({"message", "hello", "", "", 0});
    s.programs.push_back({"message", "ls", "", "", 0});
    s.programs.push_back({"teleport", "beam", "", "", 0});
    s.files.push_back({"relative.txt", ""});
    s.text_color = "plaid";
    s.text_size = 7;
    const auto problems = validate_settings(s);
    CHECK(has_problem(problems, "'Bad Name': a command name"));
    CHECK(has_problem(problems, "two programs called 'hello'"));
    CHECK(has_problem(problems, "'ls' has the same name as a built-in"));
    CHECK(has_problem(problems, "unknown type 'teleport'"));
    CHECK(has_problem(problems, "File 'relative.txt'"));
    CHECK(has_problem(problems, "Unknown color 'plaid'"));
    CHECK(has_problem(problems, "Text size"));

    OsSettings kiosk;
    kiosk.shell = false;
    CHECK(has_problem(validate_settings(kiosk), "shell is turned off"));
    kiosk.start_program = "nope";
    CHECK(has_problem(validate_settings(kiosk), "start program 'nope' does not exist"));
    kiosk.programs = {{"message", "nope", "", "hi", 0}};
    CHECK(validate_settings(kiosk).empty());
}

TEST(settings_names_and_colors) {
    CHECK(is_valid_command_name("hello"));
    CHECK(is_valid_command_name("guess-2_x"));
    CHECK(!is_valid_command_name(""));
    CHECK(!is_valid_command_name("2fast"));
    CHECK(!is_valid_command_name("Hello"));
    CHECK(!is_valid_command_name("a b"));
    CHECK_EQ(color_enum_name("bright-green"), std::string("BrightGreen"));
    CHECK_EQ(color_enum_name("default"), std::string("Default"));
    CHECK_EQ(color_enum_name("plaid"), std::string());
    CHECK_EQ(color_names().size(), size_t(17));
    for (const auto& t : program_templates()) {
        CHECK(find_program_template(t.kind) == &t);
        if (!t.number_label.empty()) CHECK(t.number_min <= t.number_default && t.number_default <= t.number_max);
    }
    CHECK(find_program_template("nope") == nullptr);
}

TEST(settings_generated_code) {
    OsSettings s = sample();
    s.programs.push_back({"message", "Bad Name", "", "skipped", 0});  // invalid: not generated
    s.programs.push_back({"countdown", "huge", "", "x", 999999});     // clamped to the template max
    const std::string code = generate_settings_code(s);
    CHECK(code.find("#include <osef/osef.h>") != std::string::npos);
    CHECK(code.find("OSEF_AUTO_MODULE(osef_generated::EditorSettings);") != std::string::npos);
    CHECK(code.find("a.welcome = \"Welcome to \\\"Test\\\"!\\nSecond line\\twith tab \\\\ and backslash\";") !=
          std::string::npos);
    CHECK(code.find("a.prompt = \"  {os} [{cwd}] % \";") != std::string::npos);
    CHECK(code.find("a.text = osef::ConsoleColor::BrightWhite;") != std::string::npos);
    CHECK(code.find("a.background = osef::ConsoleColor::Blue;") != std::string::npos);
    CHECK(code.find("a.prompt_path = osef::ConsoleColor::Default;") != std::string::npos);
    CHECK(code.find("a.text_size = 3;") != std::string::npos);
    CHECK(code.find("kernel.add_module<osef::ShellModule>()") != std::string::npos);
    CHECK(code.find("kernel.programs().add<HelloProgram>(\"hello\", \"says hi\");") != std::string::npos);
    CHECK(code.find("class GuessGameProgram") != std::string::npos);
    CHECK(code.find("class AskMeProgram") != std::string::npos);
    CHECK(code.find("int left_ = 3;") != std::string::npos);
    CHECK(code.find("int left_ = 3600;") != std::string::npos);  // "huge" clamped
    CHECK(code.find("skipped") == std::string::npos);
    CHECK(code.find("write_file(kernel, \"/home/readme.txt\", \"line 1\\nline 2\\n\");") != std::string::npos);
    CHECK(code.find("init_program") == std::string::npos);  // shell is the start program
    CHECK(code.find("std::") == std::string::npos);         // must build freestanding

    OsSettings kiosk;
    kiosk.shell = false;
    kiosk.start_program = "hello";
    kiosk.programs = {{"message", "hello", "", "hi", 0}};
    const std::string k = generate_settings_code(kiosk);
    CHECK(k.find("config.init_program = \"hello\";") != std::string::npos);
    CHECK(k.find("ShellModule") == std::string::npos);
    CHECK(k.find("write_file") == std::string::npos);  // no files -> no helper
}

TEST(settings_files_written_only_when_changed) {
    const fs::path dir = test::temp_dir("settings_files");
    std::string error;
    REQUIRE(write_settings_files(dir, sample(), &error));
    CHECK(error.empty());
    CHECK(fs::exists(dir / kSettingsFile));
    CHECK(fs::exists(dir / kGeneratedFile));
    CHECK_EQ(read_file(dir / kGeneratedFile), generate_settings_code(sample()));

    OsSettings back;
    REQUIRE(load_settings(dir / kSettingsFile, back, &error));
    CHECK(back == sample());

    // Unchanged settings do not touch the files (stable build timestamps).
    const auto stamp = fs::last_write_time(dir / kGeneratedFile);
    fs::last_write_time(dir / kGeneratedFile, stamp - std::chrono::hours(1));
    const auto old_stamp = fs::last_write_time(dir / kGeneratedFile);
    REQUIRE(write_settings_files(dir, sample(), &error));
    CHECK(fs::last_write_time(dir / kGeneratedFile) == old_stamp);

    OsSettings changed = sample();
    changed.welcome = "different";
    REQUIRE(write_settings_files(dir, changed, &error));
    CHECK(fs::last_write_time(dir / kGeneratedFile) != old_stamp);
    CHECK(!fs::exists(dir / (std::string(kGeneratedFile) + ".tmp")));

    CHECK(!load_settings(dir / "missing.ini", back, &error));
    CHECK(!error.empty());
}

TEST(settings_interrupts) {
    OsSettings s;
    s.programs = {{"message", "hello", "", "hi", 0}};
    s.interrupts = {{"vector", 0x80, "my-call", "message", "got 0x80"},
                    {"irq", 12, "mouse", "program", "hello"},
                    {"vector", 20, "bad", "message", ""},
                    {"irq", 3, "", "program", "missing"},
                    {"vector", 0x80, "dup", "message", "x"}};
    const auto problems = validate_settings(s);
    CHECK(has_problem(problems, "Interrupt 'bad': OS vectors are 48-255"));
    CHECK(has_problem(problems, "program 'missing' does not exist"));
    CHECK(has_problem(problems, "Two interrupt handlers use the same vector 128"));
    CHECK(parse_settings(serialize_settings(s)) == s);

    const std::string code = generate_settings_code(s);
    CHECK(code.find("kernel.interrupts().on_vector(0x80, \"my-call\"") != std::string::npos);
    CHECK(code.find("k.print(\"got 0x80\\n\");") != std::string::npos);
    CHECK(code.find("kernel.interrupts().on_irq(12, \"mouse\"") != std::string::npos);
    CHECK(code.find("k.exec(\"hello\");") != std::string::npos);
    CHECK(code.find("on_vector(0x14") == std::string::npos);  // invalid: skipped
    CHECK(code.find("missing") == std::string::npos);
    CHECK(code.find("\"dup\"") == std::string::npos);         // duplicate vector: first one wins
}
