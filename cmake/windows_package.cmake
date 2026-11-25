# Optional per-user installer helper, never executed by a normal product build.
add_executable(piero_path EXCLUDE_FROM_ALL tools/installer_path.c tools/path_entry.c)
target_include_directories(piero_path PRIVATE tools)
target_link_libraries(piero_path PRIVATE advapi32 user32)
target_link_options(piero_path PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/DEPENDENTLOADFLAG:0x800>")
