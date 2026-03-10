#include "crow.crowcpu_arch/crowcpu_arch.h"
#include "script.h"
#include "symbol.h"
#include "y.tab.h"
#include <assert.h>
#include <crow.POFF_c/POFF_c.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INPUT_FILES 8
#define ALIGNTO(x, y) ((y - (x % y)) % y)
#define PADTO(x, y) (x + ALIGNTO(x, y))

/* the symbol tables of each file get updated with the
 * newly calculated offset into the output file
 * so that relocations in other files know what
 * the actual offset will be in the final file.
 * however, when a file link edits a relocation within itself
 * to a symbol within itself, it needs to know
 * the absolute offset into its relative buffer.
 * we do this by storing the assigned offset of each section
 * and then subtracting it from the updated offset of the symbol and
 * relocation information */
struct input_file_section_misc
{
  unsigned assigned_offset;
};

struct input_file
{
  char const* name;
  poff_file_t filedata;
  struct input_file_section_misc* misc_section_data;
};

struct symbol_information
{
  char const* name;
  unsigned offset;
  unsigned segment_selector;
};

static struct input_file files[MAX_INPUT_FILES];
static unsigned num_files = 0;
extern FILE* yyin;

static void
assert_no_clashing_segment_selectors()
{
  struct script_segment* seg = script_segment_head;

  while (seg) {
    struct script_segment* seg2 = seg->next;
    while (seg2) {
      if (seg->selector == seg2->selector)
        fprintf(stderr,
                "overlapping segment selectors for segments <%s> and <%s>\n",
                seg->name,
                seg2->name),
          exit(1);

      seg2 = seg2->next;
    }
    seg = seg->next;
  }
}

// static void
// assert_no_overlapping_sections()
// {
//   struct script_section* section = script_section_head;

//   while (section) {
//     struct script_section* section2 = section->next;

//     while (section2) {
//       if (strcmp(section->name, section2->name) == 0)
//         continue;

//       section2 = section2->next;
//     }

//     section = section->next;
//   }
// }

/* assert that there are no relocations within any of the files
 * that point to sections that are not in the output file
 */
static void
assert_no_invalid_relocations()
{
  for (unsigned i = 0; i < num_files; i++) {
    struct input_file file = files[i];

    if (file.filedata.shortcuts.srel == NULL)
      continue;

    for (unsigned i = 0; i < file.filedata.shortcuts.srel->size; i++) {
      poff_relocation_t* rel = &file.filedata.shortcuts.srel->relocations[i];
      char const* section_name = file.filedata.section_names[rel->section].data;

      if (find_section(section_name) == NULL)
        fprintf(stderr,
                "input file %s contains relocation to non-output section %s\n",
                file.name,
                file.filedata.section_names[rel->section].data),
          exit(1);
    }
  }
}

static void
assert_symbol_collisions()
{
  for (unsigned i = 0; i < num_files; i++) {
    poff_strtab_data_t* strtab = files[i].filedata.shortcuts.strtab;
    poff_symtab_data_t* symtab = files[i].filedata.shortcuts.symtab;

    for (unsigned j = 0; j < symtab->size; j++) {
      poff_symbol_t sym = symtab->symbols[j];

      /* skip any extern references */
      if (sym.flags & POFF_SYMBOL_EXTERNAL)
        continue;

      for (unsigned k = i + 1; k < num_files; k++) {
        unsigned out;
        if (poff_find_symbol(&files[k].filedata,
                             strtab->strings[sym.name].data,
                             &out) == POFF_NO_SUCH_THING)
          continue;

        if (!(files[k].filedata.shortcuts.symtab->symbols[out].flags &
              POFF_SYMBOL_EXTERNAL))
          fprintf(stderr,
                  "redeclaration of symbol %s\n",
                  strtab->strings[sym.name].data),
            exit(1);
      }
    }
  }
}

