// SPDX-License-Identifier: MIT
//
// Bounded parallel --describe execution for check_file(): runs runnable participant descriptions
// either sequentially (when saved descriptions or a single task are used) or across a capped
// worker pool, writing each result into its own pre-allocated slot so results can be folded in
// original participant order. Internal to libs/launch/src/usecases/.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/launch/probe.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vehicle_sil::launch::detail {

/// Maximum number of concurrent --describe processes started by one check_file() call.
inline constexpr std::size_t kMaxDescribeConcurrency = 6;

struct DescribeTask
{
    std::size_t participant_index = 0;
    const config::ParticipantConfig* participant = nullptr;
    std::filesystem::path executable{};
    std::vector<std::string> arguments{};
};

struct DescribeSlot
{
    std::optional<ExecutableStamp> before{};
    std::optional<config::DescribeRun> run{};
    std::optional<ExecutableStamp> after{};
};

/// Number of workers for `task_count` runnable tasks given `hardware_threads`
/// (typically std::thread::hardware_concurrency()). Returns `task_count` when `task_count <= 1`.
[[nodiscard]] std::size_t describe_worker_count(std::size_t task_count, unsigned int hardware_threads) noexcept;

/// Runs `tasks` through `describe`, writing each task's result into the corresponding slot.
///
/// When `allow_parallel` is true and `tasks.size() > 1`, executes across at most
/// `describe_worker_count()` worker threads (bounded by `kMaxDescribeConcurrency`; if
/// `max_workers_override > 0`, bounded by `min(max_workers_override, kMaxDescribeConcurrency)`).
/// Every spawned thread finishes `describe()` synchronously before moving to the next task or
/// exiting (keeping Linux PR_SET_PDEATHSIG valid for child processes) and is joined before return
/// even if an unexpected exception is thrown.
///
/// When `allow_parallel` is false or `tasks.size() <= 1`, executes strictly sequentially on the
/// calling thread without spawning threads.
[[nodiscard]] std::vector<DescribeSlot> run_describe_tasks(const std::vector<DescribeTask>& tasks,
                                                           const DescribeSource& describe, bool record_stamps,
                                                           bool allow_parallel,
                                                           std::size_t max_workers_override = 0);

} // namespace vehicle_sil::launch::detail
