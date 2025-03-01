//===--- RuntimeValueWitness.cpp - Value Witness Runtime Implementation---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implementations of runtime determined value witness functions
// This file is intended to be statically linked into executables until it is
// fully added to the runtime.
//
//===----------------------------------------------------------------------===//

#include "BytecodeLayouts.h"
#include "../SwiftShims/swift/shims/HeapObject.h"
#include "EnumImpl.h"
#include "WeakReference.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/ABI/System.h"
#include "swift/Runtime/Error.h"
#include "swift/Runtime/HeapObject.h"
#include "llvm/Support/SwapByteOrder.h"
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include <Block.h>
#endif
#if SWIFT_PTRAUTH
#include <ptrauth.h>
#endif

using namespace swift;

static Metadata *getExistentialTypeMetadata(OpaqueValue *object) {
  return reinterpret_cast<Metadata**>(object)[NumWords_ValueBuffer];
}

template <typename FnTy, typename Reader>
static const FnTy readRelativeFunctionPointer(Reader &reader) {
  static_assert(std::is_pointer<FnTy>::value);

  auto absolute = reader.getAbsolute();
  auto relativeOffset =
      (uintptr_t)(intptr_t)(int32_t)reader.template readBytes<intptr_t>();
  FnTy fn;

#if SWIFT_PTRAUTH
  fn = (FnTy)ptrauth_sign_unauthenticated(
      (void *)((uintptr_t)absolute + relativeOffset),
      ptrauth_key_function_pointer, 0);
#else
  fn = (FnTy)((uintptr_t)absolute + relativeOffset);
#endif

  return fn;
}

typedef Metadata *(*MetadataAccessor)(const Metadata *const *);

template <typename Reader>
static const Metadata *getResilientTypeMetadata(const Metadata *metadata,
                                                Reader &reader) {
  auto fn = readRelativeFunctionPointer<MetadataAccessor>(reader);
  return fn(metadata->getGenericArgs());
}

static uint64_t readTagBytes(const uint8_t *addr, uint8_t byteCount) {
  switch (byteCount) {
  case 1:
    return addr[0];
  case 2: {
    uint16_t res = 0;
    memcpy(&res, addr, sizeof(uint16_t));
    return res;
  }
  case 4: {
    uint32_t res = 0;
    memcpy(&res, addr, sizeof(uint32_t));
    return res;
  }
  case 8: {
    uint64_t res = 0;
    memcpy(&res, addr, sizeof(uint64_t));
    return res;
  }
  default:
    swift_unreachable("Unsupported tag byte length.");
  }
}

static void handleRefCountsDestroy(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *addr);

template <typename ...Params>
static void handleEnd(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *addr,
                          Params ...params) {
  return;
}

static void errorDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  SwiftError *error = *(SwiftError**)(addr + addrOffset);
  addrOffset += sizeof(SwiftError*);
  swift_errorRelease(error);
}

static void nativeStrongDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  HeapObject *object = (HeapObject*)((*(uintptr_t *)(addr + addrOffset)) & ~_swift_abi_SwiftSpareBitsMask);
  addrOffset += sizeof(HeapObject*);
  swift_release(object);
}

static void unownedDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  HeapObject *object = (HeapObject*)((*(uintptr_t *)(addr + addrOffset)) & ~_swift_abi_SwiftSpareBitsMask);
  addrOffset += sizeof(HeapObject*);
  swift_unownedRelease(object);
}

static void weakDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  auto *object = (WeakReference *)(addr + addrOffset);
  addrOffset += sizeof(WeakReference);
  swift_weakDestroy(object);
}

static void unknownDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  void *object = *(void**)(addr + addrOffset);
  addrOffset += sizeof(void*);
  swift_unknownObjectRelease(object);
}

static void unknownUnownedDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  UnownedReference *object = (UnownedReference*)(addr + addrOffset);
  addrOffset += sizeof(UnownedReference);
  swift_unknownObjectUnownedDestroy(object);
}

static void unknownWeakDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  auto *object = (WeakReference *)(addr + addrOffset);
  addrOffset += sizeof(WeakReference);
  swift_unknownObjectWeakDestroy(object);
}

static void bridgeDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  auto *object = *(void **)(addr + addrOffset);
  addrOffset += sizeof(void*);
  swift_bridgeObjectRelease(object);
}

static void singlePayloadEnumSimpleBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  reader.modify([&](LayoutStringReader1 &reader) {
    uint64_t byteCountsAndOffset;
    size_t payloadSize;
    uint64_t zeroTagValue;
    size_t xiTagValues;
    size_t refCountBytes;
    size_t skip;

    reader.readBytes(byteCountsAndOffset, payloadSize, zeroTagValue, xiTagValues, refCountBytes, skip);

    auto extraTagBytesPattern = (uint8_t)(byteCountsAndOffset >> 62);
    auto xiTagBytesPattern = ((uint8_t)(byteCountsAndOffset >> 59)) & 0x7;
    auto xiTagBytesOffset =
        byteCountsAndOffset & std::numeric_limits<uint32_t>::max();

    if (SWIFT_UNLIKELY(extraTagBytesPattern)) {
      auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
      auto tagBytes =
          readTagBytes(addr + addrOffset + payloadSize, extraTagBytes);
      if (tagBytes) {
        xiTagBytesPattern = 0;
      }
    }

    if (SWIFT_LIKELY(xiTagBytesPattern)) {
      auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
      uint64_t tagBytes =
          readTagBytes(addr + addrOffset + xiTagBytesOffset, xiTagBytes) -
          zeroTagValue;
      if (tagBytes >= xiTagValues) {
        return;
      }
    }

    reader.skip(refCountBytes);
    addrOffset += skip;
  });
}

typedef unsigned (*GetEnumTagFn)(const uint8_t *);

static void singlePayloadEnumFNBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);

    unsigned enumTag = getEnumTag(addr + addrOffset);

    if (SWIFT_LIKELY(enumTag == 0)) {
      reader.skip(sizeof(size_t) * 2);
    } else {
      size_t refCountBytes;
      size_t skip;
      reader.readBytes(refCountBytes, skip);
      reader.skip(refCountBytes);
      addrOffset += skip;
    }
  });
}

static void singlePayloadEnumFNResolvedBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag;
    size_t refCountBytes;
    size_t skip;
    reader.readBytes(getEnumTag, refCountBytes, skip);

    unsigned enumTag = getEnumTag(addr + addrOffset);

    if (SWIFT_UNLIKELY(enumTag != 0)) {
      reader.skip(refCountBytes);
      addrOffset += skip;
    }
  });
}

static void singlePayloadEnumGenericBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  reader.modify([&](LayoutStringReader1 &reader) {
    auto tagBytesAndOffset = reader.readBytes<uint64_t>();
    auto payloadSize = reader.readBytes<size_t>();
    auto *xiType = reader.readBytes<const Metadata *>();
    auto numEmptyCases = reader.readBytes<unsigned>();
    auto refCountBytes = reader.readBytes<size_t>();
    auto skip = reader.readBytes<size_t>();

    auto extraTagBytesPattern = (uint8_t)(tagBytesAndOffset >> 62);
    auto xiTagBytesOffset =
        tagBytesAndOffset & std::numeric_limits<uint32_t>::max();

    if (SWIFT_UNLIKELY(extraTagBytesPattern)) {
      auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
      auto tagBytes = readTagBytes(addr + addrOffset + payloadSize, extraTagBytes);

      if (tagBytes) {
        xiType = nullptr;
      }
    }

    if (SWIFT_LIKELY(xiType)) {
      auto tag = xiType->vw_getEnumTagSinglePayload(
          (const OpaqueValue *)(addr + addrOffset + xiTagBytesOffset),
          numEmptyCases);
      if (SWIFT_LIKELY(tag == 0)) {
        return;
      }
    }

    reader.skip(refCountBytes);
    addrOffset += skip;
  });
}

template<auto HandlerFn>
static void multiPayloadEnumFNBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  LayoutStringReader1 nestedReader;
  uintptr_t nestedAddrOffset;
  unsigned enumTag;

  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
    reader.readBytes(numPayloads, refCountBytes, enumSize);
    nestedReader = reader;
    nestedAddrOffset = addrOffset;

    enumTag = getEnumTag(addr + addrOffset);
    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(enumTag < numPayloads)) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(enumTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    HandlerFn(metadata, nestedReader, nestedAddrOffset, addr);
  } else {
    addrOffset += enumSize;
  }
}

template<auto HandlerFn>
static void multiPayloadEnumFNResolvedBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  LayoutStringReader1 nestedReader;
  uintptr_t nestedAddrOffset;
  unsigned enumTag;

  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = reader.readBytes<GetEnumTagFn>();
    reader.readBytes(numPayloads, refCountBytes, enumSize);
    nestedReader = reader;
    nestedAddrOffset = addrOffset;

    enumTag = getEnumTag(addr + addrOffset);
    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(enumTag < numPayloads)) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(enumTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    HandlerFn(metadata, nestedReader, nestedAddrOffset, addr);
  } else {
    addrOffset += enumSize;
  }
}

