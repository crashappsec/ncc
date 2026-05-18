#pragma once

/**
 * @file xform_rpc.h
 * @brief Transform: `@rpc("svc/method")` function annotation.
 *
 * Recognizes `@rpc(...)` on a function declaration or definition and
 * (in later phases) synthesizes a CBOR dispatcher, a constructor
 * that registers the dispatcher with the runtime, and a typed
 * client stub.
 *
 * Must run FIRST in the xform registration sequence — before
 * generic_struct, typeid, option, kargs_vargs — because the
 * synthesized bodies reference type-mangled identifiers those
 * passes produce.
 */

#include "xform/transform.h"

extern void ncc_register_rpc_xform(ncc_xform_registry_t *reg);
