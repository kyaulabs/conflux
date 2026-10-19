#include <string_view>
static_assert(std::string_view(R"({"a\"b":1,"a\u0022b":2})") == std::string_view("{\"a\\\"b\":1,\"a\\u0022b\":2}"));
static_assert(std::string_view(R"(":1,"\ud83d\ude03":2})") == std::string_view("\":1,\"\\ud83d\\ude03\":2}"));
static_assert(std::string_view(R"({"message":"{\"id\":1,\"id\":2} braces } ] and slash \\"})") == std::string_view("{\"message\":\"{\\\"id\\\":1,\\\"id\\\":2} braces } ] and slash \\\\\"}"));
