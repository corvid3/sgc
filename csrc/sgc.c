#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <threads.h>

#include "sgc.h"

enum sgc_state : uint32_t
{
  state_not_collecting,

  state_mark,
  state_update,
};

struct header
{
  sgc_ref type;
  uintptr_t mark;
};

enum color
{
  WHITE,
  BLACK,
};

enum : uintptr_t
{
  COLOR_OFF = 63UL,
  COLOR_MASK = 1UL << COLOR_OFF,
};

[[gnu::const]]
static sgc_ref
align(sgc_ref const addr, size_t const alignment)
{
  return ((addr + alignment - 1) & ~(alignment - 1));
}

[[gnu::const]]
static size_t
pad(uintptr_t const addr, size_t const alignment)
{
  return align(addr, alignment) - addr;
}

static void
alignheap(struct sgc* sgc)
{
  sgc->bump += pad((uintptr_t)sgc->heap + sgc->bump, SGC_ALIGNMENT);
}

[[gnu::hot, gnu::always_inline, gnu::const]]
static inline enum color
header_color(struct header const* hdr)
{
  return (hdr->mark & COLOR_MASK) >> COLOR_OFF;
}

[[gnu::hot, gnu::always_inline]]
static inline void
header_setcolor(struct header* restrict hdr, enum color const color)
{
  hdr->mark &= SGC_REF_MASK;
  hdr->mark |= (uintptr_t)color << COLOR_OFF;
}

[[gnu::hot, gnu::always_inline, gnu::const]]
static inline sgc_ref
header_fwd(struct header const* hdr)
{
  return (hdr->mark & SGC_REF_MASK);
}

[[gnu::hot, gnu::always_inline]]
static inline void
header_setfwd(struct header* hdr, sgc_ref const ref)
{
  hdr->mark &= ~SGC_REF_MASK;
  hdr->mark |= ref & SGC_REF_MASK;
}

static int
is_typealloc(struct header const* restrict hdr)
{
  return hdr->type == SGC_NULLREF;
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

  return out;
}

[[gnu::always_inline]]
inline void*
sgc_resolve(struct sgc const* sgc, sgc_ref const ref)
{
  return sgc->heap + (ref & SGC_REF_MASK);
}

[[gnu::const, gnu::hot, gnu::always_inline]]
static inline struct header*
get_header(struct sgc* sgc, sgc_ref const what)
{
  return sgc_resolve(sgc, what - sizeof(struct header));
}

[[gnu::always_inline]]
inline struct sgc_type const*
sgc_resolve_type(struct sgc* sgc, sgc_ref const ref)
{
  sgc_ref const typeref = get_header(sgc, ref)->type;
  if (typeref == SGC_NULLREF)
    return 0;
  return sgc_resolve(sgc, typeref);
}

[[gnu::hot]]
static inline sgc_cleanup
get_cleanup(struct sgc* sgc, struct header const* restrict hdr)
{
  if (is_typealloc(hdr))
    return 0;
  return ((struct sgc_type const*)sgc_resolve(sgc, hdr->type))->cleanup;
}

[[gnu::hot]]
static inline sgc_visit
get_visit(struct sgc* sgc, struct header* hdr)
{
  if (is_typealloc(hdr))
    return 0;
  return ((struct sgc_type const*)sgc_resolve(sgc, hdr->type))->visit;
}

[[gnu::hot]]
static inline sgc_sizeof
get_sizeof(struct sgc const* restrict sgc, struct header const* restrict hdr)
{
  if (is_typealloc(hdr))
    return 0;
  return ((struct sgc_type const*)sgc_resolve(sgc, hdr->type))->size;
}

[[gnu::hot]]
static inline size_t
get_size(struct sgc* sgc,
         sgc_ref const ref,
         struct header const* restrict hdr,
         void const* ctor)
{
  enum : unsigned
  {
    typesize_mask = 0xFFFFU,
  };

  if (is_typealloc(hdr))
    return sizeof(struct sgc_type) + (hdr->type & typesize_mask);

  sgc_sizeof const sizeof_ = get_sizeof(sgc, hdr);
  assert(sizeof_ != 0);
  if ((uintptr_t)sizeof_ & SGC_SIZEBIT)
    return UINT32_MAX & (uintptr_t)sizeof_;
  return sizeof_(sgc, ref, ctor);
}

static void
try_cleanup(struct sgc* sgc, struct header const* restrict hdr, sgc_ref ref)
{
  sgc_cleanup const cleanup = get_cleanup(sgc, hdr);
  if (cleanup)
    cleanup(sgc, ref);
}

static void
try_visit(struct sgc* sgc, sgc_ref ref)
{
  if (is_typealloc(get_header(sgc, ref))) {
    struct sgc_type const* type = sgc_resolve(sgc, ref);
    if (type->static_visit)
      type->static_visit(sgc, ref);
  } else {
    sgc_visit const visit = get_visit(sgc, get_header(sgc, ref));
    if (visit)
      visit(sgc, ref);
  }

  sgc_mark(sgc, &get_header(sgc, ref)->type);
}

static void
cleanupall(struct sgc* sgc)
{
  sgc_ref ref = sizeof(struct header);

  while (ref < sgc->bump) {
    struct header const* restrict hdr = get_header(sgc, ref);
    try_cleanup(sgc, hdr, ref);
    ref +=
      align(get_size(sgc, ref, hdr, 0), SGC_ALIGNMENT) + sizeof(struct header);
  }
}

