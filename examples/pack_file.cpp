#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <vector>

#include "ae_compression/ae_compression.hpp"

namespace {

std::vector<std::uint8_t> ReadFile(char const* path) {
  auto file = std::ifstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open input file"};
  }
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void WriteFile(char const* path, std::vector<std::uint8_t> const& data) {
  auto file = std::ofstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open output file"};
  }
  file.write(reinterpret_cast<char const*>(data.data()),
             static_cast<std::streamsize>(data.size()));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: ae-compression-pack <input> <output>\n";
    return 2;
  }

  try {
    auto input = ReadFile(argv[1]);

    auto started = std::chrono::steady_clock::now();
    auto model = ae::compression::Compress(input);
    auto frame = ae::compression::PackDictionary(model);
    auto finished = std::chrono::steady_clock::now();

    auto decoded = ae::compression::Decode(frame);
    if (decoded != input) {
      std::cerr << "roundtrip verification failed\n";
      return 1;
    }

    WriteFile(argv[2], frame);

    auto stats = ae::compression::Analyze(model, input.size());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                    started);
    std::cout << "input bytes: " << input.size() << "\n";
    std::cout << "serialized model bytes: " << frame.size() << "\n";
    std::cout << "rules: " << stats.rule_count << "\n";
    std::cout << "top-level symbols: " << stats.top_level_symbols << "\n";
    std::cout << "estimated entropy bytes: " << stats.estimated_total_bytes
              << "\n";
    std::cout << "model build time: " << ms.count() << " ms\n";
  } catch (std::exception const& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
