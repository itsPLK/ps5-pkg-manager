#ifdef __APPLE__
.section __TEXT,__const
.globl _install_helper_elf
.globl _install_helper_elf_end
_install_helper_elf:
.byte 0x7f, 'E', 'L', 'F'
_install_helper_elf_end:
#else
.section .rodata
.global install_helper_elf
.global install_helper_elf_end
install_helper_elf:
.byte 0x7f, 'E', 'L', 'F'
install_helper_elf_end:
.section .note.GNU-stack,"",@progbits
#endif
