# Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# Find libgflags
#
#  LIBGFLAGS_INCLUDE_DIR - where to find gflags/gflags.h, etc.
#  LIBGFLAGS_LIBRARY     - List of libraries when using libgflags.
#  LIBGFLAGS_FOUND       - True if libgflags found.


IF (LIBGFLAGS_INCLUDE_DIR)
  # Already in cache, be silent
  SET(LIBGFLAGS_FIND_QUIETLY TRUE)
ENDIF ()

FIND_PATH(LIBGFLAGS_INCLUDE_DIR gflags/gflags.h)

FIND_LIBRARY(LIBGFLAGS_LIBRARY NAMES gflags gflags_static)

# handle the QUIETLY and REQUIRED arguments and set LIBGFLAGS_FOUND to TRUE if
# all listed variables are TRUE
INCLUDE(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBGFLAGS DEFAULT_MSG LIBGFLAGS_LIBRARY LIBGFLAGS_INCLUDE_DIR)

MARK_AS_ADVANCED(LIBGFLAGS_LIBRARY LIBGFLAGS_INCLUDE_DIR)
