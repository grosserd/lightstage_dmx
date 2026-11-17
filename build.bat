@echo off
rem Grosserd LightStage build (MSVC x64)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
set IMGUI=third_party\imgui
set GLEW=third_party\glew
set GLFW=third_party\glfw
set CF=/nologo /c /O2 /Zi /FS /Fdobj\\ls.pdb /MT /EHsc /utf-8 /std:c++17 /W3 /wd4996 /wd4244 /wd4267 /wd4305 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DGLEW_STATIC /D_CRT_SECURE_NO_WARNINGS /I"%IMGUI%" /I"%GLEW%\include" /I"%GLFW%\include"
if not exist obj mkdir obj
if not exist obj\tp mkdir obj\tp
if not exist obj\tp\imgui.obj cl %CF% "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\backends\imgui_impl_glfw.cpp" "%IMGUI%\backends\imgui_impl_opengl3.cpp" /Foobj\tp\ || goto :err
rem GLFW 3.4 (window, input, OpenGL context), compiled once
if not exist obj\tp\win32_window.obj cl /nologo /c /O2 /MT /D_GLFW_WIN32 /D_CRT_SECURE_NO_WARNINGS /I"%GLFW%\include" "%GLFW%\src\context.c" "%GLFW%\src\init.c" "%GLFW%\src\input.c" "%GLFW%\src\monitor.c" "%GLFW%\src\platform.c" "%GLFW%\src\vulkan.c" "%GLFW%\src\egl_context.c" "%GLFW%\src\osmesa_context.c" "%GLFW%\src\null_init.c" "%GLFW%\src\null_monitor.c" "%GLFW%\src\null_window.c" "%GLFW%\src\null_joystick.c" "%GLFW%\src\win32_init.c" "%GLFW%\src\win32_joystick.c" "%GLFW%\src\win32_module.c" "%GLFW%\src\win32_monitor.c" "%GLFW%\src\win32_time.c" "%GLFW%\src\win32_thread.c" "%GLFW%\src\wgl_context.c" "%GLFW%\src\window.c" "%GLFW%\src\win32_window.c" /Foobj\tp\ || goto :err
if not exist obj\tp\glew.obj cl /nologo /c /O2 /MT /DGLEW_STATIC /I"%GLEW%\include" "%GLEW%\src\glew.c" /Foobj\tp\glew.obj || goto :err
if not exist obj\tp\ufbx.obj cl /nologo /c /O2 /MT /D_CRT_SECURE_NO_WARNINGS third_party\ufbx\ufbx.c /Foobj\tp\ufbx.obj || goto :err
if not exist obj\tp\tp_impl.obj cl %CF% src\tp_impl.cpp /Foobj\tp\tp_impl.obj || goto :err
rem Ableton Link (header-only SDK + asio), own flags; compiled once (delete obj\tp\link_impl.obj to rebuild)
if not exist obj\tp\link_impl.obj cl /nologo /c /O2 /MT /EHsc /std:c++17 /bigobj /W0 /DNOMINMAX /DLINK_PLATFORM_WINDOWS=1 /DINCL_EXTRA_HTON_FUNCTIONS=1 /D_WIN32_WINNT=0x0601 /I third_party\link\include /I third_party\link\modules\asio-standalone\include src\link_impl.cpp /Foobj\tp\link_impl.obj || goto :err
cl %CF% /MP src\common.cpp src\i18n.cpp src\json.cpp src\xmlzip.cpp src\fixture.cpp src\fixture_editor.cpp src\fixture_editor_ui.cpp src\show.cpp src\sim.cpp src\dmxio.cpp src\midi.cpp src\osc.cpp src\bridge.cpp src\miditargets.cpp src\video.cpp src\effects.cpp src\timeline.cpp src\lbshow.cpp src\audio.cpp src\params.cpp src\mesh.cpp src\render.cpp src\app.cpp src\ui_tabs.cpp src\ui_effects.cpp src\ui_timeline.cpp src\demo.cpp src\resolume.cpp src\ui_resolume.cpp src\selftest.cpp src\uitest.cpp src\main.cpp /Foobj\ || goto :err
rc /nologo /fo obj\app.res src\app.rc || goto :err
link /nologo /DEBUG /OPT:REF /OPT:ICF /OUT:LightStage.exe obj\*.obj obj\tp\*.obj obj\app.res opengl32.lib ws2_32.lib winmm.lib setupapi.lib comdlg32.lib user32.lib gdi32.lib shell32.lib advapi32.lib dwmapi.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib avrt.lib powrprof.lib iphlpapi.lib windowscodecs.lib /SUBSYSTEM:WINDOWS || goto :err
echo BUILD OK
exit /b 0
:err
echo BUILD FAILED
exit /b 1
