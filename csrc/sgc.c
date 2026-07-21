#define VALGRIND

#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <threads.h>

#ifdef VALGRIND
#include <valgrind/memcheck.h>
#include <valgrind/valgrind.h>
#endif

#include "sgc.h"

/* INVARIANT:
 *   type allocations must be located in the heap BEFORE
 *   any of the instance objects in memory
 */

struct cutf8_string_view
{
  char const* str;
  size_t bytesize;
};
struct twigs_type
{
  struct sgc_type gc;

  /* typetag that this primitive type is of */
  uint64_t typetag;

  /* typename used for debugging purposes */
  struct cutf8_string_view debug_typename;
};

enum sgc_state : uint32_t
{
  state_not_collecting,

  state_mark,
  state_update,
  state_slide,
};

struct __attribute((aligned(SGC_ALIGNMENT))) header
{
  sgc_ref type;
  uintptr_t mark;
};

enum color
{
  /* not marked
   * allocations are set back to this value during slideheap */
  WHITE,

  /* allocations marked during the state_mark pass
   * follows white->gl->red path */
  RED,

  /* allocations marked during the state_update pass
   * follows red->gl->blue path */
  BLUE,
};

enum : uintptr_t
{
  COLOR_OFF = 62UL,
  COLOR_MASK = ((1UL << 2UL) - 1UL) << COLOR_OFF,
  TYPE_BIT = 1UL << 60UL,
};

[[gnu::const]]
static sgc_ref
align(sgc_ref const addr)
{
  return (addr + SGC_ALIGNMENT_MASK) & ~SGC_ALIGNMENT_MASK;
}

[[gnu::const]]
static size_t
pad(uintptr_t const addr)
{
  return align(addr) - addr;
}

[[gnu::hot]]
static inline void
header_setcolor(struct header* restrict hdr, enum color const color)
{
  hdr->mark &= SGC_REF_MASK;
  hdr->mark |= (uintptr_t)color << COLOR_OFF;
}

#define SETCOL(in, col)                                                        \
  (in).mark &= SGC_REF_MASK;                                                   \
  (in).mark |= (uintptr_t)(col) << COLOR_OFF;
#define GETCOL(in) ((in).mark & COLOR_MASK) >> COLOR_OFF
#define SETFWD(in, fwd)                                                        \
  (in).mark &= ~SGC_REF_MASK;                                                  \
  (in).mark |= (fwd) & SGC_REF_MASK;
#define GETFWD(in) ((in).mark & SGC_REF_MASK)
#define GETTY(in) ((in).type & SGC_REF_MASK)
#define IS_TYPEALLOC(in) ((in).type & TYPE_BIT)

[[gnu::hot]]
static inline void
header_setfwd(struct header* restrict hdr, sgc_ref const ref)
{
  hdr->mark &= ~SGC_REF_MASK;
  hdr->mark |= ref & SGC_REF_MASK;
}

#define min(x, y) (x) < (y) ? (x) : (y)
#define max(x, y) (x) > (y) ? (x) : (y)

struct sgc
sgc_init(size_t heap_size,
         size_t const gl_maxsize,
         void* root,
         sgc_root_visit root_visit)
{
  enum
  {
    default_gl_cap = 128,
  };

  struct sgc out;
  out.size = heap_size;
  out.bump = 0;
  out.heap = malloc(heap_size);
  out.root = root;
  out.root_visit = root_visit;

  out.state = state_not_collecting;

  out.gl_max = max(gl_maxsize, default_gl_cap);
  out.gl_cap = default_gl_cap;
  out.gl_len = 0;
  out.gl = malloc(sizeof(sgc_ref) * default_gl_cap);

#ifdef VALGRIND
  VALGRIND_CREATE_BLOCK(out.heap, out.size, "SGC-HEAP");
  VALGRIND_MAKE_MEM_NOACCESS(out.heap, out.size);
#endif

  return out;
}

