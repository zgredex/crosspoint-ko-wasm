#include <HalStorage.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
int externalRead(void* ctx, size_t offset, uint8_t* dst, size_t len) {
  auto* bytes = static_cast<std::vector<uint8_t>*>(ctx);
  if (!bytes || !dst || offset > bytes->size() || len > bytes->size() - offset) return -1;
  std::memcpy(dst, bytes->data() + offset, len);
  return static_cast<int>(len);
}
}  // namespace

int main() {
  HalStorage storage;
  const uint8_t source[] = {1, 2, 3};
  storage.mountBlob("nested/source.bin", source, sizeof(source));

  if (!storage.exists("nested/source.bin") || !storage.exists("/nested/source.bin") ||
      storage.exists("/source.bin")) {
    std::cerr << "path normalization or basename-alias isolation failed\n";
    return 1;
  }
  HalFile sourceFile = storage.open("nested/source.bin");
  if (!sourceFile || sourceFile.size() != sizeof(source)) {
    std::cerr << "relative open disagrees with mount/exists\n";
    return 1;
  }
  sourceFile = HalFile();
  if (!storage.remove("nested/source.bin") || storage.exists("/nested/source.bin")) {
    std::cerr << "relative remove disagrees with mount/exists\n";
    return 1;
  }

  if (!storage.setDerivedByteLimit(8)) return 1;
  {
    HalFile out;
    if (!storage.openFileForWrite("test", "cache.bin", out)) return 1;
    const uint8_t six[6] = {};
    const uint8_t three[3] = {};
    if (out.write(six, sizeof(six)) != sizeof(six) || storage.derivedBytes() < sizeof(six) ||
        storage.derivedBytes() > storage.derivedByteLimit()) {
      std::cerr << "derived-byte accounting missed a valid write\n";
      return 1;
    }
    const size_t charged = storage.derivedBytes();
    if (out.write(three, sizeof(three)) != 0 || out.size() != sizeof(six) ||
        storage.derivedBytes() != charged) {
      std::cerr << "aggregate derived-storage ceiling did not fail closed\n";
      return 1;
    }
  }
  if (!storage.remove("cache.bin") || storage.derivedBytes() != 0) {
    std::cerr << "derived-byte accounting was not released with the blob\n";
    return 1;
  }

  std::vector<uint8_t> external = {1, 2, 3, 4};
  storage.mountExternalBlob("mutable.zip", external.size(), externalRead, &external);
  HalFile mutableFile = storage.open("mutable.zip");
  mutableFile.markZipDirectoryValidated();
  if (mutableFile.isZipDirectoryValidated()) {
    std::cerr << "mutable external backing cached a stale ZIP validation\n";
    return 1;
  }
  storage.mountExternalBlob("immutable.zip", external.size(), externalRead, &external, true);
  HalFile immutableFile = storage.open("immutable.zip");
  immutableFile.markZipDirectoryValidated();
  if (!immutableFile.isZipDirectoryValidated()) {
    std::cerr << "immutable external backing could not cache ZIP validation\n";
    return 1;
  }

  std::cout << "storage-integrity: paths, quotas and mutable/immutable ZIP validation pass\n";
  return 0;
}
