LightStage 2.1 - source code
by Grosserd - GNU GPL v3 or later (see LICENSE.txt and COPYING.txt)
=====================================================================

Contents
  src\            LightStage source (C++17, Dear ImGui, OpenGL 4.3)
  assets\         fonts and program icon
  library\        fixture library (Open Fixture Library data + generic fixtures)
  third_party\    Dear ImGui 1.91.9, GLEW 2.1.0, ufbx, cgltf, stb,
                  Ableton Link (with standalone asio)
  tools\i18n\     helper scripts for the translation table (Python)
  build.bat       build script

Build (Windows 10/11, 64-bit)
  1. Install Visual Studio 2022 or the "Build Tools for Visual Studio 2022"
     with the workload "Desktop development with C++".
  2. Run build.bat (double click or from a command prompt).
  3. Result: LightStage.exe in this folder. It needs the folders library
     and assets next to it.

Self test
  LightStage.exe --selftest   runs the built-in tests (writes selftest.log
  and some test images into the program folder). A few tests use optional
  sample files and only print a note when they are missing.

Languages: the German interface texts in the code are the keys; the English
translations live in src\i18n_table.inc (LS_TR( "de", "en" ) lines; since 2.0
edited directly - tools\i18n holds the scripts that generated the first table). Wrap literal UI texts with L("..."), runtime
texts with Ls(text). Do not call L() in global or static initializers.
Code comments are English.
