/*
 * arena.h — compatibility shim, kept so existing internal/cbm sources that
 * include "arena.h" do not need to change.
 *
 * The real implementation lives in src/foundation/arena.{c,h} ("restructured
 * from internal/cbm/arena.h for the pure C rewrite"). The old internal header
 * shared the include guard CBM_ARENA_H with foundation/arena.h: any TU that
 * included both headers silently skipped the second one, which could leave
 * foundation-only functions (cbm_arena_calloc/reset/init_sized/total)
 * implicitly declared. Foundation is a strict superset of the old internal
 * API, so forwarding is safe.
 */
#ifndef CBM_INTERNAL_ARENA_SHIM_H
#define CBM_INTERNAL_ARENA_SHIM_H

/* Relative path: the lsp_all unity build compiles with CXXFLAGS_COMMON, which
 * does not carry -Isrc. */
#include "../../src/foundation/arena.h"

#endif /* CBM_INTERNAL_ARENA_SHIM_H */
