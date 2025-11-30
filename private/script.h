#pragma once

#include <stdbool.h>
#include <y.tab.h>

/* the maximum number of sections that may be
 * inserted into a single segment
 */
#define MAX_INSERTIONS 8
#define NO_MAX_LENGTH (-1u)
#define CURRENT_OFFSET (-1u)

typedef enum
{
  SCRIPT_SEGMENT_READ = 0x01,
  SCRIPT_SEGMENT_WRITE = 0x02,
  SCRIPT_SEGMENT_EXEC = 0x04,
} script_segment_flags;

/* these are the sections that are written out to a
 * poff executable file. these act as segments in memory,
 * i.e. r/w/x, so they go by the name of segments.
 * once all of the input sections are translated, each
 * input section is either discarded or allocated space
 * within a segment.
 * that is to say, multiple input sections may have
 * their contents consolidated into a single output segment.
 */
struct script_segment
{
  struct script_segment* next;

  char const* name;
  script_segment_flags flags;
  unsigned selector;
  unsigned capacity;

  struct script_section* consumption[MAX_INSERTIONS];
  unsigned num_consumption;
};

/* these can be imagined as pseudo-sections, they're stored in memory
 * and act as a "staging" zone for section manipulation code
 * when consolidating multiple files together.
 * for the output sections in a poff executable, see segments
 */
struct script_section
{
  struct script_section* next;

  char const* name;
  char const* target_segment;
  unsigned offset;
  unsigned length;
};

extern struct script_segment* script_segment_head;
extern struct script_section* script_section_head;

extern char const* entry_symbol_name;
extern char const* code_segment_name;
extern char const* data_segment_name;
extern char const* rodata_segment_name;

union token_value
{
  char const* str;
  unsigned num;
};

#define MAX_OUT_SEGMENTS 0x10u

void
script_dump();

void
add_script_segment(char const* name,
                   script_segment_flags flags,
                   unsigned selector,
                   unsigned length);

void
add_script_section(char const* name,
                   char const* target_segment,
                   unsigned offset);

/* this subroutine must be called after all link editing
 * and before any exportation of the segments into a poff file.
 * this function detects any potential section overlaps within a segment.
 * any overlap will be attemped to be fixed, but if otherwise impossible
 * this function will post an error and exit
 */
void
detect_section_overlaps();

unsigned
num_segments();

struct script_section*
find_section(char const* name);

struct script_segment*
find_segment(char const* name);

bool
segment_index(char const* name, unsigned* out);

script_segment_flags
parse_segment_flags(char const* in);

unsigned
parse_dec(char const* in);

unsigned
parse_hex(char const* in);

yytoken_kind_t
parse_keyword(char const* text);

int
yyerror(char const* err);

int
yylex();