template<auto HandlerFn>
static void multiPayloadEnumGenericBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  size_t tagBytes;
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  uint64_t enumTag;
  uintptr_t nestedAddrOffset;
  LayoutStringReader1 nestedReader;
  reader.modify([&](LayoutStringReader1 &reader) {
    reader.readBytes(tagBytes, numPayloads, refCountBytes, enumSize);

    nestedReader = reader;
    nestedAddrOffset = addrOffset;
    auto tagBytesOffset = enumSize - tagBytes;

    enumTag = readTagBytes(addr + addrOffset + tagBytesOffset, tagBytes);

    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(enumTag < numPayloads)) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(enumTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    HandlerFn(metadata, nestedReader, nestedAddrOffset, addr);
  } else {
    addrOffset += enumSize;
  }
}

static void singlePayloadEnumSimpleBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    uint64_t byteCountsAndOffset;
    size_t payloadSize;
    uint64_t zeroTagValue;
    size_t xiTagValues;
    size_t refCountBytes;
    size_t skip;

    reader.readBytes(byteCountsAndOffset, payloadSize, zeroTagValue, xiTagValues, refCountBytes, skip);

    auto extraTagBytesPattern = (uint8_t)(byteCountsAndOffset >> 62);
    auto xiTagBytesPattern = ((uint8_t)(byteCountsAndOffset >> 59)) & 0x7;
    auto xiTagBytesOffset =
        byteCountsAndOffset & std::numeric_limits<uint32_t>::max();

    if (SWIFT_UNLIKELY(extraTagBytesPattern)) {
      auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
      auto tagBytes =
          readTagBytes(src + addrOffset + payloadSize, extraTagBytes);
      if (tagBytes) {
        xiTagBytesPattern = 0;
      }
    }

    if (SWIFT_LIKELY(xiTagBytesPattern)) {
      auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
      uint64_t tagBytes =
          readTagBytes(src + addrOffset + xiTagBytesOffset, xiTagBytes) -
          zeroTagValue;
      if (tagBytes >= xiTagValues) {
        return;
      }
    }

    memcpy(dest + addrOffset, src + addrOffset, skip);
    reader.skip(refCountBytes);
    addrOffset += skip;
  });
}

static void singlePayloadEnumFNBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);

    unsigned enumTag = getEnumTag(src + addrOffset);

    if (SWIFT_LIKELY(enumTag == 0)) {
      reader.skip(sizeof(size_t) * 2);
    } else {
      size_t refCountBytes;
      size_t skip;
      reader.readBytes(refCountBytes, skip);
      reader.skip(refCountBytes);
      memcpy(dest + addrOffset, src + addrOffset, skip);
      addrOffset += skip;
    }
  });
}

static void singlePayloadEnumFNResolvedBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag;
    size_t refCountBytes;
    size_t skip;
    reader.readBytes(getEnumTag, refCountBytes, skip);

    unsigned enumTag = getEnumTag(src + addrOffset);

    if (SWIFT_UNLIKELY(enumTag != 0)) {
      reader.skip(refCountBytes);
      memcpy(dest + addrOffset, src + addrOffset, skip);
      addrOffset += skip;
    }
  });
}

static void singlePayloadEnumGenericBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    auto tagBytesAndOffset = reader.readBytes<uint64_t>();
    auto payloadSize = reader.readBytes<size_t>();
    auto *xiType = reader.readBytes<const Metadata *>();
    auto numEmptyCases = reader.readBytes<unsigned>();
    auto refCountBytes = reader.readBytes<size_t>();
    auto skip = reader.readBytes<size_t>();

    auto extraTagBytesPattern = (uint8_t)(tagBytesAndOffset >> 62);
    auto xiTagBytesOffset =
        tagBytesAndOffset & std::numeric_limits<uint32_t>::max();

    if (SWIFT_UNLIKELY(extraTagBytesPattern)) {
      auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
      auto tagBytes = readTagBytes(src + addrOffset + payloadSize, extraTagBytes);

      if (tagBytes) {
        xiType = nullptr;
      }
    }

    if (SWIFT_LIKELY(xiType)) {
      auto tag = xiType->vw_getEnumTagSinglePayload(
          (const OpaqueValue *)(src + addrOffset + xiTagBytesOffset),
          numEmptyCases);
      if (SWIFT_LIKELY(tag == 0)) {
        return;
      }
    }

    reader.skip(refCountBytes);
    memcpy(dest + addrOffset, src + addrOffset, skip);
    addrOffset += skip;
  });
}

template<auto HandlerFn>
static void multiPayloadEnumFNBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  LayoutStringReader1 nestedReader;
  uintptr_t nestedAddrOffset;
  unsigned enumTag;

  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
    reader.readBytes(numPayloads, refCountBytes, enumSize);
    nestedReader = reader;
    nestedAddrOffset = addrOffset;

    enumTag = getEnumTag(src + addrOffset);
    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(enumTag < numPayloads)) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(enumTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    HandlerFn(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
  } else {
    memcpy(dest + addrOffset, src + addrOffset, enumSize);
    addrOffset += enumSize;
  }
}

template<auto HandlerFn>
static void multiPayloadEnumFNResolvedBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  LayoutStringReader1 nestedReader;
  uintptr_t nestedAddrOffset;
  unsigned enumTag;

  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = reader.readBytes<GetEnumTagFn>();
    reader.readBytes(numPayloads, refCountBytes, enumSize);
    nestedReader = reader;
    nestedAddrOffset = addrOffset;

    enumTag = getEnumTag(src + addrOffset);
    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(enumTag < numPayloads)) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(enumTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    HandlerFn(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
  } else {
    memcpy(dest + addrOffset, src + addrOffset, enumSize);
    addrOffset += enumSize;
  }
}

template<auto HandlerFn>
static void multiPayloadEnumGenericBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  size_t tagBytes;
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  uint64_t enumTag;
  uintptr_t nestedAddrOffset;
  LayoutStringReader1 nestedReader;
  reader.modify([&](LayoutStringReader1 &reader) {
    reader.readBytes(tagBytes, numPayloads, refCountBytes, enumSize);

    nestedReader = reader;
    nestedAddrOffset = addrOffset;
    auto tagBytesOffset = enumSize - tagBytes;

    enumTag = readTagBytes(src + addrOffset + tagBytesOffset, tagBytes);

    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(enumTag < numPayloads)) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(enumTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    HandlerFn(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
  } else {
    memcpy(dest + addrOffset, src + addrOffset, enumSize);
    addrOffset += enumSize;
  }
}

#if SWIFT_OBJC_INTEROP
static void blockDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  void* object = (void *)(addr + addrOffset);
  addrOffset += sizeof(void*);
  _Block_release(object);
}

static void objcStrongDestroyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *addr) {
  uintptr_t object = *(uintptr_t *)(addr + addrOffset);
  addrOffset += sizeof(objc_object*);
  if (object & _swift_abi_ObjCReservedBitsMask)
    return;

  object &= ~_swift_abi_SwiftSpareBitsMask;
  objc_release((objc_object *)object);
}
#endif

static void metatypeDestroyBranchless(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *addr) {
  auto *type = reader.readBytes<const Metadata *>();
  auto *object = (OpaqueValue *)(addr + addrOffset);
  addrOffset += type->vw_size();
  type->vw_destroy(object);
}

static void existentialDestroyBranchless(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *addr) {
  OpaqueValue *object = (OpaqueValue *)(addr + addrOffset);
  auto* type = getExistentialTypeMetadata(object);
  addrOffset += sizeof(uintptr_t) * NumWords_ValueBuffer;
  if (type->getValueWitnesses()->isValueInline()) {
    type->vw_destroy(object);
  } else {
    swift_release(*(HeapObject**)object);
  }
}

static void resilientDestroyBranchless(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *addr) {
  auto *type = getResilientTypeMetadata(metadata, reader);
  auto *object = (OpaqueValue *)(addr + addrOffset);
  addrOffset += type->vw_size();
  type->vw_destroy(object);
}

typedef void (*DestrFnBranchless)(const Metadata *metadata,
                                  LayoutStringReader1 &reader,
                                  uintptr_t &addrOffset,
                                  uint8_t *addr);