void
instr_reloc(uint32_t* word, uint32_t addr)
{
  tenc32_arch_decoded_instruction instr;

  if (!tenc32_arch_decode(&instr, *word))
    fprintf(stderr, "invalid instruction: %i\n", *word), exit(1);

  switch (instr.instruction) {
    case TENC32_DECODED_MOVE:
      assert(instr.addressing == TENC32_ADDRESSING_MOVE_REG_IMM);
      instr.payload.move_reg_imm.imm = addr;
      break;

    case TENC32_DECODED_SUB:
      if (instr.addressing == TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER)
        instr.payload.arithmetic_constant_reg.lhs += addr;
      else if (instr.addressing ==
               TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE)
        instr.payload.arithmetic_reg_constant.rhs += addr;
      else
        assert(false);
      break;

    case TENC32_DECODED_ADD:
      if (instr.addressing == TENC32_ADDRESSING_ARITHMETIC_IMMEDIATE_REGISTER)
        instr.payload.arithmetic_constant_reg.lhs += addr;
      else if (instr.addressing ==
               TENC32_ADDRESSING_ARITHMETIC_REGISTER_IMMEDIATE)
        instr.payload.arithmetic_reg_constant.rhs += addr;
      else
        assert(false);
      break;

    case TENC32_DECODED_CALL:
      assert(instr.addressing == TENC32_ADDRESSING_CALL_IMMEDIATE);
      instr.payload.call_direct.offset = addr;
      break;

    case TENC32_DECODED_BRANCH_EQ:
    case TENC32_DECODED_BRANCH_NOTEQ:
    case TENC32_DECODED_BRANCH_IGREATER:
    case TENC32_DECODED_BRANCH_IGREATEREQ:
    case TENC32_DECODED_BRANCH_UGREATER:
    case TENC32_DECODED_BRANCH_UGREATEREQ:
    case TENC32_DECODED_BRANCH_ZERO:
    case TENC32_DECODED_BRANCH_NOT_ZERO:
      instr.payload.branch.ip_addend = (signed short)addr;
      break;

    case TENC32_DECODED_MUL:
    case TENC32_DECODED_SIGN_EXTEND:
    case TENC32_DECODED_AND:
    case TENC32_DECODED_OR:
    case TENC32_DECODED_XOR:
    case TENC32_DECODED_NOT:
    case TENC32_DECODED_SHIFT_LEFT:
    case TENC32_DECODED_SHIFT_RIGHT:
    case TENC32_DECODED_SYSJUMP:
    case TENC32_DECODED_HALT:
    case TENC32_DECODED_LOAD:
    case TENC32_DECODED_STORE:
    case TENC32_DECODED_SYSINT:
    case TENC32_DECODED_SYSINTRET:
    case TENC32_DECODED_LCR:
    case TENC32_DECODED_SCR:
      assert(false);
  }

  *word = tenc32_arch_encode(&instr);
}

static void
falignto(FILE* file, unsigned alignment)
{
  unsigned length = ftell(file);
  for (unsigned i = 0; i < (alignment - length % alignment) % alignment; i++)
    fputc(0, file);
}

static void
walistr(FILE* file, char const* what)
{
  unsigned len = strlen(what);
  fwrite(&len, sizeof len, 1, file);
  fwrite(what, 1, len, file);
  falignto(file, 4);
}

char const*
reltypestr(poff_relocation_type ty)
{
  switch (ty) {
    case POFF_RELOCATION_10c32_MEMORY:
      return "MEM";

    case POFF_RELOCATION_10c32_INSTR_HIGH:
      return "HI";

    case POFF_RELOCATION_10c32_INSTR_LOW:
      return "LO";

    case POFF_RELOCATION_10c32_INSTR_PC_DISTANCE:
      return "DIST";

    case POFF_RELOCATION_10c32_INSTR_PC_RELATIVE:
      return "REL";

    default:
      assert(false);
  }
}

