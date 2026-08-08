#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "memory_tracker.hpp"

#if defined(CODEC_ZLIB)
#include <zlib.h>
#elif defined(CODEC_ZSTD)
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#elif defined(CODEC_BROTLI)
#include <brotli/decode.h>
#elif defined(CODEC_XZ)
#include <lzma.h>
#elif defined(CODEC_LZ4)
#include <lz4frame.h>
#endif

namespace {

struct Buffer {
  std::uint8_t* data = nullptr;
  std::size_t size = 0;
};

Buffer ReadFile(char const* path) {
  auto* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return {};
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return {};
  }
  auto const end = std::ftell(file);
  if (end < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return {};
  }
  auto const size = static_cast<std::size_t>(end);
  auto* data = static_cast<std::uint8_t*>(std::malloc(size == 0 ? 1 : size));
  if (data == nullptr) {
    std::fclose(file);
    return {};
  }
  auto const read = size == 0 ? 0 : std::fread(data, 1, size, file);
  std::fclose(file);
  if (read != size) {
    std::free(data);
    return {};
  }
  return Buffer{data, size};
}

void Release(Buffer& buffer) {
  std::free(buffer.data);
  buffer = {};
}

bool ReadUvarint(std::uint8_t const* data, std::size_t size,
                 std::size_t& position, std::size_t& value) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  while (position < size && shift <= 63) {
    auto const byte = data[position++];
    result |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
    if ((byte & 0x80U) == 0) {
      if (result > std::numeric_limits<std::size_t>::max()) {
        return false;
      }
      value = static_cast<std::size_t>(result);
      return true;
    }
    shift += 7;
  }
  return false;
}

struct AeglHeader {
  std::size_t prefix_size = 0;
  std::size_t output_size = 0;
  std::size_t command_position = 0;
};

bool ParseAeglHeader(std::uint8_t const* data, std::size_t size,
                     AeglHeader& header) {
  static constexpr std::uint8_t magic[] = {'A', 'E', 'G', 'L'};
  if (size < 5 || std::memcmp(data, magic, sizeof(magic)) != 0 ||
      data[4] != 0) {
    return false;
  }
  std::size_t position = 5;
  if (!ReadUvarint(data, size, position, header.prefix_size) ||
      !ReadUvarint(data, size, position, header.output_size)) {
    return false;
  }
  if (header.prefix_size >
      std::numeric_limits<std::size_t>::max() - header.output_size) {
    return false;
  }
  header.command_position = position;
  return true;
}

bool DecodeAegl(std::uint8_t const* input, std::size_t input_size,
                std::uint8_t* history, std::size_t history_size,
                AeglHeader const& header) {
  auto const total_size = header.prefix_size + header.output_size;
  if (history_size < total_size) {
    return false;
  }
  auto position = header.command_position;
  std::size_t produced = 0;
  while (produced < total_size) {
    if (position >= input_size) {
      return false;
    }
    auto const token = input[position++];
    if (token <= 0x7fU) {
      auto const length = static_cast<std::size_t>(token) + 1;
      if (length > total_size - produced || length > input_size - position) {
        return false;
      }
      std::memcpy(history + produced, input + position, length);
      produced += length;
      position += length;
      continue;
    }

    std::size_t length = 0;
    if (token <= 0xbfU) {
      length = 3 + static_cast<std::size_t>(token - 0x80U);
    } else if (token == 0xc0U) {
      if (!ReadUvarint(input, input_size, position, length) || length == 0 ||
          length > total_size - produced ||
          length > input_size - position) {
        return false;
      }
      std::memcpy(history + produced, input + position, length);
      produced += length;
      position += length;
      continue;
    } else if (token == 0xc1U) {
      if (!ReadUvarint(input, input_size, position, length)) {
        return false;
      }
    } else {
      return false;
    }

    std::size_t encoded_distance = 0;
    if (!ReadUvarint(input, input_size, position, encoded_distance) ||
        encoded_distance == std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    auto const distance = encoded_distance + 1;
    if (length < 3 || length > total_size - produced ||
        distance > produced) {
      return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
      history[produced] = history[produced - distance];
      ++produced;
    }
  }
  return position == input_size;
}

#if defined(CODEC_ZLIB)
voidpf ZlibAllocate(voidpf, uInt items, uInt size) {
  return decoder_memory::Callocate(items, size);
}

void ZlibFree(voidpf, voidpf pointer) {
  decoder_memory::Deallocate(pointer);
}

bool DecodeCodec(std::uint8_t const* input, std::size_t input_size,
                 std::uint8_t* output, std::size_t output_size) {
  if (input_size > std::numeric_limits<uInt>::max() ||
      output_size > std::numeric_limits<uInt>::max()) {
    return false;
  }
  auto stream = z_stream{};
  stream.zalloc = ZlibAllocate;
  stream.zfree = ZlibFree;
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
    return false;
  }
  stream.next_in = const_cast<Bytef*>(
      reinterpret_cast<Bytef const*>(input));
  stream.avail_in = static_cast<uInt>(input_size);
  stream.next_out = reinterpret_cast<Bytef*>(output);
  stream.avail_out = static_cast<uInt>(output_size);
  auto const result = inflate(&stream, Z_FINISH);
  auto const success = result == Z_STREAM_END &&
                       stream.total_in == input_size &&
                       stream.total_out == output_size;
  inflateEnd(&stream);
  return success;
}
#elif defined(CODEC_ZSTD)
void* ZstdAllocate(void*, std::size_t size) {
  return decoder_memory::Allocate(size);
}

