// SPDX-License-Identifier: AGPL-3.0-or-later
#include "library/StemServiceJsonl.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLatin1StringView>
#include <QStringDecoder>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace conflux::library::detail {
namespace {
using Limits = StemServiceJsonl;

[[noreturn]] void invalid()
{ throw std::invalid_argument("Invalid stem service JSON line"); }
[[noreturn]] void limited()
{ throw std::length_error("Stem service JSON line exceeds a structural or byte bound"); }

bool whitespace(char value) noexcept
{ return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }

bool integerMember(const QString& name) noexcept
{
    static constexpr std::string_view names[]{
        "v", "schema", "id", "generation", "worker_generation", "current_generation", "sequence", "slot",
        "source_frames", "frames", "bytes", "sample_rate", "channels", "window_frames", "hop_frames",
        "overlap_frames", "segment_frames", "threads", "frame", "frame_start", "window_start_frame", "request_id",
        "reservation_bytes", "max_input_bytes", "max_job_bytes", "max_diagnostic_bytes", "telemetry_dropped",
        "outstanding_slots"};
    for (const auto candidate : names)
        if (name == QLatin1StringView(candidate.data(), qsizetype(candidate.size()))) return true;
    return false;
}

// This walk retains only bounded per-object keys. Qt still decodes the final
// document; scanning first preserves duplicate keys and raw integer spellings.
class Scan {
public:
    explicit Scan(QByteArrayView text) noexcept : text_(text) {}
    void run()
    {
        space();
        if (peek() != '{') invalid();
        value(0);
        space();
        if (position_ != text_.size()) invalid();
    }
private:
    char peek() const noexcept { return position_ < text_.size() ? text_[position_] : '\0'; }
    bool take(char expected) noexcept
    {
        if (position_ == text_.size() || text_[position_] != expected) return false;
        ++position_;
        return true;
    }
    void expect(char expected) { if (!take(expected)) invalid(); }
    void space() noexcept
    { while (position_ < text_.size() && whitespace(text_[position_])) ++position_; }

    unsigned hex4()
    {
        if (text_.size() - position_ < 4) invalid();
        unsigned result = 0;
        for (unsigned digit = 0; digit < 4; ++digit) {
            const auto c = text_[position_++];
            unsigned value;
            if (c >= '0' && c <= '9') value = unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') value = unsigned(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F') value = unsigned(c - 'A') + 10;
            else invalid();
            result = result * 16 + value;
        }
        return result;
    }

