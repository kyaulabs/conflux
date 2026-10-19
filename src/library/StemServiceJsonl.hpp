// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace conflux::library::detail {

// Private service-v1 wire structure validation, off the GUI/render callbacks.
// This does not validate an event schema, generation, identity or playback state.
class StemServiceJsonl {
public:
    static constexpr qsizetype maxLineBytes = 16384; // Includes terminating LF.
    static constexpr qsizetype maxBodyBytes = maxLineBytes - 1;
    static constexpr qsizetype maxEncodedKeyBytes = 512; // Includes quotes.
    static constexpr qsizetype maxKeyUtf16Units = 64;
    static constexpr std::size_t maxDepth = 8, maxObjectMembers = 32;
    static constexpr std::size_t maxTotalMembers = 128, maxArrayElements = 16;
    // Root, object member values and array elements count, including containers;
    // keys do not count as values.
    static constexpr std::size_t maxTotalValues = 256;
    static constexpr qsizetype maxNumberBytes = 64;
    static constexpr std::uint64_t maxWireInteger = 9007199254740991ULL;

    struct Feed {
        qsizetype consumed = 0;
        std::optional<QJsonObject> object;
    };
    // Consume through the first LF only, including LF in consumed. A returned
    // object owns its decoded data. Caller re-feeds any unconsumed suffix.
    // Without LF, consume all supplied bytes into one bounded incomplete body.
    // Empty input is harmless, including after successful finish().
    //
    // Reject malformed/non-object/trailing JSON, BOM, invalid UTF-8 or invalid
    // escaped surrogate pairs in ANY string, and per-object duplicate decoded keys.
    // Keys are case-sensitive, not Unicode-normalized; no NUL in member names.
    // Fixed service integer MEMBER NAMES get exact integer spelling/range when
    // their value is numeric; bool/string/null/container type checks belong to
    // later event schemas. Other numbers must be finite JSON numbers.
    // Names: v,schema,id,generation,worker_generation,current_generation,sequence,
    // slot,source_frames,frames,bytes,sample_rate,channels,window_frames,hop_frames,
    // overlap_frames,segment_frames,threads,frame,frame_start,window_start_frame,
    // request_id,reservation_bytes,max_input_bytes,max_job_bytes,
    // max_diagnostic_bytes,telemetry_dropped,outstanding_slots.
    //
    // Syntax/Unicode/duplicate/unsafe-integer errors -> invalid_argument;
    // structural/byte caps -> length_error. Either error makes this parser
    // terminal; later feed/finish throws logic_error, even on empty input.
    Feed feed(QByteArrayView bytes);

    // Pipe EOF: a nonempty partial body is invalid, even if it is valid JSON.
    // Clean EOF seals the parser and is idempotent. Later nonempty feed throws
    // logic_error; no implicit final line and no queued event are invented.
    void finish();

private:
    QByteArray body_;
    enum class State { Open, Finished, Failed };
    State state_ = State::Open;
};

} // namespace conflux::library::detail
