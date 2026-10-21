import os
import shutil

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, load
from conan.tools.scm import Version


class PaceConan(ConanFile):
    name = "pace"
    version = "0.1.0"
    license = "BUSL-1.1"
    url = "https://github.com/devansh0703/PACE"
    homepage = "https://github.com/devansh0703/PACE"
    description = (
        "Preemption-Aware Time Attribution Engine: per-thread on-CPU, preempted "
        "and blocked time for developer-tool vendors."
    )
    topics = ("profiling", "scheduler", "preemption", "attribution")

    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_ebpf": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_ebpf": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "CMakePresets.json",
        "cmake/*",
        "include/*",
        "src/*",
        "LICENSE",
        "LICENSING.md",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC
        # The eBPF tier is Linux-only.
        if self.settings.os != "Linux":
            del self.options.with_ebpf

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        self.settings.compiler.libcxx = "libstdc++11"

    def layout(self):
        cmake_layout(self)

    def requirements(self):
        if self.options.with_ebpf:
            # ConanCenter's newest libbpf is 1.3.0, so a pinned requirement
            # of the upstream release would be unsatisfiable. A range lets
            # conancenter resolve what it has while still allowing a
            # consumer to pin a newer one from a different remote.
            #
            # The BPF loader is only compiled when the eBPF tier is on, so a
            # consumer that does not use the tier never pays for it.
            self.requires("libbpf/[>=1.3.0 <2.0.0]")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        tc = CMakeToolchain(self)
        tc.variables["PACE_BUILD_TESTS"] = False
        tc.variables["PACE_BUILD_BENCH"] = False
        tc.variables["PACE_BUILD_EXAMPLES"] = False
        tc.variables["PACE_ENABLE_BPF"] = bool(self.options.with_ebpf)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSING.md", self.source_folder,
             os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "pace")
        self.cpp_info.set_property("cmake_target_name", "pace::pace")
        self.cpp_info.libs = ["pace"]
        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs = ["pthread"]
        if self.options.with_ebpf:
            self.cpp_info.libs.append("bpf")
