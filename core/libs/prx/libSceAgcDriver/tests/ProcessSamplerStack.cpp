#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <windows.h>

namespace {

std::atomic<bool> ready{false};
std::atomic<bool> done{false};
std::atomic<std::uint64_t> caller{0};

[[gnu::noinline]] void Busy() {
    caller.store(reinterpret_cast<std::uint64_t>(__builtin_return_address(0)));
    ready.store(true);
    while (!done.load(std::memory_order_relaxed)) std::atomic_signal_fence(std::memory_order_seq_cst);
}

[[gnu::noinline]] void Parent() {
    Busy();
    ready.store(false);
}

}

int main() {
    const auto path = std::filesystem::temp_directory_path() / ("anyps5-process-stack-" + std::to_string(GetCurrentProcessId()) + ".txt");
    _putenv_s("APS5_SAMPLE_THREADS", path.string().c_str());
    _putenv_s("APS5_SAMPLE_WORKER", "");
    _putenv_s("APS5_SAMPLE_WORKER_QUEUE", "0x50");
    std::thread target(Parent);
    while (!ready.load()) std::this_thread::yield();
    AgcDriver::StartWorkerSampler(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    AgcDriver::StopWorkerSampler();
    done.store(true);
    target.join();
    const auto pc = caller.load();
    DWORD64 imageBase = 0;
    if (RtlLookupFunctionEntry(pc, &imageBase, nullptr) != nullptr) {
        std::filesystem::remove(path);
        std::fprintf(stderr, "fixture unexpectedly has native Windows unwind metadata\n");
        return 1;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(pc), &module)) return 1;
    wchar_t imagePath[MAX_PATH]{};
    if (GetModuleFileNameW(module, imagePath, MAX_PATH) == 0) return 1;
    const auto imageName = std::filesystem::path(imagePath).filename().string();
    const auto offset = pc - reinterpret_cast<std::uint64_t>(module);
    std::ifstream file(path);
    std::string line;
    bool found = false;
    std::uint64_t inclusive = 0;
    while (std::getline(file, line)) {
        char name[1024]{};
        unsigned long long address = 0, self = 0, total = 0;
        if (std::sscanf(line.c_str(), " %1023s %llx %llu %llu", name, &address, &self, &total) == 4 && name == imageName && address == offset && total != 0) {
            found = true;
            inclusive += total;
        }
    }
    file.close();
    std::filesystem::remove(path);
    std::printf("process sampler caller: %s (%llu inclusive samples)\n", found ? "passed" : "missing", static_cast<unsigned long long>(inclusive));
    return found ? 0 : 1;
}
