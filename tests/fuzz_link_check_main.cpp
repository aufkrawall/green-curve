// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Entry point for the Linux fuzz-target LINK CHECK, not for fuzzing.
//
// `python build.py --test` cross-links every Linux libFuzzer target for
// x86_64-linux-gnu on a non-Linux host, so a target whose link line is missing
// a translation unit fails on the machine that caused it instead of only in
// the Linux CI job.  That cross-link cannot use -fsanitize=fuzzer (the bundled
// Zig ships no libFuzzer runtime, which is exactly why the real Linux fuzz run
// needs a host clang), so libFuzzer's own main() is absent and this file
// supplies one.
//
// It must genuinely CALL LLVMFuzzerTestOneInput: the link is performed with
// section garbage collection on, so a merely-declared entry point gets dropped
// together with the undefined references this check exists to find.
//
// The produced binary is never executed -- it is built for Linux from a
// Windows host, and on a Linux host `--fuzz` builds the real instrumented
// targets instead.  This is a link check only; it proves nothing about
// behaviour.

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(void) {
    static const uint8_t oneByte[1] = {0};
    return LLVMFuzzerTestOneInput(oneByte, sizeof(oneByte)) == 0 ? 0 : 1;
}
