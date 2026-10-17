// OSEF_HARDWARE parser / generator / rewriter (model/hardware_decl.h).

#include "test_main.h"

#include "model/hardware_decl.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace osef::editor;

namespace {

const char* kCanonical =
    "// MyOS\n"
    "\n"
    "#include <osef/hardware.h>\n"
    "\n"
    "OSEF_HARDWARE(osef::Hardware()\n"
    "    .name(\"MyOS\")\n"
    "    .arch(osef::Arch::x86_64)\n"
    "    .firmware(osef::Firmware::UEFI)\n"
    "    .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)\n"
    "    .memory_mb(128));\n"
    "\n"
    "#include <osef/osef.h>\n"
    "\n"
    "int main() { return 0; }\n";

HardwareDecl myos_decl() {
    HardwareDecl d;
    d.name = "MyOS";
    d.arch = "x86_64";
    d.firmware = "UEFI";
    d.serial = d.framebuffer = d.ps2_keyboard = true;
    d.memory_mb = 128;
    return d;
}

bool has_problem(const HardwareParse& p, const std::string& needle) {
    for (const std::string& s : p.problems)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

// Parses `decl_body` wrapped as OSEF_HARDWARE(<body>);
HardwareParse parse_body(const std::string& body) {
    return parse_hardware_decl("#include <osef/hardware.h>\nOSEF_HARDWARE(" + body + ");\n");
}

const std::string kMinimalBody =
    "osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::BIOS).devices(osef::Hw::Serial)";

} // namespace

// ---- parsing ------------------------------------------------------------------------

TEST(hwdecl_parses_canonical) {
    HardwareParse p = parse_hardware_decl(kCanonical);
    REQUIRE(p.found);
    CHECK(p.decl == myos_decl());
    CHECK(p.problems.empty());
    CHECK_EQ(p.line, 5);
    std::string src = kCanonical;
    CHECK_EQ(src.substr(p.begin, 13), std::string("OSEF_HARDWARE"));
    CHECK_EQ(src[p.end - 1], ';');
    CHECK_EQ(src.substr(p.end, 2), std::string("\n\n"));
}

TEST(hwdecl_parses_the_myos_example) {
    std::string text;
    {
        std::FILE* f = std::fopen((test::repo_root() / "examples/myos/main.cpp").string().c_str(), "rb");
        REQUIRE(f != nullptr);
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    HardwareParse p = parse_hardware_decl(text);
    REQUIRE(p.found);
    CHECK(p.problems.empty());
    CHECK_EQ(p.decl.name, std::string("MyOS"));
    CHECK_EQ(p.decl.arch, std::string("x86_64"));
    CHECK(p.decl.firmware == "UEFI" || p.decl.firmware == "BIOS");
    CHECK(p.decl.serial || p.decl.framebuffer);
}

TEST(hwdecl_defaults_when_only_required_settings) {
    HardwareParse p = parse_body(kMinimalBody);
    REQUIRE(p.found);
    CHECK(p.problems.empty());
    CHECK_EQ(p.decl.name, std::string("OSEF"));
    CHECK_EQ(p.decl.memory_mb, 128u);
    CHECK_EQ(p.decl.serial_baud, 115200u);
    CHECK_EQ(p.decl.fb_width, 0u);
    CHECK_EQ(p.decl.boot_timeout_s, 0u);
    CHECK(p.decl.serial);
    CHECK(!p.decl.framebuffer);
    CHECK(!p.decl.ps2_keyboard);
    CHECK_EQ(p.decl.firmware, std::string("BIOS"));
}

TEST(hwdecl_messy_whitespace_and_comments_inside) {
    HardwareParse p = parse_hardware_decl(
        "OSEF_HARDWARE /* a */ ( /* b */ ::osef :: Hardware ( ) // c\n"
        "   . name ( /* the name */ \"Mess\" )\n"
        "\t.arch(\n  ::osef::Arch :: x86_64 // only one\n)\n"
        "  .firmware( osef::Firmware::/*x*/UEFI )\n"
        "  .devices( ( osef::Hw::Serial /* | osef::Hw::PS2Keyboard */ ) |\n"
        "            // osef::Hw::Framebuffer\n"
        "            ::osef::Hw::PS2Keyboard )\n"
        "  .memory_mb( 256 ) ) /* after */ ;\n");
    REQUIRE(p.found);
    CHECK(p.problems.empty());
    CHECK_EQ(p.decl.name, std::string("Mess"));
    CHECK_EQ(p.decl.arch, std::string("x86_64"));
    CHECK_EQ(p.decl.firmware, std::string("UEFI"));
    CHECK(p.decl.serial);
    CHECK(!p.decl.framebuffer);
    CHECK(p.decl.ps2_keyboard);
    CHECK_EQ(p.decl.memory_mb, 256u);
}

TEST(hwdecl_calls_are_order_independent) {
    HardwareParse a = parse_body(
        "osef::Hardware().memory_mb(64).devices(osef::Hw::Framebuffer).firmware(osef::Firmware::BIOS)"
        ".name(\"X\").arch(osef::Arch::x86_64).boot_timeout(3).resolution(800, 600).serial_baud(9600)");
    HardwareParse b = parse_body(
        "osef::Hardware().serial_baud(9600).resolution(800, 600).boot_timeout(3).arch(osef::Arch::x86_64)"
        ".name(\"X\").firmware(osef::Firmware::BIOS).devices(osef::Hw::Framebuffer).memory_mb(64)");
    REQUIRE(a.found);
    REQUIRE(b.found);
    CHECK(a.problems.empty());
    CHECK(a.decl == b.decl);
    CHECK_EQ(a.decl.fb_width, 800u);
    CHECK_EQ(a.decl.fb_height, 600u);
    CHECK_EQ(a.decl.serial_baud, 9600u);
    CHECK_EQ(a.decl.boot_timeout_s, 3u);
}

TEST(hwdecl_repeated_call_last_wins) {
    HardwareParse p = parse_body(kMinimalBody + ".memory_mb(64).memory_mb(512)");
    CHECK_EQ(p.decl.memory_mb, 512u);
}

TEST(hwdecl_unqualified_names) {
    HardwareParse p = parse_body("Hardware().arch(Arch::x86_64).firmware(Firmware::UEFI).devices(Hw::kAll)");
    REQUIRE(p.found);
    CHECK(p.problems.empty());
    CHECK(p.decl.serial && p.decl.framebuffer && p.decl.ps2_keyboard);
}

TEST(hwdecl_hex_octal_binary_and_suffixes) {
    CHECK_EQ(parse_body(kMinimalBody + ".memory_mb(0x100)").decl.memory_mb, 256u);
    CHECK_EQ(parse_body(kMinimalBody + ".memory_mb(0X40u)").decl.memory_mb, 64u);
    CHECK_EQ(parse_body(kMinimalBody + ".memory_mb(512U)").decl.memory_mb, 512u);
    CHECK_EQ(parse_body(kMinimalBody + ".memory_mb(1'024)").decl.memory_mb, 1024u);
    CHECK_EQ(parse_body(kMinimalBody + ".memory_mb(0100)").decl.memory_mb, 64u);
    CHECK_EQ(parse_body(kMinimalBody + ".memory_mb(0b1000000)").decl.memory_mb, 64u);
    CHECK_EQ(parse_body(kMinimalBody + ".serial_baud(0x2580)").decl.serial_baud, 9600u);
    CHECK_EQ(parse_body(kMinimalBody + ".boot_timeout(5u)").decl.boot_timeout_s, 5u);
    HardwareParse r = parse_body(kMinimalBody + ".resolution(0x500, 720U)");
    CHECK_EQ(r.decl.fb_width, 1280u);
    CHECK_EQ(r.decl.fb_height, 720u);
    CHECK(r.problems.empty());
}

TEST(hwdecl_kall_and_numeric_devices) {
    HardwareParse a = parse_body("osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                                 ".devices(osef::Hw::kAll)");
    CHECK(a.decl.serial && a.decl.framebuffer && a.decl.ps2_keyboard);
    HardwareParse b = parse_body("osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                                 ".devices(3)");
    CHECK(b.decl.serial && b.decl.framebuffer && !b.decl.ps2_keyboard);
    CHECK(b.problems.empty());
}

TEST(hwdecl_zero_devices_reports_missing_console) {
    HardwareParse p = parse_body("osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                                 ".devices(0)");
    REQUIRE(p.found);
    CHECK(!p.decl.serial && !p.decl.framebuffer && !p.decl.ps2_keyboard);
    CHECK(has_problem(p, "OSEF_HARDWARE: enable at least one console device: .devices(osef::Hw::Serial) and/or "
                         "osef::Hw::Framebuffer"));
    CHECK(!hardware_decl_has_unparsed_code("OSEF_HARDWARE(osef::Hardware().devices(0));"));
}

TEST(hwdecl_keyboard_only_is_not_a_console) {
    HardwareParse p = parse_body("osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                                 ".devices(osef::Hw::PS2Keyboard)");
    CHECK(has_problem(p, "enable at least one console device"));
}

TEST(hwdecl_unknown_call_is_reported_and_parsing_continues) {
    std::string src = "OSEF_HARDWARE(osef::Hardware()\n"
                      "    .arch(osef::Arch::x86_64)\n"
                      "    .cores(4)\n"
                      "    .firmware(osef::Firmware::BIOS)\n"
                      "    .devices(osef::Hw::Serial)\n"
                      "    .memory_mb(64));\n";
    HardwareParse p = parse_hardware_decl(src);
    REQUIRE(p.found);
    REQUIRE_EQ(p.problems.size(), 1u);
    CHECK(has_problem(p, "line 3"));
    CHECK(has_problem(p, ".cores("));
    CHECK_EQ(p.decl.firmware, std::string("BIOS"));
    CHECK_EQ(p.decl.memory_mb, 64u);
    CHECK(hardware_decl_has_unparsed_code(src));
}

TEST(hwdecl_unparsable_argument_keeps_default) {
    std::string src = "OSEF_HARDWARE(osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                      ".devices(osef::Hw::Serial).memory_mb(kMemory).serial_baud(\"fast\"));";
    HardwareParse p = parse_hardware_decl(src);
    REQUIRE(p.found);
    CHECK_EQ(p.decl.memory_mb, 128u);
    CHECK_EQ(p.decl.serial_baud, 115200u);
    CHECK(has_problem(p, ".memory_mb("));
    CHECK(has_problem(p, ".serial_baud("));
    CHECK(hardware_decl_has_unparsed_code(src));
    CHECK(!hardware_decl_has_unparsed_code(kCanonical));
}

TEST(hwdecl_unknown_device_flag) {
    HardwareParse p = parse_body("osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                                 ".devices(osef::Hw::Serial | osef::Hw::Mouse)");
    CHECK(p.decl.serial);
    CHECK(has_problem(p, "osef::Hw::Mouse"));
    HardwareParse q = parse_body("osef::Hardware().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                                 ".devices(0x41)");  // bit 6 is not a device
    CHECK(q.decl.serial);
    CHECK(has_problem(q, "unknown device flag"));
}

TEST(hwdecl_missing_firmware_uses_static_assert_wording) {
    HardwareParse p = parse_body("osef::Hardware().arch(osef::Arch::x86_64).devices(osef::Hw::Serial)");
    REQUIRE(p.found);
    CHECK_EQ(p.decl.firmware, std::string());
    REQUIRE_EQ(p.problems.size(), 1u);
    CHECK_EQ(p.problems[0], std::string("OSEF_HARDWARE: choose the boot firmware: .firmware(osef::Firmware::BIOS) "
                                        "or .firmware(osef::Firmware::UEFI)"));
}

TEST(hwdecl_missing_arch_uses_static_assert_wording) {
    HardwareParse p = parse_body("osef::Hardware().firmware(osef::Firmware::UEFI).devices(osef::Hw::Serial)");
    REQUIRE_EQ(p.problems.size(), 1u);
    CHECK_EQ(p.problems[0], std::string("OSEF_HARDWARE: choose a CPU architecture: .arch(osef::Arch::x86_64)"));
}

TEST(hwdecl_missing_everything) {
    HardwareParse p = parse_body("osef::Hardware()");
    REQUIRE(p.found);
    CHECK_EQ(p.problems.size(), 3u);
    CHECK(has_problem(p, "choose a CPU architecture"));
    CHECK(has_problem(p, "choose the boot firmware"));
    CHECK(has_problem(p, "enable at least one console device"));
}

TEST(hwdecl_unspecified_enums_count_as_missing) {
    HardwareParse p = parse_body("osef::Hardware().arch(osef::Arch::Unspecified)"
                                 ".firmware(osef::Firmware::Unspecified).devices(osef::Hw::Serial)");
    CHECK_EQ(p.decl.arch, std::string());
    CHECK_EQ(p.decl.firmware, std::string());
    CHECK(has_problem(p, "choose a CPU architecture"));
    CHECK(has_problem(p, "choose the boot firmware"));
}

TEST(hwdecl_value_checks_match_the_compiler) {
    CHECK(has_problem(parse_body(kMinimalBody + ".memory_mb(16)"), ".memory_mb() must be at least 32"));
    CHECK(has_problem(parse_body(kMinimalBody + ".resolution(1024, 0)"), ".resolution() needs both"));
    CHECK(has_problem(parse_body(kMinimalBody + ".serial_baud(1000)"), "must divide 115200"));
    CHECK(parse_body(kMinimalBody + ".serial_baud(28800)").problems.empty());
    CHECK(has_problem(parse_body(kMinimalBody + ".name(\"\")"), "must not be empty"));
    CHECK(has_problem(parse_body(kMinimalBody + ".name(\"0123456789012345678901234567890123\")"), "too long"));
    CHECK(parse_body(kMinimalBody + ".name(\"0123456789012345678901234567890\")").problems.empty());  // 31
    CHECK(has_problem(parse_body(kMinimalBody + ".firmware(osef::Firmware::Coreboot)"), "firmware must be"));
    CHECK(has_problem(parse_body(kMinimalBody + ".arch(osef::Arch::arm64)"), "unsupported architecture"));
}

TEST(hwdecl_not_found_without_declaration) {
    HardwareParse p = parse_hardware_decl("#include <osef/osef.h>\nint main() { return 0; }\n");
    CHECK(!p.found);
    CHECK(p.problems.empty());
    CHECK(p.decl == HardwareDecl{});
    CHECK(!parse_hardware_decl("").found);
}

TEST(hwdecl_commented_out_declaration_is_ignored) {
    std::string src = "// OSEF_HARDWARE(osef::Hardware().firmware(osef::Firmware::BIOS));\n"
                      "/* OSEF_HARDWARE(osef::Hardware()\n"
                      "     .firmware(osef::Firmware::BIOS)); */\n"
                      "const char* s = \"OSEF_HARDWARE(osef::Hardware());\";\n"
                      "const char* r = R\"x(OSEF_HARDWARE(osef::Hardware());)x\";\n"
                      "int main() {}\n";
    CHECK(!parse_hardware_decl(src).found);

    std::string with = src + "OSEF_HARDWARE(" + kMinimalBody + ");\n";
    HardwareParse p = parse_hardware_decl(with);
    REQUIRE(p.found);
    CHECK_EQ(p.line, 7);
    CHECK_EQ(p.decl.firmware, std::string("BIOS"));
}

TEST(hwdecl_macro_definition_is_not_a_declaration) {
    // hardware.h itself: OSEF_HARDWARE(...) is followed by a backslash, not ';'.
    std::string src = "#define OSEF_HARDWARE(...) \\\n    namespace osef::detail {}\n"
                      "void f() { OSEF_HARDWARE_ATTR(x); }\n";
    CHECK(!parse_hardware_decl(src).found);
}

TEST(hwdecl_string_with_escaped_quotes_and_parens) {
    HardwareParse p = parse_body(kMinimalBody + ".name(\"My \\\"(OS)\\\" ;) \\\\ //x /*y*/\")");
    REQUIRE(p.found);
    CHECK_EQ(p.decl.name, std::string("My \"(OS)\" ;) \\ //x /*y*/"));
    CHECK_EQ(p.decl.firmware, std::string("BIOS"));
    CHECK(p.problems.empty());
}

TEST(hwdecl_string_escapes_and_concatenation) {
    CHECK_EQ(parse_body(kMinimalBody + ".name(\"A\\tB\\x41\\101\\u00e9\")").decl.name, std::string("A\tBAA\xC3\xA9"));
    CHECK_EQ(parse_body(kMinimalBody + ".name(\"My\" \"OS\")").decl.name, std::string("MyOS"));
    CHECK_EQ(parse_body(kMinimalBody + ".name(R\"(raw \"name\")\")").decl.name, std::string("raw \"name\""));
    CHECK_EQ(parse_body(kMinimalBody + ".name(R\"d(a)\"b)d\")").decl.name, std::string("a)\"b"));
    HardwareParse wide = parse_body(kMinimalBody + ".name(L\"wide\")");
    CHECK(has_problem(wide, ".name("));
}

TEST(hwdecl_char_literals_do_not_confuse_the_scanner) {
    std::string src = "char a = '\"'; char b = '\\''; char c = ')';\n"
                      "OSEF_HARDWARE(" + kMinimalBody + ");\n"
                      "char d = '(';\n";
    HardwareParse p = parse_hardware_decl(src);
    REQUIRE(p.found);
    CHECK(p.problems.empty());
    CHECK_EQ(p.line, 2);
}

TEST(hwdecl_bad_base_is_reported) {
    HardwareParse p = parse_body("make_hw().arch(osef::Arch::x86_64).firmware(osef::Firmware::UEFI)"
                                 ".devices(osef::Hw::Serial)");
    REQUIRE(p.found);
    CHECK(has_problem(p, "must start with osef::Hardware()"));
    CHECK_EQ(p.decl.firmware, std::string("UEFI"));  // still reads the calls
}

TEST(hwdecl_second_declaration_is_a_problem) {
    std::string src = "OSEF_HARDWARE(" + kMinimalBody + ");\nOSEF_HARDWARE(" + kMinimalBody + ");\n";
    HardwareParse p = parse_hardware_decl(src);
    REQUIRE(p.found);
    CHECK_EQ(p.line, 1);
    CHECK(has_problem(p, "line 2"));
}

TEST(hwdecl_crlf_input) {
    std::string src = "// x\r\n#include <osef/hardware.h>\r\n\r\nOSEF_HARDWARE(osef::Hardware()\r\n"
                      "    .name(\"C\")\r\n    .arch(osef::Arch::x86_64)\r\n    .firmware(osef::Firmware::UEFI)\r\n"
                      "    .devices(osef::Hw::Framebuffer) // c\r\n    .memory_mb(64));\r\nint main() {}\r\n";
    HardwareParse p = parse_hardware_decl(src);
    REQUIRE(p.found);
    CHECK(p.problems.empty());
    CHECK_EQ(p.line, 4);
    CHECK_EQ(p.decl.name, std::string("C"));
    CHECK(p.decl.framebuffer);
    CHECK_EQ(p.decl.memory_mb, 64u);
    CHECK_EQ(src.substr(p.end, 2), std::string("\r\n"));

    HardwareDecl d = p.decl;
    d.firmware = "BIOS";
    std::string out = apply_hardware_decl(src, d);
    CHECK(out.find("\r\n    .firmware(osef::Firmware::BIOS)\r\n") != std::string::npos);
    CHECK(parse_hardware_decl(out).decl == d);
    // No bare LF was introduced.
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == '\n') CHECK(i > 0 && out[i - 1] == '\r');
    CHECK(out.starts_with(src.substr(0, p.begin)));
    CHECK(out.ends_with("\r\nint main() {}\r\n"));
}

// ---- generation ---------------------------------------------------------------------

TEST(hwdecl_generate_canonical) {
    CHECK_EQ(generate_hardware_decl(myos_decl()),
             std::string("OSEF_HARDWARE(osef::Hardware()\n"
                         "    .name(\"MyOS\")\n"
                         "    .arch(osef::Arch::x86_64)\n"
                         "    .firmware(osef::Firmware::UEFI)\n"
                         "    .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)\n"
                         "    .memory_mb(128));"));
}

TEST(hwdecl_round_trips_rtc_and_pci_flags) {
    HardwareDecl decl = myos_decl();
    decl.rtc = true;
    decl.pci = true;
    const std::string generated = generate_hardware_decl(decl);
    CHECK(generated.find("osef::Hw::RTC") != std::string::npos);
    CHECK(generated.find("osef::Hw::PCI") != std::string::npos);
    const HardwareParse parsed = parse_hardware_decl(generated);
    CHECK(parsed.problems.empty());
    CHECK(parsed.decl == decl);
}

TEST(hwdecl_generate_optional_calls_and_order) {
    HardwareDecl d = myos_decl();
    d.serial = false;
    d.framebuffer = true;
    d.ps2_keyboard = true;
    d.serial_baud = 9600;
    d.fb_width = 1024;
    d.fb_height = 768;
    d.boot_timeout_s = 5;
    std::string g = generate_hardware_decl(d);
    CHECK(g.find(".devices(osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)") != std::string::npos);
    CHECK(g.find("    .serial_baud(9600)\n    .resolution(1024, 768)\n    .boot_timeout(5));") != std::string::npos);
    CHECK(g.find("\n") != std::string::npos);
    CHECK(!g.ends_with("\n"));
}

TEST(hwdecl_generate_omits_missing_required_settings) {
    HardwareDecl d;
    d.name = "Bare";
    std::string g = generate_hardware_decl(d);
    CHECK(g.find(".arch") == std::string::npos);
    CHECK(g.find(".firmware") == std::string::npos);
    CHECK(g.find(".devices") == std::string::npos);
    CHECK(g.find(".memory_mb(128)") != std::string::npos);
    HardwareParse p = parse_hardware_decl(g);
    CHECK(p.decl == d);
    CHECK_EQ(p.problems.size(), 3u);
}

TEST(hwdecl_generate_escapes_name) {
    HardwareDecl d = myos_decl();
    d.name = "a\"b\\c\nd\x01" "7";
    std::string g = generate_hardware_decl(d);
    CHECK(g.find(".name(\"a\\\"b\\\\c\\nd\\0017\")") != std::string::npos);
    CHECK_EQ(parse_hardware_decl(g).decl.name, d.name);
}

// ---- apply ---------------------------------------------------------------------------

TEST(hwdecl_apply_replaces_only_the_declaration) {
    HardwareDecl d = myos_decl();
    d.firmware = "BIOS";
    d.ps2_keyboard = false;
    std::string src = kCanonical;
    HardwareParse before = parse_hardware_decl(src);
    std::string out = apply_hardware_decl(src, d);
    CHECK_EQ(out.substr(0, before.begin), src.substr(0, before.begin));
    CHECK(out.ends_with(src.substr(before.end)));
    HardwareParse after = parse_hardware_decl(out);
    CHECK(after.decl == d);
    CHECK_EQ(after.line, before.line);
    CHECK(out.find(".devices(osef::Hw::Serial | osef::Hw::Framebuffer)\n") != std::string::npos);
    // Applying the same declaration again is a no-op.
    CHECK_EQ(apply_hardware_decl(out, d), out);
    CHECK_EQ(apply_hardware_decl(src, myos_decl()), src);
}

TEST(hwdecl_apply_inserts_after_existing_include) {
    std::string src = "// Tiny\n#include <osef/hardware.h>\n#include <osef/osef.h>\nint main() {}\n";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK_EQ(out, "// Tiny\n#include <osef/hardware.h>\n\n" + generate_hardware_decl(myos_decl()) +
                      "\n\n#include <osef/osef.h>\nint main() {}\n");
    CHECK(parse_hardware_decl(out).decl == myos_decl());
}

TEST(hwdecl_apply_inserts_after_include_followed_by_blank_line) {
    std::string src = "#include <osef/hardware.h>\n\n#include <osef/osef.h>\n";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK_EQ(out, "#include <osef/hardware.h>\n\n" + generate_hardware_decl(myos_decl()) +
                      "\n\n#include <osef/osef.h>\n");
}

TEST(hwdecl_apply_inserts_after_include_at_eof) {
    std::string src = "#include \"osef/hardware.h\"";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK_EQ(out, "#include \"osef/hardware.h\"\n\n" + generate_hardware_decl(myos_decl()) + "\n");
}

TEST(hwdecl_apply_ignores_commented_include) {
    std::string src = "// #include <osef/hardware.h>\n\nint main() {}\n";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK_EQ(out, "// #include <osef/hardware.h>\n\n#include <osef/hardware.h>\n\n" +
                      generate_hardware_decl(myos_decl()) + "\n\nint main() {}\n");
}

TEST(hwdecl_apply_adds_include_after_leading_comment_block) {
    std::string src = "// MyOS: a tour.\n"
                      "//\n"
                      "/* more */\n"
                      "\n"
                      "// about osef.h\n"
                      "#include <osef/osef.h>\n"
                      "int main() {}\n";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK_EQ(out, "// MyOS: a tour.\n//\n/* more */\n\n#include <osef/hardware.h>\n\n" +
                      generate_hardware_decl(myos_decl()) +
                      "\n\n// about osef.h\n#include <osef/osef.h>\nint main() {}\n");
    CHECK(parse_hardware_decl(out).decl == myos_decl());
}

TEST(hwdecl_apply_adds_include_after_comment_block_without_blank_line) {
    std::string src = "// header\n#include <osef/osef.h>\n";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK_EQ(out, "// header\n\n#include <osef/hardware.h>\n\n" + generate_hardware_decl(myos_decl()) +
                      "\n\n#include <osef/osef.h>\n");
}

TEST(hwdecl_apply_adds_include_at_top_without_comments) {
    std::string src = "#include <osef/osef.h>\nint main() {}\n";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK_EQ(out, "#include <osef/hardware.h>\n\n" + generate_hardware_decl(myos_decl()) + "\n\n" + src);
    CHECK_EQ(apply_hardware_decl("", myos_decl()),
             "#include <osef/hardware.h>\n\n" + generate_hardware_decl(myos_decl()) + "\n");
}

TEST(hwdecl_apply_insert_with_crlf) {
    std::string src = "// header\r\n\r\nint main() {}\r\n";
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK(out.starts_with("// header\r\n\r\n#include <osef/hardware.h>\r\n\r\nOSEF_HARDWARE(osef::Hardware()\r\n"));
    CHECK(out.ends_with(".memory_mb(128));\r\n\r\nint main() {}\r\n"));
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == '\n') CHECK(i > 0 && out[i - 1] == '\r');
    CHECK(parse_hardware_decl(out).decl == myos_decl());
}

TEST(hwdecl_apply_preserves_unicode_and_tabs_elsewhere) {
    std::string src = "// caf\xC3\xA9 \t\n#include <osef/hardware.h>\n\nOSEF_HARDWARE(" + kMinimalBody +
                      ");\t// tail\n\xE2\x9C\x93\n";
    HardwareParse p = parse_hardware_decl(src);
    std::string out = apply_hardware_decl(src, myos_decl());
    CHECK(out.starts_with(src.substr(0, p.begin)));
    CHECK(out.ends_with(";\t// tail\n\xE2\x9C\x93\n"));
}

// ---- round trip ----------------------------------------------------------------------

TEST(hwdecl_round_trip_fuzz) {
    // Deterministic xorshift so failures are reproducible.
    uint64_t state = 0x9E3779B97F4A7C15ull;
    auto next = [&]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    const char* names[] = {"MyOS", "TinyOS", "a", "With space", "q\"uote", "back\\slash", "tab\there",
                           "nl\nline", "caf\xC3\xA9", "(parens)", ";semi;", "//c", "/*c*/", ("\x01\x7F" "9"),
                           "0123456789012345678901234567890"};
    const char* arches[] = {"", "x86_64"};
    const char* firmwares[] = {"", "BIOS", "UEFI"};
    const uint32_t bauds[] = {115200, 57600, 38400, 28800, 19200, 9600, 1, 7};
    const char* sources[] = {
        "",
        "int main() {}\n",
        "// lead\n//\n\n#include <osef/osef.h>\nint main() {}\n",
        "/* block */\n#include <osef/hardware.h>\n#include <osef/osef.h>\n",
        kCanonical,
        "// x\r\n#include <osef/hardware.h>\r\n\r\nint main() {}\r\n",
        "// OSEF_HARDWARE(osef::Hardware());\nconst char* s = \"OSEF_HARDWARE(x);\";\n",
        "#include <osef/hardware.h>\nOSEF_HARDWARE(osef::Hardware().cores(2));\n",
    };
    int cases = 0;
    for (int iter = 0; iter < 600; ++iter) {
        HardwareDecl d;
        d.name = names[next() % (sizeof names / sizeof names[0])];
        d.arch = arches[next() % 2];
        d.firmware = firmwares[next() % 3];
        uint64_t devs = next();
        d.serial = devs & 1;
        d.framebuffer = devs & 2;
        d.ps2_keyboard = devs & 4;
        switch (next() % 4) {
        case 0: d.memory_mb = 128; break;
        case 1: d.memory_mb = static_cast<uint32_t>(32 + 32 * (next() % 2048)); break;
        case 2: d.memory_mb = static_cast<uint32_t>(next() % 64); break;
        default: d.memory_mb = static_cast<uint32_t>(next()); break;
        }
        d.serial_baud = bauds[next() % (sizeof bauds / sizeof bauds[0])];
        if (next() % 3 == 0) {
            d.fb_width = static_cast<uint32_t>(next() % 4000);
            d.fb_height = static_cast<uint32_t>(next() % 3000);
        }
        if (next() % 4 == 0) d.boot_timeout_s = static_cast<uint32_t>(next() % 31);
        const std::string src = sources[next() % (sizeof sources / sizeof sources[0])];

        std::string out = apply_hardware_decl(src, d);
        HardwareParse p = parse_hardware_decl(out);
        CHECK(p.found);
        CHECK(p.decl == d);
        if (!(p.decl == d)) {
            test::report_failure(__FILE__, __LINE__, "round trip failed for:\n" + out);
            break;
        }
        CHECK(!hardware_decl_has_unparsed_code(out));
        CHECK_EQ(parse_hardware_decl(generate_hardware_decl(d)).decl == d, true);
        // Idempotent.
        CHECK_EQ(apply_hardware_decl(out, d), out);
        ++cases;
    }
    CHECK_EQ(cases, 600);
}
