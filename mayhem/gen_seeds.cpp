// mayhem_gen_seeds <out-dir> - regenerates the seed corpora committed under
// mayhem/<target>/testsuite/. Built by mayhem/build.sh in the test tree (normal flags) with
// Nimble's own writer, serializer and encoders, so the seeds are real, valid inputs:
//
//   <out-dir>/fuzz_velox_reader/testsuite/*.nimble      whole Nimble files (also copied to
//   <out-dir>/fuzz_selective_reader/testsuite/           the selective reader's corpus)
//   <out-dir>/fuzz_encoding/testsuite/*.enc             single encoded streams (also copied to
//   <out-dir>/fuzz_encoding_legacy/testsuite/            the legacy factory's corpus)
//   <out-dir>/fuzz_deserializer/testsuite/*.ser         serialized batches for the schema in
//                                                        fuzz_deserializer_schema.h
//
// The output is deterministic (no randomness, no timestamps in the data it writes).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "dwio/nimble/common/Buffer.h"
#include "dwio/nimble/common/Vector.h"
#include "dwio/nimble/common/tests/NimbleFileWriter.h"
#include "dwio/nimble/encodings/tests/TestUtils.h"
#include "dwio/nimble/serializer/Serializer.h"
#include "dwio/nimble/velox/OrderedRanges.h"
#include "fuzz_deserializer_schema.h"
#include "velox/common/memory/Memory.h"
#include "velox/vector/tests/utils/VectorMaker.h"

using namespace facebook;

namespace {

std::filesystem::path gOut;

void save(const std::string& target, const std::string& name, std::string_view bytes) {
  const auto dir = gOut / target / "testsuite";
  std::filesystem::create_directories(dir);
  std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    std::cerr << "cannot write " << (dir / name) << "\n";
    std::exit(1);
  }
  std::cout << target << "/testsuite/" << name << " " << bytes.size() << " bytes\n";
}

void saveFile(const std::string& name, const std::string& bytes) {
  save("fuzz_velox_reader", name, bytes);
  save("fuzz_selective_reader", name, bytes);
}

// ---- whole files -----------------------------------------------------------------------------

velox::RowVectorPtr scalars(velox::test::VectorMaker& vm, int rows) {
  return vm.rowVector(
      {"i8", "i32", "i64", "f32", "f64", "b", "s"},
      {vm.flatVector<int8_t>(rows, [](auto row) { return row % 7; }),
       vm.flatVector<int32_t>(rows, [](auto row) { return row * 1000 - 5; }),
       vm.flatVector<int64_t>(
           rows, [](auto row) { return (row % 3) * 0x100000000LL; }),
       vm.flatVector<float>(rows, [](auto row) { return row * 0.5f; }),
       vm.flatVector<double>(rows, [](auto row) { return row / 3.0; }),
       vm.flatVector<bool>(rows, [](auto row) { return row % 5 == 0; }),
       vm.flatVector<velox::StringView>(rows, [](auto row) {
         static const char* words[] = {"nimble", "velox", "", "a longer string value!"};
         return velox::StringView(words[row % 4]);
       })});
}

velox::RowVectorPtr nested(velox::test::VectorMaker& vm, int rows) {
  auto nullEvery = [](int n) {
    return [n](auto row) { return row % n == n - 1; };
  };
  return vm.rowVector(
      {"id", "arr", "map", "row", "opt"},
      {vm.flatVector<int64_t>(rows, [](auto row) { return row; }),
       vm.arrayVector<int32_t>(
           rows,
           [](auto row) { return row % 4; },
           [](auto row, auto index) { return row * 10 + index; },
           nullEvery(6)),
       vm.mapVector<int32_t, double>(
           rows,
           [](auto row) { return row % 3; },
           [](auto /*row*/, auto index) { return index; },
           [](auto row, auto index) { return row + index * 0.25; },
           nullEvery(9)),
       vm.rowVector(
           {"x", "y"},
           {vm.flatVector<int16_t>(rows, [](auto row) { return row; }),
            vm.flatVector<velox::StringView>(rows, [](auto row) {
              return velox::StringView(row % 2 ? "odd" : "even");
            })}),
       vm.flatVector<int32_t>(
           rows, [](auto row) { return row; }, nullEvery(3))});
}

