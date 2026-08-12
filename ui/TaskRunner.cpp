// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * TaskRunner.cpp — implementation of the lightweight task runner.
 *
 * Reuses the project's existing CMake build system. Task names map to
 * shell commands that are launched through TerminalProcess.
 */

#include "TaskRunner.hpp"

#include <algorithm>
#include <filesystem>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TaskRunner::TaskRunner(std::string projectDir, std::string buildDir)
    : projectDir_(std::move(projectDir))
    , buildDir_(buildDir.empty()
                    ? (std::filesystem::path(projectDir_).empty()
                           ? std::string("build")
                           : (std::filesystem::path(projectDir_) / "build").string())
                    : std::move(buildDir))
    , tasks_(defaultTasks())
{
}

// ---------------------------------------------------------------------------
// Default task set
// ---------------------------------------------------------------------------

std::vector<TaskDef> TaskRunner::defaultTasks()
{
    return {
        TaskDef{
            "build",
            "Build Project",
            "cmake --build {buildDir} --parallel",
            "",  // cwd resolved at launch time
            true
        },
        TaskDef{
            "test",
            "Run Tests",
            "ctest --test-dir {buildDir} --output-on-failure",
            "",
            true
        },
        TaskDef{
            "check",
            "Check (Build + Test)",
            "cmake --build {buildDir} --parallel && ctest --test-dir {buildDir} --output-on-failure",
            "",
            true
        },
        TaskDef{
            "configure",
            "Configure (CMake)",
            "cmake -S {projectDir} -B {buildDir}",
            "",
            false
        },
        TaskDef{
            "clean",
            "Clean Build",
            "cmake --build {buildDir} --target clean",
            "",
            true
        },
        TaskDef{
            "format",
            "Format Code (clang-format)",
            "clang-format -i *.cpp *.hpp",
            "",
            false
        },
    };
}

// ---------------------------------------------------------------------------

const TaskDef* TaskRunner::findTask(std::string_view id) const noexcept
{
    auto it = std::find_if(tasks_.begin(), tasks_.end(),
                           [&](const TaskDef& t) { return t.id == id; });
    return (it != tasks_.end()) ? &(*it) : nullptr;
}

std::string TaskRunner::expandPlaceholders(const std::string& command) const
{
    std::string result = command;

    // Replace {buildDir}
    size_t pos = 0;
    while ((pos = result.find("{buildDir}", pos)) != std::string::npos)
    {
        result.replace(pos, 10, buildDir_);
        pos += buildDir_.size();
    }

    // Replace {projectDir}
    pos = 0;
    while ((pos = result.find("{projectDir}", pos)) != std::string::npos)
    {
        result.replace(pos, 13, projectDir_);
        pos += projectDir_.size();
    }

    return result;
}

std::vector<std::pair<std::string, std::string>> TaskRunner::taskList() const noexcept
{
    std::vector<std::pair<std::string, std::string>> list;
    list.reserve(tasks_.size());
    for (const auto& t : tasks_)
        list.emplace_back(t.id, t.label);
    return list;
}

} // namespace hathor::ui