// inline void*
// sgc_resolve(struct sgc const* sgc, sgc_ref const ref)
// {
//   return sgc->heap + (ref & SGC_REF_MASK);
// }

[[gnu::const, gnu::hot]]
static inline struct header*
get_header(struct sgc const* restrict sgc, sgc_ref const what)
{
  return sgc_resolve(sgc, what - sizeof(struct header));
}

#define likely(exp) __builtin_expect(!!(exp), 1)
#define unlikely(exp) __builtin_expect(!!(exp), 0)

[[gnu::always_inline]]
inline sgc_ref
sgc_resolve_type(struct sgc const* sgc, sgc_ref const ref)
{
  sgc_ref const typeref = GETTY(*get_header(sgc, ref));
  return typeref;
}

[[gnu::hot, gnu::const]]
static inline sgc_cleanup
get_cleanup(struct sgc const* sgc, struct header const hdr)
{
  if (unlikely(IS_TYPEALLOC(hdr)))
    return 0;
  sgc_ref const typeref = GETTY(hdr);
  struct sgc_type const* type = sgc_resolve(sgc, typeref);
  return type->cleanup;
}

[[gnu::hot, gnu::const]]
static inline sgc_visit
get_visit(struct sgc const* sgc, struct header const hdr)
{
  if (unlikely(IS_TYPEALLOC(hdr)))
    return 0;
  return ((struct sgc_type const*)sgc_resolve(sgc, GETTY(hdr)))->visit;
}

[[gnu::hot, gnu::const]]
static inline sgc_sizeof
get_sizeof(struct sgc const* restrict sgc, struct header const hdr)
{
  if (unlikely(IS_TYPEALLOC(hdr)))
    return 0;
  return ((struct sgc_type const*)sgc_resolve(sgc, GETTY(hdr)))->size;
}

[[gnu::hot, gnu::const]]
static inline size_t
get_size(struct sgc const* restrict sgc,
         sgc_ref const ref,
         struct header const hdr,
         void const* ctor)
{
  enum : unsigned
  {
    typesize_mask = 0xFFFFU,
  };

  if (unlikely(IS_TYPEALLOC(hdr)))
    return (hdr.type & typesize_mask);

  // struct cutf8_string_view strview =
  //   ((struct twigs_type const*)sgc_resolve(sgc, hdr.type))->debug_typename;
  // // printf("getting size: %.*s\n", (int)strview.bytesize, strview.str);
  // if (strview.str == 0)
  //   printf("%lu\n", ref);

  sgc_sizeof const sizeof_ = get_sizeof(sgc, hdr);
  assert(sizeof_ != 0);

  if (unlikely((uintptr_t)sizeof_ & SGC_SIZEBIT))
    return UINT32_MAX & (unsigned long)sizeof_;

  return sizeof_(sgc, ref, ctor);
}

static void
try_cleanup(struct sgc* restrict sgc, struct header const hdr, sgc_ref ref)
{
  sgc_cleanup const cleanup = get_cleanup(sgc, hdr);
  if (unlikely(cleanup))
    cleanup(sgc, ref);
}

#define hdr_prefetch(hdr)                                                      \
  do {                                                                         \
    __builtin_prefetch(((void*)(hdr)), 1, 3);                                  \
    if (0)                                                                     \
      __builtin_prefetch(((void*)(hdr)) + 8, 1, 3);                            \
  } while (0)

static void
try_visit(struct sgc* restrict sgc, sgc_ref ref)
{
  struct header const* restrict hdr_ptr = get_header(sgc, ref);
  struct header const hdr = *hdr_ptr;

  if (unlikely(IS_TYPEALLOC(hdr))) {
    struct sgc_type const* type = sgc_resolve(sgc, ref);
    if (type->static_visit)
      type->static_visit(sgc, ref);
  } else {
    sgc_visit const visit = get_visit(sgc, hdr);
    if (likely(visit))
      visit(sgc, ref);
    sgc_mark(sgc, &get_header(sgc, ref)->type);
  }
}

