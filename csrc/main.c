#include <asm-generic/errno-base.h>
#include <crow.POFF_c/POFF_c.h>
#include <crow.crowcpu_arch/crowcpu_arch.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PANIC(fmt) fputs(fmt "\n", stderr), abort();
#define MAX_OBJECT_FILES 1
#define MAX_UNQIUE_SECTIONS 256
#define THIS_VERSION 0
#define MAX_SCRIPT_LINE_LENGTH 128

typedef enum
{
  FORMAT_BINARY,
  FORMAT_PRT,
} output_format;

typedef struct
{
  uint32_t size;

  /* supplied by linker scripts,
   * adjusts the offsets of symbol resolution */
  uint32_t paddr;
} ld_merged_section_info_t;

typedef struct
{
  output_format format;

  // which sections to emit
  _Bool enable_output[POFF_SECTION_SENTINEL];
  uint32_t maximum_length[POFF_SECTION_SENTINEL];
  uint32_t physical_offset[POFF_SECTION_SENTINEL];
} ld_script_t;

static ld_merged_section_info_t merged_sections[POFF_SECTION_SENTINEL];

static char* input_paths[MAX_OBJECT_FILES];
static int num_object_files;
static poff_file_t files[MAX_OBJECT_FILES];

static _Bool use_script = 0;
static ld_script_t script;

static _Bool
would_overflow(uint32_t a, uint32_t b)
{
  return a > UINT32_MAX - b;
}

static output_format
parse_output_format(char const* in)
{
  if (strcmp(in, "bin") == 0)
    return FORMAT_BINARY;

  PANIC("unknown format");
}

static ld_script_t
load_linker_script(char* filepath)
{
  FILE* file = fopen(filepath, "rb");
  static char linebuf[MAX_SCRIPT_LINE_LENGTH];
  static char str[64];
  static int num;

  ld_script_t out = { 0 };

  while (fgets(linebuf, MAX_SCRIPT_LINE_LENGTH, file)) {
    if (feof(file))
      break;

    if (strchr(linebuf, '\n') == NULL)
      PANIC("line too long in linker script");

    // skip empty lines
    if (strlen(linebuf) == 1)
      continue;

    // skip comments, may only be on their own line
    char* first_semi = strchr(linebuf, ';');
    if (first_semi != NULL) {
      // assert that semi must be the first non-whitespace char
      for (char* i = linebuf; i < first_semi; i++)
        if (!isspace(*i))
          PANIC("semicolon comments must be on their own line, at the very "
                "beginning");
    }

    if (sscanf(linebuf, "FORMAT %63s", str) == 1)
      out.format = parse_output_format(str);
    else if (sscanf(linebuf, "USE %63s", str) == 1) {
      poff_section_type sect = poff_get_section_type_by_name(str);
      out.enable_output[sect] = 1;
    } else if (sscanf("%63s -> LENGTH = %i", str, num) == 1) {
      poff_section_type sect = poff_get_section_type_by_name(str);
      out.maximum_length[sect] = num;
    } else if (sscanf("%63s -> ORIGIN = %i", str, num) == 1) {
      poff_section_type sect = poff_get_section_type_by_name(str);
      out.physical_offset[sect] = num;
    } else
      PANIC("unknown syntax in linker file");
  }

  fclose(file);

  /* some quick sanity checks, no overlapping sections */
  for (poff_section_type section = 0; section < POFF_SECTION_SENTINEL;
       section++) {

    if (!out.enable_output[section])
      continue;

    for (poff_section_type section2 = 0; section2 < POFF_SECTION_SENTINEL;
         section2++) {
      if (!out.enable_output[section2])
        continue;

      uint32_t section1_start = out.physical_offset[section];
      uint32_t section1_end =
        out.physical_offset[section] + out.maximum_length[section];

      uint32_t section2_start = out.physical_offset[section2];
      uint32_t section2_end =
        out.physical_offset[section2] + out.maximum_length[section2];

      if ((section1_start <= section2_end && section1_end >= section2_start) ||
          (section2_start <= section1_end && section2_end >= section1_start))
        PANIC("overlapping sections in linker script");
    }
  }

  return out;
}