void ZstdFree(void*, void* pointer) {
  decoder_memory::Deallocate(pointer);
}

bool DecodeCodec(std::uint8_t const* input, std::size_t input_size,
                 std::uint8_t* output, std::size_t output_size) {
  auto memory = ZSTD_customMem{ZstdAllocate, ZstdFree, nullptr};
  auto* context = ZSTD_createDCtx_advanced(memory);
  if (context == nullptr) {
    return false;
  }
  auto const result =
      ZSTD_decompressDCtx(context, output, output_size, input, input_size);
  auto const success = !ZSTD_isError(result) && result == output_size;
  ZSTD_freeDCtx(context);
  return success;
}
#elif defined(CODEC_BROTLI)
void* BrotliAllocate(void*, std::size_t size) {
  return decoder_memory::Allocate(size);
}

void BrotliFree(void*, void* pointer) {
  decoder_memory::Deallocate(pointer);
}

bool DecodeCodec(std::uint8_t const* input, std::size_t input_size,
                 std::uint8_t* output, std::size_t output_size) {
  auto* state = BrotliDecoderCreateInstance(BrotliAllocate, BrotliFree, nullptr);
  if (state == nullptr) {
    return false;
  }
  auto available_input = input_size;
  auto const* next_input = input;
  auto available_output = output_size;
  auto* next_output = output;
  std::size_t total_output = 0;
  auto const result = BrotliDecoderDecompressStream(
      state, &available_input, &next_input, &available_output, &next_output,
      &total_output);
  auto const success = result == BROTLI_DECODER_RESULT_SUCCESS &&
                       available_input == 0 && total_output == output_size;
  BrotliDecoderDestroyInstance(state);
  return success;
}
#elif defined(CODEC_XZ)
void* XzAllocate(void*, std::size_t count, std::size_t size) {
  return decoder_memory::Callocate(count, size);
}

void XzFree(void*, void* pointer) {
  decoder_memory::Deallocate(pointer);
}

