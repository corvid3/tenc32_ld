; bare-metal/bios linker script

segments
{
	; name   rw  segmt length
	"code"   [x] @0x0  $0x1000
	"data"   [w] @0x1  $0x1000
}

sections 
{
	"code"   @0x00   >"code"
	"data"   @0x00   >"data"
	"rodata" @.      >"data"
	"entry"  @0xFFFE >"data"
}