static void
init_merged_sections()
{
  for (int i = 0; i < POFF_SECTION_SENTINEL; i++) {
    ld_merged_section_info_t* merged = &merged_sections[i];
    merged->size = 0;
    merged->paddr = 0;
  }
}

/* also calculates per-section offset per-file */
static void
calculate_merged_section_offsets()
{
  /* first, calculate the total merged sections sizes */
  for (int i = 0; i < num_object_files; i++) {
    poff_file_t* file = &files[i];

    for (int j = 0; j < POFF_SECTION_SENTINEL; j++) {
      poff_section_t* section = &file->sections[i];
      ld_merged_section_info_t* merged = &merged_sections[i];

      if (would_overflow(merged->size, section->size))
        PANIC("merged section would overflow");

      section->ld.assigned_offset = merged->size;
      merged->size += section->size;
    }
  }

  if (use_script) {
    for (poff_section_type sect = 0; sect < POFF_SECTION_SENTINEL; sect++) {
      uint32_t merged_size = merged_sections[sect].size;
      uint32_t limit = script.maximum_length[sect];

      if (merged_size > limit)
        PANIC("section is too large");

      merged_sections[sect].paddr = script.physical_offset[sect];
    }
  }
}

/* does not assert that there are no overlapping symbol names */

static void
discover_symbol_collisions()
{
  for (int i = 0; i < num_object_files; i++) {
    poff_file_t* file = &files[i];

    for (uint32_t j = 0; j < file->header.num_symbols; j++) {
      poff_symbol_t* sym = &file->symbols[i];

      /* symbol re-definitions within POFF files */
      for (uint32_t k = j + 1; k < file->header.num_symbols; k++) {
        poff_symbol_t* sym2 = &file->symbols[k];

        if (sym->name_len != sym2->name_len)
          continue;

        if (memcmp(sym->name, sym2->name, sym->name_len) != 0)
          continue;

        fprintf(
          stderr, "redefinition of symbol %.*s\n", sym->name_len, sym->name);
        abort();
      }

      /* symbol re-definitions between POFF files */
      /* disregard internal symbols, we only care about global defns */
      if (sym->type != POFF_SYMBOL_INTERNAL &&
          sym->type != POFF_SYMBOL_EXTERNAL)
        continue;

      poff_symbol_t* sym2 = NULL;

      /* TODO: can memoize true results by maintaining a list of symbols
       * that have so far succeeded
       */

      /* discover unresolved external references */
      for (int k = 0; k < num_object_files; k++) {
        poff_file_t* file = &files[i];
        sym2 = find_symbol(file, sym->name, sym->name_len, POFF_SYMBOL_GLOBAL);
        if (sym2 != NULL)
          break;
      }

      if (sym2 == NULL) {
        fprintf(stderr, "unresolved symbol %.*s\n", sym->name_len, sym->name);
        abort();
      }

      /* discover overlapping global references */
      if (sym2->type == POFF_SYMBOL_GLOBAL) {
        for (int k = 0; k < num_object_files; k++) {
          poff_file_t* file2 = &files[k];

          if (file2 == file)
            continue;

          poff_symbol_t* sym2 =
            find_symbol(file2, sym->name, sym->name_len, sym->type);
          if (sym2 != NULL) {
            fprintf(stderr,
                    "redefinition of global symbol %.*s\n",
                    sym->name_len,
                    sym->name);
            abort();
          }
        }
      }
    }
  }
}

