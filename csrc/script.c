#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "script.h"
#include "y.tab.h"

struct script_segment* script_segment_head = NULL;
struct script_section* script_section_head = NULL;
char const* entry_symbol_name = NULL;
char const* code_segment_name = NULL;
char const* data_segment_name = NULL;
char const* rodata_segment_name = NULL;

static void
check_segment_clash(char const* name)
{
  if (find_segment(name))
    fprintf(stderr, "redeclaration of output segment %s\n", name), exit(1);
}

static void
check_section_clash(char const* name)
{
  if (find_section(name))
    fprintf(stderr, "redeclaration of output section %s\n", name), exit(1);
}

struct script_section*
find_section(char const* name)
{
  struct script_section* section = script_section_head;

  while (section) {
    if (strcmp(section->name, name) == 0)
      break;
    section = section->next;
  }

  return section;
}

struct script_segment*
find_segment(char const* name)
{
  struct script_segment* segment = script_segment_head;

  while (segment) {
    if (strcmp(segment->name, name) == 0)
      break;
    segment = segment->next;
  }

  return segment;
}

void
add_script_segment(char const* name,
                   script_segment_flags flags,
                   unsigned selector,
                   unsigned length)
{
  check_segment_clash(name);

  struct script_segment* into = script_segment_head;

  if (!into) {
    script_segment_head = malloc(sizeof *script_segment_head);
    into = script_segment_head;
  } else {
    while (into->next != NULL)
      into = into->next;
    into->next = malloc(sizeof *into->next);
    into = into->next;
  }

  into->name = name;
  into->flags = flags;
  into->capacity = length;
  into->selector = selector;
  into->next = NULL;
}

void
add_script_section(char const* name,
                   char const* target_segment,
                   unsigned offset)
{
  // static char const* disallowed[] = {
  //   "strtab",
  //   "symtab",
  //   "srel",
  // };

  // for(int i = 0; i < sizeof disallowed / sizeof* disallowed; i++)
  // if(strcmp(name, disallowed[i]) == 0) fprintf(stderr, "output section may
  // not be named reserved %s\n");

  check_section_clash(name);

  if (!find_segment(target_segment))
    fprintf(stderr,
            "target segment <%s> for section <%s> is undeclared\n",
            target_segment,
            name),
      exit(1);

  struct script_section* into = script_section_head;

  if (!into) {
    script_section_head = malloc(sizeof *script_section_head);
    into = script_section_head;
  } else {
    while (into->next != NULL)
      into = into->next;
    into->next = malloc(sizeof *into->next);
    into = into->next;
  }

  into->name = name;
  into->target_segment = target_segment;
  into->offset = offset;
  into->length = 0;
  into->next = NULL;
}

bool
segment_index(char const* name, unsigned* out)
{
  unsigned i = 0;

  for (struct script_segment* seg = script_segment_head; seg; seg = seg->next) {
    if (!strcmp(name, seg->name))
      return *out = i, true;
    i++;
  }

  return false;
}

script_segment_flags
parse_segment_flags(char const* in)
{
  bool read = false, write = false, exec = false;
  for (unsigned i = 0; i < strlen(in); i++)
    switch (tolower(in[i])) {
      case 'r':
        read = true;
        break;

      case 'w':
        write = true;
        break;

      case 'x':
        exec = true;
        break;
    }

  return (read ? SCRIPT_SEGMENT_READ : 0) | (write ? SCRIPT_SEGMENT_WRITE : 0) |
         (exec ? SCRIPT_SEGMENT_EXEC : 0);
}

unsigned
parse_dec(char const* in)
{
  char* endptr;
  unsigned out = strtoul(in, &endptr, 10);
  if (*endptr != 0)
    fprintf(stderr, "invalid number in dec num\n");
  return out;
}

unsigned
parse_hex(char const* in)
{
  char* endptr;
  unsigned out = strtoul(in, &endptr, 16);
  if (*endptr != 0)
    fprintf(stderr, "invalid number in hex num\n");
  return out;
}

yytoken_kind_t
parse_keyword(char const* text)
{
  struct
  {
    char const* what;
    yytoken_kind_t kind;
  } relations[] = {
    { "sections", SECTIONS }, { "segments", SEGMENTS },

    { "entry", ENTRY },       { "data", DATA },         { "rodata", RODATA },
  };

  for (unsigned i = 0; i < sizeof relations / sizeof *relations; i++) {
    if (strcasecmp(relations[i].what, text) == 0)
      return relations[i].kind;
  }

  fprintf(stderr, "invalid identifier %s\n", text);
  exit(1);
}

extern int yylineno;

int
yyerror(char const* err)
{
  fprintf(
    stderr, "error while parsing linker file, line %i: %s\n", yylineno, err);
  return 0;
}

[[maybe_unused]] void
script_dump()
{
  for (struct script_segment* seg = script_segment_head; seg; seg = seg->next) {
    printf("segment %s\n", seg->name);
  }

  for (struct script_section* sec = script_section_head; sec; sec = sec->next) {
    printf("section %s\n", sec->name);
  }
}