    QByteArrayView string(bool key)
    {
        const auto start = position_;
        expect('"');
        while (position_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (key && position_ - start > Limits::maxEncodedKeyBytes) limited();
            if (c == '"') return text_.sliced(start, position_ - start);
            if (c < 0x20) invalid();
            if (c != '\\') continue;
            if (position_ == text_.size()) invalid();
            const auto escape = text_[position_++];
            if (escape == 'u') {
                const auto code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    // Escaped high/low surrogates must form one adjacent pair.
                    expect('\\'); expect('u');
                    const auto low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) invalid();
                } else if (code >= 0xdc00 && code <= 0xdfff) invalid();
            } else if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' &&
                       escape != 'f' && escape != 'n' && escape != 'r' && escape != 't') invalid();
            if (key && position_ - start > Limits::maxEncodedKeyBytes) limited();
        }
        invalid();
    }

    QString memberName()
    {
        const auto token = string(true);
        QByteArray wrapped("[");
        wrapped.append(token.data(), token.size()); wrapped.append(']');
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(wrapped, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray()) invalid();
        const auto array = document.array();
        if (array.size() != 1 || !array[0].isString()) invalid();
        auto name = array[0].toString();
        if (name.size() > Limits::maxKeyUtf16Units) limited();
        if (name.contains(QChar(0))) invalid();
        return name;
    }

    void number(const QString* member)
    {
        const auto start = position_;
        while (position_ < text_.size()) {
            const auto c = text_[position_];
            if (whitespace(c) || c == ',' || c == ']' || c == '}') break;
            ++position_;
            if (position_ - start > Limits::maxNumberBytes) limited();
        }
        if (!member || !integerMember(*member)) return;
        const auto token = text_.sliced(start, position_ - start);
        if (token == QByteArrayView("-0")) return;
        if (token.isEmpty() || token[0] == '-' || (token[0] == '0' && token.size() > 1)) invalid();
        std::uint64_t integer = 0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), integer);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || integer > Limits::maxWireInteger)
            invalid();
    }

    void literal(QByteArrayView expected)
    {
        if (text_.size() - position_ < expected.size() || text_.sliced(position_, expected.size()) != expected) invalid();
        position_ += expected.size();
    }

    void object(std::size_t depth)
    {
        if (depth > Limits::maxDepth) limited();
        expect('{'); space();
        if (take('}')) return;
        std::array<QString, Limits::maxObjectMembers> names;
        std::size_t count = 0;
        for (;;) {
            if (count == names.size() || totalMembers_ == Limits::maxTotalMembers) limited();
            auto name = memberName();
            for (std::size_t previous = 0; previous < count; ++previous)
                if (names[previous] == name) invalid();
            names[count++] = std::move(name); ++totalMembers_;
            space(); expect(':');
            value(depth, &names[count - 1]);
            space();
            if (take('}')) return;
            expect(','); space();
        }
    }

    void array(std::size_t depth)
    {
        if (depth > Limits::maxDepth) limited();
        expect('['); space();
        if (take(']')) return;
        std::size_t count = 0;
        for (;;) {
            if (count++ == Limits::maxArrayElements) limited();
            value(depth);
            space();
            if (take(']')) return;
            expect(','); space();
        }
    }

    void value(std::size_t depth, const QString* member = nullptr)
    {
        if (totalValues_++ == Limits::maxTotalValues) limited();
        space();
        const auto c = peek();
        if (c == '{') object(depth + 1);
        else if (c == '[') array(depth + 1);
        else if (c == '"') (void)string(false);
        else if (c == 't') literal("true");
        else if (c == 'f') literal("false");
        else if (c == 'n') literal("null");
        else if (c == '-' || (c >= '0' && c <= '9')) number(member);
        else invalid();
    }

    QByteArrayView text_;
    qsizetype position_ = 0;
    std::size_t totalMembers_ = 0, totalValues_ = 0;
};

void finiteNumbers(const QJsonValue& value)
{
    if (value.isDouble()) {
        if (!std::isfinite(value.toDouble())) invalid();
    } else if (value.isArray()) {
        for (const auto child : value.toArray()) finiteNumbers(child);
    } else if (value.isObject()) {
        for (const auto child : value.toObject()) finiteNumbers(child);
    }
}

QJsonObject decode(const QByteArray& body)
{
    if (body.startsWith("\xef\xbb\xbf")) invalid();
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString unicode = decoder(QByteArrayView(body));
    (void)unicode;
    if (decoder.hasError()) invalid();
    Scan(body).run();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) invalid();
    const auto object = document.object();
    finiteNumbers(object);
    return object;
}
} // namespace

StemServiceJsonl::Feed StemServiceJsonl::feed(QByteArrayView bytes)
{
    if (state_ == State::Failed) throw std::logic_error("Failed stem service JSONL parser is terminal");
    if (state_ == State::Finished) {
        if (bytes.isEmpty()) return {};
        throw std::logic_error("Stem service JSONL data arrived after EOF");
    }
    if (bytes.isEmpty()) return {};
    try {
        const auto remaining = maxBodyBytes - body_.size();
        // Inspect only this line's bounded prefix. A huge unconsumed suffix
        // after its first LF belongs to the caller's next fair loop turn.
        const auto probe = bytes.first(std::min(bytes.size(), remaining + 1));
        const auto newline = probe.indexOf('\n');
        if (newline >= 0) {
            body_.append(bytes.data(), newline);
            auto object = decode(body_);
            body_.clear();
            return {newline + 1, std::move(object)};
        }
        if (bytes.size() > remaining) limited();
        body_.append(bytes.data(), bytes.size());
        return {bytes.size(), std::nullopt};
    } catch (...) {
        state_ = State::Failed;
        body_.clear();
        throw;
    }
}

void StemServiceJsonl::finish()
{
    if (state_ == State::Failed) throw std::logic_error("Failed stem service JSONL parser is terminal");
    if (state_ == State::Finished) return;
    if (!body_.isEmpty()) {
        state_ = State::Failed;
        body_.clear();
        invalid();
    }
    state_ = State::Finished;
}

} // namespace conflux::library::detail