static void
apply_relocation(poff_file_t* file,
                 poff_section_type section,
                 uint32_t at,
                 uint32_t new_address)
{
  // relocations must be aligned to a 32 bit boundary
  if ((at & 0x11111) != 0) {
    fprintf(stderr, "relocations must be aligned on a 32 bit boundary\n");
    exit(1);
  }

  if (section == POFF_TEXT) {
    /* we have to update an instruction, rather than a word */
    crowcpu_arch_decoded_instruction instr;
    if (!crowcpu_arch_decode(&instr,
                             *(uint32_t*)&file->sections[section].data[at])) {
      fprintf(stderr, "unknown instruction at data address 0x%X\n", at);
      exit(1);
    }

    /* TODO: should it error on new addresses larger than the imm? */

    switch (instr.instruction) {
      case CROWCPU_DECODED_MOVE:
        switch (instr.addressing) {
          case CROWCPU_ADDRESSING_MOVE_IMM_REG:
            instr.payload.move_reg_imm.imm = new_address;
            break;

          default:
            fprintf(stderr, "invalid addressing mode\n");
            exit(1);
            break;
        }
        break;

      case CROWCPU_DECODED_LOAD:
      case CROWCPU_DECODED_STORE:
        switch (instr.addressing) {
          case CROWCPU_ADDRESSING_MEMORY_CONSTANT:
            instr.payload.mem_reg_constant.constant = new_address;
            break;

          default:
            fprintf(stderr, "invalid addressing mode\n");
            exit(1);
            break;
        }
        break;

      case CROWCPU_DECODED_TEST:
      case CROWCPU_DECODED_ADD:
      case CROWCPU_DECODED_SUB:
      case CROWCPU_DECODED_AND:
      case CROWCPU_DECODED_OR:
      case CROWCPU_DECODED_XOR:
      case CROWCPU_DECODED_SHIFT_LEFT:
      case CROWCPU_DECODED_SHIFT_RIGHT:
      case CROWCPU_DECODED_ROTATE_LEFT:
      case CROWCPU_DECODED_ROTATE_RIGHT:
        switch (instr.addressing) {
          case CROWCPU_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER:
            instr.payload.arithmetic_constant_reg.constant = new_address;
            break;

          case CROWCPU_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE:
            instr.payload.arithmetic_reg_constant.constant = new_address;
            break;

          default:
            fprintf(stderr, "invalid addressing mode\n");
            exit(1);
            break;
        }
        break;

      case CROWCPU_DECODED_NOOP:
      case CROWCPU_DECODED_NOT:
      case CROWCPU_DECODED_LOAD_GLOBAL_SEGMENT_TABLE:
      case CROWCPU_DECODED_LOAD_LOCAL_SEGMENT_TABLE:
      case CROWCPU_DECODED_LOAD_INTERRUPT_DESCRIPTOR_TABLE:
      case CROWCPU_DECODED_HALT:
        fprintf(stderr, "illegal instruction in linking procedure\n");
        exit(1);
        break;
    }

    *(uint32_t*)&file->sections[section].data[at] = crowcpu_arch_encode(&instr);
  } else {
    memcpy(
      &file->sections[section].data[at], &new_address, sizeof(new_address));
  }
}

static void
link_internal_references(poff_file_t* file)
{
  for (uint32_t i = 0; i < file->header.num_relocations; i++) {
    poff_relocation_t* rel = &file->relocations[i];
    poff_symbol_t* sym = &file->symbols[rel->symbol];

    // we only want to deal with declared symbols right now
    if (sym->type == POFF_SYMBOL_EXTERNAL)
      continue;

    apply_relocation(file,
                     rel->section,
                     rel->offset,
                     sym->offset +
                       file->sections[sym->section].ld.assigned_offset +
                       merged_sections[sym->section].paddr);
  }
}

