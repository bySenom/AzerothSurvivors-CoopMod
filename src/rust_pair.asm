; Exact original Rust aggregate return: queue Arc pointers in RAX and RDX.
; This wrapper translates it into a C++ output pointer without guessing the
; MSVC aggregate-return convention.
EXTERN azs_new_pair_address:QWORD
.code
azs_create_pair PROC FRAME
    sub rsp, 38h
    .allocstack 38h
    .endprolog
    mov [rsp+20h], rcx
    call QWORD PTR [azs_new_pair_address]
    mov rcx, [rsp+20h]
    mov [rcx], rax
    mov [rcx+8], rdx
    mov rax, rcx
    add rsp, 38h
    ret
azs_create_pair ENDP
END