[[maybe_unused]] static void
dump_relocations(poff_file_t* file)
{
  for (unsigned i = 0; i < file->shortcuts.srel->size; i++) {
    poff_relocation_t rel = file->shortcuts.srel->relocations[i];
    poff_symbol_t sym = file->shortcuts.symtab->symbols[rel.symbol];
    printf("REL: <%s.%s> -> <%s> %s\n",
           file->section_names[sym.section].data,
           file->shortcuts.strtab->strings[sym.name].data,
           file->section_names[rel.section].data,
           reltypestr(rel.type));
  }
}

static void
init_script(char const* filename)
{
  FILE* script_file = NULL;

  if (filename && access(filename, R_OK) == -1)
    fprintf(stderr, "unable to open linker script %s\n", filename), exit(1);

  if (filename)
    script_file = fopen(filename, "r");
  else
    fprintf(stderr, "expected linker script, not optional\n"), exit(1);
  assert(script_file);

  /* load the linker script */
  yyin = script_file;
  if (yyparse() == 1)
    exit(1);

  fclose(script_file);

  // script_dump();
}

static void
load_input_files(int argc, char** argv)
{
  for (int i = optind; i < argc; i++) {
    if (access(argv[i], R_OK) == -1)
      fprintf(stderr, "unable to open input file %s\n", argv[i]), exit(1);

    FILE* infile = fopen(argv[i], "r");
    assert(infile);
    struct input_file* f = &files[num_files++];
    f->name = argv[i];
    if (poff_read_file(&f->filedata, infile) != POFF_NO_ERROR)
      fprintf(stderr,
              "failed while trying to read contents of poff file %s\n",
              f->name),
        exit(1);
    fclose(infile);

    f->misc_section_data =
      malloc(sizeof *f->misc_section_data * f->filedata.header.num_sections);
    for (unsigned i = 0; i < f->filedata.header.num_sections; i++)
      f->misc_section_data[i].assigned_offset = 0;
  }
}

static void
calculate_offsets()
{
  for (struct script_section* sect = script_section_head; sect;
       sect = sect->next) {
    char const* name = sect->name;

    for (unsigned i = 0; i < num_files; i++) {
      struct input_file* file = &files[i];
      uint32_t idx;

      if (poff_find_section(&file->filedata, name, &idx) == POFF_NO_SUCH_THING)
        continue;

      /* for each symbol or relocation which references this
       * section, we must update their offsets by the current size
       * of the target segment.
       */

      if (file->filedata.shortcuts.symtab) {
        poff_symtab_data_t* symtab = file->filedata.shortcuts.symtab;
        for (unsigned j = 0; j < symtab->size; j++) {
          poff_symbol_t* symbol = &symtab->symbols[j];

          if (strcmp(file->filedata.section_names[symbol->section].data,
                     sect->name))
            continue;

          symbol->offset += sect->length;

          if (symbol->flags & POFF_SYMBOL_EXTERNAL) {
            declare_symbol(
              file->filedata.shortcuts.strtab->strings[symbol->name].data);
          } else if (symbol->flags & POFF_SYMBOL_GLOBAL) {
            define_visible_symbol(
              file->filedata.shortcuts.strtab->strings[symbol->name].data,
              file->filedata.section_names[symbol->section].data,
              symbol->offset);
          } else {
            define_static_symbol(
              file->filedata.shortcuts.strtab->strings[symbol->name].data,
              file->filedata.section_names[symbol->section].data,
              symbol->offset);
          }
        }
      }

      file->misc_section_data[idx].assigned_offset = sect->length;
      sect->length += file->filedata.sections[idx].header.size;
    }
  }
}

