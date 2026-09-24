"""Windows build-variant planning and isolated output orchestration."""

import os
import shutil
import tempfile


MSVC_VARIANT = "msvc"
RELEASE_VARIANT = "release"
WINDOWS_VARIANTS = (MSVC_VARIANT, RELEASE_VARIANT)


def selected_toolchain(toolchain, sanitizer):
    return "llvm-mingw" if sanitizer else toolchain


def package_needs_variant(os_name, variant):
    return os_name == "windows" and variant is not None


def windows_variants(toolchain, target, host_platform, builds_windows_binaries=True):
    if not builds_windows_binaries or target not in ("windows", "all"):
        return ()
    if host_platform != "win32":
        return (RELEASE_VARIANT,)
    if toolchain == "auto":
        return WINDOWS_VARIANTS
    if toolchain == "clang-cl":
        return (MSVC_VARIANT,)
    if toolchain == "llvm-mingw":
        return (RELEASE_VARIANT,)
    raise ValueError(f"unsupported Windows toolchain selection: {toolchain!r}")


def needs_release_windows_toolchain(toolchain, target, host_platform):
    return (target in ("windows", "all")
            and (host_platform != "win32" or toolchain in ("auto", "llvm-mingw")))


def _validate_variant(variant):
    if variant not in WINDOWS_VARIANTS:
        raise ValueError(f"unsupported Windows build variant: {variant!r}")


def payload_dir(script_dir, os_name, arch, variant=None):
    target = f"{os_name}-{arch}"
    if os_name == "windows" and variant is not None:
        _validate_variant(variant)
        target = os.path.join(target, variant)
    return os.path.join(script_dir, "dist", target, "greencurve")


def package_dir(script_dir, os_name, arch, variant=None):
    if os_name == "windows" and variant is not None:
        _validate_variant(variant)
    return os.path.dirname(payload_dir(script_dir, os_name, arch, variant))



def symbol_dir(script_dir, arch, variant=None):
    target = f"windows-{arch}"
    if variant is not None:
        _validate_variant(variant)
        target = os.path.join(target, variant)
    return os.path.join(script_dir, "dist", "symbols", target)


def symbol_path(script_dir, output_path, arch, toolchain, variant=None):
    name = os.path.basename(output_path)
    if name.endswith(".new"):
        name = name[:-4]
    stem = os.path.splitext(name)[0]
    extension = ".debug" if arch == "arm64" and toolchain != "clang-cl" else ".pdb"
    return os.path.join(symbol_dir(script_dir, arch, variant), stem + extension)


def activate(ctx, variant, msvc_toolchain):
    if variant == MSVC_VARIANT:
        if msvc_toolchain is None:
            raise RuntimeError("the MSVC Windows variant was selected without a toolchain")
        ctx.MSVC_TOOLCHAIN = msvc_toolchain
        ctx.ACTIVE_WINDOWS_TOOLCHAIN = "clang-cl"
    elif variant == RELEASE_VARIANT:
        ctx.MSVC_TOOLCHAIN = None
        ctx.ACTIVE_WINDOWS_TOOLCHAIN = "llvm-mingw"
    else:
        raise ValueError(f"unsupported Windows build variant: {variant!r}")
    print(f"Windows build variant: {variant} ({ctx.ACTIVE_WINDOWS_TOOLCHAIN})")


def run_windows_builds(ctx, variants, msvc_toolchain, arches, jobs, limiter):
    built = []
    for variant in variants:
        activate(ctx, variant, msvc_toolchain)
        output_variant = variant if ctx.sys.platform == "win32" else None
        specs = []
        for arch in arches:
            payload = payload_dir(ctx.SCRIPT_DIR, "windows", arch, output_variant)
            shutil.rmtree(payload, ignore_errors=True)
            os.makedirs(payload, exist_ok=True)
            gui = os.path.join(payload, "greencurve.exe")
            service = os.path.join(payload, "greencurve-service.exe")
            built.append(("windows", arch, [gui, service], output_variant))
            specs.append(lambda gui=gui, arch=arch, output_variant=output_variant:
                         ctx.compile_windows_binary(
                             output_path=gui, temp_output=gui + ".new",
                             backup_path=gui + ".bak", arch=arch,
                             jobs=jobs, limiter=limiter, variant=output_variant))
            specs.append(lambda service=service, arch=arch, output_variant=output_variant:
                         ctx.compile_windows_service_binary(
                             output_path=service, temp_output=service + ".new",
                             backup_path=service + ".bak", arch=arch,
                             jobs=jobs, limiter=limiter, variant=output_variant))
        if specs:
            ctx.build_scheduler.run_parallel(specs, jobs)
    return built


