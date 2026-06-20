#include "sgc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

struct list
{
  int car;
  sgc_ref cdr;
};

static void
list_visit(struct sgc* sgc, sgc_ref const ref)
{
  struct list* list = sgc_resolve(sgc, ref);
  sgc_mark(sgc, &list->cdr);
  // printf("VISITED: %#x\n", ref);
}

struct sgc_type const list_type = {
  .static_size = sgc_static_size(sizeof(struct list)),
  .visit = (sgc_visit)list_visit,
  .cleanup = 0,
  .static_visit = 0,
};

enum
{
  MAX_ROOTS = 64,
  MAX_FRAMES = 12,
};

struct root
{
  sgc_ref list_head;
  sgc_ref list_type;

  sgc_ref weak_type;
  sgc_ref int_type;

  sgc_ref* roots[MAX_ROOTS];
  unsigned rootidx;
};

static void
introduce_type(struct root* root, struct sgc* sgc)
{
  sgc_ref type = sgc_alloc_type(sgc, 0);
  struct sgc_type* type_alloc = sgc_resolve(sgc, type);
  *type_alloc = list_type;

  root->list_type = type;
}

static void
addroot(struct root* root, sgc_ref* ref)
{
  assert(root->rootidx < MAX_ROOTS);
  root->roots[root->rootidx++] = ref;
}

// static void
// popframe(struct root* root)
// {
//   assert(root->frameidx != 0);
//   root->rootidx -= root->frames[--root->frameidx];
// }

// static void
// pushframe(struct root* root)
// {
//   assert(root->frameidx != MAX_FRAMES);
//   root->frames[root->frameidx++] = 0;
// }

static void
root_visit(struct sgc* sgc, struct root* root)
{
  sgc_mark(sgc, &root->int_type);
  sgc_mark(sgc, &root->weak_type);

  for (unsigned i = 0; i < root->rootidx; i++)
    sgc_mark(sgc, root->roots[i]);
}

static sgc_ref
cons(struct root* root, struct sgc* sgc, int car, sgc_ref cdr)
{
  sgc_ref const list_ref = sgc_alloc(sgc, root->list_type, 0);
  if (list_ref == SGC_NULLREF)
    return SGC_NULLREF;
  if (sgc_resolve_type(sgc, list_ref) != root->list_type)
    abort();
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
    iterations = 5120000
  };

  sgc_ref list = cons(root, sgc, rand(), SGC_NULLREF);
  addroot(root, &list);

  for (unsigned i = 0; i < iterations; i++) {
    sgc_ref const new = cons(root, sgc, rand(), list);
    if (new == SGC_NULLREF) {
      // printf("breaking @ %i\n", i);
      break;
    }

    list = new;
  }

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

static struct sgc_type const boxed_int = {
  .static_size = sgc_static_size(sizeof(int)),
  .visit = 0,
  .static_visit = 0,
  .cleanup = 0,
};

struct weak
{
  sgc_ref weak_int;
  sgc_ref strong_int;
};

static void
visit_weak(struct sgc* sgc, sgc_ref const ref)
{
  struct weak* weak = sgc_resolve(sgc, ref);
  sgc_mark(sgc, &weak->strong_int);
  sgc_mark_weak(sgc, &weak->weak_int);
}

static struct sgc_type const weak_test = {
  .static_size = sgc_static_size(sizeof(struct weak)),
  .visit = visit_weak,
  .static_visit = 0,
  .cleanup = 0,
};

static void
introduce_weak(struct root* root, struct sgc* sgc)
{
  sgc_ref weak_alloc = sgc_alloc_type(sgc, 0);
  sgc_ref int_alloc = sgc_alloc_type(sgc, 0);

  struct sgc_type* weak_type_ptr = sgc_resolve(sgc, weak_alloc);
  struct sgc_type* int_type_ptr = sgc_resolve(sgc, int_alloc);

  *weak_type_ptr = weak_test;
  *int_type_ptr = boxed_int;

  root->weak_type = weak_alloc;
  root->int_type = int_alloc;
}

static void
do_weak(struct root* root, struct sgc* sgc)
{
  sgc_ref weak_alloc = sgc_alloc(sgc, root->weak_type, 0);
  addroot(root, &weak_alloc);
  struct weak* weak = sgc_resolve(sgc, weak_alloc);
  weak->strong_int = sgc_alloc(sgc, root->int_type, 0);
  weak->weak_int = sgc_alloc(sgc, root->int_type, 0);

  printf("%x %x %x %x %x\n",
         root->weak_type,
         root->int_type,
         weak_alloc,
         weak->strong_int,
         weak->weak_int);

  printf("collecting\n");
  sgc_collect(sgc);
  weak = sgc_resolve(sgc, weak_alloc);
  printf("%x %x %x %x %x\n",
         root->weak_type,
         root->int_type,
         weak_alloc,
         weak->strong_int,
         weak->weak_int);
  sgc_collect(sgc);
  weak = sgc_resolve(sgc, weak_alloc);
}

int
main()
{
  enum
  {
    mibi = 1024 * 1024,
    heapsize = mibi * 256,
  };

  struct root root;
  root.rootidx = 0;
  root.list_head = SGC_NULLREF;

  // introduce_type(&root, &sgc);

  struct sgc sgc = sgc_init(heapsize, -1, &root, (sgc_root_visit)root_visit);
  introduce_weak(&root, &sgc);

  do_weak(&root, &sgc);

  // alloc_list(&sgc, &root);
  // root.list_head = alloc_list(&sgc, &root);
  // printf("%i %i/%i\n", listsize(&sgc, root.list_head), sgc.bump, sgc.size);
  // print_list(&sgc, root.list_head);
  // sgc_collect(&sgc);
  // print_list(&sgc, root.list_head);
  sgc_uninit(&sgc);
}