static void
link_external_references(poff_file_t* file)
{
  for (uint32_t i = 0; i < file->header.num_relocations; i++) {
    poff_relocation_t* rel = &file->relocations[i];
    poff_symbol_t* sym = &file->symbols[rel->symbol];

    poff_file_t* res_file = NULL;
    poff_symbol_t* res_sym = NULL;

    // we only want to deal with nondeclared symbols right now
    if (sym->type != POFF_SYMBOL_EXTERNAL)
      continue;

    for (int i = 0; i < num_object_files; i++) {
      res_file = &files[i];
      res_sym = find_symbol(file, sym->name, sym->name_len, sym->offset);
      if (res_sym != NULL)
        break;
    }

    // locate the symbol definition
    apply_relocation(file,
                     rel->section,
                     rel->offset,
                     res_sym->offset +
                       res_file->sections[res_sym->section].ld.assigned_offset +
                       merged_sections[res_sym->section].paddr);
  }
}

static void
link()
{
  for (int i = 0; i < num_object_files; i++) {
    poff_file_t* file = &files[i];
    link_internal_references(file);
    link_external_references(file);
  }
}

static void
emit_prt()
{
  FILE* outfile = fopen("a.out", "w");

  poff_header_t header;
  memcpy(header.magic, poff_header_magic, 4);
  header.type = POFF_RUNTIME;
  header.num_symbols = 0;
  header.num_relocations = 0;

  poff_write_header(&header, outfile);

  for (poff_section_type section = 0; section < POFF_SECTION_SENTINEL;
       section++) {

    if (!script.enable_output[section])
      continue;

    for (int i = 0; i < num_object_files; i++) {
      poff_file_t* file = &files[i];
      poff_section_t* from = &file->sections[section];
      poff_write_section(from, outfile);
    }
  }

  fclose(outfile);
}

struct binary_sort_data
{
  uint32_t origin;
  poff_section_type type;
};

static int
sort_binary_data(void const* lhs_, void const* rhs_)
{
  struct binary_sort_data const* lhs = lhs_;
  struct binary_sort_data const* rhs = rhs_;

  return (lhs->origin < rhs->origin) ? -1 : 1;
}

static void
emit_binary()
{
  FILE* outfile = fopen("a.out", "w");

  struct binary_sort_data sort_data[POFF_SECTION_SENTINEL];

  for (poff_section_type section = 0; section < POFF_SECTION_SENTINEL;
       section++) {
    sort_data[section].type = section;
    sort_data[section].origin = merged_sections[section].paddr;
  }

  qsort(sort_data,
        POFF_SECTION_SENTINEL,
        sizeof(struct binary_sort_data),
        sort_binary_data);

  for (int i = 0; i < POFF_SECTION_SENTINEL; i++) {
    poff_section_type section = sort_data[i].type;
    for (int i = 0; i < num_object_files; i++) {
      poff_file_t* file = &files[i];
      poff_section_t* from = &file->sections[section];
      fwrite(from->data, from->size, 1, outfile);
    }
  }

  fclose(outfile);
}

int
main(int argc, char** argv)
{
  char c;
  while ((c = getopt(argc, argv, "+l:")) != -1) {
    switch (c) {
      case 'l':
        script = load_linker_script(optarg);
        use_script = 1;
        break;

      default:
        PANIC("unknown argument");
    }
  }

  num_object_files = argc - optind;

  if (num_object_files > MAX_OBJECT_FILES)
    PANIC("too many object files as input, max ");

  if (num_object_files == 0)
    PANIC("suppply at least one object file to link");

  // support up to four object files (eventually)

  for (int i = 0; i < num_object_files; i++)
    input_paths[i] = argv[i + optind];

  for (int i = 0; i < num_object_files; i++) {
    FILE* file = fopen(input_paths[i], "r");
    if (file == NULL)
      fprintf(stderr,
              "failed to open file %s, %s\n",
              input_paths[i],
              strerror(errno)),
        exit(1);

    poff_load(&files[i], file);

    fclose(file);
  }

  init_merged_sections();
  discover_symbol_collisions();
  calculate_merged_section_offsets();
  link();

  if (!use_script)
    emit_prt();
  else {
    switch (script.format) {
      case FORMAT_BINARY:
        emit_binary();
        break;

      case FORMAT_PRT:
        emit_prt();
        break;
    }
  }
}
