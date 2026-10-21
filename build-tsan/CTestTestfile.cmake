# CMake generated Testfile for 
# Source directory: /home/devansh/kernel_suite/pace-sdk
# Build directory: /home/devansh/kernel_suite/pace-sdk/build-tsan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("pace_unit_tests" "/home/devansh/kernel_suite/pace-sdk/build-tsan/pace_unit_tests")
set_tests_properties("pace_unit_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;183;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
add_test("pace_primitive_tests" "/home/devansh/kernel_suite/pace-sdk/build-tsan/pace_primitive_tests")
set_tests_properties("pace_primitive_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;192;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
add_test("pace_concurrency_tests" "/home/devansh/kernel_suite/pace-sdk/build-tsan/pace_concurrency_tests")
set_tests_properties("pace_concurrency_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;201;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
add_test("pace_lifecycle_tests" "/home/devansh/kernel_suite/pace-sdk/build-tsan/pace_lifecycle_tests")
set_tests_properties("pace_lifecycle_tests" PROPERTIES  _BACKTRACE_TRIPLES "/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;208;add_test;/home/devansh/kernel_suite/pace-sdk/CMakeLists.txt;0;")
subdirs("examples")
