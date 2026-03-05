#pragma once
/**
 * @file query.h
 * @brief Unicode query stub (standalone extraction).
 *
 * The full n00b runtime provides composable codepoint query filters.
 * This stub just includes the dependent headers so that code referencing
 * query.h compiles. The actual query functions are not needed by the
 * parser.
 */

#include "ncc.h"
#include "lib/option.h"
#include "unicode/properties.h"
#include "unicode/encoding.h"
#include "unicode/identifiers.h"
