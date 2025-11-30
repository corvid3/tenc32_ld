#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "crow.brbt/brbt.h"
#include "symbol.h"

static struct brbt symbol_table;

static int
string_comparator(char const** lhs, char const** rhs)
{
  return strcmp(*lhs, *rhs);
}

void
init_symbol_table()
{
  symbol_table = brbt_create(sizeof(struct edit_symbol),
                             offsetof(struct edit_symbol, name),
                             brbt_create_default_policy(),
                             NULL,
                             (brbt_comparator)string_comparator);
}

void
declare_symbol(char const* name)
{
  if (brbt_find(&symbol_table, &name) == BRBT_NIL) {
    struct edit_symbol new;
    new.name = strdup(name);
    new.static_symbols = brbt_create(sizeof(struct symbol),
                                     offsetof(struct symbol, section),
                                     brbt_create_default_policy(),
                                     NULL,
                                     (brbt_comparator)string_comparator);
    new.is_defined = false;
    brbt_insert(&symbol_table, &new, true);
  }
}

void
define_visible_symbol(char const* name, char const* section, unsigned offset)
{
  assert(section);
  assert(name);

  declare_symbol(name);
  struct edit_symbol* sym = brbt_find_get(&symbol_table, &name);
  if (sym->is_defined)
    fprintf(stderr, "external redeclaration of symbol %s\n", name), exit(1);
  sym->is_defined = true;
  sym->externally_visible.section = section;
  sym->externally_visible.offset = offset;
}

void
define_static_symbol(char const* name, char const* section, unsigned offset)
{
  assert(name);
  assert(section);

  declare_symbol(name);
  struct edit_symbol* sym = brbt_find_get(&symbol_table, &name);
  if (brbt_find(&sym->static_symbols, &section) == BRBT_NIL) {
    struct symbol new;
    new.section = strdup(section);
    new.offset = offset;
    brbt_insert(&sym->static_symbols, &new, true);
  }
}

struct symbol*
find_static_symbol(char const* name, char const* section)
{
  struct edit_symbol* edit = brbt_find_get(&symbol_table, &name);

  if (!edit)
    return NULL;

  return brbt_find_get(&edit->static_symbols, &section);
}

struct symbol*
find_global_symbol(char const* name)
{
  struct edit_symbol* edit = brbt_find_get(&symbol_table, &name);
  if (!edit || !edit->is_defined)
    return NULL;
  return &edit->externally_visible;
}

bool
is_global_symbol_defined(char const* name)
{
  struct edit_symbol* edit = brbt_find_get(&symbol_table, &name);
  return !edit || edit->is_defined;
}