bool DecodeCodec(std::uint8_t const* input, std::size_t input_size,
                 std::uint8_t* output, std::size_t output_size) {
  auto allocator = lzma_allocator{XzAllocate, XzFree, nullptr};
  lzma_stream stream = LZMA_STREAM_INIT;
  stream.allocator = &allocator;
  if (lzma_stream_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
    return false;
  }
  stream.next_in = input;
  stream.avail_in = input_size;
  stream.next_out = output;
  stream.avail_out = output_size;
  auto const result = lzma_code(&stream, LZMA_FINISH);
  auto const success = result == LZMA_STREAM_END && stream.avail_in == 0 &&
                       stream.avail_out == 0;
  lzma_end(&stream);
  return success;
}
#elif defined(CODEC_LZ4)
bool DecodeCodec(std::uint8_t const* input, std::size_t input_size,
                 std::uint8_t* output, std::size_t output_size) {
  LZ4F_dctx* context = nullptr;
  auto result =
      LZ4F_createDecompressionContext(&context, LZ4F_VERSION);
  if (LZ4F_isError(result) || context == nullptr) {
    return false;
  }
  std::size_t input_position = 0;
  std::size_t output_position = 0;
  std::size_t hint = 1;
  bool success = true;
  while (hint != 0) {
    auto source_size = input_size - input_position;
    auto destination_size = output_size - output_position;
    hint = LZ4F_decompress(
        context, output + output_position, &destination_size,
        input + input_position, &source_size, nullptr);
    if (LZ4F_isError(hint)) {
      success = false;
      break;
    }
    input_position += source_size;
    output_position += destination_size;
    if (source_size == 0 && destination_size == 0 && hint != 0) {
      success = false;
      break;
    }
  }
  success = success && input_position == input_size &&
            output_position == output_size;
  LZ4F_freeDecompressionContext(context);
  return success;
}
#endif

char const* DefaultCodecName() {
#if defined(CODEC_BASELINE)
  return "baseline";
#elif defined(CODEC_AEGL)
  return "aegl_minimal";
#elif defined(CODEC_ZLIB)
  return "raw_deflate";
#elif defined(CODEC_ZSTD)
  return "zstd";
#elif defined(CODEC_BROTLI)
  return "brotli";
#elif defined(CODEC_XZ)
  return "xz";
#elif defined(CODEC_LZ4)
  return "lz4_frame";
#else
  return "unknown";
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: decoder-bench <compressed> <original> [label]\n");
    return 2;
  }
  auto compressed = ReadFile(argv[1]);
  auto original = ReadFile(argv[2]);
  if (compressed.data == nullptr || original.data == nullptr) {
    std::fprintf(stderr, "failed to read decoder fixture\n");
    Release(compressed);
    Release(original);
    return 3;
  }

  std::size_t caller_buffer_size = original.size;
  AeglHeader aegl_header{};
#if defined(CODEC_AEGL)
  if (!ParseAeglHeader(compressed.data, compressed.size, aegl_header) ||
      aegl_header.output_size != original.size) {
    std::fprintf(stderr, "bad AEGL fixture header\n");
    Release(compressed);
    Release(original);
    return 4;
  }
  caller_buffer_size = aegl_header.prefix_size + aegl_header.output_size;
#endif

  auto* caller_buffer = static_cast<std::uint8_t*>(
      std::malloc(caller_buffer_size == 0 ? 1 : caller_buffer_size));
  if (caller_buffer == nullptr) {
    Release(compressed);
    Release(original);
    return 5;
  }

  auto const snapshot = decoder_memory::BeginMeasurement();
  bool decoded = false;
  std::uint8_t const* decoded_output = caller_buffer;
#if defined(CODEC_BASELINE)
  std::memcpy(caller_buffer, original.data, original.size);
  decoded = true;
#elif defined(CODEC_AEGL)
  decoded = DecodeAegl(compressed.data, compressed.size, caller_buffer,
                       caller_buffer_size, aegl_header);
  decoded_output = caller_buffer + aegl_header.prefix_size;
#else
  decoded = DecodeCodec(compressed.data, compressed.size, caller_buffer,
                        original.size);
#endif

  auto const workspace_peak = decoder_memory::PeakDelta(snapshot);
  auto const live_after_decode = decoder_memory::LiveDelta(snapshot);
  auto const allocations = decoder_memory::AllocationCount();
  auto const content_matches = decoded &&
      std::memcmp(decoded_output, original.data, original.size) == 0;
  auto const* label = argc >= 4 ? argv[3] : DefaultCodecName();
  std::printf("%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%d\n", label,
              compressed.size, original.size, caller_buffer_size,
              workspace_peak, live_after_decode, allocations,
              caller_buffer_size + workspace_peak,
              content_matches ? 1 : 0);

  std::free(caller_buffer);
  Release(compressed);
  Release(original);
  return content_matches ? 0 : 6;
}
