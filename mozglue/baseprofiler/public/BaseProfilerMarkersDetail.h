/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BaseProfilerMarkersDetail_h
#define BaseProfilerMarkersDetail_h

#ifndef BaseProfilerMarkers_h
#  error "This header should only be #included by BaseProfilerMarkers.h"
#endif

#include "mozilla/BaseProfilerMarkersPrerequisites.h"

#ifdef MOZ_GECKO_PROFILER

//                        ~~ HERE BE DRAGONS ~~
//
// Everything below is internal implementation detail, you shouldn't need to
// look at it unless working on the profiler code.

#  include "mozilla/JSONWriter.h"
#  include "mozilla/ProfileBufferEntryKinds.h"

#  include <limits>
#  include <tuple>

namespace mozilla::baseprofiler {
// Implemented in platform.cpp
MFBT_API ProfileChunkedBuffer& profiler_get_core_buffer();
}  // namespace mozilla::baseprofiler

namespace mozilla::base_profiler_markers_detail {

// Get the core buffer from the profiler, and cache it in a
// non-templated-function static reference.
inline ProfileChunkedBuffer& CachedBaseCoreBuffer() {
  static ProfileChunkedBuffer& coreBuffer =
      baseprofiler::profiler_get_core_buffer();
  return coreBuffer;
}

struct Streaming {
  // A `Deserializer` is a free function that can read a serialized payload from
  // an `EntryReader` and streams it as JSON object properties.
  using Deserializer = void (*)(ProfileBufferEntryReader&, JSONWriter&);

  // A `DeserializerTag` will be added before the payload, to help select the
  // correct deserializer when reading back the payload.
  using DeserializerTag = uint8_t;

  // Store a deserializer and get its `DeserializerTag`.
  // This is intended to be only used once per deserializer, so it should be
  // called to initialize a `static const` tag that will be re-used by all
  // markers of the corresponding payload type.
  MFBT_API static DeserializerTag TagForDeserializer(
      Deserializer aDeserializer);

  // Get the `Deserializer` for a given `DeserializerTag`.
  MFBT_API static Deserializer DeserializerForTag(DeserializerTag aTag);
};

// This helper will examine a marker type's `StreamJSONMarkerData` function, see
// specialization below.
template <typename T>
struct StreamFunctionTypeHelper;

// Helper specialization that takes the expected
// `StreamJSONMarkerData(JSONWriter&, ...)` function and provide information
// about the `...` parameters.
template <typename R, typename... As>
struct StreamFunctionTypeHelper<R(JSONWriter&, As...)> {
  constexpr static size_t scArity = sizeof...(As);
  using TupleType =
      std::tuple<std::remove_cv_t<std::remove_reference_t<As>>...>;

  // Serialization function that takes the exact same parameter types
  // (const-ref'd) as `StreamJSONMarkerData`. This has to be inside the helper
  // because only here can we access the raw parameter pack `As...`.
  // And because we're using the same argument types through
  // references-to-const, permitted implicit conversions can happen.
  static ProfileBufferBlockIndex Serialize(
      ProfileChunkedBuffer& aBuffer, const ProfilerString8View& aName,
      MarkerOptions&& aOptions, Streaming::DeserializerTag aDeserializerTag,
      const As&... aAs) {
    // Note that options are first after the entry kind, because they contain
    // the thread id, which is handled first to filter markers by threads.
    return aBuffer.PutObjects(ProfileBufferEntryKind::Marker, aOptions, aName,
                              aDeserializerTag, aAs...);
  }
};

// Helper for a marker type.
// A marker type is defined in a `struct` with some expected static member
// functions. See example in BaseProfilerMarkers.h.
template <typename MarkerType>
struct MarkerTypeSerialization {
  // Definitions to access the expected `StreamJSONMarkerData(JSONWriter&, ...)`
  // function and its parameters.
  using StreamFunctionType =
      StreamFunctionTypeHelper<decltype(MarkerType::StreamJSONMarkerData)>;
  constexpr static size_t scStreamFunctionParameterCount =
      StreamFunctionType::scArity;
  using StreamFunctionUserParametersTuple =
      typename StreamFunctionType::TupleType;
  template <size_t i>
  using StreamFunctionParameter =
      std::tuple_element_t<i, StreamFunctionUserParametersTuple>;