const DestrFnBranchless destroyTableBranchless[] = {
  &handleEnd,
  &errorDestroyBranchless,
  &nativeStrongDestroyBranchless,
  &unownedDestroyBranchless,
  &weakDestroyBranchless,
  &unknownDestroyBranchless,
  &unknownUnownedDestroyBranchless,
  &unknownWeakDestroyBranchless,
  &bridgeDestroyBranchless,
#if SWIFT_OBJC_INTEROP
  &blockDestroyBranchless,
  &objcStrongDestroyBranchless,
#else
  nullptr,
  nullptr,
#endif
  nullptr, // Custom
  &metatypeDestroyBranchless,
  nullptr, // Generic
  &existentialDestroyBranchless,
  &resilientDestroyBranchless,
  &singlePayloadEnumSimpleBranchless,
  &singlePayloadEnumFNBranchless,
  &singlePayloadEnumFNResolvedBranchless,
  &singlePayloadEnumGenericBranchless,
  &multiPayloadEnumFNBranchless<handleRefCountsDestroy>,
  &multiPayloadEnumFNResolvedBranchless<handleRefCountsDestroy>,
  &multiPayloadEnumGenericBranchless<handleRefCountsDestroy>,
};

static void handleRefCountsDestroy(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *addr) {
  while (true) {
    auto tag = reader.readBytes<uint64_t>();
    addrOffset += (tag & ~(0xFFULL << 56));
    tag >>= 56;
    if (SWIFT_UNLIKELY(tag == 0)) {
      return;
    }

    destroyTableBranchless[tag](metadata, reader, addrOffset, addr);
  }
}

extern "C" void
swift_generic_destroy(swift::OpaqueValue *address, const Metadata *metadata) {
  const uint8_t *layoutStr = metadata->getLayoutString();
  LayoutStringReader1 reader{layoutStr + layoutStringHeaderSize};
  uintptr_t addrOffset = 0;
  handleRefCountsDestroy(metadata, reader, addrOffset, (uint8_t *)address);
}

void swift::swift_generic_arrayDestroy(swift::OpaqueValue *address, size_t count, size_t stride, const Metadata *metadata) {
  const uint8_t *layoutStr = metadata->getLayoutString();
  for (size_t i = 0; i < count; i++) {
    LayoutStringReader1 reader{layoutStr + layoutStringHeaderSize};
    uintptr_t addrOffset = i * stride;
    handleRefCountsDestroy(metadata, reader, addrOffset, (uint8_t *)address);
  }
}

static void handleRefCountsInitWithCopy(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *dest,
                          uint8_t *src);

static void errorRetainBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  SwiftError *object = *(SwiftError **)(src + _addrOffset);
  memcpy(dest + addrOffset, &object, sizeof(SwiftError*));
  addrOffset = _addrOffset + sizeof(SwiftError *);
  swift_errorRetain(object);
}

static void nativeStrongRetainBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  uintptr_t object = *(uintptr_t *)(src + _addrOffset);
  memcpy(dest + _addrOffset, &object, sizeof(HeapObject*));
  object &= ~_swift_abi_SwiftSpareBitsMask;
  addrOffset = _addrOffset + sizeof(HeapObject *);
  swift_retain((HeapObject *)object);
}

static void unownedRetainBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  uintptr_t object = *(uintptr_t *)(src + _addrOffset);
  memcpy(dest + _addrOffset, &object, sizeof(HeapObject*));
  object &= ~_swift_abi_SwiftSpareBitsMask;
  addrOffset = _addrOffset + sizeof(HeapObject *);
  swift_unownedRetain((HeapObject *)object);
}

static void weakCopyInitBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *destObject = (WeakReference *)(dest + _addrOffset);
  auto *srcObject = (WeakReference *)(src + _addrOffset);
  addrOffset = _addrOffset + sizeof(WeakReference);
  swift_weakCopyInit(destObject, srcObject);
}

static void unknownRetainBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  void *object = *(void **)(src + _addrOffset);
  memcpy(dest + _addrOffset, &object, sizeof(void*));
  addrOffset = _addrOffset + sizeof(void *);
  swift_unknownObjectRetain(object);
}

static void unknownUnownedCopyInitBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  UnownedReference *objectDest = (UnownedReference*)(dest + _addrOffset);
  UnownedReference *objectSrc = (UnownedReference*)(src + _addrOffset);
  addrOffset = _addrOffset + sizeof(UnownedReference);
  swift_unknownObjectUnownedCopyInit(objectDest, objectSrc);
}

static void unknownWeakCopyInitBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *destObject = (WeakReference *)(dest + _addrOffset);
  auto *srcObject = (WeakReference *)(src + _addrOffset);
  addrOffset = _addrOffset + sizeof(WeakReference);
  swift_unknownObjectWeakCopyInit(destObject, srcObject);
}

static void bridgeRetainBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  void *object = *(void **)(src + _addrOffset);
  memcpy(dest + _addrOffset, &object, sizeof(void*));
  addrOffset = _addrOffset + sizeof(void*);
  swift_bridgeObjectRetain(object);
}

#if SWIFT_OBJC_INTEROP
static void blockCopyBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *copy = _Block_copy(*(void**)(src + _addrOffset));
  memcpy(dest + _addrOffset, &copy, sizeof(void*));
  addrOffset = _addrOffset + sizeof(void*);
}

static void objcStrongRetainBranchless(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  uintptr_t object = *(uintptr_t *)(src + _addrOffset);
  memcpy(dest + _addrOffset, &object, sizeof(objc_object *));
  addrOffset = _addrOffset + sizeof(objc_object *);
  if (object & _swift_abi_ObjCReservedBitsMask)
    return;
  object &= ~_swift_abi_SwiftSpareBitsMask;
  objc_retain((objc_object *)object);
}
#endif

static void metatypeInitWithCopyBranchless(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = reader.readBytes<const Metadata *>();
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + type->vw_size();
  type->vw_initializeWithCopy(destObject, srcObject);
}

static void existentialInitWithCopyBranchless(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = getExistentialTypeMetadata((OpaqueValue*)(src + _addrOffset));
  auto *destObject = (ValueBuffer *)(dest + _addrOffset);
  auto *srcObject = (ValueBuffer *)(src + _addrOffset);
  addrOffset = _addrOffset + (sizeof(uintptr_t) * NumWords_ValueBuffer);
  type->vw_initializeBufferWithCopyOfBuffer(destObject, srcObject);
}

static void resilientInitWithCopyBranchless(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = getResilientTypeMetadata(metadata, reader);
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + type->vw_size();
  type->vw_initializeWithCopy(destObject, srcObject);
}

typedef void (*InitFn)(const Metadata *metadata,
                                 LayoutStringReader1 &reader,
                                 uintptr_t &addrOffset,
                                 uint8_t *dest,
                                 uint8_t *src);

static const InitFn initWithCopyTable[] = {
  &handleEnd,
  &errorRetainBranchless,
  &nativeStrongRetainBranchless,
  &unownedRetainBranchless,
  &weakCopyInitBranchless,
  &unknownRetainBranchless,
  &unknownUnownedCopyInitBranchless,
  &unknownWeakCopyInitBranchless,
  &bridgeRetainBranchless,
#if SWIFT_OBJC_INTEROP
  &blockCopyBranchless,
  &objcStrongRetainBranchless,
#else
  nullptr,
  nullptr,
#endif
  nullptr, // Custom
  &metatypeInitWithCopyBranchless,
  nullptr, // Generic
  &existentialInitWithCopyBranchless,
  &resilientInitWithCopyBranchless,
  &singlePayloadEnumSimpleBranchless,
  &singlePayloadEnumFNBranchless,
  &singlePayloadEnumFNResolvedBranchless,
  &singlePayloadEnumGenericBranchless,
  &multiPayloadEnumFNBranchless<handleRefCountsInitWithCopy>,
  &multiPayloadEnumFNResolvedBranchless<handleRefCountsInitWithCopy>,
  &multiPayloadEnumGenericBranchless<handleRefCountsInitWithCopy>,
};

static void handleRefCountsInitWithCopy(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *dest,
                          uint8_t *src) {
  while (true) {
    uintptr_t _addrOffset = addrOffset;
    auto tag = reader.readBytes<uint64_t>();
    auto offset = (tag & ~(0xFFULL << 56));
    if (SWIFT_UNLIKELY(offset)) {
      memcpy(dest + _addrOffset, src + _addrOffset, offset);
    }
    addrOffset = _addrOffset + offset;
    tag >>= 56;
    if (SWIFT_UNLIKELY(tag == 0)) {
      return;
    }

    initWithCopyTable[tag](metadata, reader, addrOffset, dest, src);
  }
}

extern "C" swift::OpaqueValue *
swift_generic_initWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                           const Metadata *metadata) {
  const uint8_t *layoutStr = metadata->getLayoutString();
  LayoutStringReader1 reader{layoutStr + layoutStringHeaderSize};
  uintptr_t addrOffset = 0;
  handleRefCountsInitWithCopy(metadata, reader, addrOffset, (uint8_t *)dest, (uint8_t *)src);

  assert(addrOffset == metadata->vw_size());

  return dest;
}

void swift::swift_generic_arrayInitWithCopy(swift::OpaqueValue *dest,
                                     swift::OpaqueValue *src,
                                     size_t count,
                                     size_t stride,
                                     const Metadata *metadata) {
  const uint8_t *layoutStr = metadata->getLayoutString();
  for (size_t i = 0; i < count; i++) {
    LayoutStringReader1 reader{layoutStr + layoutStringHeaderSize};
    uintptr_t addrOffset = i * stride;
    handleRefCountsInitWithCopy(metadata, reader, addrOffset, (uint8_t *)dest, (uint8_t *)src);
  }
}

