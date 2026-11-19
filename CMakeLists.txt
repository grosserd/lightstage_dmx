# LightStage - CMake build (Windows: Visual Studio 2022 / Build Tools; macOS: in preparation)
#
#   cmake -S . -B build
#   cmake --build build --config Release
#
# Result: build/LightStage/LightStage(.exe) with the folders assets and library next to it.
# build.bat (MSVC, Windows) stays as the quick build used by the self test scripts.

cmake_minimum_required( VERSION 3.26 )
project( LightStage VERSION 2.1 LANGUAGES C CXX )
if( APPLE )
	#GLFW's Cocoa sources (.m) need Objective-C, the LightStage macOS backends (.mm) Objective-C++
	enable_language( OBJC )
	enable_language( OBJCXX )
endif()
if( APPLE )
	enable_language( OBJCXX )#media backends (video_mac.mm, audio_mac.mm, image_mac.mm)
endif()

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
if( NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES )
	set( CMAKE_BUILD_TYPE Release )
endif()
if( MSVC )
	set( CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" )#static runtime like build.bat (/MT)
endif()

set( TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party )
set( OUT_DIR ${CMAKE_BINARY_DIR}/LightStage )

#---- GLFW 3.4 (window, input, OpenGL context)
set( GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE )
set( GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE )
set( GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE )
set( GLFW_INSTALL OFF CACHE BOOL "" FORCE )
add_subdirectory( ${TP}/glfw ${CMAKE_BINARY_DIR}/glfw EXCLUDE_FROM_ALL )

#---- OpenGL + GLEW (static)
find_package( OpenGL REQUIRED )
add_library( ls_glew STATIC ${TP}/glew/src/glew.c )
target_include_directories( ls_glew PUBLIC ${TP}/glew/include )
target_compile_definitions( ls_glew PUBLIC GLEW_STATIC )
target_link_libraries( ls_glew PUBLIC OpenGL::GL )

#---- Dear ImGui 1.91.9 (GLFW + OpenGL 3 backends)
add_library( ls_imgui STATIC
	${TP}/imgui/imgui.cpp ${TP}/imgui/imgui_draw.cpp ${TP}/imgui/imgui_tables.cpp ${TP}/imgui/imgui_widgets.cpp
	${TP}/imgui/backends/imgui_impl_glfw.cpp ${TP}/imgui/backends/imgui_impl_opengl3.cpp )
target_include_directories( ls_imgui PUBLIC ${TP}/imgui )
target_link_libraries( ls_imgui PUBLIC glfw ls_glew )

#---- ufbx (FBX import)
add_library( ls_ufbx STATIC ${TP}/ufbx/ufbx.c )
target_include_directories( ls_ufbx PUBLIC ${TP}/ufbx )

#---- Ableton Link (header-only SDK + standalone asio), own flags
add_library( ls_link STATIC src/link_impl.cpp )
target_include_directories( ls_link PRIVATE ${TP}/link/include ${TP}/link/modules/asio-standalone/include )
if( WIN32 )
	target_compile_definitions( ls_link PRIVATE LINK_PLATFORM_WINDOWS=1 INCL_EXTRA_HTON_FUNCTIONS=1 _WIN32_WINNT=0x0601 NOMINMAX )
	target_compile_options( ls_link PRIVATE /bigobj /W0 )
	target_link_libraries( ls_link PUBLIC ws2_32 iphlpapi winmm )
elseif( APPLE )
	target_compile_definitions( ls_link PRIVATE LINK_PLATFORM_MACOSX=1 )
else()
	target_compile_definitions( ls_link PRIVATE LINK_PLATFORM_LINUX=1 )
endif()

#---- LightStage
set( LS_SOURCES
	src/common.cpp src/i18n.cpp src/json.cpp src/xmlzip.cpp src/fixture.cpp src/fixture_editor.cpp src/fixture_editor_ui.cpp
	src/show.cpp src/sim.cpp src/dmxio.cpp src/midi.cpp src/osc.cpp src/bridge.cpp src/netsock.cpp src/miditargets.cpp src/video.cpp
	src/effects.cpp src/timeline.cpp src/lbshow.cpp src/audio.cpp src/params.cpp src/mesh.cpp src/render.cpp src/app.cpp
	src/ui_tabs.cpp src/ui_effects.cpp src/ui_timeline.cpp src/demo.cpp src/resolume.cpp src/ui_resolume.cpp
	src/selftest.cpp src/uitest.cpp src/main.cpp src/tp_impl.cpp )
#platform layer (dialogs, window, priorities): Win32 / Cocoa
if( WIN32 )
	list( APPEND LS_SOURCES src/platform_win.cpp )
elseif( APPLE )
	enable_language( OBJCXX )
	list( APPEND LS_SOURCES src/platform_mac.mm )
	set_source_files_properties( src/platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc" )
endif()
#serial port for USB-DMX (Enttec DMX USB Pro): Win32 COM port / termios
if( WIN32 )
	list( APPEND LS_SOURCES src/serial_win.cpp )
else()
	list( APPEND LS_SOURCES src/serial_posix.cpp )
endif()
#media backends: Windows = Media Foundation / waveOut / waveIn / WIC, macOS = AVFoundation / AudioQueue / ImageIO
if( WIN32 )
	list( APPEND LS_SOURCES src/video_win.cpp src/audio_win.cpp )
elseif( APPLE )
	set( LS_MAC_MEDIA src/video_mac.mm src/audio_mac.mm src/image_mac.mm )
	set_source_files_properties( ${LS_MAC_MEDIA} PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wno-deprecated-declarations" )
	list( APPEND LS_SOURCES ${LS_MAC_MEDIA} )
endif()
if( WIN32 )
	add_executable( LightStage WIN32 ${LS_SOURCES} src/app.rc )
else()
	add_executable( LightStage ${LS_SOURCES} )
endif()
#MIDI port backend (midi_port.h): winmm / CoreMIDI; other platforms use the empty fallback in midi.cpp
if( WIN32 )
	target_sources( LightStage PRIVATE src/midi_win.cpp )
elseif( APPLE )
	target_sources( LightStage PRIVATE src/midi_mac.cpp )
	target_link_libraries( LightStage PRIVATE "-framework CoreMIDI" "-framework CoreFoundation" )
endif()
target_include_directories( LightStage PRIVATE src ${TP} ${CMAKE_CURRENT_SOURCE_DIR} )#root: app.rc finds assets\lightstage.ico
target_link_libraries( LightStage PRIVATE ls_imgui ls_glew glfw ls_ufbx ls_link )
set_target_properties( LightStage PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR} )
foreach( cfg Debug Release RelWithDebInfo MinSizeRel )
	string( TOUPPER ${cfg} CFG )
	set_target_properties( LightStage PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${CFG} ${OUT_DIR} )
endforeach()

if( APPLE )
	target_link_libraries( LightStage PRIVATE "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework AudioToolbox"
		"-framework CoreAudio" "-framework ImageIO" "-framework CoreGraphics" "-framework Foundation" )
endif()
if( MSVC )
	target_compile_definitions( LightStage PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS )
	target_compile_options( LightStage PRIVATE /utf-8 /W3 /wd4996 /wd4244 /wd4267 /wd4305 /MP /EHsc )
	target_link_libraries( LightStage PRIVATE ws2_32 winmm setupapi comdlg32 user32 gdi32 shell32 advapi32 dwmapi
		mfplat mfreadwrite mfuuid ole32 avrt powrprof iphlpapi windowscodecs dbghelp )
	target_link_options( LightStage PRIVATE /OPT:REF /OPT:ICF )
endif()
if( APPLE )
	target_link_libraries( LightStage PRIVATE "-framework Cocoa" "-framework UniformTypeIdentifiers" )
	target_link_libraries( LightStage PRIVATE "-framework IOKit" "-framework CoreFoundation" )#serial port list (serial_posix.cpp)
endif()

#program data next to the program (ExeDir): fonts / icon and the fixture library
add_custom_command( TARGET LightStage POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/assets ${OUT_DIR}/assets
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_CURRENT_SOURCE_DIR}/library ${OUT_DIR}/library
	VERBATIM )