static void
relocate()
{
  for (unsigned i = 0; i < num_files; i++) {
    struct input_file* file = &files[i];

    poff_strtab_data_t* strtab = file->filedata.shortcuts.strtab;
    poff_symtab_data_t* symtab = file->filedata.shortcuts.symtab;
    poff_srel_data_t* srel = file->filedata.shortcuts.srel;

    for (unsigned i = 0; i < srel->size; i++) {
      poff_relocation_t rel = srel->relocations[i];

      assert(rel.offset % 4 == 0);

      poff_symbol_t* relsym = &symtab->symbols[rel.symbol];

      char const* rel_symbol_name = strtab->strings[relsym->name].data;
      char const* rel_symbol_section =
        file->filedata.section_names[relsym->section].data;
      char const* rel_into_section_name =
        file->filedata.section_names[rel.section].data;

      // printf("relocating <%s.%s> to <%s> with %s\n",
      //        rel_symbol_section,
      //        rel_symbol_name,
      //        rel_into_section_name,
      //        reltypestr(rel.type));

      /* for segments that which are not exported,
       * do not apply relocations if there is relocations from
       * a non-exported segment to a non-exported segment
       * however, it is an error to relocate a pointer
       * in an exported segment to a segment not exported
       */

      if (find_segment(rel_symbol_section) &&
          !find_segment(rel_into_section_name)) {
        fprintf(stderr,
                "invalid relocation to a non-exported section, %s to %s\n",
                rel_symbol_section,
                rel_into_section_name);
        exit(1);
      }

      /* if the section wasn't assigned an output segment,
       * then don't waste time relocating anything */
      if (!segment_for_section(rel_symbol_section))
        continue;

      struct symbol* sym;

      struct script_section* outsection = NULL;
      struct script_segment* segment = NULL;
      unsigned point_to = 0;

      /* static sym takes precedence */
      if ((sym = find_static_symbol(rel_symbol_name, rel_symbol_section))) {
        outsection = find_section(sym->section);
        assert(outsection);
        segment = find_segment(outsection->target_segment);
        assert(segment);

        TENC32_SET_SEGMENT(point_to, segment->selector);
        TENC32_SET_SEGMENT_OFFSET(point_to, outsection->offset + sym->offset);
      } else if ((sym = find_global_symbol(rel_symbol_name))) {
        outsection = find_section(sym->section);
        assert(outsection);
        segment = find_segment(outsection->target_segment);
        assert(segment);

        TENC32_SET_SEGMENT(point_to, segment->selector);
        TENC32_SET_SEGMENT_OFFSET(point_to, outsection->offset + sym->offset);
      } else {
        fprintf(stderr, "undefined symbol %s\n", rel_symbol_name);
        exit(1);
      }

      uint32_t* wordptr = (uint32_t*)&file->filedata.sections[rel.section]
                            .data.data.data[rel.offset];

      switch (rel.type) {
        case POFF_RELOCATION_10c32_MEMORY:
          *wordptr = point_to;
          break;

        case POFF_RELOCATION_10c32_INSTR_HIGH:
          instr_reloc(wordptr, point_to >> 16U);
          break;

        case POFF_RELOCATION_10c32_INSTR_LOW:
          instr_reloc(wordptr, point_to);
          break;

        case POFF_RELOCATION_10c32_INSTR_PC_DISTANCE:
        case POFF_RELOCATION_10c32_INSTR_PC_RELATIVE: {
          /* for pc_distance, the point to must be within the current
           * segment, and also must be within 16 bits of distance
           */
          struct script_section* instr_section =
            find_section(rel_into_section_name);
          struct script_segment* instr_resting_segment =
            find_segment(instr_section->target_segment);
          unsigned instr_resting_segment_id = instr_resting_segment->selector;
          unsigned pc = 0;
          TENC32_SET_SEGMENT(pc, instr_resting_segment_id);
          TENC32_SET_SEGMENT_OFFSET(
            pc,
            file->misc_section_data[rel.section].assigned_offset + rel.offset);

          if (TENC32_GET_SEGMENT(pc) != TENC32_GET_SEGMENT(point_to))
            fprintf(stderr,
                    "distance relocation happens between multiple segments (%s "
                    "to %s) (%i to %i)\n",
                    instr_resting_segment->name,
                    outsection->name,
                    TENC32_GET_SEGMENT(pc),
                    TENC32_GET_SEGMENT(point_to)),
              exit(1);

          if (rel.type == POFF_RELOCATION_10c32_INSTR_PC_DISTANCE)
            instr_reloc(wordptr, abs((signed)point_to - (signed)pc));
          else
            instr_reloc(wordptr, (signed)point_to - (signed)pc);
        } break;
      }
    }
  }
}