static void handleRefCountsInitWithTake(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *dest,
                          uint8_t *src);

static void unknownWeakInitWithTake(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  auto *destObject = (WeakReference *)(dest + addrOffset);
  auto *srcObject = (WeakReference *)(src + addrOffset);
  addrOffset += sizeof(WeakReference);

  swift_unknownObjectWeakTakeInit(destObject, srcObject);
}

static void metatypeInitWithTake(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *dest,
                          uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = reader.readBytes<const Metadata *>();
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + type->vw_size();

  type->vw_initializeWithTake(destObject, srcObject);
}

static void existentialInitWithTake(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto* type = getExistentialTypeMetadata((OpaqueValue*)(src + addrOffset));
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + (sizeof(uintptr_t) * NumWords_ValueBuffer);
  if (type->getValueWitnesses()->isValueInline()) {
    type->vw_initializeWithTake(destObject, srcObject);
  } else {
    memcpy(destObject, srcObject, sizeof(uintptr_t));
  }
}

static void resilientInitWithTake(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *dest,
                          uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = getResilientTypeMetadata(metadata, reader);
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + type->vw_size();
  type->vw_initializeWithTake(destObject, srcObject);
}

static const InitFn initWithTakeTable[] = {
  &handleEnd,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  nullptr,
  &unknownWeakInitWithTake,
  &bridgeRetainBranchless,
  nullptr,
  nullptr,
  nullptr, // Custom
  &metatypeInitWithTake,
  nullptr, // Generic
  &existentialInitWithTake,
  &resilientInitWithTake,
  &singlePayloadEnumSimpleBranchless,
  &singlePayloadEnumFNBranchless,
  &singlePayloadEnumFNResolvedBranchless,
  &singlePayloadEnumGenericBranchless,
  &multiPayloadEnumFNBranchless<handleRefCountsInitWithTake>,
  &multiPayloadEnumFNResolvedBranchless<handleRefCountsInitWithTake>,
  &multiPayloadEnumGenericBranchless<handleRefCountsInitWithTake>,
};

static void handleRefCountsInitWithTake(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *dest,
                          uint8_t *src) {
  while (true) {
    uintptr_t _addrOffset = addrOffset;
    auto tag = reader.readBytes<uint64_t>();
    auto offset = (tag & ~(0xFFULL << 56));
    if (SWIFT_UNLIKELY(offset)) {
      memcpy(dest + _addrOffset, src + _addrOffset, offset);
    }
    _addrOffset += offset;
    tag >>= 56;
    if (SWIFT_UNLIKELY(tag == 0)) {
      addrOffset = _addrOffset;
      return;
    }

    if (auto handler = initWithTakeTable[tag]) {
      addrOffset = _addrOffset;
      handler(metadata, reader, addrOffset, dest, src);
    } else {
      memcpy(dest + _addrOffset, src + _addrOffset, sizeof(uintptr_t));
      addrOffset = _addrOffset + sizeof(uintptr_t);
    }
  }
}

extern "C" swift::OpaqueValue *
swift_generic_initWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                           const Metadata *metadata) {
  if (SWIFT_LIKELY(metadata->getValueWitnesses()->isBitwiseTakable())) {
    size_t size = metadata->vw_size();
    memcpy(dest, src, size);
    return dest;
  }

  const uint8_t *layoutStr = metadata->getLayoutString();
  LayoutStringReader1 reader{layoutStr + layoutStringHeaderSize};
  uintptr_t addrOffset = 0;

  handleRefCountsInitWithTake(metadata, reader, addrOffset, (uint8_t *)dest, (uint8_t *)src);

  assert(addrOffset == metadata->vw_size());

  return dest;
}

static void errorAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  SwiftError *destObject = *(SwiftError **)(dest + _addrOffset);
  SwiftError *srcObject = *(SwiftError **)(src + _addrOffset);
  memcpy(dest + addrOffset, &srcObject, sizeof(SwiftError*));
  addrOffset = _addrOffset + sizeof(SwiftError *);
  swift_errorRelease(destObject);
  swift_errorRetain(srcObject);
}

static void nativeStrongAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  uintptr_t destObject = *(uintptr_t *)(dest + _addrOffset);
  uintptr_t srcObject = *(uintptr_t *)(src + _addrOffset);
  memcpy(dest + _addrOffset, &srcObject, sizeof(HeapObject*));
  srcObject &= ~_swift_abi_SwiftSpareBitsMask;
  destObject &= ~_swift_abi_SwiftSpareBitsMask;
  addrOffset = _addrOffset + sizeof(HeapObject *);
  swift_release((HeapObject *)destObject);
  swift_retain((HeapObject *)srcObject);
}

static void unownedAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  uintptr_t destObject = *(uintptr_t *)(dest + _addrOffset);
  uintptr_t srcObject = *(uintptr_t *)(src + _addrOffset);
  memcpy(dest + _addrOffset, &srcObject, sizeof(HeapObject*));
  destObject &= ~_swift_abi_SwiftSpareBitsMask;
  srcObject &= ~_swift_abi_SwiftSpareBitsMask;
  addrOffset = _addrOffset + sizeof(HeapObject *);
  swift_unownedRelease((HeapObject *)destObject);
  swift_unownedRetain((HeapObject *)srcObject);
}

static void weakAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *destObject = (WeakReference *)(dest + _addrOffset);
  auto *srcObject = (WeakReference *)(src + _addrOffset);
  addrOffset = _addrOffset + sizeof(WeakReference);
  swift_weakCopyAssign(destObject, srcObject);
}

static void unknownAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  void *destObject = *(void **)(dest + _addrOffset);
  void *srcObject = *(void **)(src + _addrOffset);
  memcpy(dest + _addrOffset, &srcObject, sizeof(void*));
  addrOffset = _addrOffset + sizeof(void *);
  swift_unknownObjectRelease(destObject);
  swift_unknownObjectRetain(srcObject);
}

static void unknownUnownedAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  UnownedReference *objectDest = (UnownedReference*)(dest + _addrOffset);
  UnownedReference *objectSrc = (UnownedReference*)(src + _addrOffset);
  addrOffset = _addrOffset + sizeof(UnownedReference);
  swift_unknownObjectUnownedCopyAssign(objectDest, objectSrc);
}

static void unknownWeakAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *destObject = (WeakReference *)(dest + _addrOffset);
  auto *srcObject = (WeakReference *)(src + _addrOffset);
  addrOffset = _addrOffset + sizeof(WeakReference);
  swift_unknownObjectWeakCopyAssign(destObject, srcObject);
}

static void bridgeAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  void *destObject = *(void **)(dest + _addrOffset);
  void *srcObject = *(void **)(src + _addrOffset);
  memcpy(dest + _addrOffset, &srcObject, sizeof(void*));
  addrOffset = _addrOffset + sizeof(void*);
  swift_bridgeObjectRelease(destObject);
  swift_bridgeObjectRetain(srcObject);
}

#if SWIFT_OBJC_INTEROP
static void blockAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  _Block_release(*(void **)(dest + _addrOffset));
  auto *copy = _Block_copy(*(void **)(src + _addrOffset));
  memcpy(dest + _addrOffset, &copy, sizeof(void*));
  addrOffset = _addrOffset + sizeof(void*);
}

static void objcStrongAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  uintptr_t destObject = *(uintptr_t *)(dest + _addrOffset);
  uintptr_t srcObject = *(uintptr_t *)(src + _addrOffset);
  memcpy(dest + _addrOffset, &srcObject, sizeof(objc_object*));
  addrOffset = _addrOffset + sizeof(objc_object*);

  if (!(destObject & _swift_abi_ObjCReservedBitsMask)) {
    destObject &= ~_swift_abi_SwiftSpareBitsMask;
    objc_release((objc_object *)destObject);
  }

  if (!(srcObject & _swift_abi_ObjCReservedBitsMask)) {
    srcObject &= ~_swift_abi_SwiftSpareBitsMask;
    objc_retain((objc_object *)srcObject);
  }
}
#endif

static void metatypeAssignWithCopy(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = reader.readBytes<const Metadata *>();
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + type->vw_size();
  type->vw_assignWithCopy(destObject, srcObject);
}

static void existentialAssignWithCopy(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = getExistentialTypeMetadata((OpaqueValue*)(src + _addrOffset));
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + (sizeof(uintptr_t) * NumWords_ValueBuffer);
  if (type->getValueWitnesses()->isValueInline()) {
    type->vw_assignWithCopy(destObject, srcObject);
  } else {
    swift_release(*(HeapObject**)destObject);
    memcpy(destObject, srcObject, sizeof(uintptr_t));
    swift_retain(*(HeapObject**)srcObject);
  }
}

