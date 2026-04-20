#include "sgc.h"
#include <assert.h>
#include <stdlib.h>

enum sgc_state : uint32_t
{
  state_not_collecting,

  state_compact,
  state_update,
};

struct header
{
  struct sgc_type const* type;
  uintptr_t mark;
};

enum color
{
  /* unset, original state */
  white,

  /* object has been allocated space */
  red,

  /* object has been allocated space and it's child members
   * have all been updated */
  green,
};

enum : uintptr_t
{
  color_off = 62,
  color_mask = 2UL << color_off,
};

static enum color
header_color(struct header const* hdr)
{
  return (hdr->mark & color_mask) >> color_off;
}

static void
header_setcolor(struct header* hdr, enum color color)
{
  hdr->mark &= SGC_REF_MASK;
  hdr->mark |= (uintptr_t)color << color_off;
}

static sgc_ref
header_fwd(struct header const* hdr)
{
  return (hdr->mark & SGC_REF_MASK);
}

static void
header_setfwd(struct header* hdr, sgc_ref const ref)
{
  hdr->mark &= ~SGC_REF_MASK;
  hdr->mark |= ref & SGC_REF_MASK;
}

struct sgc
sgc_init(size_t heap_size,
         size_t const gl_maxsize,
         void* root,
         sgc_root_visit root_visit)
{
  struct sgc out;
  out.size = heap_size;
  out.bump = 0;
  out.heap = malloc(heap_size);
  out.root = root;
  out.root_visit = root_visit;

  out.state = state_not_collecting;

  out.gl_max = gl_maxsize;
  out.gl_len = 0;
  out.gl = malloc(sizeof(sgc_ref) *
                  (gl_maxsize == -1UL ? SGC_DEFAULT_GLMAXSIZE : gl_maxsize));

  return out;
}

void
sgc_uninit(struct sgc* sgc)
{
  if (sgc->heap)
    free(sgc->heap);
  if (sgc->gl)
    free(sgc->gl);
}

void*
sgc_resolve(struct sgc* sgc, sgc_ref const ref)
{
  return sgc->heap + (ref & SGC_REF_MASK);
}

static struct header*
get_header(struct sgc* sgc, sgc_ref const what)
{
  return sgc_resolve(sgc, what - sizeof(struct header));
}

struct sgc_type const*
sgc_resolve_type(struct sgc* sgc, sgc_ref const ref)
{
  return get_header(sgc, ref)->type;
}

static void
gl_push(struct sgc* sgc, sgc_ref const what)
{
  /* FIXME: longjump to indicate out of memory */
  if (sgc->gl_len >= sgc->gl_max)
    __builtin_abort();

  sgc->gl[sgc->gl_len++] = what;
}

static sgc_ref
gl_pop(struct sgc* sgc)
{
  if (sgc->gl_len == 0)
    __builtin_abort();
  return sgc->gl[--sgc->gl_len];
}

static sgc_ref
align(sgc_ref const addr, size_t const alignment)
{

  return ((addr + alignment - 1) & ~(alignment - 1));
}

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

  if (consumption >= freespace(sgc)) {
    /* FIXME: longjmp to indicate OOM */
    if (sgc->state != state_not_collecting)
      __builtin_abort();

    sgc_collect(sgc);
  }

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

  switch (sgc->state) {
    case state_compact:
      if (color == red)
        return;

      header_setcolor(hdr, red);
      {
        sgc_ref const newspace =
          allocspace(sgc, sgc_resolve_type(sgc, *what)->size(sgc, *what));
        header_setfwd(hdr, newspace);
      }
      gl_push(sgc, *what);
      break;

    case state_update:
      if (color == green)
        return;
      gl_push(sgc, *what);
      header_setcolor(hdr, green);
      *what = header_fwd(hdr);
      break;

    case state_not_collecting:
      __builtin_unreachable();
  }
}

/* returns the next allocation ref, or NULLREF */
static size_t
compact(struct sgc* sgc, sgc_ref ref)
{
  struct header* hdr = get_header(sgc, ref);
  size_t const allocsize = hdr->type->size(sgc, ref);

  if (header_color(hdr) == green) {
    sgc_ref const fwd = header_fwd(hdr);
    header_setcolor(hdr, white);
    header_setfwd(hdr, SGC_NULLREF);

    __builtin_memmove(sgc->heap + fwd - sizeof(struct header),
                      sgc->heap + ref - sizeof(struct header),
                      allocsize + sizeof(struct header));
  }

  return align(ref + allocsize, SGC_ALIGNMENT) + sizeof(struct header);
}

static void
compactheap(struct sgc* sgc, uintptr_t const oldbump)
{
  sgc_ref begin = sizeof(struct header);

  while (begin < oldbump)
    begin = compact(sgc, begin);
}

static void
sweep(struct sgc* sgc)
{
  sgc->root_visit(sgc, sgc->root);

  while (sgc->gl_len > 0) {
    sgc_ref const ref = gl_pop(sgc);
    sgc_resolve_type(sgc, ref)->visit(sgc, ref);
  }
}

/* forces a collection */
void
sgc_collect(struct sgc* sgc)
{
  uintptr_t const oldbump = sgc->bump;
  sgc->bump = 0;

  sgc->state = state_compact;
  sweep(sgc);
  assert(sgc->gl_len == 0);
  sgc->state = state_update;
  sweep(sgc);

  compactheap(sgc, oldbump);

  sgc->state = state_not_collecting;
}

sgc_ref
sgc_alloc(struct sgc* sgc, struct sgc_type const* type)
{
  size_t const size = type->size(sgc, SGC_NULLREF);
  sgc_ref const out = allocspace(sgc, size);
  if (out == SGC_NULLREF)
    return SGC_NULLREF;
  struct header* hdr = get_header(sgc, out);
  hdr->type = type;
  header_setcolor(hdr, white);
  header_setfwd(hdr, SGC_NULLREF);
  return out;
}