static inline size_t
next_alloc(size_t const size, sgc_ref const ref)
{
  return align((ref & SGC_REF_MASK) + size) + sizeof(struct header);
}

static void
cleanupall(struct sgc* sgc)
{
  sgc_ref ref = sizeof(struct header);

  while (ref < sgc->bump) {
    struct header const* hdr_ptr = get_header(sgc, ref);
    struct header const hdr = *hdr_ptr;
    size_t const size = get_size(sgc, ref, hdr, 0);
    try_cleanup(sgc, hdr, ref);
    ref = next_alloc(size, ref);

    /* VALGRIND note: cannot mark allocations as being NOACCESS
     * here, as sgc types would be marked as NOACCESS
     * while future objects of said types would read the free'd types */
  }
}

void
sgc_uninit(struct sgc* sgc)
{
  cleanupall(sgc);
  if (sgc->heap) {
    free(sgc->heap);
#ifdef VALGRIND
    VALGRIND_MAKE_MEM_NOACCESS(sgc->heap, sgc->size);
#endif
  }

  if (sgc->gl)
    free(sgc->gl);
}

static void
gl_ensure(struct sgc* restrict sgc)
{
  if (likely(sgc->gl_len + 1 < sgc->gl_cap))
    return;
  if (unlikely(sgc->gl_cap == sgc->gl_max))
    longjmp(sgc->oom_leave, 0);
  sgc->gl_cap = min(sgc->gl_cap * 2, sgc->gl_max);
  void* new_array = realloc(sgc->gl, sgc->gl_cap * sizeof(size_t));
  if (unlikely(!new_array))
    longjmp(sgc->oom_leave, 0);
  sgc->gl = new_array;
}

static inline void
gl_push(struct sgc* restrict sgc, sgc_ref const what)
{
  __builtin_prefetch(&sgc->gl[sgc->gl_len], 1, 0);
  gl_ensure(sgc);
  sgc->gl[sgc->gl_len++] = what;
}

static sgc_ref
gl_pop(struct sgc* sgc)
{
  if (sgc->gl_len == 0)
    longjmp(sgc->oom_leave, 0);
  return sgc->gl[--sgc->gl_len];
}

[[gnu::const]]
static size_t
freespace(struct sgc const* restrict sgc)
{
  return sgc->size - sgc->bump;
}

static sgc_ref
allocspace(struct sgc* sgc, size_t const size)
{
  uintptr_t const alignment_consumption = pad((uintptr_t)sgc->heap + sgc->bump);
  uintptr_t const consumption =
    alignment_consumption + size + sizeof(struct header);

  if (unlikely(consumption >= freespace(sgc)))
    return SGC_NULLREF;

  sgc->bump += alignment_consumption;
  sgc->bump += sizeof(struct header);
  sgc_ref const out = sgc->bump;
  sgc->bump += size;
  return out;
}

size_t
getfwd(struct header* hdr)
{
  return GETFWD(*hdr);
}

[[gnu::flatten]]
void
sgc_mark(struct sgc* sgc, sgc_ref* what)
{
  sgc_ref const old = *what;
  struct header* restrict hdr_ptr = get_header(sgc, *what);
  __builtin_prefetch(hdr_ptr, 1, 3);

  if (unlikely(*what == SGC_NULLREF))
    return;
  if (unlikely(sgc->state == state_not_collecting))
    return;

  hdr_prefetch(sgc_resolve(sgc, GETTY(*hdr_ptr)));
  struct header hdr = *hdr_ptr;

  enum color const color = GETCOL(hdr);
  if (sgc->state == state_mark) {
    SETCOL(hdr, RED);
  } else {
    if (!IS_TYPEALLOC(hdr))
      hdr.type = GETFWD(*get_header(sgc, hdr.type));
    SETCOL(hdr, BLUE);
    *what &= ~SGC_REF_MASK;
    *what |= GETFWD(hdr);
  }

  enum color const new_col = GETCOL(hdr);
  if (color == new_col)
    return;

  gl_push(sgc, old);
  *hdr_ptr = hdr;
}

