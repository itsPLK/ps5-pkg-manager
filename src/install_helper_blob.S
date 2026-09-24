/* Built separately, then embedded so deployment still needs just pkgmgr.elf. */
.section .rodata.install_helper,"a",@progbits
.balign 16
.global install_helper_elf
.global install_helper_elf_end
install_helper_elf:
.incbin "build/install-helper.elf"
install_helper_elf_end:
.section .note.GNU-stack,"",@progbits
