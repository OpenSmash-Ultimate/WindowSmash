@echo off
rem M5: builds and runs the netcode test (hle_rb_net.c alone, no game) with input delays 0, 2 and 4.
setlocal
set ROOT=%~dp0..\..\..
set OUT=%ROOT%\build\rb_net_test.exe
"%ROOT%\deps\llvm\bin\clang-cl.exe" /nologo /O2 /W3 /winsysroot "%ROOT%\deps\msvc" -fuse-ld=lld "%~dp0rb_net_test.c" "%~dp0..\src\hle_rb_net.c" /Fe"%OUT%" /Fo"%ROOT%\build\\" /link /winsysroot:"%ROOT%\deps\msvc" || exit /b 1
for %%d in (0 2 4) do "%OUT%" %%d || exit /b 1
rem (M6: a peer that runs slower for a while: the frame advantage holds the other back)
"%OUT%" 2 slow || exit /b 1
rem (M7: 3 and 4 players, everything through the host; delays 0, 2, 4; one slow; and 3 players sabotaged)
for %%n in (3 4) do for %%d in (0 2 4) do "%OUT%" %%d - %%n || exit /b 1
"%OUT%" 2 slow 3 || exit /b 1
"%OUT%" 2 norollback 3 >nul && (echo FAIL: the sabotaged 3-player run passed & exit /b 1)
rem (the test can fail: with rollbacks off it must)
"%OUT%" 2 norollback >nul && (echo FAIL: the sabotaged run passed & exit /b 1)
echo sabotaged run failed as it should
rem (M6: the link alone, on loopback: found by the other game, following it to another port, strangers dropped)
set LOUT=%ROOT%\build\rb_link_test.exe
"%ROOT%\deps\llvm\bin\clang-cl.exe" /nologo /O2 /W3 /winsysroot "%ROOT%\deps\msvc" -fuse-ld=lld "%~dp0rb_link_test.c" "%~dp0..\src\hle_rb_link.c" /Fe"%LOUT%" /Fo"%ROOT%\build\\" /link /winsysroot:"%ROOT%\deps\msvc" ws2_32.lib ole32.lib oleaut32.lib || exit /b 1
"%LOUT%" || exit /b 1
