#pragma once

// doctest 2.4.12 includes <ciso646> under clang, which libstdc++ 16 flags with #warning (removed in C++20).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-W#warnings"
#include <doctest/doctest.h>
#pragma clang diagnostic pop