static int
consumption_comparator(struct script_section** lhs, struct script_section** rhs)
{
  if ((*lhs)->offset < (*rhs)->offset)
    return -1;

  if ((*lhs)->offset > (*rhs)->offset)
    return 1;

  return 0;
}

#define INSERT_CONSUMPTION(segment, section)                                   \
  if (segment->num_consumption + 1 >= MAX_INSERTIONS)                          \
    fprintf(stderr,                                                            \
            "too many section insertions into segment %s, remove sections or " \
            "recompile with a higher parameter\n",                             \
            segment->name),                                                    \
      exit(1);                                                                 \
  segment->consumption[segment->num_consumption++] = section

#define INSERT_CONSUMPTION_AT(segment, section, where)                         \
  if (segment->num_consumption + 1 >= MAX_INSERTIONS)                          \
    fprintf(stderr,                                                            \
            "too many section insertions into segment %s, remove sections or " \
            "recompile with a higher parameter\n",                             \
            segment->name),                                                    \
      exit(1);                                                                 \
  memmove(segment->consumption[where + 1],                                     \
          segment->consumption[where],                                         \
          (segment->num_consumption - where) * sizeof(*segment->consumption)); \
  segment->consumption[where] = section, segment->num_consumption++

#define SORT_CONSUMPTION(segment)                                              \
  qsort(segment->consumption,                                                  \
        segment->num_consumption,                                              \
        sizeof *segment->consumption,                                          \
        (__compar_fn_t)consumption_comparator)

/* generates a linear graph representing the "consumption" of space
 * by each section that is to exported to a given segment.
 * this graph is stored in the "consumption" field of the segment structure.
 * the graph is sorted front-to-back in increasing offset
 */
static void
compute_consumption()
{
  for (struct script_segment* segment = script_segment_head; segment;
       segment = segment->next) {

    /* first, add in all of the "definite" sections */
    for (struct script_section* section = script_section_head; section;
         section = section->next) {
      /* this section does not appear within the segment, skip it */
      if (strcmp(segment->name, section->name))
        continue;

      /* -1 is the marker for an indefinite section, skip em for now */
      if (section->offset == -1u)
        break;

      INSERT_CONSUMPTION(segment, section);
    }

    SORT_CONSUMPTION(segment);

    /* then, try to find space for the undefined section */
    for (struct script_section* section = script_section_head; section;
         section = section->next) {

      /* this section does not appear within the segment, skip it */
      if (strcmp(segment->name, section->name))
        continue;

      /* this section has already been designated a consumption
       * slot in the previous step, skip it */
      if (section->offset != -1u)
        continue;

      /* if there is space at the beginning, we are free to take it */
      if (segment->num_consumption == 0) {
        INSERT_CONSUMPTION(segment, section);
        section->offset = 0;
        continue;
      }

      if (segment->consumption[0]->offset > section->length) {
        INSERT_CONSUMPTION_AT(segment, section, 0);
        section->offset = 0;
        continue;
      }

      /* find potential space between sections */
      for (unsigned i = 0; i < segment->num_consumption - 1; i++) {
        struct script_section* a = segment->consumption[i];
        struct script_section* b = segment->consumption[i + 1];
        unsigned a_end = a->offset + a->length;
        unsigned b_start = b->offset;
        unsigned gap = b_start - a_end;
        if (section->length < gap) {
          INSERT_CONSUMPTION_AT(segment, section, i + 1);
          section->offset = a_end;
          goto end;
        }
      }

      /* ok just put it at the end */
      section->offset =
        segment->consumption[segment->num_consumption - 1]->offset +
        segment->consumption[segment->num_consumption - 1]->length;
      INSERT_CONSUMPTION(segment, section);

    end:;
    }
  }
}

[[maybe_unused]] void
dump_consumption_graph(struct script_segment* segment)
{
  printf("-- CONSUMPTION GRAPH <%s> --\n", segment->name);

  for (unsigned i = 0; i < segment->num_consumption; i++) {
    struct script_section* section = segment->consumption[i];
    printf("%s @0x%x, length 0x%x\n",
           section->name,
           section->offset,
           section->length);
  }

  putchar('\n');
}

void
detect_section_overlaps()
{
  compute_consumption();

  for (struct script_segment* segment = script_segment_head; segment;
       segment = segment->next) {
    // dump_consumption_graph(segment);

    for (unsigned i = 0; i < segment->num_consumption; i++) {
      struct script_section* section = segment->consumption[i];
      if (section->offset > segment->capacity)
        fprintf(stderr,
                "section %s is located out of bounds in segment %s\n",
                section->name,
                segment->name),
          exit(1);

      if (section->offset + section->length > segment->capacity)
        fprintf(stderr,
                "section %s overflows out of bounds in segment %s\n",
                section->name,
                segment->name),
          exit(1);

      if (i == segment->num_consumption - 1)
        break;

      struct script_section* next_section = segment->consumption[i + 1];

      if (section->offset + section->length >= next_section->offset)
        fprintf(stderr,
                "section %s overflows onto section %s in segment %s\n",
                section->name,
                next_section->name,
                segment->name),
          exit(1);
    }
  }
}

unsigned
num_segments()
{
  unsigned count = 0;

  for (struct script_segment* seg = script_segment_head; seg; seg = seg->next)
    count++;

  return count;
}