  template <typename... Ts>
  static ProfileBufferBlockIndex Serialize(ProfileChunkedBuffer& aBuffer,
                                           const ProfilerString8View& aName,
                                           MarkerOptions&& aOptions,
                                           const Ts&... aTs) {
    static_assert(!std::is_same_v<MarkerType,
                                  ::mozilla::baseprofiler::markers::NoPayload>,
                  "NoPayload should have been handled in the caller.");
    // Note that the tag is stored in a function-static object, and this
    // function is static in a templated struct, so there should only be one tag
    // per MarkerType.
    // Making the tag class-static may have been more efficient (to avoid a
    // thread-safe init check at every call), but random global static
    // initialization order would make it more complex to coordinate with
    // `Streaming::TagForDeserializer()`, and also would add a (small) cost for
    // everybody, even the majority of users not using the profiler.
    static const Streaming::DeserializerTag tag =
        Streaming::TagForDeserializer(Deserialize);
    return StreamFunctionType::Serialize(aBuffer, aName, std::move(aOptions),
                                         tag, aTs...);
  }

 private:
  // This templated function will recursively deserialize each argument expected
  // by `MarkerType::StreamJSONMarkerData()` on the stack, and call it at the
  // end. E.g., for `StreamJSONMarkerData(int, char)`:
  // - DeserializeArguments<0>(aER, aWriter) reads an int and calls:
  // - DeserializeArguments<1>(aER, aWriter, const int&) reads a char and calls:
  // - MarkerType::StreamJSONMarkerData(aWriter, const int&, const char&).
  // Prototyping on godbolt showed that clang and gcc can flatten these
  // recursive calls into one function with successive reads followed by the one
  // stream call; tested up to 40 arguments: https://godbolt.org/z/5KeeM4
  template <size_t i = 0, typename... Args>
  static void DeserializeArguments(ProfileBufferEntryReader& aEntryReader,
                                   JSONWriter& aWriter, const Args&... aArgs) {
    static_assert(sizeof...(Args) == i,
                  "We should have collected `i` arguments so far");
    if constexpr (i < scStreamFunctionParameterCount) {
      // Deserialize the i-th argument on this stack.
      auto argument = aEntryReader.ReadObject<StreamFunctionParameter<i>>();
      // Add our local argument to the next recursive call.
      DeserializeArguments<i + 1>(aEntryReader, aWriter, aArgs..., argument);
    } else {
      // We've read all the arguments, finally call the `StreamJSONMarkerData`
      // function, which should write the appropriate JSON elements for this
      // marker type. Note that the MarkerType-specific "type" element is
      // already written.
      MarkerType::StreamJSONMarkerData(aWriter, aArgs...);
    }
  }

