// SPDX-License-Identifier: AGPL-3.0-or-later
#include "audio/PipeWireMetadata.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using conflux::audio::detail::parseDefaultSinkName;

void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

void accepts(const char* json, const char* expected)
{
    std::array<char, 4096> name{};
    require(parseDefaultSinkName(json, name), "Valid published default sink metadata was rejected");
    require(std::string_view(name.data()) == expected, "Default sink metadata escapes or name changed");
}
void rejects(const char* json)
{
    std::array<char, 4096> name{};
    name.fill('x');
    require(!parseDefaultSinkName(json, name) && name[0] == '\0',
        "Invalid metadata was accepted or left a stale default sink name");
}
} // namespace

int main()
{
    try {
        accepts(R"({"name":"sink-name"})", "sink-name");
        accepts(" \t{ \"name\" : \"sink-name\" }\r\n", "sink-name");
        accepts(R"({"name":"sink-\"quoted\"\\path\/end"})", "sink-\"quoted\"\\path/end");
        accepts(R"({"na\u006de":"s\u0069nk-\u00e9-\ud83c\udfa7"})",
            "sink-\xC3\xA9-\xF0\x9F\x8E\xA7");
        rejects(nullptr);
        for (const auto* json : {"", " ", "{}", "[]", "null", R"({"other":"sink"})",
                R"({"name":""})", R"({"name":null})", R"({"name":17})", R"({"name":true})",
                R"({"name":[]})", R"({"name":{}})", R"({"name":"a","name":"b"})",
                R"({"name":"a","other":1})", R"({"other":{"name":"a"},"name":"b"})",
                R"({"name":"a","other":{"nested":[1,2]}})", R"({"name":"sink")",
                R"({"name":"sink})", R"({"name" "sink"})", R"({name:"sink"})",
                R"({"name":"sink",})", R"({"name":"sink"} trailing)",
                R"({"name":"sink"}{"name":"other"})", R"(junk {"name":"sink"})",
                R"({"name":"sink\q"})", R"({"name":"sink\uZZZZ"})", R"({"name":"sink\u0000x"})",
                R"({"name":"\ud800"})", R"({"name":"\udc00"})", R"({"name":"\ud800\u0041"})"})
            rejects(json);

        const std::string longName(4000, 'a');
        accepts(("{\"name\":\"" + longName + "\"}").c_str(), longName.c_str());
        rejects(("{\"name\":\"" + std::string(4300, 'a') + "\"}").c_str());
        rejects((std::string(conflux::audio::detail::maximumDefaultSinkJsonBytes, ' ') +
            "{\"name\":\"sink\"}").c_str());
        std::array<char, 4> small{'x','x','x','!'};
        require(!parseDefaultSinkName(R"({"name":"sink"})", std::span(small).first(3)) &&
                small[0] == '\0' && small.back() == '!',
            "Small metadata output buffer overflowed or retained a stale name");
        require(!parseDefaultSinkName(R"({"name":"sink"})", {}), "Empty output buffer was accepted");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
