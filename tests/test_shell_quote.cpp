/**
 * @file test_shell_quote.cpp
 * @brief Unit tests for shell_quote(): the shell reads the result back as the original string.
 */

#include <cstdio>
#include <filesystem>
#include <string>

#include "gtest/gtest.h"

#include "trossen_sdk/utils/shell_quote.hpp"

using trossen::utils::shell_quote;

namespace {

/// What `sh` prints for `printf %s <word>`, i.e. the string the shell made of @p word.
std::string shell_reads(const std::string& word) {
  const std::string cmd = "printf %s " + word;
  std::string out;
  if (FILE* pipe = popen(cmd.c_str(), "r")) {
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
    pclose(pipe);
  }
  return out;
}

}  // namespace

TEST(ShellQuoteTest, WrapsInSingleQuotes) {
  EXPECT_EQ(shell_quote(std::string("a b")), "'a b'");
  EXPECT_EQ(shell_quote(std::string("it's")), "'it'\\''s'");
  EXPECT_EQ(shell_quote(std::filesystem::path("/data collection/x.mp4")),
            "'/data collection/x.mp4'");
}

TEST(ShellQuoteTest, ShellReadsBackTheOriginal) {
  for (const std::string s : {"/data collection/episode 1.mp4", "cam$(echo hi);`echo hi`",
                              "it's \"quoted\"", "*?[a]~", ""}) {
    EXPECT_EQ(shell_reads(shell_quote(s)), s) << s;
  }
}
