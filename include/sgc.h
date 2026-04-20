#pragma once

/* simple compacting garbage collector */

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

struct sgc;

/* only first 36 bits are used */
typedef uintptr_t sgc_ref;

typedef size_t (*sgc_sizeof)(struct sgc*, sgc_ref);
typedef void (*sgc_visit)(struct sgc*, sgc_ref);
typedef void (*sgc_root_visit)(struct sgc*, void*);
typedef void (*sgc_cleanup)(struct sgc*, sgc_ref);

struct sgc_type
{
  void* userdata;
  sgc_sizeof size;
  sgc_visit visit;

  /* cleanup functions are potentially called when an object is finally
   * destroyed/cleaned by the garbage collector.
   * note that these are not finalizers - objects may not be resurrected.
   * use these for cleaning up system resources that could have potentially
   * leaked due to user mismanagement. */
  sgc_cleanup cleanup;
};

enum sgc_state : uint32_t;

struct sgc
{
  void* heap;
  uintptr_t size;
  uintptr_t bump;

  void* root;
  sgc_root_visit root_visit;

  sgc_ref* gl;
  size_t gl_len;
  size_t gl_max;

  enum sgc_state state;

  jmp_buf oom_leave;
};

#define SGC_UTILIZATION(sgc) (sgc).bump

enum : uintptr_t
{
  /* a sgc instance can hold up to 64gb of memory */
  SGC_REF_MASK = (1UL << 36U) - 1UL,

  /* ~8kb of overhead */
  SGC_DEFAULT_GLMAXSIZE = 1024,

  SGC_ALIGNMENT = 8,

  SGC_NULLREF = SGC_REF_MASK,
};

/* gl_maxsize can be set to -1U for default */
struct sgc
sgc_init(size_t heap_size,
         size_t gl_maxsize,
         void* root,
         sgc_root_visit root_visit);

void
sgc_uninit(struct sgc*);

/* attempts to allocate space for a type.
 * returns NULLREF if there is not enough space for the allocation.
 * does not invoke sgc_collect. */
sgc_ref
sgc_alloc(struct sgc*, struct sgc_type const*);

void*
sgc_resolve(struct sgc*, sgc_ref);
struct sgc_type const*
sgc_resolve_type(struct sgc*, sgc_ref);

void
sgc_mark(struct sgc*, sgc_ref*);

/* runs the garbage collector, updating any native references.
 * returns 0 on success
 * returns 1 on out-of-memory:
 *   the GC requires some memory overhead to keep track of the
 *   set of marked objects; if the GC runs out of overhead,
 *   the system will fail to gc.
 * FIXME: maybe allow arbitrary greylist reallocations?
 */
int
sgc_collect(struct sgc*);