velox::RowVectorPtr featureMap(velox::test::VectorMaker& vm, int rows) {
  return vm.rowVector(
      {"features", "label"},
      {vm.mapVector<int32_t, float>(
           rows,
           [](auto /*row*/) { return 3; },
           [](auto /*row*/, auto index) { return index + 1; },
           [](auto row, auto index) { return row * 1.5f + index; }),
       vm.flatVector<int32_t>(rows, [](auto row) { return row % 2; })});
}

void genFiles(velox::memory::MemoryPool& pool) {
  velox::test::VectorMaker vm(&pool);

  saveFile("scalars.nimble", nimble::test::createNimbleFile(pool, scalars(vm, 40)));
  saveFile("nested.nimble", nimble::test::createNimbleFile(pool, nested(vm, 30)));
  saveFile(
      "multi_stripe.nimble",
      nimble::test::createNimbleFile(
          pool, {scalars(vm, 20), scalars(vm, 5), scalars(vm, 33)}));
  saveFile(
      "empty_batch.nimble", nimble::test::createNimbleFile(pool, scalars(vm, 0)));
  {
    nimble::VeloxWriterOptions options;
    options.flatMapColumns = {{"features", {}}};
    saveFile(
        "flatmap.nimble",
        nimble::test::createNimbleFile(pool, featureMap(vm, 25), options));
  }
  {
    nimble::VeloxWriterOptions options;
    options.dictionaryArrayColumns = {"arr"};
    saveFile(
        "dictionary_array.nimble",
        nimble::test::createNimbleFile(pool, nested(vm, 30), options));
  }
}

// ---- single encodings ------------------------------------------------------------------------

template <typename E, typename F>
void genEncoding(
    velox::memory::MemoryPool& pool,
    const std::string& name,
    int rows,
    F valueAt,
    nimble::CompressionType compression = nimble::CompressionType::Uncompressed) {
  using T = typename E::cppDataType;
  nimble::Buffer buffer{pool};
  nimble::Vector<T> values{&pool};
  for (int i = 0; i < rows; ++i) {
    values.push_back(valueAt(i));
  }
  const auto encoded = nimble::test::Encoder<E>::encode(buffer, values, compression);
  save("fuzz_encoding", name, encoded);
  // The legacy factory reads the same stream prefix (encoding type, data type, row count).
  save("fuzz_encoding_legacy", name, encoded);
}

void genEncodings(velox::memory::MemoryPool& pool) {
  using nimble::CompressionType;
  static const std::vector<std::string> words = {
      "alpha", "alphabet", "alpine", "beta", "", "gamma ray burst", "alpha"};
  auto word = [](int i) { return std::string_view(words[i % words.size()]); };

  genEncoding<nimble::TrivialEncoding<uint32_t>>(
      pool, "trivial_u32.enc", 50, [](int i) { return uint32_t(i * 37); });
  genEncoding<nimble::TrivialEncoding<uint32_t>>(
      pool, "trivial_u32_zstd.enc", 200, [](int i) { return uint32_t(i % 4); },
      CompressionType::Zstd);
  genEncoding<nimble::TrivialEncoding<std::string_view>>(
      pool, "trivial_string.enc", 20, word);
  genEncoding<nimble::TrivialEncoding<bool>>(
      pool, "trivial_bool.enc", 70, [](int i) { return i % 3 == 0; });
  genEncoding<nimble::RLEEncoding<uint32_t>>(
      pool, "rle_u32.enc", 120, [](int i) { return uint32_t(i / 30); });
  genEncoding<nimble::RLEEncoding<bool>>(
      pool, "rle_bool.enc", 90, [](int i) { return (i / 20) % 2 == 0; });
  genEncoding<nimble::DictionaryEncoding<uint32_t>>(
      pool, "dictionary_u32.enc", 80, [](int i) { return uint32_t(1000 + i % 5); });
  genEncoding<nimble::DictionaryEncoding<std::string_view>>(
      pool, "dictionary_string.enc", 40, word);
  genEncoding<nimble::FixedBitWidthEncoding<uint32_t>>(
      pool, "fixed_bit_width_u32.enc", 64, [](int i) { return uint32_t(500 + i % 13); });
  genEncoding<nimble::SparseBoolEncoding>(
      pool, "sparse_bool.enc", 300, [](int i) { return i % 41 == 0; });
  genEncoding<nimble::VarintEncoding<uint64_t>>(
      pool, "varint_u64.enc", 40, [](int i) { return uint64_t(1) << (i % 60); });
  genEncoding<nimble::ConstantEncoding<int64_t>>(
      pool, "constant_i64.enc", 25, [](int) { return int64_t(-42); });
  genEncoding<nimble::MainlyConstantEncoding<uint32_t>>(
      pool, "mainly_constant_u32.enc", 100,
      [](int i) { return uint32_t(i % 17 == 0 ? i : 7); });
  genEncoding<nimble::ForEncoding<uint32_t>>(
      pool, "for_u32.enc", 100, [](int i) { return uint32_t(100000 + i % 50); });
  genEncoding<nimble::PFOREncoding<uint32_t>>(
      pool, "pfor_u32.enc", 300,
      [](int i) { return uint32_t(i % 97 == 0 ? 1u << 30 : i % 16); });
  genEncoding<nimble::BlockBitPackingEncoding<uint32_t>>(
      pool, "block_bit_packing_u32.enc", 300, [](int i) { return uint32_t(i % 200); });
  genEncoding<nimble::DeltaEncoding<uint32_t>>(
      pool, "delta_u32.enc", 100, [](int i) { return uint32_t(i * 3 + (i % 2)); });
  genEncoding<nimble::HuffmanEncoding<uint32_t>>(
      pool, "huffman_u32.enc", 200, [](int i) { return uint32_t(i % 10 < 7 ? 1 : i % 10); });
  genEncoding<nimble::ALPEncoding<double>>(
      pool, "alp_f64.enc", 100, [](int i) { return 10.25 + i * 0.01; });
}

