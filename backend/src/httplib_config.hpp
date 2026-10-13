#pragma once
// 统一 cpp-httplib 编译选项（需在所有包含 httplib.h 的地方保持一致）
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#ifndef CPPHTTPLIB_THREAD_POOL_COUNT
#define CPPHTTPLIB_THREAD_POOL_COUNT 16
#endif
#include <httplib/httplib.h>
