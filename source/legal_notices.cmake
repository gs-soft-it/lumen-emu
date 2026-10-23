function(lumen_attach_legal_notices target_name)
	if(NOT TARGET "${target_name}")
		message(FATAL_ERROR "Cannot attach legal notices to missing target: ${target_name}")
	endif()

	# A bundle source can have only one MACOSX_PACKAGE_LOCATION. Generate a
	# copy from the canonical notice to preserve its existing root location
	# while also providing the project notice directory used by every target.
	set(LUMEN_PROJECT_LEGAL_THIRD_PARTY
		"${PROJECT_BINARY_DIR}/lumen-legal/THIRD_PARTY_NOTICES.txt")
	configure_file("${PROJECT_SOURCE_DIR}/licenses/THIRD_PARTY_NOTICES.txt"
		"${LUMEN_PROJECT_LEGAL_THIRD_PARTY}" COPYONLY)

	set(LUMEN_LEGAL_RESOURCES
		"root|${PROJECT_SOURCE_DIR}/licenses/THIRD_PARTY_NOTICES.txt"
		"lumen|${PROJECT_SOURCE_DIR}/../LICENSE"
		"lumen|${PROJECT_SOURCE_DIR}/../NOTICE"
		"lumen|${PROJECT_SOURCE_DIR}/../LICENSES.md"
		"lumen|${LUMEN_PROJECT_LEGAL_THIRD_PARTY}"
		"kyty|${PROJECT_SOURCE_DIR}/gen4/gen5/compatibility/LICENSES/Kyty-MIT.txt"
		"cpuinfo|${PROJECT_SOURCE_DIR}/3rdparty/cpuinfo/LICENSE"
		"clog|${PROJECT_SOURCE_DIR}/3rdparty/cpuinfo/deps/clog/LICENSE"
		"easy-profiler|${PROJECT_SOURCE_DIR}/3rdparty/easy_profiler/LICENSE.MIT"
		"googletest|${PROJECT_SOURCE_DIR}/3rdparty/gtest/LICENSE"
		"libatrac9|${PROJECT_SOURCE_DIR}/3rdparty/libatrac9/LICENSE"
		"lua|${PROJECT_SOURCE_DIR}/3rdparty/lua/license"
		"lzma|${PROJECT_SOURCE_DIR}/3rdparty/lzma/LICENSE.txt"
		"magic-enum|${PROJECT_SOURCE_DIR}/3rdparty/magic_enum/LICENSE"
		"miniz|${PROJECT_SOURCE_DIR}/3rdparty/miniz/LICENSE"
		"rijndael|${PROJECT_SOURCE_DIR}/3rdparty/rijndael/LICENSE.txt"
		"sdl2|${PROJECT_SOURCE_DIR}/3rdparty/sdl2/LICENSE.txt"
		"sdl2-hidapi|${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/src/hidapi/LICENSE.txt"
		"sdl2-hidapi|${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/src/hidapi/LICENSE-bsd.txt"
		"sdl2-yuv2rgb|${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/src/video/yuv2rgb/LICENSE"
		"wxsqlite3|${PROJECT_SOURCE_DIR}/3rdparty/sqlite/LICENCE.txt"
		"sqlite|${PROJECT_SOURCE_DIR}/3rdparty/sqlite/sqlite-license.txt"
		"stb|${PROJECT_SOURCE_DIR}/3rdparty/stb/LICENSE"
		"vulkan|${PROJECT_SOURCE_DIR}/3rdparty/vulkan/LICENSE"
		"vulkan|${PROJECT_SOURCE_DIR}/3rdparty/vulkan/LICENSE.txt"
		"spirv-cross|${PROJECT_SOURCE_DIR}/3rdparty/vulkan/SPIRV-Cross-MIT.txt"
		"spirv-cross|${PROJECT_SOURCE_DIR}/3rdparty/vulkan/SPIRV-Cross-Khronos-Headers.txt"
		"spirv-cross|${PROJECT_SOURCE_DIR}/3rdparty/vulkan/SPIRV-Cross-selection.json"
		"winpthreads|${PROJECT_SOURCE_DIR}/3rdparty/winpthread/COPYING"
		"xxhash|${PROJECT_SOURCE_DIR}/3rdparty/xxhash/LICENSE"
		"zstd|${PROJECT_SOURCE_DIR}/3rdparty/zstd/LICENSE"
	)

	if(MACOS AND NOT IOS AND LUMEN_MACOS_FEXCORE_RUNTIME)
		# The audited Mac closure uses the same seven license texts already
		# retained for Android. Reuse those files without importing the
		# Android-specific laboratory notice or qualifying distribution.
		# rpmalloc has a normalized final newline; SoftFloat is the audited
		# notice transcription, so these are resource hashes, not source hashes.
		set(LUMEN_FEX_LEGAL_ROOT
			"${PROJECT_SOURCE_DIR}/../android/app/src/fexLab/assets/Legal")
		set(LUMEN_FEX_LEGAL_RESOURCES
			"FEX|f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee"
			"fmt|a65fcc5a1095fc33cc37e4fff69366e49d7e92f613558a2b95ed1b70e33758b4"
			"xxhash|6ffedbc0f7878612d2b23589f1ff2ab15633e1df7963a5d9fc750ec5500c7e7a"
			"unordered_dense|4f5a62734899ec0ba85d3e8264a7ed1fcb4bcb8f0b4359c0afd627b6314ae599"
			"rpmalloc|2b886bfac9bf7b24d5eaca6437abccfd1d31068ed6d1c06abda74ebf1e55d51a"
			"SoftFloat-3e|ddcb981bd77da11fc970b30d319748a288767f8b7511bd0365696d3fc1e2085a"
			"cephes|edf1440b625e6d434a986cdb535c702001afab8959d94ce6275441a58cfd0ebb"
		)
		foreach(LUMEN_FEX_LEGAL_RESOURCE IN LISTS LUMEN_FEX_LEGAL_RESOURCES)
			string(REPLACE "|" ";" LUMEN_FEX_LEGAL_FIELDS "${LUMEN_FEX_LEGAL_RESOURCE}")
			list(GET LUMEN_FEX_LEGAL_FIELDS 0 LUMEN_FEX_LEGAL_COMPONENT)
			list(GET LUMEN_FEX_LEGAL_FIELDS 1 LUMEN_FEX_LEGAL_EXPECTED_SHA256)
			set(LUMEN_FEX_LEGAL_FILE "${LUMEN_FEX_LEGAL_ROOT}/${LUMEN_FEX_LEGAL_COMPONENT}/LICENSE")
			if(NOT EXISTS "${LUMEN_FEX_LEGAL_FILE}")
				message(FATAL_ERROR "Required FEX legal notice is missing: ${LUMEN_FEX_LEGAL_FILE}")
			endif()
			file(SHA256 "${LUMEN_FEX_LEGAL_FILE}" LUMEN_FEX_LEGAL_SHA256)
			if(NOT LUMEN_FEX_LEGAL_SHA256 STREQUAL LUMEN_FEX_LEGAL_EXPECTED_SHA256)
				message(FATAL_ERROR "FEX legal notice hash mismatch: ${LUMEN_FEX_LEGAL_FILE}")
			endif()
			set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LUMEN_FEX_LEGAL_FILE}")
			# Keep the FEX xxHash notice separate from Lumen's own xxHash pin.
			list(APPEND LUMEN_LEGAL_RESOURCES
				"fexcore/${LUMEN_FEX_LEGAL_COMPONENT}|${LUMEN_FEX_LEGAL_FILE}")
		endforeach()
	endif()

	if(MACOS AND NOT IOS AND DEFINED LUMEN_QEMU_TCG_LIBRARY AND
	   NOT LUMEN_QEMU_TCG_LIBRARY STREQUAL "")
		find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
		set(LUMEN_QEMU_LEGAL_STAGER "${PROJECT_SOURCE_DIR}/cpu/qemu/tools/StageMacLegal.py")
		set(LUMEN_QEMU_LEGAL_OUTPUT "${PROJECT_BINARY_DIR}/lumen-legal/qemu-tcg")
		set(LUMEN_QEMU_LEGAL_CMAKE "${PROJECT_BINARY_DIR}/lumen-legal/qemu-tcg-resources.cmake")
		execute_process(COMMAND "${Python3_EXECUTABLE}" "${LUMEN_QEMU_LEGAL_STAGER}"
			--library "${LUMEN_QEMU_TCG_LIBRARY}"
			--deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
			--output "${LUMEN_QEMU_LEGAL_OUTPUT}" --cmake-output "${LUMEN_QEMU_LEGAL_CMAKE}" --refresh
			RESULT_VARIABLE LUMEN_QEMU_LEGAL_RESULT
			OUTPUT_VARIABLE LUMEN_QEMU_LEGAL_RECEIPT ERROR_VARIABLE LUMEN_QEMU_LEGAL_ERROR)
		if(NOT LUMEN_QEMU_LEGAL_RESULT EQUAL 0)
			message(FATAL_ERROR "QEMU legal staging failed: ${LUMEN_QEMU_LEGAL_RECEIPT}${LUMEN_QEMU_LEGAL_ERROR}")
		endif()
		include("${LUMEN_QEMU_LEGAL_CMAKE}")
		list(APPEND LUMEN_LEGAL_RESOURCES ${LUMEN_QEMU_LEGAL_RESOURCES})
		get_filename_component(LUMEN_QEMU_LIBRARY_DIR "${LUMEN_QEMU_TCG_LIBRARY}" DIRECTORY)
		file(GLOB_RECURSE LUMEN_QEMU_ORIGINAL_NOTICES CONFIGURE_DEPENDS
			"${LUMEN_QEMU_LIBRARY_DIR}/licenses/*")
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			"${LUMEN_QEMU_TCG_LIBRARY}.json" "${LUMEN_QEMU_TCG_LIBRARY}.macos.json"
			"${LUMEN_QEMU_LEGAL_STAGER}" ${LUMEN_QEMU_ORIGINAL_NOTICES})
	endif()

	file(GLOB_RECURSE LUMEN_GEN4_LEGAL_FILES CONFIGURE_DEPENDS
		"${PROJECT_SOURCE_DIR}/licenses/gen4/*")
	foreach(LUMEN_GEN4_LEGAL_FILE IN LISTS LUMEN_GEN4_LEGAL_FILES)
		file(RELATIVE_PATH LUMEN_GEN4_LEGAL_RELATIVE
			"${PROJECT_SOURCE_DIR}/licenses/gen4" "${LUMEN_GEN4_LEGAL_FILE}")
		get_filename_component(LUMEN_GEN4_LEGAL_PARENT "${LUMEN_GEN4_LEGAL_RELATIVE}" DIRECTORY)
		list(APPEND LUMEN_LEGAL_RESOURCES "gen4/${LUMEN_GEN4_LEGAL_PARENT}|${LUMEN_GEN4_LEGAL_FILE}")
	endforeach()

	get_target_property(LUMEN_LEGAL_IS_BUNDLE "${target_name}" MACOSX_BUNDLE)
	foreach(LUMEN_LEGAL_RESOURCE IN LISTS LUMEN_LEGAL_RESOURCES)
		string(REPLACE "|" ";" LUMEN_LEGAL_FIELDS "${LUMEN_LEGAL_RESOURCE}")
		list(GET LUMEN_LEGAL_FIELDS 0 LUMEN_LEGAL_COMPONENT)
		list(GET LUMEN_LEGAL_FIELDS 1 LUMEN_LEGAL_FILE)
		if(NOT EXISTS "${LUMEN_LEGAL_FILE}")
			message(FATAL_ERROR "Required legal notice is missing: ${LUMEN_LEGAL_FILE}")
		endif()

		if(APPLE AND LUMEN_LEGAL_IS_BUNDLE)
			if(LUMEN_LEGAL_COMPONENT STREQUAL "root")
				set(LUMEN_LEGAL_BUNDLE_LOCATION "Resources/Legal")
			else()
				set(LUMEN_LEGAL_BUNDLE_LOCATION "Resources/Legal/${LUMEN_LEGAL_COMPONENT}")
			endif()
			set_source_files_properties("${LUMEN_LEGAL_FILE}" PROPERTIES
				MACOSX_PACKAGE_LOCATION "${LUMEN_LEGAL_BUNDLE_LOCATION}"
				HEADER_FILE_ONLY TRUE
			)
			target_sources("${target_name}" PRIVATE "${LUMEN_LEGAL_FILE}")
		else()
			if(LUMEN_LEGAL_COMPONENT STREQUAL "root")
				set(LUMEN_LEGAL_INSTALL_LOCATION "Legal")
			else()
				set(LUMEN_LEGAL_INSTALL_LOCATION "Legal/${LUMEN_LEGAL_COMPONENT}")
			endif()
			# fc_script and the desktop launcher share these sidecars. Install
			# each file/destination once, including standalone command-line builds.
			set(LUMEN_LEGAL_INSTALL_KEY "${LUMEN_LEGAL_INSTALL_LOCATION}|${LUMEN_LEGAL_FILE}")
			get_property(LUMEN_LEGAL_INSTALLED GLOBAL PROPERTY LUMEN_INSTALLED_LEGAL_RESOURCES)
			if(NOT LUMEN_LEGAL_INSTALL_KEY IN_LIST LUMEN_LEGAL_INSTALLED)
				install(FILES "${LUMEN_LEGAL_FILE}" DESTINATION "${LUMEN_LEGAL_INSTALL_LOCATION}"
					COMPONENT LumenLegal)
				set_property(GLOBAL APPEND PROPERTY LUMEN_INSTALLED_LEGAL_RESOURCES
					"${LUMEN_LEGAL_INSTALL_KEY}")
			endif()
		endif()
	endforeach()
endfunction()
