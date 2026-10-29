#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <catch2/catch_amalgamated.hpp>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  int code = 0;
  {
    Catch::Session session;
    code = session.applyCommandLine(argc, argv);
    if (code == 0) code = session.run();
  }
#if defined(__EMSCRIPTEN__)
  // Asyncify resumes main outside the initial startup call, which cannot forward its returned status.
  emscripten_force_exit(code);
#endif
  return code;
}
