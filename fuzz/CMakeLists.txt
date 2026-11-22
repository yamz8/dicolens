# One libFuzzer program for each target, named fuzz_<target>, and the program
# that writes their seeds. bin/fuzz builds and runs them.
set(fuzz_targets parse decode ljpeg rle native frames marks reports scan)

foreach(target ${fuzz_targets})
  add_executable(fuzz_${target} ${target}.cpp)
  target_link_libraries(fuzz_${target} PRIVATE dicolens_core)
  target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
endforeach()

add_executable(dicolens_fuzz_seeds seeds.cpp)
target_include_directories(dicolens_fuzz_seeds PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(dicolens_fuzz_seeds PRIVATE dicolens_core)

list(TRANSFORM fuzz_targets PREPEND fuzz_ OUTPUT_VARIABLE fuzz_programs)
add_custom_target(fuzzers DEPENDS ${fuzz_programs} dicolens_fuzz_seeds)
