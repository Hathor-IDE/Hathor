// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * TaskRunner.hpp — L-4: lightweight task runner for common project actions.
 *
 * Reuses the project's existing CMake build system rather than inventing a
 * second build system. Task names map to shell commands that the task runner
 * expands and launches through TerminalProcess.
 *
 * Tasks are JUCE-free and testable — they are pure data (name → command spec).
 * The TerminalPanel owns a TaskRunner instance and uses it to populate the
 * quick-launch task list.
 *
 * Requirement references: L-4 §Architecture ("lightweight task runner")
 */

#include <functional>
#include <string>
#include <vector>

namespace hathor::ui {

/**
 * A single named task (e.g. "build", "test", "check").
 *
 * @param id     Stable machine-readable identifier (e.g. "build").
 * @param label  Human-readable label shown in the UI (e.g. "Build Project").
 * @param command The command to run. May contain the placeholder
 *                "{buildDir}" which is replaced with the project's build
 *                directory at launch time.
 * @param cwd    Working directory for the command (relative to project root
 *               or absolute). Empty = project root (cwd at launch).
 * @param isLongRunning  If true, the task is expected to produce output over
 *                        time (e.g. build). If false, it's expected to
 *                        complete quickly (e.g. version check).
 */
struct TaskDef
{
    std::string id;
    std::string label;
    std::string command;
    std::string cwd;
    bool isLongRunning = true;
};

/**
 * TaskRunner — maps task names to TaskDef command specs.
 *
 * The runner is populated at construction with the Hathor project's known
 * tasks. It resolves the build directory from the current working directory
 * or an explicit override, and expands {buildDir} placeholders in commands.
 *
 * JUCE-free: no JUCE headers required. All methods are safe to call from
 * the JUCE message thread (they are non-blocking and allocation-light).
 */
class TaskRunner
{
public:
    /**
     * Construct with the project root directory (usually cwd at app launch).
     * @param projectDir  The project root to resolve {buildDir} and cwd
     *                    against.
     * @param buildDir    The build directory (defaults to "<projectDir>/build"
     *                    if not provided).
     */
    explicit TaskRunner(std::string projectDir,
                        std::string buildDir = {});

    /** Default set of project tasks. */
    static std::vector<TaskDef> defaultTasks();

    /** All registered tasks. */
    const std::vector<TaskDef>& tasks() const noexcept { return tasks_; }

    /** Look up a task by id. Returns nullptr if not found. */
    const TaskDef* findTask(std::string_view id) const noexcept;

    /**
     * Expand {buildDir} and {projectDir} placeholders in a command string.
     */
    std::string expandPlaceholders(const std::string& command) const;

    /**
     * Get the list of quick-launch task labels/ids for the UI.
     */
    std::vector<std::pair<std::string, std::string>> taskList() const noexcept;

    /** Project root directory. */
    const std::string& projectDir() const noexcept { return projectDir_; }

    /** Build directory. */
    const std::string& buildDir() const noexcept { return buildDir_; }

private:
    std::string projectDir_;
    std::string buildDir_;
    std::vector<TaskDef> tasks_;
};

} // namespace hathor::ui
