#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "ae_compression/experimental/grammar_lz.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes ReadFile(char const* path) {
  auto file = std::ifstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open AEGL fixture input"};
  }
  return {std::istreambuf_iterator<char>{file},
          std::istreambuf_iterator<char>{}};
}

void WriteFile(char const* path, Bytes const& data) {
  auto file = std::ofstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open AEGL fixture output"};
  }
  file.write(reinterpret_cast<char const*>(data.data()),
             static_cast<std::streamsize>(data.size()));
  if (!file) {
    throw std::runtime_error{"failed to write AEGL fixture"};
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: make-aegl-fixture <input> <output>\n";
    return 2;
  }
  auto input = ReadFile(argv[1]);
  auto options = ae::compression::experimental::GrammarLzOptions{};
  options.max_chain = 256;
  options.max_match = 65536;
  auto frame = ae::compression::experimental::PackPlainLz(input, options);
  if (ae::compression::experimental::DecodeGrammarLz(frame.bytes) != input) {
    throw std::runtime_error{"AEGL fixture roundtrip failed"};
  }
  WriteFile(argv[2], frame.bytes);
  std::cout << "aegl_plain," << frame.bytes.size() << ',' << input.size()
            << ',' << frame.stats.prefix_bytes << ','
            << frame.stats.command_bytes << '\n';
  return 0;
}
