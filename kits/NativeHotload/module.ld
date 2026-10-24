ENTRY(nlh_entry)
PHDRS { code PT_LOAD FLAGS(5); }
SECTIONS {
    . = 0;
    .text : { *(.text .text.*) } :code
    .rodata : { *(.rodata .rodata.*) } :code
    .forbidden : { *(.data .data.* .bss .bss.* COMMON .got .got.* .tdata .tbss .init_array .fini_array) } :code
    ASSERT(SIZEOF(.forbidden) == 0, "hot modules require explicit state; no globals/GOT/TLS/constructors")
    .symtab 0 : { *(.symtab) }
    .strtab 0 : { *(.strtab) }
    .shstrtab 0 : { *(.shstrtab) }
    /DISCARD/ : { *(.comment .note* .eh_frame* .llvm_addrsig) }
}
