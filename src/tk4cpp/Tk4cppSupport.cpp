/**
 * @file Tk4cppSupport.cpp
 * @brief Tk4cppExecutor and TK4CPP-based helpers.
 * @details Wraps TK4CPP::TP::ThreadPool and uses TK4CPP::Utility::System / FolderHandler for hardware and
 *          formatting queries.
 * @version 0.1.0
 * @author Claudio Cavallero
 */

#include "diskutilities/tk4cpp/Tk4cppSupport.h"

#include <TK4CPP/Utility/FolderHandler.hpp>
#include <TK4CPP/Utility/System.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace diskutilities::tk4cpp {

TK4CPP::TP::ThreadPoolConfig Tk4cppExecutor::makeConfig(unsigned threads, std::string name) {
    TK4CPP::TP::ThreadPoolConfig config;
    config.minThreads = threads;
    config.maxThreads = threads; // fixed size: see class documentation
    config.name = std::move(name);
    return config;
}

Tk4cppExecutor::Tk4cppExecutor(unsigned threads, std::string name)
    : threads_(threads != 0 ? threads : recommendedScanThreads()),
      pool_(makeConfig(static_cast<unsigned>(threads_), std::move(name))) {
    pool_.start();
}

void Tk4cppExecutor::post(Task task) {
    // std::function is copyable, FunctionTask takes a move_only_function: the conversion is implicit.
    const auto result = pool_.submit(TK4CPP::TP::FunctionTask::Func(std::move(task)), "scan-directory");
    if (!result)
        throw std::runtime_error(std::string("TK4CPP ThreadPool: ") + TK4CPP::TP::toString(result.error()));
}

unsigned recommendedScanThreads() {
    const unsigned cores = TK4CPP::Utility::System::physicalCores();
    return std::clamp(cores, 1u, 8u);
}

std::uint64_t recommendedNodeLimit() {
    const unsigned long long freeBytes = TK4CPP::Utility::System::freeMemory();
    if (freeBytes == 0) return 0;
    // Leave a quarter of the available memory to the rest of the system.
    return (freeBytes / 4 * 3) / kBytesPerNode;
}

std::string formatBytes(std::uint64_t bytes) {
    return TK4CPP::Utility::FolderHandler::formatSize(static_cast<std::uintmax_t>(bytes));
}

} // namespace diskutilities::tk4cpp
