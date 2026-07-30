#include "include/sgc.h"
#include <stdio.h>
#include <stdlib.h>

enum
{
  NUM_REFS = 1024,
  MOD = 1024,
};

struct root
{
  struct sgc sgc;
  sgc_ref alloc_type;
  sgc_ref pair_type;
  sgc_ref alloc_refs[NUM_REFS];
  sgc_ref pair_refs[NUM_REFS];
  size_t numrefs;
};

struct alloc
{
  size_t size;
  char buf[];
};

static size_t
alloc_sizeof(struct sgc const* sgc, sgc_ref ref, size_t const* ctor)
{
  if (ref == SGC_NULLREF)
    return sizeof(struct alloc) + *ctor;
  struct alloc* a = sgc_resolve(sgc, ref);
  return sizeof *a + a->size;
}

static struct sgc_type const alloc_type = {
  .size = (sgc_sizeof)alloc_sizeof,
  .visit = 0,
  .cleanup = 0,
  .clear = sgc_clear_set_zero,
  .static_visit = 0,
};

struct pair
{
  sgc_ref lhs;
  sgc_ref rhs;
};

static void
pair_visit(struct sgc* sgc, sgc_ref ref)
{
  struct pair* p = sgc_resolve(sgc, ref);
  sgc_mark(sgc, &p->lhs);
  sgc_mark(sgc, &p->rhs);
}

static struct sgc_type const pair_type = {
  .static_size = sgc_static_size(sizeof(struct pair)),
  .visit = pair_visit,
  .cleanup = 0,
  .clear = sgc_clear_set_zero,
  .static_visit = 0,
};

static void
introduce_type(struct root* r)
{
  {
    r->alloc_type = sgc_alloc_type(&r->sgc, sizeof alloc_type);
    struct sgc_type* ptr = sgc_resolve(&r->sgc, r->alloc_type);
    *ptr = alloc_type;
  }
  {
    r->pair_type = sgc_alloc_type(&r->sgc, sizeof pair_type);
    struct sgc_type* ptr = sgc_resolve(&r->sgc, r->pair_type);
    *ptr = pair_type;
  }
}

static sgc_ref
make_alloc(struct root* root, size_t const size)
{
  sgc_ref out = sgc_alloc(&root->sgc, root->alloc_type, &size);
  struct alloc* a = sgc_resolve(&root->sgc, out);
  a->size = size;
  return out;
}

static sgc_ref
make_pair(struct root* root)
{
  sgc_ref out = sgc_alloc(&root->sgc, root->pair_type, 0);
  struct pair* a = sgc_resolve(&root->sgc, out);
  a->lhs = make_alloc(root, 31);
  a->rhs = make_alloc(root, 31);
  return out;
}

static void
gen_refs(struct root* r)
{
  for (size_t i = 0; i < NUM_REFS; i++) {
    size_t const size = (rand() % MOD) + 1;
    // size_t const size = MOD;
    make_alloc(r, size);
    r->alloc_refs[r->numrefs] = make_alloc(r, size);
    make_alloc(r, size);

    r->pair_refs[r->numrefs++] = make_pair(r);
  }
}

static void
visit_root(struct sgc* sgc, void* ptr)
{
  struct root* r = ptr;

  sgc_mark(sgc, &r->alloc_type);
  sgc_mark(sgc, &r->pair_type);

  for (size_t i = 0; i < r->numrefs; i++)
    sgc_mark(sgc, &r->alloc_refs[i]);
  for (size_t i = 0; i < r->numrefs; i++)
    sgc_mark(sgc, &r->pair_refs[i]);
}

static struct root*
makeroot()
{
  enum
  {
    KILO = 1024,
    MIBI = KILO * KILO,
    HEAP_SIZE = 8 * MIBI,
  };

  struct root* root = malloc(sizeof *root);
  root->sgc = sgc_init(HEAP_SIZE, SGC_GLINDEF, root, visit_root);
  root->numrefs = 0;

  return root;
}

static void
cleanup(struct root* r)
{
  sgc_uninit(&r->sgc);
  free(r);
}

static void
seedrand()
{
  FILE* f = fopen("/dev/random", "r");
  size_t s = 0;
  (void)fread(&s, sizeof s, 1, f);
  srand(s);
  fclose(f);
}

int
main()
{
  seedrand();
  struct root* r = makeroot();
  introduce_type(r);
  gen_refs(r);
  printf("garbage: %lu\n", r->sgc.bump);
  fflush(stdout);
  cleanup(r);
}
