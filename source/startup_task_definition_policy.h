// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#pragma once

enum StartupTaskDefinitionClass {
    STARTUP_TASK_DEFINITION_BROKEN = 0,
    STARTUP_TASK_DEFINITION_COMPATIBLE_LEGACY = 1,
    STARTUP_TASK_DEFINITION_CANONICAL = 2,
};

// Pure task-XML classification entry point used by production and executable
// fixtures.  It performs no Task Scheduler query or mutation.
StartupTaskDefinitionClass startup_task_definition_classify_xml(
    const WCHAR* xml, const WCHAR* expectedUser, const WCHAR* exePath,
    const WCHAR* cfgPath, const WCHAR* expectedWorkingDir,
    char* detail, size_t detailSize);