static void
padtil(FILE* outfile, unsigned const padding)
{
  for (unsigned i = 0; i < padding; i++)
    fputc(0xAF, outfile);
}

static void
emit(char const* outpath)
{
  FILE* outfile;

  if (outpath && access(outpath, W_OK) == -1)
    fprintf(stderr, "unable to open output filepath %s\n", outpath), exit(1);

  if (outpath)
    outfile = fopen(outpath, "w");
  else
    outfile = fopen("a.out", "w");

  assert(outfile);

  /* poff header */
  {
    poff_header_t aout_hdr;
    memcpy(aout_hdr.magic, poff_header_magic, sizeof poff_header_magic);
    aout_hdr.version = THIS_POFF_VERSION;
    aout_hdr.flags = POFF_HEADER_IS_EXECUTABLE;
    aout_hdr.num_sections = 1 + num_segments();
    aout_hdr.arch = POFF_ARCH_10c32;
    memset(aout_hdr.reserved, 0, sizeof aout_hdr.reserved);

    poff_write_header(&aout_hdr, outfile);
  }

  /* section names */
  {
    /* prt shall be the first section in the poff file */
    walistr(outfile, "prt");

    struct script_segment* output_segments = script_segment_head;
    for (; output_segments; output_segments = output_segments->next)
      walistr(outfile, output_segments->name);
  }

  /* section headers */
  {
    falignto(outfile, POFF_SECTION_HEADERS_ALIGNMENT);

    unsigned counter = ftell(outfile);
    counter += (1 + num_segments()) * POFF_SECTION_HEADER_BYTESIZE;

    /* prt header */
    {
      poff_section_header_t prt;
      prt.size = POFF_10c32_RUNTIME_DATA_BYTESIZE;
      prt.offset = counter;
      prt.section_type = POFF_SECTION_META;
      memset(prt.reserved, 0, sizeof prt.reserved);

      counter += PADTO(prt.size, 32);
      poff_write_section_header(&prt, outfile);
    }

    /* generated headers */
    for (struct script_segment* segment = script_segment_head; segment;
         segment = segment->next) {
      poff_section_header_t hdr;
      hdr.offset = counter;
      hdr.section_type = POFF_SECTION_META;
      memset(hdr.reserved, 0, sizeof hdr.reserved);

      if (segment->capacity != -1u)
        hdr.size = segment->capacity;
      else {
        if (segment->num_consumption == 0)
          hdr.size = 0;
        else {
          hdr.size =
            segment->consumption[segment->num_consumption - 1]->offset +
            segment->consumption[segment->num_consumption - 1]->length;
        }
      }

      counter += PADTO(hdr.size, 32);
      poff_write_section_header(&hdr, outfile);
    }
  }

  /* section payloads */
  {
    /* prt payload */
    {
      falignto(outfile, 32);

      unsigned entry = -1, code_section = -1, code = -1, data_section = -1,
               data = -1, rodata_section = -1, rodata = -1;

      if (code_segment_name) {
        struct script_segment* seg = find_segment(code_segment_name);
        if (!seg)
          fprintf(
            stderr,
            "runtime code segment defined as segment %s, but is undefined\n",
            code_segment_name),
            exit(1);

        if (segment_index(code_segment_name, &code_section)) {
          code_section++;
          code = seg->selector;
        }
      }

      if (data_segment_name) {
        struct script_segment* seg = find_segment(data_segment_name);
        if (!seg)
          fprintf(
            stderr,
            "runtime data segment defined as segment %s, but is undefined\n",
            data_segment_name),
            exit(1);
        if (segment_index(data_segment_name, &data_section)) {
          data_section++;
          data = seg->selector;
        }
      }

      if (rodata_segment_name) {
        struct script_segment* seg = find_segment(rodata_segment_name);
        if (!seg)
          fprintf(
            stderr,
            "runtime rodata segment defined as segment %s, but is undefined\n",
            rodata_segment_name),
            exit(1);
        if (segment_index(rodata_segment_name, &rodata_section)) {
          rodata_section++;
          rodata = seg->selector;
        }
      }

      struct symbol* entry_sym = find_global_symbol(entry_symbol_name);

      if (!entry_sym)
        fprintf(stderr, "entry symbol <%s> undeclared\n", entry_symbol_name),
          exit(1);
      if (!is_global_symbol_defined(entry_symbol_name))
        fprintf(stderr, "entry symbol <%s> undefined\n", entry_symbol_name),
          exit(1);

      struct script_section* section_sym = find_section(entry_sym->section);
      entry = entry_sym->offset;

      if (strcmp(code_segment_name, section_sym->target_segment) != 0) {
        fprintf(stderr,
                "entry symbol %s does not appear in the code segment %s, "
                "rather it appears in %s\n",
                entry_symbol_name,
                code_segment_name,
                entry_sym->section),
          exit(1);
      }

      fwrite(&entry, sizeof entry, 1, outfile);
      fwrite(&code_section, sizeof code_section, 1, outfile);
      fwrite(&code, sizeof code, 1, outfile);
      fwrite(&data_section, sizeof data_section, 1, outfile);
      fwrite(&data, sizeof data, 1, outfile);
      fwrite(&rodata_section, sizeof rodata_section, 1, outfile);
      fwrite(&rodata, sizeof rodata, 1, outfile);
    }

    /* generated segments */
    for (struct script_segment* segment = script_segment_head; segment;
         segment = segment->next) {
      unsigned begin = ftell(outfile);
      falignto(outfile, 32);
      for (unsigned i = 0; i < segment->num_consumption; i++) {
        struct script_section* sec = segment->consumption[i];
        printf("OUTPUT SECTION: %s\n", sec->name);

        for (unsigned i = 0; i < num_files; i++) {
          uint32_t idx = 0;
          if (poff_find_section(&files[i].filedata, sec->name, &idx) !=
              POFF_NO_ERROR)
            continue;
          fwrite(files[i].filedata.sections[idx].data.data.data,
                 1,
                 files[i].filedata.sections[idx].header.size,
                 outfile);
        }
        if (i < segment->num_consumption - 1) {
          struct script_section* nextsec = segment->consumption[i + 1];
          padtil(outfile, nextsec->offset - (sec->offset + sec->length));
        }
      }

      unsigned end = ftell(outfile);
      if (segment->capacity != -1u) {
        unsigned gap = segment->capacity - (end - begin);
        for (unsigned i = 0; i < gap; i++)
          fputc(0, outfile);
        falignto(outfile, 32);
      }
    }
  }
}

int
main(int argc, char** argv)
{
  char c;
  char const* outpath = NULL;
  char const* linker_script_filename = NULL;

  while ((c = getopt(argc, argv, "o:L:")) != -1) {
    switch (c) {
      case 'o':
        outpath = optarg;
        break;

      case 'L':
        linker_script_filename = optarg;
        break;

      case '?':
        fprintf(stderr, "unknown argument %c\n", optopt);
        exit(1);
    }
  }

  if (optind == argc)
    fprintf(stderr, "expected at least one input file\n"), exit(1);

  init_script(linker_script_filename);
  load_input_files(argc, argv);

  init_symbol_table();
  assert_no_clashing_segment_selectors();
  assert_no_invalid_relocations();
  assert_symbol_collisions();

  calculate_offsets();
  detect_section_overlaps();

  relocate();
  emit(outpath);
}