void
sgc_uninit(struct sgc* sgc)
{
  cleanupall(sgc);
  if (sgc->heap)
    free(sgc->heap);
  if (sgc->gl)
    free(sgc->gl);
}

static void
gl_ensure(struct sgc* restrict sgc)
{
  if (sgc->gl_len + 1 < sgc->gl_cap)
    return;
  if (sgc->gl_cap == sgc->gl_max)
    longjmp(sgc->oom_leave, 0);
  sgc->gl_cap = min(sgc->gl_cap * 2, sgc->gl_max);
  void* new_array = realloc(sgc->gl, sgc->gl_cap * sizeof(size_t));
  if (!new_array)
    longjmp(sgc->oom_leave, 0);
  sgc->gl = new_array;
}

static void
gl_push(struct sgc* restrict sgc, sgc_ref const what)
{
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
  alignheap(sgc);
  uintptr_t const consumption = size + sizeof(struct header);

  if (consumption >= freespace(sgc))
    return SGC_NULLREF;

  sgc->bump += sizeof(struct header);
  sgc_ref const out = sgc->bump;
  sgc->bump += size;
  return out;
}

#define hdr_prefetch(hdr)                                                      \
  do {                                                                         \
    __builtin_prefetch(((void*)(hdr)), 1, 3);                                  \
    if (0)                                                                     \
      __builtin_prefetch(((void*)(hdr)) + 8, 1, 3);                            \
  } while (0)

void
sgc_mark(struct sgc* sgc, sgc_ref* what)
{
  if (*what == SGC_NULLREF)
    return;
  if (sgc->state == state_not_collecting)
    return;

  struct header* restrict hdr = get_header(sgc, *what);
  hdr_prefetch(hdr);

  enum color const color = header_color(hdr);
  if (color == BLACK)
    return;

  header_setcolor(hdr, BLACK);
  gl_push(sgc, *what);
  if (sgc->state == state_update)
    *what = header_fwd(hdr);
}

static inline size_t
next_hdr(size_t const size, sgc_ref const ref)
{
  return align(ref + size, SGC_ALIGNMENT) + sizeof(struct header);
}

/* returns the next allocation ref, or NULLREF */
static size_t
slide(struct sgc* sgc, sgc_ref ref)
{
  struct header* hdr = get_header(sgc, ref);
  hdr_prefetch(hdr);

  size_t const allocsize = get_size(sgc, ref, hdr, 0);

  if (header_color(hdr) == BLACK) {
    sgc_ref const fwd = header_fwd(hdr);
    header_setcolor(hdr, WHITE);
    header_setfwd(hdr, SGC_NULLREF);

    __builtin_memmove(sgc->heap + fwd - sizeof(struct header),
                      sgc->heap + ref - sizeof(struct header),
                      allocsize + sizeof(struct header));
  } else {
    sgc_cleanup cleanup = get_cleanup(sgc, hdr);
    if (cleanup)
      cleanup(sgc, ref);
  }

  return align(ref + allocsize, SGC_ALIGNMENT) + sizeof(struct header);
}

static void
slideheap(struct sgc* sgc, uintptr_t const oldbump)
{
  sgc_ref begin = sizeof(struct header);
  while (begin < oldbump)
    begin = slide(sgc, begin);
}

static sgc_ref
compactref(struct sgc* sgc, sgc_ref ref)
{
  struct header* hdr = get_header(sgc, ref);
  hdr_prefetch(hdr);

  size_t const allocsize = get_size(sgc, ref, hdr, 0);

  if (header_color(hdr) == BLACK) {
    header_setcolor(hdr, WHITE);
    sgc_ref const newspace = allocspace(sgc, get_size(sgc, ref, hdr, 0));
    header_setfwd(hdr, newspace);
  } else {
    try_cleanup(sgc, hdr, ref);
  }

  return align(ref + allocsize, SGC_ALIGNMENT) + sizeof(struct header);
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

sgc_ref
sgc_alloc(struct sgc* restrict sgc, sgc_ref const type, void const* ctor_params)
{
  struct sgc_type const* type_ = sgc_resolve(sgc, type);
  size_t size = 0;
  if ((uintptr_t)type_->size >= UINT32_MAX)
    size = type_->static_size & UINT32_MAX;
  else
    size = type_->size(sgc, SGC_NULLREF, ctor_params);
  sgc_ref const out = allocspace(sgc, size);
  if (out == SGC_NULLREF)
    return SGC_NULLREF;

  struct header* hdr = get_header(sgc, out);
  hdr->type = type;
  header_setcolor(hdr, WHITE);
  header_setfwd(hdr, SGC_NULLREF);
  return out;
}

sgc_ref
sgc_alloc_type(struct sgc* restrict sgc, size_t const size_)
{

  size_t const size = max(sizeof(struct sgc_type), size_);
  sgc_ref const out = allocspace(sgc, size);

  if (out == SGC_NULLREF)
    return SGC_NULLREF;

  struct header* hdr = get_header(sgc, out);
  hdr->type = SGC_NULLREF;
  header_setcolor(hdr, WHITE);
  header_setfwd(hdr, SGC_NULLREF);
  return out;
}
