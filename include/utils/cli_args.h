#pragma once

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace june::cli {

// Parse a whole-number command-line value. The entire text must be an
// optional '-' followed by digits, and the number must lie in [min, max].
// Anything else throws with a message naming the option and the bad text,
// so a typo never silently falls back to the config value.
inline long long parseInteger(const std::string& option,
                              const std::string& text, long long min,
                              long long max) {
  const std::string range =
      "[" + std::to_string(min) + ", " + std::to_string(max) + "]";
  const size_t first_digit = (!text.empty() && text[0] == '-') ? 1 : 0;
  bool all_digits = text.size() > first_digit;
  for (size_t i = first_digit; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') all_digits = false;
  }
  if (!all_digits) {
    throw std::invalid_argument("--" + option + " must be a whole number in " +
                                range + ", got '" + text + "'");
  }
  errno = 0;
  const long long value = std::strtoll(text.c_str(), nullptr, 10);
  if (errno == ERANGE || value < min || value > max) {
    throw std::invalid_argument("--" + option + " must be in " + range +
                                ", got '" + text + "'");
  }
  return value;
}

}  // namespace june::cli
