#include <string>

#include "doctest.h"
#include "utils/cli_args.h"

using june::cli::parseInteger;

TEST_CASE("cli integer: plain whole numbers inside the range are accepted") {
  CHECK(parseInteger("days", "7", 1, 100) == 7);
  CHECK(parseInteger("days", "1", 1, 100) == 1);
  CHECK(parseInteger("days", "100", 1, 100) == 100);
  CHECK(parseInteger("seed", "0", 0, 4294967295LL) == 0);
  CHECK(parseInteger("seed", "4294967295", 0, 4294967295LL) == 4294967295LL);
}

TEST_CASE("cli integer: text that is not wholly a number is rejected") {
  for (const char* bad : {"", "abc", "7abc", "12x", "7.5", " 7", "7 ", "+7",
                          "-", "--7", "0x10", "1e3"}) {
    CHECK_THROWS_AS(parseInteger("days", bad, 0, 100), std::invalid_argument);
  }
}

TEST_CASE("cli integer: values outside the range are rejected") {
  CHECK_THROWS_AS(parseInteger("days", "0", 1, 100), std::invalid_argument);
  CHECK_THROWS_AS(parseInteger("days", "-3", 1, 100), std::invalid_argument);
  CHECK_THROWS_AS(parseInteger("days", "101", 1, 100), std::invalid_argument);
  CHECK_THROWS_AS(parseInteger("seed", "-5", 0, 4294967295LL),
                  std::invalid_argument);
  CHECK_THROWS_AS(parseInteger("seed", "4294967296", 0, 4294967295LL),
                  std::invalid_argument);
}

TEST_CASE("cli integer: numbers too large for 64 bits are rejected") {
  CHECK_THROWS_AS(parseInteger("seed", "99999999999999999999999", 0, 100),
                  std::invalid_argument);
}

TEST_CASE("cli integer: the error names the option and the offending text") {
  try {
    parseInteger("seed", "5000000000", 0, 4294967295LL);
    FAIL("expected an exception");
  } catch (const std::invalid_argument& e) {
    const std::string msg = e.what();
    CHECK(msg.find("--seed") != std::string::npos);
    CHECK(msg.find("5000000000") != std::string::npos);
    CHECK(msg.find("4294967295") != std::string::npos);
  }
}