def run_windows_check_builds(ctx, variants, msvc_toolchain, arches, jobs,
                             limiter, work):
    for variant in variants:
        activate(ctx, variant, msvc_toolchain)
        output_variant = variant if ctx.sys.platform == "win32" else None
        specs = []
        for arch in arches:
            target = os.path.join(work, variant, arch)
            os.makedirs(target, exist_ok=True)
            gui = os.path.join(target, "greencurve.exe")
            service = os.path.join(target, "greencurve-service.exe")
            specs.append(lambda gui=gui, arch=arch, output_variant=output_variant:
                         ctx.compile_windows_binary(
                             output_path=gui, temp_output=gui + ".new",
                             backup_path="", finalize=False, arch=arch,
                             jobs=jobs, limiter=limiter, variant=output_variant))
            specs.append(lambda service=service, arch=arch, output_variant=output_variant:
                         ctx.compile_windows_service_binary(
                             output_path=service, temp_output=service + ".new",
                             backup_path="", finalize=False, arch=arch,
                             jobs=jobs, limiter=limiter, variant=output_variant))
        if specs:
            ctx.build_scheduler.run_parallel(specs, jobs)


def run_self_tests():
    if windows_variants("auto", "all", "win32") != WINDOWS_VARIANTS:
        raise AssertionError("native Windows auto must select both variants")
    if windows_variants("clang-cl", "windows", "win32") != (MSVC_VARIANT,):
        raise AssertionError("forced clang-cl must select only MSVC")
    if windows_variants("llvm-mingw", "windows", "win32") != (RELEASE_VARIANT,):
        raise AssertionError("forced llvm-mingw must select only the release variant")
    if windows_variants("auto", "windows", "linux") != (RELEASE_VARIANT,):
        raise AssertionError("Linux hosts must retain the release Windows path")
    if windows_variants("auto", "linux", "win32") != ():
        raise AssertionError("Linux-only builds must not select a Windows variant")
    if selected_toolchain("auto", True) != "llvm-mingw":
        raise AssertionError("sanitizer builds must select llvm-mingw")
    if not package_needs_variant("windows", MSVC_VARIANT):
        raise AssertionError("native Windows packages must reactivate their variant")
    if package_needs_variant("windows", None):
        raise AssertionError("Linux-hosted Windows packages must stay unscoped")
    if not needs_release_windows_toolchain("auto", "all", "win32"):
        raise AssertionError("native Windows auto must provision the release path")
    if not needs_release_windows_toolchain("auto", "windows", "linux"):
        raise AssertionError("Linux-hosted Windows builds must provision Zig")
    if needs_release_windows_toolchain("clang-cl", "all", "win32"):
        raise AssertionError("forced clang-cl must not provision the release compiler")
    root = os.path.join("repo", "dist")
    msvc_payload = payload_dir("repo", "windows", "x64", MSVC_VARIANT)
    release_payload = payload_dir("repo", "windows", "x64", RELEASE_VARIANT)
    if msvc_payload == release_payload:
        raise AssertionError("Windows variants share a payload directory")
    if package_dir("repo", "windows", "x64", MSVC_VARIANT) != os.path.dirname(msvc_payload):
        raise AssertionError("variant package directory is not scoped")
    if package_dir("repo", "windows", "x64") != os.path.join(root, "windows-x64"):
        raise AssertionError("unscoped Windows package directory changed")
    if package_dir("repo", "linux", "x64") != os.path.join(root, "linux-x64"):
        raise AssertionError("Linux package directory changed")
    if package_dir("repo", "linux", "arm64") != os.path.join(root, "linux-arm64"):
        raise AssertionError("Linux ARM64 package directory changed")
    if payload_dir("repo", "windows", "x64") != os.path.join(root, "windows-x64", "greencurve"):
        raise AssertionError("host-independent Windows payload path changed")
    if symbol_path("repo", "greencurve.exe", "x64", "clang-cl", MSVC_VARIANT) == \
            symbol_path("repo", "greencurve.exe", "x64", "llvm-mingw", RELEASE_VARIANT):
        raise AssertionError("variant symbol paths collide")

    class Scheduler:
        def run_parallel(self, specs, jobs):
            for spec in specs:
                spec()

    class Host:
        platform = "win32"

    class Context:
        SCRIPT_DIR = "repo"
        sys = Host()
        ACTIVE_WINDOWS_TOOLCHAIN = None
        MSVC_TOOLCHAIN = None
        build_scheduler = Scheduler()
        calls = []

        def compile_windows_binary(self, **kwargs):
            self.calls.append((self.ACTIVE_WINDOWS_TOOLCHAIN,
                               kwargs["variant"]))

        def compile_windows_service_binary(self, **kwargs):
            self.calls.append((self.ACTIVE_WINDOWS_TOOLCHAIN,
                               kwargs["variant"]))

    context = Context()
    with tempfile.TemporaryDirectory(prefix="greencurve-variants-") as work:
        run_windows_check_builds(
            context, WINDOWS_VARIANTS, object(), ["x64"], 1, None, work)
    expected = [("clang-cl", MSVC_VARIANT), ("clang-cl", MSVC_VARIANT),
                ("llvm-mingw", RELEASE_VARIANT), ("llvm-mingw", RELEASE_VARIANT)]
    if context.calls != expected:
        raise AssertionError(f"variant check dispatch order is wrong: {context.calls!r}")
    print("build_variants self-tests passed")
