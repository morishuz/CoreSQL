cmake_minimum_required(VERSION 3.20)
project(CoreSQL VERSION 0.1.0 LANGUAGES CXX)
include(cmake/CheckToolchain.cmake)

option(CORESQL_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(CORESQL_THREAD_SANITIZER "Enable thread sanitizer (separate build)" OFF)
if(CORESQL_SANITIZERS AND CORESQL_THREAD_SANITIZER)
  message(FATAL_ERROR "Use separate builds for address/undefined and thread sanitizers")
endif()
option(CORESQL_BENCHMARKS "Build the optional SQLite comparison harness" OFF)
set(CORESQL_MAX_ENCODED_MIB "1024" CACHE STRING "Maximum encoded live state, snapshot and record size in MiB")
if(NOT CORESQL_MAX_ENCODED_MIB MATCHES "^[1-9][0-9]*$" OR CORESQL_MAX_ENCODED_MIB GREATER 16384)
  message(FATAL_ERROR "CORESQL_MAX_ENCODED_MIB must be between 1 and 16384")
endif()
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

add_library(coresql
  src/core.cpp src/read_snapshot.cpp src/query_cursor.cpp src/pager.cpp src/paged_index.cpp src/expression.cpp src/bound.cpp
  src/query.cpp src/query_scan.cpp src/query_join.cpp src/query_reduce.cpp
  src/disjunctive_join.cpp src/relation.cpp src/subquery_plan.cpp src/mutation.cpp
  src/transaction.cpp src/constraints.cpp src/schema.cpp src/registry.cpp src/index.cpp
  src/ordered_index.cpp src/ordered_scan.cpp src/index_selection.cpp src/key_index.cpp src/snapshot.cpp src/storage.cpp
  addons/scalars/type.cpp addons/aggregates/aggregates.cpp addons/relational/functions.cpp addons/hash_index/index.cpp)
target_include_directories(coresql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(coresql PUBLIC cxx_std_20)
target_link_libraries(coresql PUBLIC Threads::Threads)
# Private storage headers are also compiled by in-tree tests and diagnostics.
# Propagate their configuration in builds without imposing it on installed consumers.
target_compile_definitions(coresql PUBLIC
  $<BUILD_INTERFACE:CORESQL_MAX_ENCODED_MIB=${CORESQL_MAX_ENCODED_MIB}>)
set_target_properties(coresql PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(coresql PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
if(CORESQL_SANITIZERS)
  target_compile_options(coresql PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(coresql PUBLIC -fsanitize=address,undefined)
endif()
if(CORESQL_THREAD_SANITIZER)
  target_compile_options(coresql PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(coresql PUBLIC -fsanitize=thread)
endif()

# Add-ons use the public core API; SQL remains an independently linked frontend.
function(coresql_addon name source)
  add_library(coresql_${name} ${source})
  target_link_libraries(coresql_${name} PUBLIC coresql)
  target_compile_options(coresql_${name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endfunction()
coresql_addon(blob addons/blob/type.cpp)
coresql_addon(vector addons/vector/type.cpp)
coresql_addon(timestamp addons/timestamp/type.cpp)
coresql_addon(spatial addons/spatial/type.cpp)
coresql_addon(json addons/json/type.cpp)
coresql_addon(graph addons/graph/graph.cpp)
coresql_addon(date addons/date/type.cpp)
coresql_addon(decimal addons/decimal/type.cpp)
add_library(coresql_sql
  sql/parser.cpp sql/lower.cpp sql/select.cpp sql/execute.cpp sql/cursor.cpp sql/read_connection.cpp sql/insert.cpp sql/upsert.cpp sql/returning.cpp sql/using.cpp
  sql/values.cpp sql/coercion.cpp sql/typing.cpp sql/type_lowering.cpp
  sql/type_adapters.cpp sql/relations.cpp sql/strings.cpp sql/query_cache.cpp
  addons/blob/sql.cpp addons/decimal/sql.cpp addons/date/sql.cpp addons/vector/sql.cpp addons/scalars/sql.cpp)
target_link_libraries(coresql_sql PUBLIC coresql coresql_date coresql_decimal coresql_vector coresql_blob)
target_compile_options(coresql_sql PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

# Identical target names for add_subdirectory() and installed find_package().
set(CORESQL_LIBRARIES coresql coresql_blob coresql_sql coresql_vector coresql_timestamp coresql_spatial coresql_json coresql_graph coresql_date coresql_decimal)
foreach(target IN LISTS CORESQL_LIBRARIES)
  if(target STREQUAL "coresql")
    set(component core)
  else()
    string(REPLACE "coresql_" "" component "${target}")
  endif()
  set_target_properties(${target} PROPERTIES EXPORT_NAME ${component} CXX_EXTENSIONS OFF)
  add_library(CoreSQL::${component} ALIAS ${target})
endforeach()
install(TARGETS ${CORESQL_LIBRARIES} EXPORT CoreSQLTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/coresql DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE PROVENANCE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/CoreSQL)
install(EXPORT CoreSQLTargets NAMESPACE CoreSQL:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreSQL)
configure_package_config_file(cmake/CoreSQLConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CoreSQLConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreSQL)
# Experimental releases make no source or ABI compatibility promise.
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/CoreSQLConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/CoreSQLConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CoreSQLConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoreSQL)

add_executable(coresql_custom_type examples/custom_type/main.cpp examples/custom_type/type.cpp examples/custom_type/sql.cpp)
target_link_libraries(coresql_custom_type PRIVATE coresql_sql)

add_executable(coresql_demo examples/demo.cpp)
find_package(Threads REQUIRED)
add_library(coresql_landmark_worker STATIC examples/landmark_memory/worker.cpp)
target_link_libraries(coresql_landmark_worker PUBLIC coresql_sql Threads::Threads)
add_executable(coresql_landmark_memory examples/landmark_memory/main.cpp)
target_link_libraries(coresql_landmark_memory PRIVATE coresql_sql)
target_link_libraries(coresql_demo PRIVATE coresql_vector coresql_timestamp)
add_executable(coresql_documents examples/documents.cpp)
target_link_libraries(coresql_documents PRIVATE coresql_vector coresql_timestamp)
add_executable(coresql_collections examples/collections.cpp)
target_link_libraries(coresql_collections PRIVATE coresql)
add_executable(coresql_json_demo examples/json.cpp)
target_link_libraries(coresql_json_demo PRIVATE coresql_json)
add_executable(coresql_graph_demo examples/graph.cpp)
target_link_libraries(coresql_graph_demo PRIVATE coresql_graph)
add_executable(coresql_sql_cli examples/sql.cpp)
target_link_libraries(coresql_sql_cli PRIVATE coresql_sql)

add_executable(coresql_admin examples/admin.cpp)
target_link_libraries(coresql_admin PRIVATE coresql_sql)
add_executable(coresql_persistent_sql examples/persistent_sql.cpp)
target_link_libraries(coresql_persistent_sql PRIVATE coresql_sql)
install(TARGETS coresql_sql_cli coresql_admin coresql_persistent_sql RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

function(coresql_test target name source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE ${ARGN})
  add_test(NAME ${name} COMMAND ${target})
endfunction()

if(BUILD_TESTING)
  coresql_test(coresql_checkpoint_codec_test checkpoint_codec tests/checkpoint_codec_test.cpp coresql_sql coresql_timestamp)
  coresql_test(coresql_index_ownership_test index_ownership tests/index_ownership_test.cpp coresql_sql)
  coresql_test(coresql_concurrent_snapshot_test concurrent_snapshots tests/concurrent_snapshot_test.cpp coresql_sql Threads::Threads)
  coresql_test(coresql_background_checkpoint_test background_checkpoints tests/background_checkpoint_test.cpp coresql Threads::Threads)
  target_compile_definitions(coresql_background_checkpoint_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_sql_compatibility_test sql_compatibility tests/sql_compatibility_test.cpp coresql_sql)
  coresql_test(coresql_ordered_scan_test ordered_scan tests/ordered_scan_test.cpp coresql_sql coresql_timestamp)
  target_compile_definitions(coresql_ordered_scan_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_composite_range_test composite_range tests/composite_range_test.cpp coresql_sql)
  target_compile_definitions(coresql_composite_range_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_constraint_recovery_test constraint_recovery tests/constraint_recovery_test.cpp coresql_sql)
  target_compile_definitions(coresql_constraint_recovery_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_query_cache_test query_cache tests/query_cache_test.cpp coresql_sql)
  coresql_test(coresql_upsert_test upsert tests/upsert_test.cpp coresql_sql)
  coresql_test(coresql_blob_test blob tests/blob_test.cpp coresql_sql)
  coresql_test(coresql_constraints_test constraints tests/constraints_test.cpp coresql_sql)
  coresql_test(coresql_landmark_worker_test landmark_worker tests/landmark_worker_test.cpp coresql_landmark_worker)
  coresql_test(coresql_query_control_test query_control tests/query_control_test.cpp coresql_sql)
  target_compile_definitions(coresql_query_control_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_query_cursor_test query_cursor tests/query_cursor_test.cpp coresql_sql)
  target_compile_definitions(coresql_query_cursor_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_sql_application_features_test sql_application_features tests/sql_application_features_test.cpp coresql_sql)
  coresql_test(coresql_native_join_test native_hash_joins tests/native_join_test.cpp coresql_sql)
  target_compile_definitions(coresql_native_join_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_landmark_memory_test landmark_memory tests/landmark_memory_test.cpp coresql_sql)
  coresql_test(coresql_sql_type_dispatch_test sql_type_dispatch tests/sql_type_dispatch_test.cpp coresql_sql coresql_timestamp)
  target_compile_definitions(coresql_sql_type_dispatch_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_sql_scalar_adapter_test sql_scalar_adapters tests/sql_scalar_adapter_test.cpp coresql_sql)
  coresql_test(coresql_sql_vector_test sql_vectors tests/sql_vector_test.cpp coresql_sql)
  coresql_test(coresql_sql_type_adapter_test sql_type_adapters tests/sql_type_adapter_test.cpp coresql_sql)
  coresql_test(coresql_encoded_limit_test encoded_size_limit tests/encoded_limit_test.cpp coresql)
  add_test(NAME backup_restore_cli COMMAND ${CMAKE_COMMAND}
    -DEXAMPLE=$<TARGET_FILE:coresql_persistent_sql> -DADMIN=$<TARGET_FILE:coresql_admin>
    -DCLI=$<TARGET_FILE:coresql_sql_cli> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/admin-cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/admin_cli.cmake)
  coresql_test(coresql_generated_ids_test generated_integer_ids tests/generated_ids_test.cpp coresql_sql)
  target_compile_definitions(coresql_generated_ids_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_readiness_test application_readiness tests/readiness_test.cpp coresql_sql)
  target_compile_definitions(coresql_readiness_test PRIVATE CORESQL_TESTING)
  if(CORESQL_MAX_ENCODED_MIB GREATER_EQUAL 128)
    coresql_test(coresql_large_storage_test larger_persistent_storage tests/large_storage_test.cpp coresql)
    set_tests_properties(larger_persistent_storage PROPERTIES RUN_SERIAL TRUE)
  endif()
  coresql_test(coresql_local_guard_test local_guards tests/local_guard_test.cpp coresql_sql)
  coresql_test(coresql_group_table_test group_table tests/group_table_test.cpp coresql)
  coresql_test(coresql_predicate_reuse_test predicate_reuse tests/predicate_reuse_test.cpp coresql)
  coresql_test(coresql_membership_lookup_test membership_lookups tests/membership_lookup_test.cpp coresql_sql)
  coresql_test(coresql_projection_cache_test projection_cache tests/projection_cache_test.cpp coresql)
  coresql_test(coresql_prepared_function_test prepared_functions tests/prepared_function_test.cpp coresql)
  coresql_test(coresql_filtered_join_test filtered_join_views tests/filtered_join_test.cpp coresql)
  target_compile_definitions(coresql_filtered_join_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_snapshot_import_test snapshot_import tests/snapshot_import_test.cpp coresql)
  coresql_test(coresql_text_test text_semantics tests/text_test.cpp coresql_sql)
  coresql_test(coresql_table_view_test table_row_views tests/table_view_test.cpp coresql)
  target_compile_definitions(coresql_table_view_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_stream_aggregate_test streaming_aggregates tests/stream_aggregate_test.cpp coresql)
  target_compile_definitions(coresql_stream_aggregate_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_borrowed_on_test borrowed_on_joins tests/borrowed_on_test.cpp coresql)
  target_compile_definitions(coresql_borrowed_on_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_outer_hash_test outer_hash_candidates tests/outer_hash_test.cpp coresql_sql)
  target_compile_definitions(coresql_outer_hash_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_correlated_source_test correlated_sources tests/correlated_source_test.cpp coresql_sql)
  target_compile_definitions(coresql_correlated_source_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_join_prefix_test join_prefix tests/join_prefix_test.cpp coresql_sql)
  target_compile_definitions(coresql_join_prefix_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_indexed_join_input_test indexed_join_inputs tests/indexed_join_input_test.cpp coresql_sql)
  target_compile_definitions(coresql_indexed_join_input_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_primary_range_test primary_ranges tests/primary_range_test.cpp coresql_sql coresql_timestamp)
  target_compile_definitions(coresql_primary_range_test PRIVATE CORESQL_TESTING)
  target_compile_definitions(coresql PRIVATE CORESQL_TESTING)
  coresql_test(coresql_disjunctive_join_test disjunctive_join tests/disjunctive_join_test.cpp coresql_sql coresql_timestamp)
  target_compile_definitions(coresql_disjunctive_join_test PRIVATE CORESQL_TESTING)
  coresql_test(coresql_sql_relations_test sql_derived_relations tests/sql_relations_test.cpp coresql_sql)
  coresql_test(coresql_decimal_test exact_decimals tests/decimal_test.cpp coresql_sql)
  coresql_test(coresql_date_test calendar_dates tests/date_test.cpp coresql_sql)
  coresql_test(coresql_core_test core_without_addons tests/core_test.cpp coresql)
  coresql_test(coresql_paging_test paging tests/paging_test.cpp coresql_sql)
  coresql_test(coresql_comparison_test bound_comparisons tests/comparison_test.cpp coresql)
  coresql_test(coresql_addon_test addon_contracts tests/addon_test.cpp coresql_vector coresql_timestamp)
  coresql_test(coresql_mutation_test atomic_mutations tests/mutation_test.cpp coresql_vector coresql_timestamp)
  coresql_test(coresql_point_mutation_test point_mutations tests/point_mutation_test.cpp coresql)
  coresql_test(coresql_storage_test durable_recovery tests/storage_test.cpp coresql_vector coresql_timestamp)
  coresql_test(coresql_chunk_test chunk_snapshots tests/chunk_test.cpp coresql)
  coresql_test(coresql_query_test query_ordering_and_schema tests/query_test.cpp coresql_vector coresql_timestamp)
  coresql_test(coresql_document_test document_application tests/document_test.cpp coresql_vector coresql_timestamp)
  coresql_test(coresql_borrow_test borrowed_query_values tests/borrow_test.cpp coresql)
  coresql_test(coresql_primary_test primary_keys tests/primary_test.cpp coresql_vector)
  coresql_test(coresql_filter_test compound_filters tests/filter_test.cpp coresql_vector coresql_timestamp)
  coresql_test(coresql_join_test inner_joins_and_clear tests/join_test.cpp coresql_vector)
  coresql_test(coresql_index_integrity_test index_integrity tests/index_integrity_test.cpp coresql_spatial coresql_json)
  coresql_test(coresql_extension_index_test extension_indexes tests/extension_index_test.cpp coresql_spatial coresql_timestamp)
  coresql_test(coresql_json_test extracted_json_keys tests/json_test.cpp coresql_json)
  coresql_test(coresql_row_index_test row_index_results tests/row_index_test.cpp coresql_json)
  coresql_test(coresql_savepoint_test transaction_savepoints tests/savepoint_test.cpp coresql)
  coresql_test(coresql_relational_test relational_extensions tests/relational_test.cpp coresql)
  coresql_test(coresql_graph_test graph_extension tests/graph_test.cpp coresql_graph)
  coresql_test(coresql_outer_test outer_extension_contract tests/outer_test.cpp coresql_sql coresql_timestamp coresql_vector)
  coresql_test(coresql_sql_test sql_frontend tests/sql_test.cpp coresql_sql coresql_graph)
  # Private diagnostics and fault injection remain absent from production builds.
  foreach(target
    coresql_storage_test
    coresql_query_test
    coresql_document_test
    coresql_primary_test
    coresql_filter_test
    coresql_join_test
    coresql_extension_index_test
    coresql_json_test
    coresql_sql_test
    coresql_sql_relations_test
  )
    target_compile_definitions(${target} PRIVATE CORESQL_TESTING)
  endforeach()

  add_executable(coresql_nullable_test tests/nullable_test.cpp)
  target_link_libraries(coresql_nullable_test PRIVATE coresql_sql coresql_timestamp)
  add_test(NAME nullable_storage COMMAND coresql_nullable_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/before-nullable.corelog)
  add_test(NAME custom_type_example COMMAND coresql_custom_type)
  add_test(NAME example COMMAND coresql_demo)
  add_test(NAME collection_example COMMAND coresql_collections)
  add_test(NAME json_example COMMAND coresql_json_demo)
  add_test(NAME graph_example COMMAND coresql_graph_demo)
  add_test(NAME sql_cli COMMAND coresql_sql_cli ${CMAKE_CURRENT_SOURCE_DIR}/examples/sql.sql)
  add_test(NAME document_cli COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:coresql_documents> -DWORK=${CMAKE_CURRENT_BINARY_DIR}/document-cli
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/documents_cli.cmake)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME generated_ids_against_sqlite COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/generated_ids_differential.py $<TARGET_FILE:coresql_sql_cli>)
    add_test(NAME reference_setup COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_setup_test.py)
    add_executable(coresql_slt_bridge tests/sqllogictest/bridge.cpp)
    target_link_libraries(coresql_slt_bridge PRIVATE coresql_sql)
    target_compile_definitions(coresql_slt_bridge PRIVATE CORESQL_TESTING)
    add_executable(coresql_allocation_bridge tests/sqllogictest/bridge.cpp benchmarks/allocation_profile.cpp)
    target_link_libraries(coresql_allocation_bridge PRIVATE coresql_sql)
    target_compile_definitions(coresql_allocation_bridge PRIVATE CORESQL_TESTING CORESQL_ALLOCATION_PROFILE)

    foreach(slt_file select1 select2 select3 select4 select5)
      add_test(NAME sqllogictest_${slt_file} COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/sqllogictest/run.py
        --bridge $<TARGET_FILE:coresql_slt_bridge>
        --file upstream/${slt_file}.test
        --baseline ${CMAKE_CURRENT_SOURCE_DIR}/tests/sqllogictest/baseline.json)
      # Full corpus checks are substantially slower in unoptimized sanitizer builds.
      if(CORESQL_SANITIZERS)
        set_tests_properties(sqllogictest_${slt_file} PROPERTIES TIMEOUT 900)
      else()
        set_tests_properties(sqllogictest_${slt_file} PROPERTIES TIMEOUT 300)
      endif()
    endforeach()
    add_test(NAME sql_outer_coercion_differential COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/sqllogictest/test_outer_coercion.py $<TARGET_FILE:coresql_slt_bridge>)
    add_test(NAME sql_derived_differential COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/sqllogictest/test_relations.py $<TARGET_FILE:coresql_slt_bridge>)
    add_test(NAME sql_decimal_differential COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/sqllogictest/test_decimal.py $<TARGET_FILE:coresql_slt_bridge>)
    add_test(NAME sql_relational_differential COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/sqllogictest/test_relational.py $<TARGET_FILE:coresql_slt_bridge>)
    add_test(NAME sqllogictest_runner COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/sqllogictest/test_runner.py $<TARGET_FILE:coresql_slt_bridge>)
  endif()
endif()

# SQLite is an optional test/benchmark oracle and never links into the core.
if(CORESQL_BENCHMARKS)
  add_executable(coresql_paging_profile benchmarks/paging_profile.cpp)
  target_link_libraries(coresql_paging_profile PRIVATE coresql)
  add_executable(coresql_landmark_latency benchmarks/landmark_latency.cpp)
  target_link_libraries(coresql_landmark_latency PRIVATE coresql_landmark_worker)
  add_executable(coresql_prepared_profile benchmarks/prepared_profile.cpp)
  target_link_libraries(coresql_prepared_profile PRIVATE coresql_sql)
  add_executable(coresql_landmark_soak benchmarks/landmark_soak.cpp)
  target_link_libraries(coresql_landmark_soak PRIVATE coresql_landmark_worker)
  add_executable(coresql_operational_profile benchmarks/operational_profile.cpp)
  target_link_libraries(coresql_operational_profile PRIVATE coresql_sql)
  set(CORESQL_SQLITE_SOURCE "" CACHE PATH "Pinned SQLite source directory produced by benchmarks/reference/prepare.py")
  if(NOT EXISTS "${CORESQL_SQLITE_SOURCE}/test/speedtest1.c")
    message(FATAL_ERROR "Set CORESQL_SQLITE_SOURCE to the source directory from benchmarks/reference/prepare.py; see benchmarks/reference/README.md")
  endif()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/reference/sqlite.json" reference_json)
  string(JSON expected_harness GET "${reference_json}" speedtest1_sha256)
  file(SHA256 "${CORESQL_SQLITE_SOURCE}/test/speedtest1.c" harness_hash)
  if(NOT harness_hash STREQUAL expected_harness)
    message(FATAL_ERROR "SQLite speedtest1.c does not match the pinned benchmark harness")
  endif()
  set(CORESQL_SQLITE_AMALGAMATION "" CACHE FILEPATH "Optional pinned sqlite3.c reference")
  if(CORESQL_SQLITE_AMALGAMATION)
    enable_language(C)
    add_library(coresql_sqlite_reference STATIC "${CORESQL_SQLITE_AMALGAMATION}")
    get_filename_component(SQLITE_REFERENCE_DIR "${CORESQL_SQLITE_AMALGAMATION}" DIRECTORY)
    target_include_directories(coresql_sqlite_reference PUBLIC "${SQLITE_REFERENCE_DIR}")
    add_library(SQLite::SQLite3 ALIAS coresql_sqlite_reference)
  else()
    find_package(SQLite3 REQUIRED)
  endif()
  add_executable(coresql_compare benchmarks/compare.cpp)
  target_link_libraries(coresql_compare PRIVATE coresql SQLite::SQLite3)
  add_executable(coresql_storage_compare benchmarks/storage_compare.cpp)
  target_link_libraries(coresql_storage_compare PRIVATE coresql SQLite::SQLite3)
  add_executable(coresql_application_benchmark benchmarks/application/main.cpp)
  target_link_libraries(coresql_application_benchmark PRIVATE coresql_sql SQLite::SQLite3)
  if(BUILD_TESTING)
    foreach(engine coresql sqlite)
      add_test(NAME application_${engine}_memory COMMAND coresql_application_benchmark
        ${engine} memory 1000 32 3 0 "${CMAKE_CURRENT_BINARY_DIR}" all)
      add_test(NAME application_${engine}_durable COMMAND coresql_application_benchmark
        ${engine} durable 1000 32 50 1 "${CMAKE_CURRENT_BINARY_DIR}" all)
    endforeach()
  endif()
  add_executable(coresql_query_profile benchmarks/query_profile.cpp)
  target_link_libraries(coresql_query_profile PRIVATE coresql_vector)
  add_executable(coresql_primary_compare benchmarks/primary_compare.cpp)
  target_link_libraries(coresql_primary_compare PRIVATE coresql_timestamp SQLite::SQLite3)
  add_executable(coresql_join_compare benchmarks/join_compare.cpp)
  target_link_libraries(coresql_join_compare PRIVATE coresql SQLite::SQLite3)
  add_executable(coresql_json_compare benchmarks/json_compare.cpp)
  target_link_libraries(coresql_json_compare PRIVATE coresql_json SQLite::SQLite3)
  add_executable(coresql_update_profile benchmarks/update_profile.cpp)
  target_link_libraries(coresql_update_profile PRIVATE coresql)
  add_executable(coresql_sort_profile benchmarks/sort_profile.cpp)
  target_link_libraries(coresql_sort_profile PRIVATE coresql)

  enable_language(C)
  # Compile the upstream harness unchanged. A second object exposes its input
  # generators to the adapted comparison without copying their implementation.
  add_executable(sqlite_speedtest1 "${CORESQL_SQLITE_SOURCE}/test/speedtest1.c")
  target_link_libraries(sqlite_speedtest1 PRIVATE SQLite::SQLite3)
  add_library(speedtest1_inputs OBJECT "${CORESQL_SQLITE_SOURCE}/test/speedtest1.c")
  target_compile_definitions(speedtest1_inputs PRIVATE main=sqlite_speedtest1_unused_main)
  target_link_libraries(speedtest1_inputs PRIVATE SQLite::SQLite3)
  add_executable(coresql_speedtest1_compare benchmarks/speedtest1_compare.cpp $<TARGET_OBJECTS:speedtest1_inputs>)
  target_link_libraries(coresql_speedtest1_compare PRIVATE coresql SQLite::SQLite3)
  target_compile_options(coresql_speedtest1_compare PRIVATE -Wall -Wextra -Wpedantic)
  add_executable(coresql_speedtest1_main benchmarks/speedtest1_main.cpp $<TARGET_OBJECTS:speedtest1_inputs>)
  target_link_libraries(coresql_speedtest1_main PRIVATE coresql_sql SQLite::SQLite3)

  if(BUILD_TESTING)
    coresql_test(coresql_filter_sqlite_test filters_against_sqlite tests/filter_sqlite_test.cpp coresql SQLite::SQLite3)
    coresql_test(coresql_join_sqlite_test joins_against_sqlite tests/join_sqlite_test.cpp coresql SQLite::SQLite3)
    coresql_test(coresql_json_sqlite_test json_against_sqlite tests/json_sqlite_test.cpp coresql_json SQLite::SQLite3)
    coresql_test(coresql_sql_differential_test sql_against_sqlite tests/sql_differential_test.cpp coresql_sql SQLite::SQLite3)
    add_test(NAME speedtest1_subset_against_sqlite COMMAND coresql_speedtest1_compare 1 1)
    add_test(NAME speedtest1_main_against_sqlite COMMAND coresql_speedtest1_main 1)
    add_test(NAME speedtest1_sql_frontend COMMAND coresql_speedtest1_main 1 sql 2)
  endif()
endif()