 public:
  static void Deserialize(ProfileBufferEntryReader& aEntryReader,
                          JSONWriter& aWriter) {
    aWriter.StringProperty("type", MarkerType::MarkerTypeName());
    DeserializeArguments(aEntryReader, aWriter);
  }
};

template <>
struct MarkerTypeSerialization<::mozilla::baseprofiler::markers::NoPayload> {
  // Nothing! NoPayload has special handling avoiding payload work.
};

template <typename MarkerType, typename... Ts>
static ProfileBufferBlockIndex AddMarkerWithOptionalStackToBuffer(
    ProfileChunkedBuffer& aBuffer, const ProfilerString8View& aName,
    MarkerOptions&& aOptions, const Ts&... aTs) {
  if constexpr (std::is_same_v<MarkerType,
                               ::mozilla::baseprofiler::markers::NoPayload>) {
    static_assert(sizeof...(Ts) == 0,
                  "NoPayload does not accept any payload arguments.");
    // Note that options are first after the entry kind, because they contain
    // the thread id, which is handled first to filter markers by threads.
    return aBuffer.PutObjects(
        ProfileBufferEntryKind::Marker, std::move(aOptions), aName,
        base_profiler_markers_detail::Streaming::DeserializerTag(0));
  } else {
    return MarkerTypeSerialization<MarkerType>::Serialize(
        aBuffer, aName, std::move(aOptions), aTs...);
  }
}

// Pointer to a function that can capture a backtrace into the provided
// `ProfileChunkedBuffer`, and returns true when successful.
using BacktraceCaptureFunction = bool (*)(ProfileChunkedBuffer&);

// Add a marker with the given name, options, and arguments to the given buffer.
// Because this may be called from either Base or Gecko Profiler functions, the
// appropriate backtrace-capturing function must also be provided.
template <typename MarkerType, typename... Ts>
ProfileBufferBlockIndex AddMarkerToBuffer(
    ProfileChunkedBuffer& aBuffer, const ProfilerString8View& aName,
    MarkerOptions&& aOptions,
    BacktraceCaptureFunction aBacktraceCaptureFunction, const Ts&... aTs) {
  if (aOptions.ThreadId().IsUnspecified()) {
    // If yet unspecified, set thread to this thread where the marker is added.
    aOptions.Set(MarkerThreadId::CurrentThread());
  }

  if (aOptions.IsTimingUnspecified()) {
    // If yet unspecified, set timing to this instant of adding the marker.
    aOptions.Set(MarkerTiming::InstantNow());
  }

  if (aOptions.Stack().IsCaptureNeeded()) {
    // A capture was requested, let's attempt to do it here&now. This avoids a
    // lot of allocations that would be necessary if capturing a backtrace
    // separately.
    // TODO use a local on-stack byte buffer to remove last allocation.
    // TODO reduce internal profiler stack levels, see bug 1659872.
    ProfileBufferChunkManagerSingle chunkManager(64 * 1024);
    ProfileChunkedBuffer chunkedBuffer(
        ProfileChunkedBuffer::ThreadSafety::WithoutMutex, chunkManager);
    aOptions.StackRef().UseRequestedBacktrace(
        aBacktraceCaptureFunction(chunkedBuffer) ? &chunkedBuffer : nullptr);
    // This call must be made from here, while chunkedBuffer is in scope.
    return AddMarkerWithOptionalStackToBuffer<MarkerType>(
        aBuffer, aName, std::move(aOptions), aTs...);
  }

  return AddMarkerWithOptionalStackToBuffer<MarkerType>(
      aBuffer, aName, std::move(aOptions), aTs...);
}

template <typename NameCallback, typename StackCallback>
[[nodiscard]] bool DeserializeAfterKindAndStream(
    ProfileBufferEntryReader& aEntryReader, JSONWriter& aWriter,
    int aThreadIdOrZero, NameCallback&& aNameCallback,
    StackCallback&& aStackCallback) {
  // Each entry is made up of the following:
  //   ProfileBufferEntry::Kind::Marker, <- already read by caller
  //   options,                          <- next location in entries
  //   name,
  //   payload
  const MarkerOptions options = aEntryReader.ReadObject<MarkerOptions>();
  if (aThreadIdOrZero != 0 &&
      options.ThreadId().ThreadId() != aThreadIdOrZero) {
    // A specific thread is being read, we're not in it.
    return false;
  }
  // Write the information to JSON with the following schema:
  // [name, startTime, endTime, phase, category, data]
  aWriter.StartArrayElement();
  {
    std::forward<NameCallback>(aNameCallback)(
        aEntryReader.ReadObject<mozilla::ProfilerString8View>());

    const double startTime = options.Timing().GetStartTime();
    aWriter.DoubleElement(startTime);

    const double endTime = options.Timing().GetEndTime();
    aWriter.DoubleElement(endTime);

    aWriter.IntElement(static_cast<int64_t>(options.Timing().MarkerPhase()));

    aWriter.IntElement(static_cast<int64_t>(options.Category().Category()));

    if (const auto tag =
            aEntryReader.ReadObject<mozilla::base_profiler_markers_detail::
                                        Streaming::DeserializerTag>();
        tag != 0) {
      aWriter.StartObjectElement(JSONWriter::SingleLineStyle);
      {
        // Stream "common props".

        // TODO: Move this to top-level tuple, when frontend supports it.
        if (!options.InnerWindowId().IsUnspecified()) {
          // Here, we are converting uint64_t to double. Both Browsing Context
          // and Inner Window IDs are created using
          // `nsContentUtils::GenerateProcessSpecificId`, which is specifically
          // designed to only use 53 of the 64 bits to be lossless when passed
          // into and out of JS as a double.
          aWriter.DoubleProperty(
              "innerWindowID",
              static_cast<double>(options.InnerWindowId().Id()));
        }

        // TODO: Move this to top-level tuple, when frontend supports it.
        if (ProfileChunkedBuffer* chunkedBuffer =
                options.Stack().GetChunkedBuffer();
            chunkedBuffer) {
          aWriter.StartObjectProperty("stack");
          { std::forward<StackCallback>(aStackCallback)(*chunkedBuffer); }
          aWriter.EndObject();
        }

        // Stream the payload, including the type.
        mozilla::base_profiler_markers_detail::Streaming::Deserializer
            deserializer = mozilla::base_profiler_markers_detail::Streaming::
                DeserializerForTag(tag);
        MOZ_RELEASE_ASSERT(deserializer);
        deserializer(aEntryReader, aWriter);
      }
      aWriter.EndObject();
    }
  }
  aWriter.EndArray();
  return true;
}

}  // namespace mozilla::base_profiler_markers_detail

