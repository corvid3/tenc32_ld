# defines output segments, which are
# converted into sections in the output file
# with some metadata tagged in the 'prt' section
segments
{
	"code" [rx] @0x00 $0x2000
	"data" [rw] @0x01 $0x2000
}

# defines which linked sections should be output,
# which segments the sections should be copied to,
# and to which offset in the segment they should be at.
# each entry starts with a name of a section that should be exported,
# followed by an optional offset into a segment, then by the segment name
# a section may not be declared multiple times
sections 
{
	"code"   @0x0000 >"code"
	"entry"  @0x0FFE >"code"
	"data"           >"data"
	"rodata"         >"data"
}

# first argument sets the code segment selector in the prt section
# second argument is looked up as a global symbol and marked as
#   the entry point to the program. must be located within the code segment
entry("code", "entry")

# argument sets the data segment selector in the prt section
data("data")

# argument sets the rodata segment selector in the prt section
# rodata("rodata")
