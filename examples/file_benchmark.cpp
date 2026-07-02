#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <zlib.h>

#include "ae_compression/ae_compression.hpp"

namespace {

std::vector<std::uint8_t> ReadFile(char const* path) {
  auto file = std::ifstream{path, std::ios::binary};
  if (!file) {
    throw std::runtime_error{"failed to open input file"};
  }
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint8_t> ZlibCompress(std::span<std::uint8_t const> input) {
  auto out_size = compressBound(static_cast<uLong>(input.size()));
  auto out = std::vector<std::uint8_t>(out_size);

  auto result = compress2(reinterpret_cast<Bytef*>(out.data()), &out_size,
                          reinterpret_cast<Bytef const*>(input.data()),
                          static_cast<uLong>(input.size()), Z_BEST_COMPRESSION);
  if (result != Z_OK) {
    throw std::runtime_error{"zlib compress2 failed"};
  }
  out.resize(out_size);
  return out;
}

double Ratio(std::size_t input_size, std::size_t compressed_size) {
  if (compressed_size == 0) {
    return 0.0;
  }
  return static_cast<double>(input_size) / static_cast<double>(compressed_size);
}

std::string CsvName(char const* path) {
  auto name = std::filesystem::path{path}.filename().string();
  for (auto& ch : name) {
    if (ch == ',' || ch == '\n' || ch == '\r') {
      ch = '_';
    }
  }
  return name;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: ae-compression-file-benchmark <file> [file...]\n";
    return 2;
  }

  std::cout << "file,size,model_rules,top_symbols,total_symbols,"
               "estimate_bytes,estimate_ratio,packed_model_bytes,"
               "packed_model_ratio,zlib9_bytes,zlib9_ratio,model_ms,zlib_us\n"
            << std::flush;

  try {
    for (int i = 1; i < argc; ++i) {
      auto input = ReadFile(argv[i]);

      auto model_start = std::chrono::steady_clock::now();
      auto model = ae::compression::Compress(input);
      auto model_end = std::chrono::steady_clock::now();

      auto packed_model = ae::compression::PackDictionary(model);
      auto decoded = ae::compression::Decompress(model);
      if (decoded != input) {
        throw std::runtime_error{"model roundtrip failed"};
      }

      auto zlib_start = std::chrono::steady_clock::now();
      auto zlib = ZlibCompress(input);
      auto zlib_end = std::chrono::steady_clock::now();

      auto stats = ae::compression::Analyze(model, input.size());
      auto model_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(model_end -
                                                                model_start)
              .count();
      auto zlib_us =
          std::chrono::duration_cast<std::chrono::microseconds>(zlib_end -
                                                                zlib_start)
              .count();

      std::cout << CsvName(argv[i]) << ',' << input.size() << ','
                << stats.rule_count << ',' << stats.top_level_symbols << ','
                << stats.total_symbols << ',' << stats.estimated_total_bytes
                << ',' << Ratio(input.size(), stats.estimated_total_bytes)
                << ',' << packed_model.size() << ','
                << Ratio(input.size(), packed_model.size()) << ','
                << zlib.size() << ',' << Ratio(input.size(), zlib.size()) << ','
                << model_ms << ',' << zlib_us << '\n'
                << std::flush;
    }
  } catch (std::exception const& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
