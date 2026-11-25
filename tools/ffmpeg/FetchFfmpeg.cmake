# Fetches the pinned static ffmpeg build that ships beside the executable in a release, verifies it and writes
# ffmpeg[.exe], ffmpeg-LICENSE.txt and ffmpeg-README.txt into OUT.
#
#   cmake -DPLATFORM=linux64|win64 [-DOUT=<dir>] [-DARCHIVE=<already downloaded archive>] -P tools/ffmpeg/FetchFfmpeg.cmake

if(CMAKE_VERSION VERSION_LESS 3.18)
	message(FATAL_ERROR "needs CMake 3.18 or newer")
endif()

set(FF_TAG     "autobuild-2026-09-30-13-08")
set(FF_VERSION "n9.0.2-17-g2a571b6068")
set(FF_COMMIT  "2a571b6068")
set(FF_BRANCH  "release/9.0")
set(FF_BASEURL "https://github.com/BtbN/FFmpeg-Builds/releases/download/${FF_TAG}")

if(PLATFORM STREQUAL "linux64")
	set(FF_FILE "ffmpeg-${FF_VERSION}-linux64-gpl-9.0.tar.xz")
	set(FF_SHA256 "68ee646831adaae2495618346f3bba94ff207ff83bbd34d643e7004730d66269")
	set(FF_EXE "ffmpeg")
elseif(PLATFORM STREQUAL "win64")
	set(FF_FILE "ffmpeg-${FF_VERSION}-win64-gpl-9.0.zip")
	set(FF_SHA256 "a0e45723c72141975f51d8666302e614711745f3102b704ca3f82c897a58d278")
	set(FF_EXE "ffmpeg.exe")
else()
	message(FATAL_ERROR "PLATFORM must be linux64 or win64")
endif()

if(NOT OUT)
	set(OUT "${CMAKE_CURRENT_LIST_DIR}/out/${PLATFORM}")
endif()
get_filename_component(OUT "${OUT}" ABSOLUTE)
set(FF_CACHE "${CMAKE_CURRENT_LIST_DIR}/out/cache")
set(FF_WORK "${OUT}/.extract")

if(ARCHIVE)
	get_filename_component(FF_ARCHIVE "${ARCHIVE}" ABSOLUTE)
else()
	set(FF_ARCHIVE "${FF_CACHE}/${FF_FILE}")
endif()

if(EXISTS "${FF_ARCHIVE}")
	file(SHA256 "${FF_ARCHIVE}" FF_HAVE)
	if(NOT FF_HAVE STREQUAL FF_SHA256)
		message(FATAL_ERROR "${FF_ARCHIVE}: sha256 ${FF_HAVE}, expected ${FF_SHA256}")
	endif()
else()
	message(STATUS "downloading ${FF_BASEURL}/${FF_FILE}")
	file(DOWNLOAD "${FF_BASEURL}/${FF_FILE}" "${FF_ARCHIVE}.part"
		EXPECTED_HASH SHA256=${FF_SHA256} TLS_VERIFY ON SHOW_PROGRESS STATUS FF_STATUS)
	list(GET FF_STATUS 0 FF_CODE)
	if(NOT FF_CODE EQUAL 0)
		file(REMOVE "${FF_ARCHIVE}.part")
		message(FATAL_ERROR "download failed: ${FF_STATUS}")
	endif()
	file(RENAME "${FF_ARCHIVE}.part" "${FF_ARCHIVE}")
endif()

file(REMOVE_RECURSE "${FF_WORK}")
file(ARCHIVE_EXTRACT INPUT "${FF_ARCHIVE}" DESTINATION "${FF_WORK}"
	PATTERNS "*/bin/${FF_EXE}" "*/LICENSE.txt")
file(GLOB FF_BIN "${FF_WORK}/*/bin/${FF_EXE}")
file(GLOB FF_LICENSE "${FF_WORK}/*/LICENSE.txt")
list(LENGTH FF_BIN FF_NBIN)
list(LENGTH FF_LICENSE FF_NLICENSE)
if(NOT FF_NBIN EQUAL 1 OR NOT FF_NLICENSE EQUAL 1)
	message(FATAL_ERROR "unexpected archive layout in ${FF_FILE}")
endif()

file(MAKE_DIRECTORY "${OUT}")
file(COPY "${FF_BIN}" DESTINATION "${OUT}"
	FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
configure_file("${FF_LICENSE}" "${OUT}/ffmpeg-LICENSE.txt" COPYONLY)
file(REMOVE_RECURSE "${FF_WORK}")
file(SHA256 "${OUT}/${FF_EXE}" FF_EXE_SHA256)

file(WRITE "${OUT}/ffmpeg-README.txt"
"${FF_EXE} is FFmpeg ${FF_VERSION}, a static build from BtbN/FFmpeg-Builds release ${FF_TAG}
(${FF_FILE}, sha256 ${FF_SHA256}; ${FF_EXE} sha256 ${FF_EXE_SHA256}).

It is a separate program that NewJK runs to encode videos; it is not linked into NewJK. This build is
licensed under the GNU General Public License version 3; the full text is in ffmpeg-LICENSE.txt.

Source code:
  FFmpeg ${FF_BRANCH} at commit ${FF_COMMIT}: https://git.ffmpeg.org/ffmpeg.git
  The build scripts, which name every bundled library and its exact version:
    https://github.com/BtbN/FFmpeg-Builds (tag ${FF_TAG})
  For three years from the release that contains this file, the NewJK project will provide the complete
  corresponding source of this build on request: https://github.com/jkanewmod/newjk/issues
")

message(STATUS "${OUT}: ${FF_EXE}, ffmpeg-LICENSE.txt, ffmpeg-README.txt")
