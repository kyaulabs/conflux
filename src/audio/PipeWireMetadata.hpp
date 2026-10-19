// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <spa/utils/json.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace conflux::audio::detail {
inline constexpr std::size_t maximumDefaultSinkJsonBytes = 16384;
inline constexpr std::size_t maximumDefaultSinkNameBytes = 4096;

inline bool metadataWhitespace(char value) noexcept
{ return value == ' ' || value == '\t' || value == '\n' || value == '\r'; }

inline int metadataHex(char value) noexcept
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

// The SPA tokenizer accepts relaxed JSON. Check quoted string syntax before
// decoding so malformed escapes/truncated tokens cannot become node names.
inline bool metadataStringToken(const char* token, int length) noexcept
{
    if (length < 2 || token[0] != '"' || token[length - 1] != '"') return false;
    for (int index = 1; index < length - 1; ++index) {
        const auto value = static_cast<unsigned char>(token[index]);
        if (value < 0x20 || value == '"') return false;
        if (value != '\\') continue;
        if (++index >= length - 1) return false;
        const char escaped = token[index];
        if (escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' || escaped == 'f' ||
            escaped == 'n' || escaped == 'r' || escaped == 't') continue;
        if (escaped != 'u' || index + 4 >= length - 1) return false;
        unsigned int code = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const int hex = metadataHex(token[++index]);
            if (hex < 0) return false;
            code = code * 16U + static_cast<unsigned int>(hex);
        }
        if (!code || (code >= 0xdc00 && code <= 0xdfff)) return false;
        if (code < 0xd800 || code > 0xdbff) continue;
        if (index + 6 >= length - 1 || token[index + 1] != '\\' || token[index + 2] != 'u') return false;
        index += 2;
        unsigned int low = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const int hex = metadataHex(token[++index]);
            if (hex < 0) return false;
            low = low * 16U + static_cast<unsigned int>(hex);
        }
        if (low < 0xdc00 || low > 0xdfff) return false;
    }
    return true;
}

// WirePlumber publishes {"name":"node.name"}. Accept exactly that one string
// field; unknown fields (including nested objects) are deliberately rejected.
// Uses SPA APIs present in PipeWire 0.3.65 and 1.0.5. Control-thread only.
inline bool parseDefaultSinkName(const char* value, std::span<char> name) noexcept
{
    if (name.empty()) return false;
    name[0] = '\0';
    if (!value) return false;
    const auto length = strnlen(value, maximumDefaultSinkJsonBytes + 1);
    if (!length || length > maximumDefaultSinkJsonBytes) return false;
    std::string_view json(value, length);
    while (!json.empty() && metadataWhitespace(json.front())) json.remove_prefix(1);
    while (!json.empty() && metadataWhitespace(json.back())) json.remove_suffix(1);
    if (json.size() < 2 || json.front() != '{' || json.back() != '}') return false;

    spa_json document{}, object{};
    spa_json_init(&document, json.data(), json.size());
    if (spa_json_enter_object(&document, &object) <= 0) return false;
    const char* cursor = json.data() + 1;
    const char* token = nullptr;
    auto skipSpace = [&] { while (cursor < json.data() + json.size() && metadataWhitespace(*cursor)) ++cursor; };
    skipSpace();
    int count = spa_json_next(&object, &token);
    std::array<char, 128> key{};
    if (count <= 0 || token != cursor || !metadataStringToken(token, count) ||
        spa_json_parse_stringn(token, count, key.data(), int(key.size())) <= 0 ||
        std::strcmp(key.data(), "name") != 0) return false;
    cursor = token + count;
    skipSpace();
    if (cursor == json.data() + json.size() || *cursor++ != ':') return false;
    skipSpace();
    count = spa_json_next(&object, &token);
    if (count <= 0 || token != cursor || !metadataStringToken(token, count)) return false;
    std::array<char, maximumDefaultSinkNameBytes> decoded{};
    if (spa_json_parse_stringn(token, count, decoded.data(), int(decoded.size())) <= 0 || !decoded[0]) return false;
    cursor = token + count;
    skipSpace();
    if (cursor != json.data() + json.size() - 1 || *cursor != '}') return false;
    if (spa_json_next(&object, &token) != 0) return false;
    const auto nameLength = std::strlen(decoded.data());
    if (nameLength >= name.size()) return false;
    std::memcpy(name.data(), decoded.data(), nameLength + 1);
    return true;
}

} // namespace conflux::audio::detail