namespace mozilla {

// ----------------------------------------------------------------------------
// Serializer, Deserializer: ProfilerStringView<CHAR>

// The serialization starts with a ULEB128 number that encodes both whether the
// ProfilerStringView is literal (Least Significant Bit = 0) or not (LSB = 1),
// plus the string length (excluding null terminator) in bytes, shifted left by
// 1 bit. Following that number:
// - If literal, the string pointer value.
// - If non-literal, the contents as bytes (excluding null terminator if any).
template <typename CHAR>
struct ProfileBufferEntryWriter::Serializer<ProfilerStringView<CHAR>> {
  static Length Bytes(const ProfilerStringView<CHAR>& aString) {
    MOZ_RELEASE_ASSERT(
        aString.Length() < std::numeric_limits<Length>::max() / 2,
        "Double the string length doesn't fit in Length type");
    const Length stringLength = static_cast<Length>(aString.Length());
    if (aString.IsLiteral()) {
      // Literal -> Length shifted left and LSB=0, then pointer.
      return ULEB128Size(stringLength << 1 | 0u) +
             static_cast<ProfileChunkedBuffer::Length>(sizeof(const CHAR*));
    }
    // Non-literal -> Length shifted left and LSB=1, then string size in bytes.
    return ULEB128Size((stringLength << 1) | 1u) + stringLength * sizeof(CHAR);
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const ProfilerStringView<CHAR>& aString) {
    MOZ_RELEASE_ASSERT(
        aString.Length() < std::numeric_limits<Length>::max() / 2,
        "Double the string length doesn't fit in Length type");
    const Length stringLength = static_cast<Length>(aString.Length());
    if (aString.IsLiteral()) {
      // Literal -> Length shifted left and LSB=0, then pointer.
      aEW.WriteULEB128(stringLength << 1 | 0u);
      aEW.WriteObject(WrapProfileBufferRawPointer(aString.Data()));
      return;
    }
    // Non-literal -> Length shifted left and LSB=1, then string size in bytes.
    aEW.WriteULEB128(stringLength << 1 | 1u);
    aEW.WriteBytes(aString.Data(), stringLength * sizeof(CHAR));
  }
};

template <typename CHAR>
struct ProfileBufferEntryReader::Deserializer<ProfilerStringView<CHAR>> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       ProfilerStringView<CHAR>& aString) {
    const Length lengthAndIsLiteral = aER.ReadULEB128<Length>();
    const Length stringLength = lengthAndIsLiteral >> 1;
    if ((lengthAndIsLiteral & 1u) == 0u) {
      // LSB==0 -> Literal string, read the string pointer.
      aString.mStringView = std::basic_string_view<CHAR>(
          aER.ReadObject<const CHAR*>(), stringLength);
      aString.mOwnership = ProfilerStringView<CHAR>::Ownership::Literal;
      return;
    }
    // LSB==1 -> Not a literal string, allocate a buffer to store the string
    // (plus terminal, for safety), and give it to the ProfilerStringView; Note
    // that this is a secret use of ProfilerStringView, which is intended to
    // only be used between deserialization and JSON streaming.
    CHAR* buffer = new CHAR[stringLength + 1];
    aER.ReadBytes(buffer, stringLength * sizeof(CHAR));
    buffer[stringLength] = CHAR(0);
    aString.mStringView = std::basic_string_view<CHAR>(buffer, stringLength);
    aString.mOwnership =
        ProfilerStringView<CHAR>::Ownership::OwnedThroughStringView;
  }

  static ProfilerStringView<CHAR> Read(ProfileBufferEntryReader& aER) {
    const Length lengthAndIsLiteral = aER.ReadULEB128<Length>();
    const Length stringLength = lengthAndIsLiteral >> 1;
    if ((lengthAndIsLiteral & 1u) == 0u) {
      // LSB==0 -> Literal string, read the string pointer.
      return ProfilerStringView<CHAR>(
          aER.ReadObject<const CHAR*>(), stringLength,
          ProfilerStringView<CHAR>::Ownership::Literal);
    }
    // LSB==1 -> Not a literal string, allocate a buffer to store the string
    // (plus terminal, for safety), and give it to the ProfilerStringView; Note
    // that this is a secret use of ProfilerStringView, which is intended to
    // only be used between deserialization and JSON streaming.
    CHAR* buffer = new CHAR[stringLength + 1];
    aER.ReadBytes(buffer, stringLength * sizeof(CHAR));
    buffer[stringLength] = CHAR(0);
    return ProfilerStringView<CHAR>(
        buffer, stringLength,
        ProfilerStringView<CHAR>::Ownership::OwnedThroughStringView);
  }
};

