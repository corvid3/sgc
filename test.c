#include "sgc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct list
{
  int car;
  sgc_ref cdr;
};

static size_t
list_size(struct sgc* sgc, sgc_ref const ref, void const* ctor)
{
  (void)sgc;
  (void)ref;
  (void)ctor;
  return sizeof(struct list);
}

static void
list_visit(struct sgc* sgc, sgc_ref const ref)
{
  struct list* list = sgc_resolve(sgc, ref);
  sgc_mark(sgc, &list->cdr);
  // printf("VISITED: %#x\n", ref);
}

struct sgc_type const list_type = {
  .size = (sgc_sizeof)list_size,
  .visit = (sgc_visit)list_visit,
  .cleanup = 0,
  .userdata = 0,
};

enum
{
  MAX_ROOTS = 64,
  MAX_FRAMES = 12,
};

struct root
{
  sgc_ref list_head;
  sgc_ref* roots[MAX_ROOTS];
  sgc_ref frames[MAX_FRAMES];
  unsigned rootidx;
  unsigned frameidx;
};

static void
addroot(struct root* root, sgc_ref* ref)
{
  assert(root->rootidx < MAX_ROOTS);
  assert(root->frameidx != 0);
  root->frames[root->frameidx - 1]++;
  root->roots[root->rootidx++] = ref;
}

static void
popframe(struct root* root)
{
  assert(root->frameidx != 0);
  root->rootidx -= root->frames[--root->frameidx];
}

static void
pushframe(struct root* root)
{
  assert(root->frameidx != MAX_FRAMES);
  root->frames[root->frameidx++] = 0;
}

static void
root_visit(struct sgc* sgc, struct root* root)
{
  sgc_mark(sgc, &root->list_head);
}

static sgc_ref
cons(struct sgc* sgc, int car, sgc_ref cdr)
{
  sgc_ref const list_ref = sgc_alloc(sgc, &list_type, 0);
  if (list_ref == SGC_NULLREF)
    return SGC_NULLREF;
  struct list* list = sgc_resolve(sgc, list_ref);
  list->car = car;
  list->cdr = cdr;
  return list_ref;
}

static sgc_ref
alloc_list(struct sgc* sgc, struct root* root)
{
  enum
  {
    iterations = 512000
  };

  pushframe(root);
  sgc_ref list = cons(sgc, rand(), SGC_NULLREF);
  addroot(root, &list);

  for (unsigned i = 0; i < iterations; i++) {
    sgc_ref const new = cons(sgc, rand(), list);
    if (new == SGC_NULLREF)
      break;
    list = new;
  }

  popframe(root);
  return list;
}

static int
listsize(struct sgc* sgc, sgc_ref ref)
{
  signed i = 0;
  while (ref != SGC_NULLREF) {
    struct list* list = sgc_resolve(sgc, ref);
    ref = list->cdr;

    i++;
  }
  return i;
}

static void
print_list(struct sgc* sgc, sgc_ref ref)
{
  while (ref != SGC_NULLREF) {
    struct list* list = sgc_resolve(sgc, ref);
    printf("%i ", list->car);
    ref = list->cdr;
  }
  putchar('\n');
}

static void
print_root(struct sgc* sgc, struct root* root)
{

  printf("root: %#x %lu\n",
         root->list_head,
         ((struct list*)sgc_resolve(sgc, root->list_head))->car);
}

int
main()
{
  enum
  {
    mibi = 1024 * 1024,
    heapsize = mibi * 64,
  };

  struct root root;
  root.frameidx = 0;
  root.rootidx = 0;
  root.list_head = SGC_NULLREF;

  struct sgc sgc = sgc_init(heapsize, -1, &root, (sgc_root_visit)root_visit);

  alloc_list(&sgc, &root);
  root.list_head = alloc_list(&sgc, &root);
  printf("%i %i/%i\n", listsize(&sgc, root.list_head), sgc.bump, sgc.size);
  // print_list(&sgc, root.list_head);
  sgc_collect(&sgc);
  // print_list(&sgc, root.list_head);
  sgc_uninit(&sgc);
}
