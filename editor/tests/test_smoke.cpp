// Proves the harness links and runs; real tests live next to it.

#include "test_main.h"

#include <string>

TEST(smoke_harness_runs) {
    CHECK(1 + 1 == 2);
    CHECK_EQ(std::string("osef"), "osef");
    CHECK_EQ(size_t{3}, 3);
    CHECK_NE(1, 2);
}

TEST(smoke_temp_dir_is_empty) {
    const auto dir = osef::editor::test::temp_dir("smoke");
    REQUIRE(std::filesystem::is_directory(dir));
    CHECK(std::filesystem::is_empty(dir));
}