// Serializer, Deserializer: MarkerCategory

// The serialization contains both category numbers encoded as ULEB128.
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerCategory> {
  static Length Bytes(const MarkerCategory& aCategory) {
    return ULEB128Size(static_cast<uint32_t>(aCategory.CategoryPair())) +
           ULEB128Size(static_cast<uint32_t>(aCategory.Category()));
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const MarkerCategory& aCategory) {
    aEW.WriteULEB128(static_cast<uint32_t>(aCategory.CategoryPair()));
    aEW.WriteULEB128(static_cast<uint32_t>(aCategory.Category()));
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerCategory> {
  static void ReadInto(ProfileBufferEntryReader& aER,
                       MarkerCategory& aCategory) {
    aCategory.mCategoryPair = static_cast<baseprofiler::ProfilingCategoryPair>(
        aER.ReadULEB128<uint32_t>());
    aCategory.mCategory = static_cast<baseprofiler::ProfilingCategory>(
        aER.ReadULEB128<uint32_t>());
  }

  static MarkerCategory Read(ProfileBufferEntryReader& aER) {
    return MarkerCategory(static_cast<baseprofiler::ProfilingCategoryPair>(
                              aER.ReadULEB128<uint32_t>()),
                          static_cast<baseprofiler::ProfilingCategory>(
                              aER.ReadULEB128<uint32_t>()));
  }
};

// ----------------------------------------------------------------------------
// Serializer, Deserializer: MarkerTiming

// The serialization starts with the marker phase, followed by one or two
// timestamps as needed.
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerTiming> {
  static Length Bytes(const MarkerTiming& aTiming) {
    MOZ_ASSERT(!aTiming.IsUnspecified());
    const auto phase = aTiming.MarkerPhase();
    switch (phase) {
      case MarkerTiming::Phase::Instant:
        return SumBytes(phase, aTiming.StartTime());
      case MarkerTiming::Phase::Interval:
        return SumBytes(phase, aTiming.StartTime(), aTiming.EndTime());
      case MarkerTiming::Phase::IntervalStart:
        return SumBytes(phase, aTiming.StartTime());
      case MarkerTiming::Phase::IntervalEnd:
        return SumBytes(phase, aTiming.EndTime());
      default:
        MOZ_RELEASE_ASSERT(phase == MarkerTiming::Phase::Instant ||
                           phase == MarkerTiming::Phase::Interval ||
                           phase == MarkerTiming::Phase::IntervalStart ||
                           phase == MarkerTiming::Phase::IntervalEnd);
        return 0;  // Only to avoid build errors.
    }
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const MarkerTiming& aTiming) {
    MOZ_ASSERT(!aTiming.IsUnspecified());
    const auto phase = aTiming.MarkerPhase();
    switch (phase) {
      case MarkerTiming::Phase::Instant:
        aEW.WriteObjects(phase, aTiming.StartTime());
        return;
      case MarkerTiming::Phase::Interval:
        aEW.WriteObjects(phase, aTiming.StartTime(), aTiming.EndTime());
        return;
      case MarkerTiming::Phase::IntervalStart:
        aEW.WriteObjects(phase, aTiming.StartTime());
        return;
      case MarkerTiming::Phase::IntervalEnd:
        aEW.WriteObjects(phase, aTiming.EndTime());
        return;
      default:
        MOZ_RELEASE_ASSERT(phase == MarkerTiming::Phase::Instant ||
                           phase == MarkerTiming::Phase::Interval ||
                           phase == MarkerTiming::Phase::IntervalStart ||
                           phase == MarkerTiming::Phase::IntervalEnd);
        return;
    }
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerTiming> {
  static void ReadInto(ProfileBufferEntryReader& aER, MarkerTiming& aTiming) {
    aTiming.mPhase = aER.ReadObject<MarkerTiming::Phase>();
    switch (aTiming.mPhase) {
      case MarkerTiming::Phase::Instant:
        aTiming.mStartTime = aER.ReadObject<TimeStamp>();
        aTiming.mEndTime = TimeStamp{};
        break;
      case MarkerTiming::Phase::Interval:
        aTiming.mStartTime = aER.ReadObject<TimeStamp>();
        aTiming.mEndTime = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::IntervalStart:
        aTiming.mStartTime = aER.ReadObject<TimeStamp>();
        aTiming.mEndTime = TimeStamp{};
        break;
      case MarkerTiming::Phase::IntervalEnd:
        aTiming.mStartTime = TimeStamp{};
        aTiming.mEndTime = aER.ReadObject<TimeStamp>();
        break;
      default:
        MOZ_RELEASE_ASSERT(aTiming.mPhase == MarkerTiming::Phase::Instant ||
                           aTiming.mPhase == MarkerTiming::Phase::Interval ||
                           aTiming.mPhase ==
                               MarkerTiming::Phase::IntervalStart ||
                           aTiming.mPhase == MarkerTiming::Phase::IntervalEnd);
        break;
    }
  }

  static MarkerTiming Read(ProfileBufferEntryReader& aER) {
    TimeStamp start;
    TimeStamp end;
    auto phase = aER.ReadObject<MarkerTiming::Phase>();
    switch (phase) {
      case MarkerTiming::Phase::Instant:
        start = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::Interval:
        start = aER.ReadObject<TimeStamp>();
        end = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::IntervalStart:
        start = aER.ReadObject<TimeStamp>();
        break;
      case MarkerTiming::Phase::IntervalEnd:
        end = aER.ReadObject<TimeStamp>();
        break;
      default:
        MOZ_RELEASE_ASSERT(phase == MarkerTiming::Phase::Instant ||
                           phase == MarkerTiming::Phase::Interval ||
                           phase == MarkerTiming::Phase::IntervalStart ||
                           phase == MarkerTiming::Phase::IntervalEnd);
        break;
    }
    return MarkerTiming(start, end, phase);
  }
};

// ----------------------------------------------------------------------------
// Serializer, Deserializer: MarkerStack

// The serialization only contains the `ProfileChunkedBuffer` from the
// backtrace; if there is no backtrace or if it's empty, this will implicitly
// store a nullptr (see
// `ProfileBufferEntryWriter::Serializer<ProfilerChunkedBuffer*>`).
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerStack> {
  static Length Bytes(const MarkerStack& aStack) {
    return SumBytes(aStack.GetChunkedBuffer());
  }

  static void Write(ProfileBufferEntryWriter& aEW, const MarkerStack& aStack) {
    aEW.WriteObject(aStack.GetChunkedBuffer());
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerStack> {
  static void ReadInto(ProfileBufferEntryReader& aER, MarkerStack& aStack) {
    aStack = Read(aER);
  }

  static MarkerStack Read(ProfileBufferEntryReader& aER) {
    return MarkerStack(aER.ReadObject<UniquePtr<ProfileChunkedBuffer>>());
  }
};

// ----------------------------------------------------------------------------
// Serializer, Deserializer: MarkerOptions

// The serialization contains all members (either trivially-copyable, or they
// provide their specialization above).
template <>
struct ProfileBufferEntryWriter::Serializer<MarkerOptions> {
  static Length Bytes(const MarkerOptions& aOptions) {
    return SumBytes(aOptions.Category(), aOptions.ThreadId(), aOptions.Timing(),
                    aOptions.Stack(), aOptions.InnerWindowId());
  }

  static void Write(ProfileBufferEntryWriter& aEW,
                    const MarkerOptions& aOptions) {
    aEW.WriteObjects(aOptions.Category(), aOptions.ThreadId(),
                     aOptions.Timing(), aOptions.Stack(),
                     aOptions.InnerWindowId());
  }
};

template <>
struct ProfileBufferEntryReader::Deserializer<MarkerOptions> {
  static void ReadInto(ProfileBufferEntryReader& aER, MarkerOptions& aOptions) {
    aER.ReadIntoObjects(aOptions.mCategory, aOptions.mThreadId,
                        aOptions.mTiming, aOptions.mStack,
                        aOptions.mInnerWindowId);
  }

  static MarkerOptions Read(ProfileBufferEntryReader& aER) {
    MarkerOptions options;
    ReadInto(aER, options);
    return options;
  }
};

}  // namespace mozilla

#endif  // MOZ_GECKO_PROFILER

#endif  // BaseProfilerMarkersDetail_h
