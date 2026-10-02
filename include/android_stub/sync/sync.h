/* SPDX-License-Identifier: MIT */
/*
 * Upstream Mesa stores this as a symlink to ../android/sync.h.  Git for
 * Windows (core.symlinks=false) checked it out as a plain text file holding
 * the literal target path, which breaks the build.  Use an include instead so
 * the tree works regardless of symlink support.
 */
#include "../android/sync.h"