static void resilientAssignWithCopy(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  uintptr_t _addrOffset = addrOffset;
  auto *type = getResilientTypeMetadata(metadata, reader);
  auto *destObject = (OpaqueValue *)(dest + _addrOffset);
  auto *srcObject = (OpaqueValue *)(src + _addrOffset);
  addrOffset = _addrOffset + type->vw_size();
  type->vw_assignWithCopy(destObject, srcObject);
}

static void handleSingleRefCountDestroy(const Metadata *metadata,
                                        LayoutStringReader1 &reader,
                                        uintptr_t &addrOffset,
                                        uint8_t *addr) {
  auto tag = reader.readBytes<uint64_t>();
  addrOffset += (tag & ~(0xFFULL << 56));
  tag >>= 56;
  if (SWIFT_UNLIKELY(tag == 0)) {
    return;
  }
  destroyTableBranchless[tag](metadata, reader, addrOffset, addr);
}

static void handleSingleRefCountInitWithCopy(const Metadata *metadata,
                                        LayoutStringReader1 &reader,
                                        uintptr_t &addrOffset,
                                        uint8_t *dest,
                                        uint8_t *src) {
  auto tag = reader.readBytes<uint64_t>();
  addrOffset += (tag & ~(0xFFULL << 56));
  tag >>= 56;
  if (SWIFT_UNLIKELY(tag == 0)) {
    return;
  }
  initWithCopyTable[tag](metadata, reader, addrOffset, dest, src);
}

static void singlePayloadEnumSimpleAssignWithCopy(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    uint64_t srcTagBytes = 0;
    uint64_t destTagBytes = 0;
    uint64_t byteCountsAndOffset;
    size_t payloadSize;
    uint64_t zeroTagValue;
    size_t xiTagValues;
    size_t refCountBytes;
    size_t skip;

    reader.readBytes(byteCountsAndOffset, payloadSize, zeroTagValue, xiTagValues, refCountBytes, skip);

    auto extraTagBytesPattern = (uint8_t)(byteCountsAndOffset >> 62);
    auto xiTagBytesPattern = ((uint8_t)(byteCountsAndOffset >> 59)) & 0x7;
    auto xiTagBytesOffset =
        byteCountsAndOffset & std::numeric_limits<uint32_t>::max();

    if (SWIFT_UNLIKELY(extraTagBytesPattern)) {
      auto extraTagBytes = 1 << (extraTagBytesPattern - 1);

      srcTagBytes = readTagBytes(src + addrOffset + payloadSize, extraTagBytes);
      destTagBytes = readTagBytes(dest + addrOffset + payloadSize, extraTagBytes);
    }

    if (SWIFT_LIKELY(xiTagBytesPattern)) {
      auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
      srcTagBytes = srcTagBytes ? 0 :
          readTagBytes(src + addrOffset + xiTagBytesOffset, xiTagBytes) -
          zeroTagValue;
      destTagBytes = destTagBytes ? 0 :
          readTagBytes(dest + addrOffset + xiTagBytesOffset, xiTagBytes) -
          zeroTagValue;
    }

    if (srcTagBytes >= xiTagValues && destTagBytes >= xiTagValues) {
      return;
    } else if (srcTagBytes >= xiTagValues) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      while (reader.layoutStr < end) {
        handleSingleRefCountInitWithCopy(metadata, reader, addrOffset, dest, src);
      }
      return;
    } else if (destTagBytes >= xiTagValues) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      auto nestedAddrOffset = addrOffset;
      while (reader.layoutStr < end) {
        handleSingleRefCountDestroy(metadata, reader, nestedAddrOffset, dest);
      }
    } else {
      reader.skip(refCountBytes);
    }

    memcpy(dest + addrOffset, src + addrOffset, skip);
    addrOffset += skip;
  });
}

static void singlePayloadEnumFNAssignWithCopy(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
    size_t refCountBytes;
    size_t skip;
    reader.readBytes(refCountBytes, skip);

    unsigned srcTag = getEnumTag(src + addrOffset);
    unsigned destTag = getEnumTag(dest + addrOffset);

    if (SWIFT_UNLIKELY(srcTag == 0 && destTag == 0)) {
      return;
    } else if (srcTag == 0) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      while (reader.layoutStr < end) {
        handleSingleRefCountInitWithCopy(metadata, reader, addrOffset, dest, src);
      }
      return;
    } else if (destTag == 0) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      auto nestedAddrOffset = addrOffset;
      while (reader.layoutStr < end) {
        handleSingleRefCountDestroy(metadata, reader, nestedAddrOffset, dest);
      }
    } else {
      reader.skip(refCountBytes);
    }

    memcpy(dest + addrOffset, src + addrOffset, skip);
    addrOffset += skip;
  });
}

static void singlePayloadEnumFNResolvedAssignWithCopy(const Metadata *metadata,
                               LayoutStringReader1 &reader,
                               uintptr_t &addrOffset,
                               uint8_t *dest,
                               uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag;
    size_t refCountBytes;
    size_t skip;
    reader.readBytes(getEnumTag, refCountBytes, skip);

    unsigned srcTag = getEnumTag(src + addrOffset);
    unsigned destTag = getEnumTag(dest + addrOffset);

    if (SWIFT_UNLIKELY(srcTag == 0 && destTag == 0)) {
      return;
    } else if (srcTag == 0) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      while (reader.layoutStr < end) {
        handleSingleRefCountInitWithCopy(metadata, reader, addrOffset, dest, src);
      }
      return;
    } else if (destTag == 0) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      auto nestedAddrOffset = addrOffset;
      while (reader.layoutStr < end) {
        handleSingleRefCountDestroy(metadata, reader, nestedAddrOffset, dest);
      }
    } else {
      reader.skip(refCountBytes);
    }

    memcpy(dest + addrOffset, src + addrOffset, skip);
    addrOffset += skip;
  });
}

static void singlePayloadEnumGenericAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  reader.modify([&](LayoutStringReader1 &reader) {
    uint64_t srcTag = 0;
    uint64_t destTag = 0;
    auto tagBytesAndOffset = reader.readBytes<uint64_t>();
    auto payloadSize = reader.readBytes<size_t>();
    auto *xiType = reader.readBytes<const Metadata *>();
    auto numEmptyCases = reader.readBytes<unsigned>();
    auto refCountBytes = reader.readBytes<size_t>();
    auto skip = reader.readBytes<size_t>();

    auto extraTagBytesPattern = (uint8_t)(tagBytesAndOffset >> 62);
    auto xiTagBytesOffset =
        tagBytesAndOffset & std::numeric_limits<uint32_t>::max();

    if (SWIFT_UNLIKELY(extraTagBytesPattern)) {
      auto extraTagBytes = 1 << (extraTagBytesPattern - 1);
      srcTag = readTagBytes(src + addrOffset + payloadSize, extraTagBytes);
      destTag = readTagBytes(dest + addrOffset + payloadSize, extraTagBytes);
    }

    if (SWIFT_LIKELY(xiType)) {
      if (!srcTag) {
        srcTag = xiType->vw_getEnumTagSinglePayload(
            (const OpaqueValue *)(src + addrOffset + xiTagBytesOffset),
            numEmptyCases);
      }

      if (!destTag) {
        destTag = xiType->vw_getEnumTagSinglePayload(
            (const OpaqueValue *)(dest + addrOffset + xiTagBytesOffset),
            numEmptyCases);
      }
    }

    if (SWIFT_UNLIKELY(srcTag == 0 && destTag == 0)) {
      return;
    } else if (srcTag == 0) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      while (reader.layoutStr < end) {
        handleSingleRefCountInitWithCopy(metadata, reader, addrOffset, dest, src);
      }
      return;
    } else if (destTag == 0) {
      const uint8_t *end = (reader.layoutStr + refCountBytes);
      auto nestedAddrOffset = addrOffset;
      while (reader.layoutStr < end) {
        handleSingleRefCountDestroy(metadata, reader, nestedAddrOffset, dest);
      }
    } else {
      reader.skip(refCountBytes);
    }

    memcpy(dest + addrOffset, src + addrOffset, skip);
    addrOffset += skip;
  });
}

