# Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
# Compile the facade with isolated synthetic tables, avoiding FEX or QEMU
# initialization. Real translated execution remains a separate runtime gate.
include_guard(GLOBAL)
add_executable(lumen_memory_handoff_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/MemoryHandoffTest.cpp")
target_compile_features(lumen_memory_handoff_test PRIVATE cxx_std_20)
target_compile_options(lumen_memory_handoff_test PRIVATE -Wall -Wextra -Werror)
if(BUILD_TESTING)
    add_test(NAME lumen_memory_handoff COMMAND lumen_memory_handoff_test)
    set_tests_properties(lumen_memory_handoff PROPERTIES TIMEOUT 15 LABELS "cpu;gpu;memory;handoff")
endif()
add_executable(lumen_guest_cpu_selector_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/GuestCpuSelectorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GuestCpuRuntime.cpp")
target_include_directories(lumen_guest_cpu_selector_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../../android/app/src/main/cpp")
target_compile_features(lumen_guest_cpu_selector_test PRIVATE cxx_std_20)
target_compile_options(lumen_guest_cpu_selector_test PRIVATE -Wall -Wextra -Werror)
add_executable(lumen_guest_cpu_dual_selector_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/GuestCpuSelectorTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GuestCpuRuntime.cpp")
target_include_directories(lumen_guest_cpu_dual_selector_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../../android/app/src/main/cpp")
target_compile_features(lumen_guest_cpu_dual_selector_test PRIVATE cxx_std_20)
target_compile_options(lumen_guest_cpu_dual_selector_test PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lumen_guest_cpu_dual_selector_test PRIVATE LUMEN_WITH_QEMU_TCG=1)
if(BUILD_TESTING)
    foreach(mode default names unavailable registration native-legacy native-v2 mutation-legacy-v1 mutation-legacy-v2 mutation-v3 handoff-legacy-v3 diagnostic-legacy-v4 locked-probe locked-registration
                 locked-mutation environment invalid-environment memory-query effects-query concurrent)
        add_test(NAME lumen_guest_cpu_selector_${mode}
            COMMAND lumen_guest_cpu_selector_test "${mode}")
        set_tests_properties(lumen_guest_cpu_selector_${mode} PROPERTIES
            TIMEOUT 15 LABELS "cpu;translator;synthetic")
    endforeach()
    foreach(mode default names environment invalid-environment)
        add_test(NAME lumen_guest_cpu_dual_selector_${mode}
            COMMAND lumen_guest_cpu_dual_selector_test "${mode}")
        set_tests_properties(lumen_guest_cpu_dual_selector_${mode} PROPERTIES
            TIMEOUT 15 LABELS "cpu;translator;synthetic")
    endforeach()
endif()
