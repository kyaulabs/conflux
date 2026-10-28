#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>

static std::uint64_t cosineCalls = 0;
extern "C" double __real_cos(double);
extern "C" double __wrap_cos(double value) {
    ++cosineCalls;
    return __real_cos(value);
}

#include SOURCE_FILE

int main(int argc, char **argv) {
    if (argc != 2) return 3;
    const auto began = std::clock();
    const auto &table = conflux::audio::filters();
    const double cpuMs = 1000.0 * (std::clock() - began) / CLOCKS_PER_SEC;
    if (table.coefficients.size() != 3739200) return 4;
    for (float value : table.coefficients)
        if (!std::isfinite(value)) return 5;
    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(table.coefficients.data()),
                 std::streamsize(table.coefficients.size() * sizeof(float)));
    output.close();
    if (!output) return 6;
    std::cout << "{\"coefficients\":" << table.coefficients.size()
              << ",\"cosine_calls\":" << cosineCalls
              << ",\"thread_cpu_observation_ms\":" << cpuMs
              << ",\"cosine_budget\":131196,\"GPU_model_audio_started\":false}\n";
    return cosineCalls <= 131196 ? 0 : 2;
}