static void multiPayloadEnumFNAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  LayoutStringReader1 nestedReader;
  uintptr_t nestedAddrOffset;
  unsigned srcTag;
  unsigned destTag;

  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
    reader.readBytes(numPayloads, refCountBytes, enumSize);
    nestedReader = reader;
    nestedAddrOffset = addrOffset;

    srcTag = getEnumTag(src + addrOffset);
    destTag = getEnumTag(dest + addrOffset);
    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(srcTag < numPayloads && destTag < numPayloads)) {
    addrOffset += enumSize;
    size_t srcRefCountOffset = nestedReader.peekBytes<size_t>(srcTag * sizeof(size_t));
    size_t destRefCountOffset = nestedReader.peekBytes<size_t>(destTag * sizeof(size_t));
    LayoutStringReader1 nestedReaderDest = nestedReader;
    nestedReader.skip((numPayloads * sizeof(size_t)) + srcRefCountOffset);
    nestedReaderDest.skip((numPayloads * sizeof(size_t)) + destRefCountOffset);
    auto nestedAddrOffsetDest = nestedAddrOffset;
    handleRefCountsDestroy(metadata, nestedReaderDest, nestedAddrOffsetDest, dest);
    handleRefCountsInitWithCopy(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
    return;
  } else if (srcTag < numPayloads) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(srcTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    handleRefCountsInitWithCopy(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
    return;
  } else if (destTag < numPayloads) {
    size_t refCountOffset =
        nestedReader.peekBytes<size_t>(destTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    handleRefCountsDestroy(metadata, nestedReader, nestedAddrOffset, dest);
  }

  memcpy(dest + addrOffset, src + addrOffset, enumSize);
  addrOffset += enumSize;
}

static void multiPayloadEnumFNResolvedAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  LayoutStringReader1 nestedReader;
  uintptr_t nestedAddrOffset;
  unsigned srcTag;
  unsigned destTag;

  reader.modify([&](LayoutStringReader1 &reader) {
    GetEnumTagFn getEnumTag = reader.readBytes<GetEnumTagFn>();
    reader.readBytes(numPayloads, refCountBytes, enumSize);
    nestedReader = reader;
    nestedAddrOffset = addrOffset;

    srcTag = getEnumTag(src + addrOffset);
    destTag = getEnumTag(dest + addrOffset);
    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(srcTag < numPayloads && destTag < numPayloads)) {
    addrOffset += enumSize;
    size_t srcRefCountOffset = nestedReader.peekBytes<size_t>(srcTag * sizeof(size_t));
    size_t destRefCountOffset = nestedReader.peekBytes<size_t>(destTag * sizeof(size_t));
    LayoutStringReader1 nestedReaderDest = nestedReader;
    nestedReader.skip((numPayloads * sizeof(size_t)) + srcRefCountOffset);
    nestedReaderDest.skip((numPayloads * sizeof(size_t)) + destRefCountOffset);
    auto nestedAddrOffsetDest = nestedAddrOffset;
    handleRefCountsDestroy(metadata, nestedReaderDest, nestedAddrOffsetDest, dest);
    handleRefCountsInitWithCopy(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
    return;
  } else if (srcTag < numPayloads) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(srcTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    handleRefCountsInitWithCopy(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
    return;
  } else if (destTag < numPayloads) {
    size_t refCountOffset =
        nestedReader.peekBytes<size_t>(destTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    handleRefCountsDestroy(metadata, nestedReader, nestedAddrOffset, dest);
  }

  memcpy(dest + addrOffset, src + addrOffset, enumSize);
  addrOffset += enumSize;
}

static void multiPayloadEnumGenericAssignWithCopy(const Metadata *metadata,
                             LayoutStringReader1 &reader,
                             uintptr_t &addrOffset,
                             uint8_t *dest,
                             uint8_t *src) {
  size_t tagBytes;
  size_t numPayloads;
  size_t refCountBytes;
  size_t enumSize;
  uint64_t srcTag;
  uint64_t destTag;
  uintptr_t nestedAddrOffset;
  LayoutStringReader1 nestedReader;
  reader.modify([&](LayoutStringReader1 &reader) {
    reader.readBytes(tagBytes, numPayloads, refCountBytes, enumSize);

    nestedReader = reader;
    nestedAddrOffset = addrOffset;
    auto tagBytesOffset = enumSize - tagBytes;

    srcTag = readTagBytes(src + addrOffset + tagBytesOffset, tagBytes);
    destTag = readTagBytes(dest + addrOffset + tagBytesOffset, tagBytes);

    reader.skip(refCountBytes + (numPayloads * sizeof(size_t)));
  });

  if (SWIFT_LIKELY(srcTag < numPayloads && destTag < numPayloads)) {
    addrOffset += enumSize;
    size_t srcRefCountOffset = nestedReader.peekBytes<size_t>(srcTag * sizeof(size_t));
    size_t destRefCountOffset = nestedReader.peekBytes<size_t>(destTag * sizeof(size_t));
    LayoutStringReader1 nestedReaderDest = nestedReader;
    nestedReader.skip((numPayloads * sizeof(size_t)) + srcRefCountOffset);
    nestedReaderDest.skip((numPayloads * sizeof(size_t)) + destRefCountOffset);
    auto nestedAddrOffsetDest = nestedAddrOffset;
    handleRefCountsDestroy(metadata, nestedReaderDest, nestedAddrOffsetDest, dest);
    handleRefCountsInitWithCopy(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
    return;
  } else if (srcTag < numPayloads) {
    addrOffset += enumSize;
    size_t refCountOffset = nestedReader.peekBytes<size_t>(srcTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    handleRefCountsInitWithCopy(metadata, nestedReader, nestedAddrOffset, dest, src);
    auto trailingBytes = addrOffset - nestedAddrOffset;
    if (trailingBytes)
      memcpy(dest + nestedAddrOffset, src + nestedAddrOffset, trailingBytes);
    return;
  } else if (destTag < numPayloads) {
    size_t refCountOffset =
        nestedReader.peekBytes<size_t>(destTag * sizeof(size_t));
    nestedReader.skip((numPayloads * sizeof(size_t)) + refCountOffset);
    handleRefCountsDestroy(metadata, nestedReader, nestedAddrOffset, dest);
  }

  memcpy(dest + addrOffset, src + addrOffset, enumSize);
  addrOffset += enumSize;
}

static const InitFn assignWithCopyTable[] = {
  &handleEnd,
  &errorAssignWithCopy,
  &nativeStrongAssignWithCopy,
  &unownedAssignWithCopy,
  &weakAssignWithCopy,
  &unknownAssignWithCopy,
  &unknownUnownedAssignWithCopy,
  &unknownWeakAssignWithCopy,
  &bridgeAssignWithCopy,
#if SWIFT_OBJC_INTEROP
  &blockAssignWithCopy,
  &objcStrongAssignWithCopy,
#else
  nullptr,
  nullptr,
#endif
  nullptr, // Custom
  &metatypeAssignWithCopy,
  nullptr, // Generic
  &existentialAssignWithCopy,
  &resilientAssignWithCopy,
  &singlePayloadEnumSimpleAssignWithCopy,
  &singlePayloadEnumFNAssignWithCopy,
  &singlePayloadEnumFNResolvedAssignWithCopy,
  &singlePayloadEnumGenericAssignWithCopy,
  &multiPayloadEnumFNAssignWithCopy,
  &multiPayloadEnumFNResolvedAssignWithCopy,
  &multiPayloadEnumGenericAssignWithCopy,
};

static void handleRefCountsAssignWithCopy(const Metadata *metadata,
                          LayoutStringReader1 &reader,
                          uintptr_t &addrOffset,
                          uint8_t *dest,
                          uint8_t *src) {
  while (true) {
    uintptr_t _addrOffset = addrOffset;
    auto tag = reader.readBytes<uint64_t>();
    auto offset = (tag & ~(0xFFULL << 56));
    if (offset) {
      memcpy(dest + _addrOffset, src + _addrOffset, offset);
    }
    addrOffset = _addrOffset + offset;
    tag >>= 56;
    if (SWIFT_UNLIKELY(tag == 0)) {
      return;
    }

    assignWithCopyTable[tag](metadata, reader, addrOffset, dest, src);
  }
}

extern "C" swift::OpaqueValue *
swift_generic_assignWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                             const Metadata *metadata) {
  const uint8_t *layoutStr = metadata->getLayoutString();
  LayoutStringReader1 reader{layoutStr + layoutStringHeaderSize};
  uintptr_t addrOffset = 0;
  handleRefCountsAssignWithCopy(metadata, reader, addrOffset, (uint8_t *)dest,
                                (uint8_t *)src);

  assert(addrOffset == metadata->vw_size());

  return dest;
}

void swift::swift_generic_arrayAssignWithCopy(swift::OpaqueValue *dest,
                                              swift::OpaqueValue *src,
                                              size_t count, size_t stride,
                                              const Metadata *metadata) {
  const uint8_t *layoutStr = metadata->getLayoutString();
  for (size_t i = 0; i < count; i++) {
    LayoutStringReader1 reader{layoutStr + layoutStringHeaderSize};
    uintptr_t addrOffset = i * stride;
    handleRefCountsAssignWithCopy(metadata, reader, addrOffset, (uint8_t *)dest,
                                  (uint8_t *)src);
  }
}

extern "C" swift::OpaqueValue *
swift_generic_assignWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src,
                             const Metadata *metadata) {
  swift_generic_destroy(dest, metadata);
  return swift_generic_initWithTake(dest, src, metadata);
}

extern "C" unsigned swift_singletonEnum_getEnumTag(swift::OpaqueValue *address,
                                                   const Metadata *metadata) {
  return 0;
}

extern "C" void swift_singletonEnum_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  return;
}

template <typename T>
static inline T handleSinglePayloadEnumSimpleTag(
    LayoutStringReader &reader, uint8_t *addr,
    std::function<std::optional<T>(size_t, size_t, uint8_t)>
        extraTagBytesHandler,
    std::function<T(size_t, uint64_t, uint8_t, unsigned, size_t, uint8_t)>
        xiHandler) {
  auto byteCountsAndOffset = reader.readBytes<uint64_t>();
  auto extraTagBytesPattern = (uint8_t)(byteCountsAndOffset >> 62);
  auto xiTagBytesPattern = ((uint8_t)(byteCountsAndOffset >> 59)) & 0x7;
  auto xiTagBytesOffset =
      byteCountsAndOffset & std::numeric_limits<uint32_t>::max();
  auto numExtraTagBytes = 1 << (extraTagBytesPattern - 1);
  auto payloadSize = reader.readBytes<size_t>();
  auto zeroTagValue = reader.readBytes<uint64_t>();
  auto payloadNumExtraInhabitants = reader.readBytes<size_t>();

  if (extraTagBytesPattern) {
    if (auto result = extraTagBytesHandler(payloadNumExtraInhabitants,
                                           payloadSize, numExtraTagBytes)) {
      return *result;
    }
  }

  return xiHandler(payloadNumExtraInhabitants, zeroTagValue, xiTagBytesPattern,
                   xiTagBytesOffset, payloadSize, numExtraTagBytes);
}

extern "C" unsigned swift_enumSimple_getEnumTag(swift::OpaqueValue *address,
                                                const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [addr](size_t payloadNumExtraInhabitants, size_t payloadSize,
             uint8_t numExtraTagBytes) -> std::optional<unsigned> {
    auto tagBytes = readTagBytes(addr + payloadSize, numExtraTagBytes);
    if (tagBytes) {
      unsigned caseIndexFromExtraTagBits =
          payloadSize >= 4 ? 0 : (tagBytes - 1U) << (payloadSize * 8U);
      unsigned caseIndexFromValue = loadEnumElement(addr, payloadSize);
      unsigned noPayloadIndex =
          (caseIndexFromExtraTagBits | caseIndexFromValue) +
          payloadNumExtraInhabitants;
      return noPayloadIndex + 1;
    }

    return std::nullopt;
  };

  auto xihandler = [addr](size_t payloadNumExtraInhabitants,
                          uint64_t zeroTagValue, uint8_t xiTagBytesPattern,
                          unsigned xiTagBytesOffset, size_t payloadSize,
                          uint8_t numExtraTagBytes) -> unsigned {
    if (xiTagBytesPattern) {
      auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
      uint64_t tagBytes =
          readTagBytes(addr + xiTagBytesOffset, xiTagBytes) - zeroTagValue;
      if (tagBytes < payloadNumExtraInhabitants) {
        return tagBytes + 1;
      }
    }

    return 0;
  };

  return handleSinglePayloadEnumSimpleTag<unsigned>(
      reader, addr, extraTagBytesHandler, xihandler);
}

extern "C" void swift_enumSimple_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [addr, tag](size_t payloadNumExtraInhabitants, size_t payloadSize,
                  uint8_t numExtraTagBytes) -> std::optional<bool> {
    if (tag <= payloadNumExtraInhabitants) {
      return std::nullopt;
    }

    unsigned noPayloadIndex = tag - 1;
    unsigned caseIndex = noPayloadIndex - payloadNumExtraInhabitants;
    unsigned payloadIndex, extraTagIndex;
    if (payloadSize >= 4) {
      extraTagIndex = 1;
      payloadIndex = caseIndex;
    } else {
      unsigned payloadBits = payloadSize * 8U;
      extraTagIndex = 1U + (caseIndex >> payloadBits);
      payloadIndex = caseIndex & ((1U << payloadBits) - 1U);
    }

    // Store into the value.
    if (payloadSize)
      storeEnumElement(addr, payloadIndex, payloadSize);
    if (numExtraTagBytes)
      storeEnumElement(addr + payloadSize, extraTagIndex, numExtraTagBytes);

    return true;
  };

  auto xihandler = [addr, tag](size_t payloadNumExtraInhabitants,
                               uint64_t zeroTagValue, uint8_t xiTagBytesPattern,
                               unsigned xiTagBytesOffset, size_t payloadSize,
                               uint8_t numExtraTagBytes) -> bool {
    if (xiTagBytesPattern) {
      auto xiTagBytes = 1 << (xiTagBytesPattern - 1);
      if (tag <= payloadNumExtraInhabitants) {
        if (numExtraTagBytes != 0)
          storeEnumElement(addr + payloadSize, 0, numExtraTagBytes);

        if (tag == 0)
          return true;

        storeEnumElement(addr + xiTagBytesOffset, tag - 1 + zeroTagValue,
                         xiTagBytes);
      }
    }
    return true;
  };

  handleSinglePayloadEnumSimpleTag<bool>(reader, addr, extraTagBytesHandler,
                                         xihandler);
}

extern "C"
unsigned swift_enumFn_getEnumTag(swift::OpaqueValue *address,
                                 const Metadata *metadata) {
  auto addr = reinterpret_cast<const uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};
  auto getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);

  return getEnumTag(addr);
}

extern "C" unsigned
swift_multiPayloadEnumGeneric_getEnumTag(swift::OpaqueValue *address,
                                         const Metadata *metadata) {
  auto addr = reinterpret_cast<const uint8_t *>(address);
  LayoutStringReader1 reader{metadata->getLayoutString() +
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto tagBytes = reader.readBytes<size_t>();
  auto numPayloads = reader.readBytes<size_t>();
  reader.skip(sizeof(size_t));
  auto enumSize = reader.readBytes<size_t>();
  auto payloadSize = enumSize - tagBytes;

  auto enumTag = (unsigned)readTagBytes(addr + payloadSize, tagBytes);
  if (enumTag < numPayloads) {
    return enumTag;
  }

  auto payloadValue = loadEnumElement(addr, payloadSize);

  if (payloadSize >= 4) {
    return numPayloads + payloadValue;
  } else {
    unsigned numPayloadBits = payloadSize * CHAR_BIT;
    return (payloadValue | (enumTag - numPayloads) << numPayloadBits) +
           numPayloads;
  }
}

extern "C" void swift_multiPayloadEnumGeneric_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto numTagBytes = reader.readBytes<size_t>();
  auto numPayloads = reader.readBytes<size_t>();
  reader.skip(sizeof(size_t));
  auto enumSize = reader.readBytes<size_t>();
  auto payloadSize = enumSize - numTagBytes;

  if (tag < numPayloads) {
    // For a payload case, store the tag after the payload area.
    auto tagBytes = addr + payloadSize;
    storeEnumElement(tagBytes, tag, numTagBytes);
  } else {
    // For an empty case, factor out the parts that go in the payload and
    // tag areas.
    unsigned whichEmptyCase = tag - numPayloads;
    unsigned whichTag, whichPayloadValue;
    if (payloadSize >= 4) {
      whichTag = numPayloads;
      whichPayloadValue = whichEmptyCase;
    } else {
      unsigned numPayloadBits = payloadSize * CHAR_BIT;
      whichTag = numPayloads + (whichEmptyCase >> numPayloadBits);
      whichPayloadValue = whichEmptyCase & ((1U << numPayloadBits) - 1U);
    }
    auto tagBytes = addr + payloadSize;
    storeEnumElement(tagBytes, whichTag, numTagBytes);
    storeEnumElement(addr, whichPayloadValue, payloadSize);
  }
}

template <typename T>
static inline T handleSinglePayloadEnumGenericTag(
    LayoutStringReader &reader, uint8_t *addr,
    std::function<std::optional<T>(const Metadata *, size_t, uint8_t)>
        extraTagBytesHandler,
    std::function<T(const Metadata *, unsigned, unsigned, size_t, uint8_t)>
        xiHandler) {
  auto tagBytesAndOffset = reader.readBytes<uint64_t>();
  auto extraTagBytesPattern = (uint8_t)(tagBytesAndOffset >> 62);
  auto xiTagBytesOffset =
      tagBytesAndOffset & std::numeric_limits<uint32_t>::max();
  auto numExtraTagBytes = 1 << (extraTagBytesPattern - 1);
  auto payloadSize = reader.readBytes<size_t>();
  auto xiType = reader.readBytes<const Metadata *>();

  if (extraTagBytesPattern) {
    if (auto result =
            extraTagBytesHandler(xiType, payloadSize, numExtraTagBytes)) {
      return *result;
    }
  }

  auto numEmptyCases = reader.readBytes<unsigned>();

  return xiHandler(xiType, xiTagBytesOffset, numEmptyCases, payloadSize,
                   numExtraTagBytes);
}

extern "C" unsigned
swift_singlePayloadEnumGeneric_getEnumTag(swift::OpaqueValue *address,
                                          const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [addr](const Metadata *xiType, size_t payloadSize,
             uint8_t numExtraTagBytes) -> std::optional<unsigned> {
    auto tagBytes = readTagBytes(addr + payloadSize, numExtraTagBytes);
    if (tagBytes) {
      unsigned payloadNumExtraInhabitants =
          xiType ? xiType->vw_getNumExtraInhabitants() : 0;
      unsigned caseIndexFromExtraTagBits =
          payloadSize >= 4 ? 0 : (tagBytes - 1U) << (payloadSize * 8U);
      unsigned caseIndexFromValue = loadEnumElement(addr, payloadSize);
      unsigned noPayloadIndex =
          (caseIndexFromExtraTagBits | caseIndexFromValue) +
          payloadNumExtraInhabitants;
      return noPayloadIndex + 1;
    }

    return std::nullopt;
  };

  auto xihandler = [addr](const Metadata *xiType, unsigned xiTagBytesOffset,
                          unsigned numEmptyCases, size_t payloadSize,
                          uint8_t numExtraTagBytes) -> unsigned {
    if (xiType) {
      return xiType->vw_getEnumTagSinglePayload(
          (const OpaqueValue *)(addr + xiTagBytesOffset), numEmptyCases);
    }

    return 0;
  };

  return handleSinglePayloadEnumGenericTag<unsigned>(
      reader, addr, extraTagBytesHandler, xihandler);
}

extern "C" void swift_singlePayloadEnumGeneric_destructiveInjectEnumTag(
    swift::OpaqueValue *address, unsigned tag, const Metadata *metadata) {
  auto addr = reinterpret_cast<uint8_t *>(address);
  LayoutStringReader reader{metadata->getLayoutString(),
                            layoutStringHeaderSize + sizeof(uint64_t)};

  auto extraTagBytesHandler =
      [=](const Metadata *xiType, size_t payloadSize,
          uint8_t numExtraTagBytes) -> std::optional<bool> {
    unsigned payloadNumExtraInhabitants =
        xiType ? xiType->vw_getNumExtraInhabitants() : 0;
    if (tag <= payloadNumExtraInhabitants) {
      return std::nullopt;
    }

    unsigned noPayloadIndex = tag - 1;
    unsigned caseIndex = noPayloadIndex - payloadNumExtraInhabitants;
    unsigned payloadIndex, extraTagIndex;
    if (payloadSize >= 4) {
      extraTagIndex = 1;
      payloadIndex = caseIndex;
    } else {
      unsigned payloadBits = payloadSize * 8U;
      extraTagIndex = 1U + (caseIndex >> payloadBits);
      payloadIndex = caseIndex & ((1U << payloadBits) - 1U);
    }

    // Store into the value.
    if (payloadSize)
      storeEnumElement(addr, payloadIndex, payloadSize);
    if (numExtraTagBytes)
      storeEnumElement(addr + payloadSize, extraTagIndex, numExtraTagBytes);

    return true;
  };

  auto xihandler = [=](const Metadata *xiType, unsigned xiTagBytesOffset,
                       unsigned numEmptyCases, size_t payloadSize,
                       uint8_t numExtraTagBytes) -> bool {
    unsigned payloadNumExtraInhabitants =
        xiType ? xiType->vw_getNumExtraInhabitants() : 0;
    if (tag <= payloadNumExtraInhabitants) {
      if (numExtraTagBytes != 0)
        storeEnumElement(addr + payloadSize, 0, numExtraTagBytes);

      if (tag == 0)
        return true;

      xiType->vw_storeEnumTagSinglePayload(
          (swift::OpaqueValue *)(addr + xiTagBytesOffset), tag, numEmptyCases);
    }
    return true;
  };

  handleSinglePayloadEnumGenericTag<bool>(reader, addr, extraTagBytesHandler,
                                          xihandler);
}

extern "C" swift::OpaqueValue *
swift_generic_initializeBufferWithCopyOfBuffer(swift::ValueBuffer *dest,
                                               swift::ValueBuffer *src,
                                               const Metadata *metadata) {
  if (metadata->getValueWitnesses()->isValueInline()) {
    return swift_generic_initWithCopy((swift::OpaqueValue *)dest,
                                      (swift::OpaqueValue *)src, metadata);
  } else {
    memcpy(dest, src, sizeof(swift::HeapObject *));
    swift_retain(*(swift::HeapObject **)src);
    return (swift::OpaqueValue *)&(*(swift::HeapObject **)dest)[1];
  }
}

void swift::swift_resolve_resilientAccessors(uint8_t *layoutStr,
                                             size_t layoutStrOffset,
                                             const uint8_t *fieldLayoutStr,
                                             const Metadata *fieldType) {
  LayoutStringWriter writer{layoutStr, layoutStrOffset};
  LayoutStringReader reader{fieldLayoutStr, 0};
  while (true) {
    size_t currentOffset = reader.offset + layoutStringHeaderSize;
    uint64_t size = reader.readBytes<uint64_t>();
    RefCountingKind tag = (RefCountingKind)(size >> 56);
    size &= ~(0xffULL << 56);

    switch (tag) {
    case RefCountingKind::End:
      return;
    case RefCountingKind::Resilient: {
      auto *type = getResilientTypeMetadata(fieldType, reader);
      writer.offset = layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::Metatype) << 56) | size;
      writer.writeBytes(tagAndOffset);
      writer.writeBytes(type);
      break;
    }
    case RefCountingKind::Metatype:
      reader.skip(sizeof(uintptr_t));
      break;
    case RefCountingKind::SinglePayloadEnumSimple:
      reader.skip((2 * sizeof(uint64_t)) + (4 * sizeof(size_t)));
      break;

    case RefCountingKind::SinglePayloadEnumFN: {
      auto getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
      writer.offset = layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::SinglePayloadEnumFNResolved) << 56) |
          size;
      writer.writeBytes(tagAndOffset);
      writer.writeBytes(getEnumTag);
      reader.skip(2 * sizeof(size_t));
      break;
    }

    case RefCountingKind::SinglePayloadEnumFNResolved:
      reader.skip(3 * sizeof(size_t));
      break;

    case RefCountingKind::SinglePayloadEnumGeneric: {
      reader.skip(sizeof(uint64_t) +  // tag + offset
                  sizeof(uint64_t) +  // extra tag bytes + XI offset
                  sizeof(size_t) +    // payload size
                  sizeof(uintptr_t) + // XI metadata
                  sizeof(unsigned));  // num empty cases
      auto refCountBytes = reader.readBytes<size_t>();
      reader.skip(sizeof(size_t) + // bytes to skip if no payload case
                  refCountBytes);
      break;
    }

    case RefCountingKind::MultiPayloadEnumFN: {
      auto getEnumTag = readRelativeFunctionPointer<GetEnumTagFn>(reader);
      writer.offset = layoutStrOffset + currentOffset - layoutStringHeaderSize;
      uint64_t tagAndOffset =
          (((uint64_t)RefCountingKind::MultiPayloadEnumFNResolved) << 56) |
          size;
      writer.writeBytes(tagAndOffset);
      writer.writeBytes(getEnumTag);

      size_t numCases = reader.readBytes<size_t>();
      auto refCountBytes = reader.readBytes<size_t>();

      // skip enum size
      reader.skip(sizeof(size_t));

      size_t casesBeginOffset = layoutStrOffset + reader.offset +
                                (numCases * sizeof(size_t));

      auto fieldCasesBeginOffset = fieldLayoutStr + (numCases * sizeof(size_t)) + reader.offset;
      for (size_t j = 0; j < numCases; j++) {
        size_t caseOffset = reader.readBytes<size_t>();
        const uint8_t *caseLayoutString = fieldCasesBeginOffset +
                                          caseOffset;
        swift_resolve_resilientAccessors(layoutStr,
                                         casesBeginOffset + caseOffset,
                                         caseLayoutString, fieldType);
      }
      reader.skip(refCountBytes);
      break;
    }

    case RefCountingKind::MultiPayloadEnumFNResolved: {
      // skip function pointer
      reader.skip(sizeof(uintptr_t));
      size_t numCases = reader.readBytes<size_t>();
      size_t refCountBytes = reader.readBytes<size_t>();
      // skip enum size, offsets and ref counts
      reader.skip(sizeof(size_t) + (numCases * sizeof(size_t)) + refCountBytes);
      break;
    }

    case RefCountingKind::MultiPayloadEnumGeneric: {
      reader.skip(sizeof(size_t));
      auto numPayloads = reader.readBytes<size_t>();
      auto refCountBytes = reader.readBytes<size_t>();
      reader.skip(sizeof(size_t) * (numPayloads + 1) + refCountBytes);
      break;
    }

    default:
      break;
    }
  }
}

extern "C"
void swift_generic_instantiateLayoutString(const uint8_t* layoutStr,
                                           Metadata* type) {
  type->setLayoutString(layoutStr);
}
