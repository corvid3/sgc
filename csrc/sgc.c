#include <assert.h>
#include <setjmp.h>
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
  struct sgc_type const* type;
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
    default_gl_cap = 64,
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

void*
sgc_resolve(struct sgc* sgc, sgc_ref const ref)
{
  return sgc->heap + (ref & SGC_REF_MASK);
}

[[gnu::pure, gnu::hot, gnu::always_inline]]
static inline struct header*
get_header(struct sgc* sgc, sgc_ref const what)
{
  return sgc_resolve(sgc, what - sizeof(struct header));
}

[[gnu::hot]]
struct sgc_type const*
sgc_resolve_type(struct sgc* sgc, sgc_ref const ref)
{
  return get_header(sgc, ref)->type;
}

static void
cleanupall(struct sgc* sgc)
{
  sgc_ref ref = sizeof(struct header);

  while (ref < sgc->bump) {
    struct header* hdr = get_header(sgc, ref);
    if (hdr->type->cleanup)
      hdr->type->cleanup(sgc, ref);
    ref += align(hdr->type->size(sgc, ref, 0), SGC_ALIGNMENT) +
           sizeof(struct header);
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

void
sgc_mark(struct sgc* sgc, sgc_ref* what)
{
  if (*what == SGC_NULLREF)
    return;
  if (sgc->state == state_not_collecting)
    return;

  struct header* hdr = get_header(sgc, *what);
  enum color const color = header_color(hdr);
  if (color == BLACK)
    return;

  header_setcolor(hdr, BLACK);
  gl_push(sgc, *what);
  if (sgc->state == state_update)
    *what = header_fwd(hdr);
}

/* returns the next allocation ref, or NULLREF */
static size_t
slide(struct sgc* sgc, sgc_ref ref)
{
  struct header* hdr = get_header(sgc, ref);
  size_t const allocsize = hdr->type->size(sgc, ref, 0);

  if (header_color(hdr) == BLACK) {
    sgc_ref const fwd = header_fwd(hdr);
    header_setcolor(hdr, WHITE);
    header_setfwd(hdr, SGC_NULLREF);

    __builtin_memmove(sgc->heap + fwd - sizeof(struct header),
                      sgc->heap + ref - sizeof(struct header),
                      allocsize + sizeof(struct header));
  } else {
    if (hdr->type->cleanup)
      hdr->type->cleanup(sgc, ref);
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
  size_t const allocsize = hdr->type->size(sgc, ref, 0);

  if (header_color(hdr) == BLACK) {
    header_setcolor(hdr, WHITE);
    sgc_ref const newspace =
      allocspace(sgc, sgc_resolve_type(sgc, ref)->size(sgc, ref, 0));
    header_setfwd(hdr, newspace);
  } else {
    if (hdr->type->cleanup)
      hdr->type->cleanup(sgc, ref);
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
  while (sgc->gl_len > 0) {
    sgc_ref const ref = gl_pop(sgc);
    sgc_resolve_type(sgc, ref)->visit(sgc, ref);
  }
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
sgc_alloc(struct sgc* restrict sgc,
          struct sgc_type const* restrict const type,
          void const* ctor_params)
{
  size_t const size = type->size(sgc, SGC_NULLREF, ctor_params);
  sgc_ref const out = allocspace(sgc, size);
  if (out == SGC_NULLREF)
    return SGC_NULLREF;

  struct header* hdr = get_header(sgc, out);
  hdr->type = type;
  header_setcolor(hdr, WHITE);
  header_setfwd(hdr, SGC_NULLREF);
  return out;
}
