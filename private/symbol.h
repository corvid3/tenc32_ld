#pragma once

#include "crow.brbt/brbt.h"
#include <stdbool.h>

struct symbol
{
  char const* section;
  unsigned offset;
};

struct edit_symbol
{
  char const* name;

  /* static definitions that share this symbols name,
   * sorted per-section name
   */
  struct brbt static_symbols;

  /* there can only be one externally visible symbol for each
   * link edited executable, so we just store it here.
   * this is set to mark if there is a definition
   * located within a section that uses this symbol.
   */
  bool is_defined;

  struct symbol externally_visible;
};

void
init_symbol_table();

void
declare_symbol(char const*);

void
define_visible_symbol(char const* name, char const* section, unsigned offset);

void
define_static_symbol(char const* name, char const* section, unsigned offset);

struct symbol*
find_static_symbol(char const* name, char const* section);

struct symbol*
find_global_symbol(char const* name);

bool
is_global_symbol_defined(char const* name);

/* asserts that there are no undefined symbols
 * within the linkers symbol table.
 * this should be ran after consolidating all of the
 * input files into the output glob
 */
void
assert_no_extern_symbols();
