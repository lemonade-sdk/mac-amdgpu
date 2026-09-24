add_executable(rocprofmac main.cpp)
target_link_libraries(rocprofmac PRIVATE hsa-runtime64)
target_compile_options(rocprofmac PRIVATE -Wall -Wextra -Werror)
# Keep the qualification command stable within the selected HSA build tree.
set_target_properties(rocprofmac PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