[[gnu::flatten]] int
sgc_mark_weak(struct sgc* sgc, sgc_ref* what)
{
  struct header* restrict hdr_ptr = get_header(sgc, *what);
  __builtin_prefetch(hdr_ptr, 1, 3);

  if (unlikely(*what == SGC_NULLREF))
    return 0;
  if (unlikely(sgc->state == state_not_collecting))
    return 0;

  hdr_prefetch(sgc_resolve(sgc, GETTY(*hdr_ptr)));
  struct header hdr = *hdr_ptr;

  enum color const color = GETCOL(hdr);
  if (sgc->state == state_update) {
    *what &= ~SGC_REF_MASK;
    if (color == RED)
      *what |= GETFWD(hdr);
    else if (color == WHITE)
      return *what |= SGC_NULLREF, 1;
  }

  return 0;
}

/* returns the next allocation ref, or NULLREF */
[[gnu::flatten]]
static size_t
slide(struct sgc* sgc, sgc_ref ref)
{
  struct header* hdr_ptr = get_header(sgc, ref);
  hdr_prefetch(hdr_ptr);
  hdr_prefetch(sgc_resolve(sgc, hdr_ptr->type));
  register struct header hdr = *hdr_ptr;

  size_t const allocsize = get_size(sgc, ref, hdr, 0);

  if (unlikely(GETCOL(hdr) == BLUE)) {
    sgc_ref const fwd = GETFWD(hdr);
    SETCOL(hdr, WHITE);
    SETFWD(hdr, SGC_NULLREF);

#ifdef VALGRIND
    VALGRIND_MAKE_MEM_DEFINED(sgc->heap + fwd - sizeof(struct header),
                              allocsize + sizeof(struct header));
#endif

    __builtin_memmove(sgc->heap + fwd - sizeof(struct header),
                      sgc->heap + ref - sizeof(struct header),
                      allocsize + sizeof(struct header));

#ifdef VALGRIND
    sgc_ref const ref_true = ref - sizeof(struct header);
    sgc_ref const fwd_true = fwd - sizeof(struct header);
    size_t const size_true = allocsize + sizeof(struct header);
    if (ref_true < fwd_true + size_true) {
      VALGRIND_MAKE_MEM_NOACCESS(sgc->heap + fwd_true + size_true,
                                 (ref_true + size_true) -
                                   (fwd_true + size_true));
    } else {
      VALGRIND_MAKE_MEM_NOACCESS(sgc->heap + ref - sizeof(struct header),
                                 allocsize + sizeof(struct header));
    }
#endif

  } else {
#ifdef VALGRIND
    VALGRIND_MAKE_MEM_NOACCESS(sgc->heap + ref - sizeof(struct header),
                               allocsize + sizeof(struct header));
#endif
  }

  return next_alloc(allocsize, ref);
}

static void
slideheap(struct sgc* sgc, uintptr_t const oldbump)
{
  sgc->state = state_slide;
  sgc_ref begin = sizeof(struct header);
  while (begin < oldbump)
    begin = slide(sgc, begin);
}

[[gnu::flatten]]
static sgc_ref
compactref(struct sgc* sgc, sgc_ref const ref)
{
  struct header* hdr_ptr = get_header(sgc, ref);
  hdr_prefetch(hdr_ptr);
  hdr_prefetch(sgc_resolve(sgc, GETTY(*hdr_ptr)));
  register struct header hdr = *hdr_ptr;

  size_t const allocsize = get_size(sgc, ref, hdr, 0);

  if (unlikely(GETCOL(hdr) == RED)) {
    sgc_ref const newspace = allocspace(sgc, allocsize);
    SETFWD(hdr, newspace);
  } else {
    try_cleanup(sgc, hdr, ref);
  }

  *hdr_ptr = hdr;
  return next_alloc(allocsize, ref);
}