// ---- serialized batches ----------------------------------------------------------------------

velox::RowVectorPtr serializerBatch(velox::test::VectorMaker& vm, int rows) {
  auto type = nimble_fuzz::deserializerRowType();
  return vm.rowVector(
      type->names(),
      {vm.flatVector<int64_t>(rows, [](auto row) { return row * 11; }),
       vm.flatVector<bool>(rows, [](auto row) { return row % 2 == 0; }),
       vm.flatVector<double>(rows, [](auto row) { return row * 0.125; }),
       vm.flatVector<velox::StringView>(rows, [](auto row) {
         return velox::StringView(row % 3 ? "nimble" : "serialized row batch");
       }),
       vm.arrayVector<int32_t>(
           rows,
           [](auto row) { return row % 3; },
           [](auto row, auto index) { return row + index; }),
       vm.mapVector<velox::StringView, float>(
           rows,
           [](auto row) { return row % 2 + 1; },
           [](auto /*row*/, auto index) {
             return velox::StringView(index ? "k1" : "k0");
           },
           [](auto row, auto index) { return row * 2.0f + index; }),
       vm.rowVector(
           {"x", "y"},
           {vm.flatVector<int16_t>(rows, [](auto row) { return row; }),
            vm.flatVector<velox::StringView>(rows, [](auto row) {
              return velox::StringView(row % 2 ? "y1" : "y0");
            })})});
}

void genSerialized(
    velox::memory::MemoryPool& pool,
    const std::string& name,
    int rows,
    nimble::SerializerOptions options) {
  velox::test::VectorMaker vm(&pool);
  auto batch = serializerBatch(vm, rows);
  nimble::Serializer serializer{options, batch->type(), &pool};
  save(
      "fuzz_deserializer",
      name,
      serializer.serialize(batch, nimble::OrderedRanges::of(0, batch->size())));
}

void genSerialized(velox::memory::MemoryPool& pool) {
  using nimble::SerializationVersion;
  genSerialized(pool, "default.ser", 12, {});
  genSerialized(pool, "one_row.ser", 1, {});
  genSerialized(
      pool, "serialization_v4.ser", 20, {.version = SerializationVersion::kSerialization});
  genSerialized(pool, "legacy.ser", 20, {.version = SerializationVersion::kLegacy});
  genSerialized(
      pool,
      "zstd.ser",
      200,
      {.compressionType = nimble::CompressionType::Zstd,
       .version = SerializationVersion::kSerialization});
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <out-dir>\n";
    return 2;
  }
  gOut = argv[1];
  velox::memory::initializeMemoryManager(velox::memory::MemoryManager::Options{});
  auto root = velox::memory::memoryManager()->addRootPool("gen_seeds");
  auto pool = root->addLeafChild("leaf");
  genFiles(*pool);
  genEncodings(*pool);
  genSerialized(*pool);
  return 0;
}
