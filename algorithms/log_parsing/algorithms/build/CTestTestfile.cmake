# CMake generated Testfile for 
# Source directory: /src/algorithms/log_parsing/algorithms
# Build directory: /src/algorithms/log_parsing/algorithms/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[parser_unit]=] "/src/algorithms/log_parsing/algorithms/build/logtop_parser_tests")
set_tests_properties([=[parser_unit]=] PROPERTIES  _BACKTRACE_TRIPLES "/src/algorithms/log_parsing/algorithms/CMakeLists.txt;28;add_test;/src/algorithms/log_parsing/algorithms/CMakeLists.txt;0;")
add_test([=[tracker_unit]=] "/src/algorithms/log_parsing/algorithms/build/logtop_tracker_tests")
set_tests_properties([=[tracker_unit]=] PROPERTIES  _BACKTRACE_TRIPLES "/src/algorithms/log_parsing/algorithms/CMakeLists.txt;34;add_test;/src/algorithms/log_parsing/algorithms/CMakeLists.txt;0;")
add_test([=[end_to_end]=] "bash" "/src/algorithms/log_parsing/algorithms/tests/run_e2e.sh")
set_tests_properties([=[end_to_end]=] PROPERTIES  ENVIRONMENT "LOGTOP=/src/algorithms/log_parsing/algorithms/build/logtop" _BACKTRACE_TRIPLES "/src/algorithms/log_parsing/algorithms/CMakeLists.txt;37;add_test;/src/algorithms/log_parsing/algorithms/CMakeLists.txt;0;")
