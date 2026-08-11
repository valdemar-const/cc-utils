// executor.cpp — single translation unit that pulls in exactly one platform
// implementation of cc::basic::basic_executor<Tag>. The declarations (tags,
// result/error types, specialization method decls, the native `executor` alias)
// live in executor.hpp; each platform's body lives in its own .txx fragment so
// the two never share a file. This file only routes.

#include "executor.hpp"

#if defined(_WIN32)
#  include "executor_win32.txx"
#elif defined(__unix__) || defined(__APPLE__)
#  include "executor_posix.txx"
#else
#  error "cc::basic::executor: unsupported platform"
#endif
