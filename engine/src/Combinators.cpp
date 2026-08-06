// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Combinators.cpp
 *
 * All combinator implementations are in the header (Combinators.hpp) because
 * they are function templates parameterised on the event value type T.
 * C++ requires template definitions to be visible at instantiation sites, so
 * they cannot be split into a separate .cpp without explicit instantiation.
 *
 * This file exists to satisfy the CMakeLists.txt STATIC library source list.
 * It intentionally contains no compiled code.
 */

#include "hathor/Combinators.hpp"
