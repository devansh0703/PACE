# CMake generated Testfile for 
# Source directory: /home/devansh/kernel_suite/pace-sdk
# Build directory: /home/devansh/kernel_suite/pace-sdk/build_nobpf
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("pace_unit_tests" "/home/devansh/kernel_suite/pace-sdk/build_nobpf/pace_unit_tests")
set_tests_properties("pace_unit_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;193;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
add_test("pace_primitive_tests" "/home/devansh/kernel_suite/pace-sdk/build_nobpf/pace_primitive_tests")
set_tests_properties("pace_primitive_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;202;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
add_test("pace_concurrency_tests" "/home/devansh/kernel_suite/pace-sdk/build_nobpf/pace_concurrency_tests")
set_tests_properties("pace_concurrency_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;211;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
add_test("pace_lifecycle_tests" "/home/devansh/kernel_suite/pace-sdk/build_nobpf/pace_lifecycle_tests")
set_tests_properties("pace_lifecycle_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;225;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
subdirs("examples")
