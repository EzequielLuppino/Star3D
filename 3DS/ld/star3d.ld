/* Star 3D: devkitARM's 3dsx.ld with the game's memory grouped together.
   The data and bss of the objects that make up the game state (foxhollow's
   code and the platform objects it keeps pointers into) are placed between
   __game_data_start/end and __game_bss_start/end, so a save state can copy
   them as two blocks (port/savestate_3ds.c). */
/*--------------------------------------------------------------------------------
	This Source Code Form is subject to the terms of the Mozilla Public License,
	v. 2.0. If a copy of the MPL was not distributed with this file, You can
	obtain one at https://mozilla.org/MPL/2.0/.
--------------------------------------------------------------------------------*/
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

PHDRS
{
	code   PT_LOAD FLAGS(5) /* Read | Execute */;
	rodata PT_LOAD FLAGS(4) /* Read */;
	data   PT_LOAD FLAGS(6) /* Read | Write */;
}

SECTIONS
{
	/* =========== CODE section =========== */

	PROVIDE(__start__ = 0x100000);
	. = __start__;

	.text ALIGN(0x1000) :
	{
		/* .init */
		KEEP( *(.crt0) )
		KEEP( *(.init) )
		. = ALIGN(4);

		/* .text */
		*(.text)
		*(.text.*)
		*(.glue_7)
		*(.glue_7t)
		*(.stub)
		*(.gnu.warning)
		*(.gnu.linkonce.t*)
		. = ALIGN(4);

		/* .fini */
		KEEP( *(.fini) )
		. = ALIGN(4);
	} : code
	__text_end = .;

	/* =========== RODATA section =========== */

	. = ALIGN(0x1000);
	__rodata_start = .;

	.rodata :
	{
		*(.rodata)
		*(.roda)
		*(.rodata.*)
		*all.rodata*(*)
		*(.gnu.linkonce.r*)
		SORT(CONSTRUCTORS)
		. = ALIGN(4);
		__tdata_align = .;
		LONG (ALIGNOF(.tdata));
		. = ALIGN(4);
	} : rodata

	.ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } : rodata
	__exidx_start = .;
	.ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } : rodata
	__exidx_end = .;

	/* =========== DATA section =========== */

	. = ALIGN(0x1000);

	.data :
	{
		__game_data_start = .;
		*/fox/game/*(.data .data.*)
		*/fox/patched/*(.data .data.*)
		*/fox/port/*(.data .data.*)
		*/fox/musyx/*(.data .data.*)
		*/fox/mtx/*(.data .data.*)
		*/3ds/stubs/*(.data .data.*)
		*/port/rand.o(.data .data.*)
		*/port/os_3ds.o(.data .data.*)
		*/port/ar_3ds.o(.data .data.*)
		*/port/snapmem_3ds.o(.data .data.*)
		. = ALIGN(4);
		__game_data_end = .;
		*(.data)
		*(.data.*)
		*(.gnu.linkonce.d*)
		CONSTRUCTORS
		. = ALIGN(4);
	} : data

	.tdata : ALIGN(4)
	{
		__tdata_lma = .;
		*(.tdata)
		*(.tdata.*)
		*(.gnu.linkonce.td.*)
		. = ALIGN(4);
		__tdata_lma_end = .;
	} : data

	.tbss : ALIGN(4)
	{
		*(.tbss)
		*(.tbss.*)
		*(.gnu.linkonce.tb.*)
		*(.tcommon)
		. = ALIGN(4);
	} : data

	/*
    __tdata_align_val = ABSOLUTE(ALIGNOF(.tdata));
    __tdata_size = ABSOLUTE(SIZEOF(.tdata));
    __tbss_align = ABSOLUTE(ALIGNOF(.tbss));
    __tbss_size = ABSOLUTE(SIZEOF(.tbss));
	*/

	.preinit_array ALIGN(4) :
	{
		PROVIDE (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE (__preinit_array_end = .);
	} : data

	.init_array ALIGN(4) :
	{
		PROVIDE (__init_array_start = .);
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		PROVIDE (__init_array_end = .);
	} : data

	.fini_array ALIGN(4) :
	{
		PROVIDE (__fini_array_start = .);
		KEEP (*(.fini_array))
		KEEP (*(SORT(.fini_array.*)))
		PROVIDE (__fini_array_end = .);
	} : data

	.ctors ALIGN(4) :
	{
		KEEP (*crtbegin.o(.ctors)) /* MUST be first -- GCC requires it */
		KEEP (*(EXCLUDE_FILE (*crtend.o) .ctors))
		KEEP (*(SORT(.ctors.*)))
		KEEP (*(.ctors))
	} : data

	.dtors ALIGN(4) :
	{
		KEEP (*crtbegin.o(.dtors))
		KEEP (*(EXCLUDE_FILE (*crtend.o) .dtors))
		KEEP (*(SORT(.dtors.*)))
		KEEP (*(.dtors))
	} : data

	__bss_start__ = .;
	.bss ALIGN(4) :
	{
		*(.dynbss)
		__game_bss_start = .;
		*/fox/game/*(.bss .bss.* COMMON)
		*/fox/patched/*(.bss .bss.* COMMON)
		*/fox/port/*(.bss .bss.* COMMON)
		*/fox/musyx/*(.bss .bss.* COMMON)
		*/fox/mtx/*(.bss .bss.* COMMON)
		*/3ds/stubs/*(.bss .bss.* COMMON)
		*/port/rand.o(.bss .bss.* COMMON)
		*/port/os_3ds.o(.bss .bss.* COMMON)
		*/port/ar_3ds.o(.bss .bss.* COMMON)
		*/port/snapmem_3ds.o(.bss .bss.* COMMON)
		. = ALIGN(4);
		__game_bss_end = .;
		*(.bss)
		*(.bss.*)
		*(.gnu.linkonce.b*)
		*(COMMON)

		/* Reserve space for the TLS segment of the main thread.
		   We need (__tls_start - 8) to be aligned the same as .tdata, to account for
		   the 8-byte ARM TLS header. Since the header is not actually used for
		   ARM_TLS_LE32 relocation, we just fake it by subtracting 8 from the data
		   offset.
		 */
		. = 8 + ABSOLUTE(ALIGN(ABSOLUTE(. - 8), MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))));
		__tls_start = .;
		. += SIZEOF(.tdata);

        /* Ensure the alignment of .tbss is accounted for in the total size,
           since SIZEOF(.tdata) doesn't include any padding before .tbss.
         */
        . = ALIGN(ALIGNOF(.tbss));
        . += SIZEOF(.tbss);
		__tls_end = .;
	} : data
	__bss_end__ = .;

	__end__ = ABSOLUTE(.) ;

	/* ==================
	   ==== Metadata ====
	   ================== */

	/* Discard sections that difficult post-processing */
	/DISCARD/ : { *(.group .comment .note) }

	/* Stabs debugging sections. */
	.stab          0 : { *(.stab) }
	.stabstr       0 : { *(.stabstr) }
	.stab.excl     0 : { *(.stab.excl) }
	.stab.exclstr  0 : { *(.stab.exclstr) }
	.stab.index    0 : { *(.stab.index) }
	.stab.indexstr 0 : { *(.stab.indexstr) }

	/* DWARF debug sections.
	   Symbols in the DWARF debugging sections are relative to the beginning
	   of the section so we begin them at 0. */

	/* DWARF 1 */
	.debug          0 : { *(.debug) }
	.line           0 : { *(.line) }

	/* GNU DWARF 1 extensions */
	.debug_srcinfo  0 : { *(.debug_srcinfo) }
	.debug_sfnames  0 : { *(.debug_sfnames) }

	/* DWARF 1.1 and DWARF 2 */
	.debug_aranges  0 : { *(.debug_aranges) }
	.debug_pubnames 0 : { *(.debug_pubnames) }

	/* DWARF 2 */
	.debug_info     0 : { *(.debug_info) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }
	.debug_macinfo  0 : { *(.debug_macinfo) }
}
