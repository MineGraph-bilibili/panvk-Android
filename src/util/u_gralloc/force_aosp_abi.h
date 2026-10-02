#pragma once

// 1. Force NDK's config_site to load first so its include guard (_LIBCPP___CONFIG_SITE) gets set
#include <__config_site>

// 2. Override the namespace back to AOSP's standard __1
#undef _LIBCPP_ABI_NAMESPACE
#define _LIBCPP_ABI_NAMESPACE __1