static void
compactheap(struct sgc* sgc, uintptr_t const old_bump)
{
  sgc_ref begin = sizeof(struct header);
  while (begin < old_bump)
    begin = compactref(sgc, begin);
}

static void
traverse(struct sgc* sgc)
{
  sgc->root_visit(sgc, sgc->root);
  while (sgc->gl_len > 0)
    try_visit(sgc, gl_pop(sgc));
}

static void
mark(struct sgc* sgc)
{
  sgc->state = state_mark;
  traverse(sgc);
  assert(sgc->gl_len == 0);
}

static void
update(struct sgc* sgc)
{
  sgc->state = state_update;
  traverse(sgc);
  assert(sgc->gl_len == 0);
}

/* forces a collection */
[[gnu::flatten]]
int
sgc_collect(struct sgc* sgc)
{
  uintptr_t const oldbump = sgc->bump;
  sgc->bump = 0;

  if (setjmp(sgc->oom_leave))
    return sgc->bump = oldbump, 1;

  mark(sgc);
  compactheap(sgc, oldbump);
  update(sgc);
  slideheap(sgc, oldbump);

  sgc->state = state_not_collecting;
  return 0;
}

[[gnu::flatten]]
sgc_ref
sgc_alloc(struct sgc* restrict sgc, sgc_ref const type, void const* ctor_params)
{
  struct sgc_type const* type_ = sgc_resolve(sgc, type);

  size_t size = 0;
  if (type_->static_size & SGC_SIZEBIT)
    size = type_->static_size & UINT32_MAX;
  else
    size = type_->size(sgc, SGC_NULLREF, ctor_params);
  sgc_ref const out = allocspace(sgc, size);

  if (unlikely(out == SGC_NULLREF))
    return SGC_NULLREF;

  struct header* hdr = get_header(sgc, out);

#ifdef VALGRIND
  VALGRIND_MAKE_MEM_DEFINED(hdr, sizeof *hdr);
  VALGRIND_MAKE_MEM_UNDEFINED(hdr + 1, size);
#endif

  hdr->type = type;
  header_setcolor(hdr, WHITE);
  header_setfwd(hdr, SGC_NULLREF);

  if (type_->clear)
    type_->clear(sgc, out, size);

  return out;
}

[[gnu::flatten]]
sgc_ref
sgc_alloc_type(struct sgc* restrict sgc, size_t const size_)
{
  size_t const size = max(sizeof(struct sgc_type), size_);
  sgc_ref const out = allocspace(sgc, size);

  if (unlikely(out == SGC_NULLREF))
    return SGC_NULLREF;

  struct header* hdr = get_header(sgc, out);

#ifdef VALGRIND
  VALGRIND_MAKE_MEM_DEFINED(hdr, sizeof *hdr);
  VALGRIND_MAKE_MEM_UNDEFINED(hdr + 1, size);
#endif

  hdr->type = TYPE_BIT | size;
  header_setcolor(hdr, WHITE);
  header_setfwd(hdr, SGC_NULLREF);

  return out;
}

sgc_ref
sgc_ptr_to_ref(struct sgc* const sgc, void* const ptr)
{
  return ptr - sgc->heap;
}

size_t
sgc_ref_sizeof(struct sgc* sgc, sgc_ref ref)
{
  if (ref == SGC_NULLREF)
    return 0;

  struct header* hdr = get_header(sgc, ref);
  size_t const out = get_size(sgc, ref, *hdr, 0);
  return out;
}

size_t
sgc_ref_total_usage(struct sgc* sgc, sgc_ref ref)
{
  return align(sgc_ref_sizeof(sgc, ref)) + sizeof(struct header);
}

void
sgc_clear_set_zero(struct sgc const* sgc, sgc_ref ref, size_t alloc_size)
{
  __builtin_memset(sgc_resolve(sgc, ref), 0, alloc_size);
}
