#include "include/ir.h"
#include <algorithm>
#include <sstream>

namespace {

class Runtime {
    std::ostringstream out;
    std::string name;
    std::vector<std::string> needed;
    bool active = false, leaf = false;

    bool has(const std::string &helper) const {
        return std::find(needed.begin(), needed.end(), helper) != needed.end();
    }

    void require(const std::string &helper) {
        if (!has(helper)) needed.push_back(helper);
    }
public:
    // Include only helpers called by this program or another selected helper.
    Runtime(const std::vector<std::string> &helpers) : needed(helpers) {
        if (has("printf"))
            for (const auto &type : {"int", "uint", "float", "char", "string"}) require("print_" + std::string(type));
        if (has("scanf"))
            for (const auto &helper : {"read_int", "read_float", "read_string", "getchar", "skip_space"}) require(helper);
        if (has("snapshot")) { require("malloc"); require("copy"); }
        if (has("rewind")) { require("free"); require("copy"); }
        if (has("history_clear")) require("free");
        out << ".data\n.align 2\n";
        if (has("malloc") || has("free")) out << "__mv_rt_free_head: .word 0\n";
        if (has("getchar")) out << "__mv_rt_pushback: .word -2\n__mv_rt_input_byte: .space 1\n";
        if (has("printf") || has("scanf"))
            out << "__mv_rt_format_message: .asciiz \"minerva: unsupported format\\n\"\n";
        out << ".text\n";
        if (has("printf") || has("scanf")) out << R"(
__mv_rt_format_error:
    la $a0, __mv_rt_format_message
    li $v0, 4
    syscall
    li $a0, 1
    li $v0, 17
    syscall
)";
    }

    void begin(const std::string &suffix, bool isLeaf = false) {
        active = has(suffix); leaf = isLeaf;
        if (!active) return;
        name = "__mv_rt_" + suffix;
        out << name << ":\n";
        if (leaf) return;
        out << "    addiu $sp, $sp, -64\n    sw $fp, 56($sp)\n"
            << "    sw $ra, 60($sp)\n    move $fp, $sp\n";
        for (int i = 0; i < 8; ++i) out << "    sw $s" << i << ", " << i * 4 << "($fp)\n";
    }

    void arg(const std::string &reg, int index) {
        if (active) out << "    lw " << reg << ", " << (leaf ? 0 : 64) + index * 4 << (leaf ? "($sp)\n" : "($fp)\n");
    }
    void code(const std::string &body) { if (active) out << body; }

    void end() {
        if (!active) return;
        if (leaf) { out << "    jr $ra\n    nop\n"; return; }
        out << name << "_return:\n";
        for (int i = 0; i < 8; ++i) out << "    lw $s" << i << ", " << i * 4 << "($fp)\n";
        out << "    lw $ra, 60($fp)\n    lw $t9, 56($fp)\n    addiu $sp, $fp, 64\n"
            << "    move $fp, $t9\n    jr $ra\n    nop\n";
    }

    std::string str() const { return out.str(); }
};

// The unsigned printer builds decimal digits backwards in its frame.
void emitPrinting(Runtime &r) {
    const char *names[] = {"int", "float", "char", "string"};
    int services[] = {1, 2, 11, 4};
    for (int i = 0; i < 4; ++i) {
        r.begin(std::string("print_") + names[i], true); r.arg("$t0", 0);
        r.code(i == 1 ? "    mtc1 $t0, $f12\n" : "    move $a0, $t0\n");
        r.code("    li $v0, " + std::to_string(services[i]) + "\n    syscall\n    move $v0, $zero\n");
        r.end();
    }
    r.begin("print_uint"); r.arg("$t0", 0);
    r.code(R"(    addiu $t1, $fp, 47
    sb $zero, 0($t1)
    li $t2, 10
__mv_rt_uint_digits:
    divu $t0, $t2
    mflo $t0
    mfhi $t3
    addiu $t3, $t3, 48
    addiu $t1, $t1, -1
    sb $t3, 0($t1)
    bne $t0, $zero, __mv_rt_uint_digits
    nop
    move $a0, $t1
    li $v0, 4
    syscall
    move $v0, $zero
)"); r.end();
}

// Dynamic formats share the same scalar printers as literal formats.
void emitPrintf(Runtime &r) {
    r.begin("printf"); r.arg("$s0", 0);
    r.code(R"(    addiu $s1, $fp, 68
__mv_rt_printf_loop:
    lbu $t0, 0($s0)
    beq $t0, $zero, __mv_rt_printf_done
    nop
    addiu $s0, $s0, 1
    li $t1, 37
    bne $t0, $t1, __mv_rt_printf_character
    nop
    lbu $s2, 0($s0)
    addiu $s0, $s0, 1
    beq $s2, $t1, __mv_rt_printf_percent
    nop
    lw $t0, 0($s1)
    addiu $s1, $s1, 4
    li $t1, 100
    la $t3, __mv_rt_print_int
    beq $s2, $t1, __mv_rt_printf_action
    nop
    li $t1, 117
    la $t3, __mv_rt_print_uint
    beq $s2, $t1, __mv_rt_printf_action
    nop
    li $t1, 102
    la $t3, __mv_rt_print_float
    beq $s2, $t1, __mv_rt_printf_action
    nop
    li $t1, 99
    la $t3, __mv_rt_print_char
    beq $s2, $t1, __mv_rt_printf_action
    nop
    li $t1, 115
    la $t3, __mv_rt_print_string
    bne $s2, $t1, __mv_rt_format_error
    nop
__mv_rt_printf_action:
    addiu $sp, $sp, -8
    sw $t0, 0($sp)
    jalr $t3
    nop
    addiu $sp, $sp, 8
    b __mv_rt_printf_loop
    nop
__mv_rt_printf_percent:
    li $t0, 37
__mv_rt_printf_character:
    move $a0, $t0
    li $v0, 11
    syscall
    b __mv_rt_printf_loop
    nop
__mv_rt_printf_done:
    move $v0, $zero
)"); r.end();
}

// The allocator reuses whole blocks and grows through SPIM sbrk.
void emitAllocator(Runtime &r) {
    r.begin("malloc"); r.arg("$s0", 0);
    r.code(R"(    blez $s0, __mv_rt_malloc_null
    nop
    li $t0, 2147483624
    sltu $t1, $t0, $s0
    bne $t1, $zero, __mv_rt_malloc_null
    nop
    addiu $s0, $s0, 7
    li $t0, -8
    and $s0, $s0, $t0
    lw $t0, __mv_rt_free_head
    move $t1, $zero
__mv_rt_malloc_search:
    beq $t0, $zero, __mv_rt_malloc_grow
    nop
    lw $t2, 0($t0)
    sltu $t2, $t2, $s0
    beq $t2, $zero, __mv_rt_malloc_found
    nop
    move $t1, $t0
    lw $t0, 4($t0)
    b __mv_rt_malloc_search
    nop
__mv_rt_malloc_found:
    lw $t2, 4($t0)
    beq $t1, $zero, __mv_rt_malloc_head
    nop
    sw $t2, 4($t1)
    b __mv_rt_malloc_result
    nop
__mv_rt_malloc_head:
    sw $t2, __mv_rt_free_head
__mv_rt_malloc_result:
    addiu $v0, $t0, 8
    b __mv_rt_malloc_return
    nop
__mv_rt_malloc_grow:
    addiu $a0, $s0, 15
    li $v0, 9
    syscall
    blez $v0, __mv_rt_malloc_null
    nop
    addiu $t0, $v0, 7
    li $t1, -8
    and $t0, $t0, $t1
    sw $s0, 0($t0)
    sw $zero, 4($t0)
    b __mv_rt_malloc_result
    nop
__mv_rt_malloc_null:
    move $v0, $zero
)"); r.end();
    r.begin("free"); r.arg("$t0", 0);
    r.code(R"(    beq $t0, $zero, __mv_rt_free_done
    nop
    addiu $t0, $t0, -8
    lw $t1, __mv_rt_free_head
    sw $t1, 4($t0)
    sw $t0, __mv_rt_free_head
__mv_rt_free_done:
    move $v0, $zero
)"); r.end();
}

// Histories own allocator nodes containing a next pointer and object bytes.
void emitHistory(Runtime &r) {
    r.begin("copy"); r.arg("$t0", 0); r.arg("$t1", 1); r.arg("$t2", 2);
    r.code(R"(    blez $t2, __mv_rt_copy_done
    nop
    beq $t0, $t1, __mv_rt_copy_done
    nop
__mv_rt_copy_loop:
    lbu $t3, 0($t1)
    sb $t3, 0($t0)
    addiu $t0, $t0, 1
    addiu $t1, $t1, 1
    addiu $t2, $t2, -1
    bne $t2, $zero, __mv_rt_copy_loop
    nop
__mv_rt_copy_done:
    move $v0, $zero
)"); r.end();
    r.begin("snapshot"); r.arg("$s0", 0); r.arg("$s1", 1); r.arg("$s2", 2);
    r.code(R"(    addiu $t0, $s1, 4
    addiu $sp, $sp, -8
    sw $t0, 0($sp)
    jal __mv_rt_malloc
    nop
    addiu $sp, $sp, 8
    move $s3, $v0
    beq $s3, $zero, __mv_rt_snapshot_done
    nop
    lw $t0, 0($s2)
    sw $t0, 0($s3)
    addiu $t0, $s3, 4
    addiu $sp, $sp, -16
    sw $t0, 0($sp)
    sw $s0, 4($sp)
    sw $s1, 8($sp)
    jal __mv_rt_copy
    nop
    addiu $sp, $sp, 16
    sw $s3, 0($s2)
__mv_rt_snapshot_done:
    move $v0, $zero
)"); r.end();
    r.begin("rewind"); r.arg("$s0", 0); r.arg("$s1", 1); r.arg("$s2", 2); r.arg("$s3", 3);
    r.code(R"(__mv_rt_rewind_loop:
    blez $s3, __mv_rt_rewind_done
    nop
    lw $s4, 0($s2)
    beq $s4, $zero, __mv_rt_rewind_done
    nop
    addiu $t0, $s4, 4
    addiu $sp, $sp, -16
    sw $s0, 0($sp)
    sw $t0, 4($sp)
    sw $s1, 8($sp)
    jal __mv_rt_copy
    nop
    addiu $sp, $sp, 16
    lw $t0, 0($s4)
    sw $t0, 0($s2)
    addiu $sp, $sp, -8
    sw $s4, 0($sp)
    jal __mv_rt_free
    nop
    addiu $sp, $sp, 8
    addiu $s3, $s3, -1
    b __mv_rt_rewind_loop
    nop
__mv_rt_rewind_done:
    move $v0, $zero
)"); r.end();
    r.begin("history_clear"); r.arg("$s0", 0);
    r.code(R"(__mv_rt_history_clear_loop:
    lw $s1, 0($s0)
    beq $s1, $zero, __mv_rt_history_clear_done
    nop
    lw $t0, 0($s1)
    sw $t0, 0($s0)
    addiu $sp, $sp, -8
    sw $s1, 0($sp)
    jal __mv_rt_free
    nop
    addiu $sp, $sp, 8
    b __mv_rt_history_clear_loop
    nop
__mv_rt_history_clear_done:
    move $v0, $zero
)"); r.end();
}

// One shared pushback slot keeps later conversions on the same input line.
void emitInput(Runtime &r) {
    r.begin("getchar");
    r.code(R"(    lw $v0, __mv_rt_pushback
    li $t0, -2
    beq $v0, $t0, __mv_rt_getchar_read
    nop
    sw $t0, __mv_rt_pushback
    b __mv_rt_getchar_return
    nop
__mv_rt_getchar_read:
    li $a0, 0
    la $a1, __mv_rt_input_byte
    li $a2, 1
    li $v0, 14
    syscall
    blez $v0, __mv_rt_getchar_eof
    nop
    lbu $v0, __mv_rt_input_byte
    b __mv_rt_getchar_return
    nop
__mv_rt_getchar_eof:
    li $v0, -1
)"); r.end();
    r.begin("skip_space");
    r.code(R"(__mv_rt_skip_space_loop:
    jal __mv_rt_getchar
    nop
    li $t0, 32
    beq $v0, $t0, __mv_rt_skip_space_loop
    nop
    addiu $t0, $v0, -9
    sltiu $t0, $t0, 5
    bne $t0, $zero, __mv_rt_skip_space_loop
    nop
)"); r.end();
}

// Decimal integer accumulation preserves unsigned bits and optional signs.
void emitReadInteger(Runtime &r) {
    r.begin("read_int"); r.arg("$s0", 0);
    r.code(R"(    move $s1, $zero
    move $s2, $zero
    move $s3, $zero
    jal __mv_rt_skip_space
    nop
    move $s4, $v0
    li $t0, 45
    bne $s4, $t0, __mv_rt_read_int_plus
    nop
    li $s2, 1
    b __mv_rt_read_int_sign
    nop
__mv_rt_read_int_plus:
    li $t0, 43
    bne $s4, $t0, __mv_rt_read_int_loop
    nop
__mv_rt_read_int_sign:
    jal __mv_rt_getchar
    nop
    move $s4, $v0
__mv_rt_read_int_loop:
    addiu $t0, $s4, -48
    sltiu $t1, $t0, 10
    beq $t1, $zero, __mv_rt_read_int_finish
    nop
    li $t1, 10
    multu $s1, $t1
    mflo $s1
    addu $s1, $s1, $t0
    addiu $s3, $s3, 1
    jal __mv_rt_getchar
    nop
    move $s4, $v0
    b __mv_rt_read_int_loop
    nop
__mv_rt_read_int_finish:
    sw $s4, __mv_rt_pushback
    move $v0, $zero
    beq $s3, $zero, __mv_rt_read_int_return
    nop
    beq $s2, $zero, __mv_rt_read_int_store
    nop
    subu $s1, $zero, $s1
__mv_rt_read_int_store:
    sw $s1, 0($s0)
    li $v0, 1
)"); r.end();
}

// Floats use a decimal mantissa and an optional signed exponent.
void emitReadFloat(Runtime &r) {
    r.begin("read_float"); r.arg("$s0", 0);
    r.code(R"(    move $s1, $zero
    move $s2, $zero
    move $s3, $zero
    move $s4, $zero
    move $s6, $zero
    move $s7, $zero
    jal __mv_rt_skip_space
    nop
    move $s5, $v0
    li $t0, 45
    bne $s5, $t0, __mv_rt_read_float_plus
    nop
    li $s3, 1
    b __mv_rt_read_float_sign
    nop
__mv_rt_read_float_plus:
    li $t0, 43
    bne $s5, $t0, __mv_rt_read_float_mantissa
    nop
__mv_rt_read_float_sign:
    jal __mv_rt_getchar
    nop
    move $s5, $v0
__mv_rt_read_float_mantissa:
    addiu $t0, $s5, -48
    sltiu $t1, $t0, 10
    beq $t1, $zero, __mv_rt_read_float_dot
    nop
    mtc1 $s1, $f0
    li $t1, 0x41200000
    mtc1 $t1, $f2
    mul.s $f0, $f0, $f2
    mtc1 $t0, $f2
    cvt.s.w $f2, $f2
    add.s $f0, $f0, $f2
    mfc1 $s1, $f0
    addu $s2, $s2, $s6
    addiu $s4, $s4, 1
    b __mv_rt_read_float_next
    nop
__mv_rt_read_float_dot:
    li $t0, 46
    bne $s5, $t0, __mv_rt_read_float_exponent
    nop
    bne $s6, $zero, __mv_rt_read_float_finish
    nop
    li $s6, 1
__mv_rt_read_float_next:
    jal __mv_rt_getchar
    nop
    move $s5, $v0
    b __mv_rt_read_float_mantissa
    nop
__mv_rt_read_float_exponent:
    beq $s4, $zero, __mv_rt_read_float_failure
    nop
    li $t0, 101
    beq $s5, $t0, __mv_rt_read_float_exp_start
    nop
    li $t0, 69
    bne $s5, $t0, __mv_rt_read_float_finish
    nop
__mv_rt_read_float_exp_start:
    sw $zero, 32($fp)
    sw $zero, 36($fp)
    jal __mv_rt_getchar
    nop
    move $s5, $v0
    li $t0, 45
    bne $s5, $t0, __mv_rt_read_float_exp_plus
    nop
    li $t0, 1
    sw $t0, 32($fp)
    b __mv_rt_read_float_exp_sign
    nop
__mv_rt_read_float_exp_plus:
    li $t0, 43
    bne $s5, $t0, __mv_rt_read_float_exp_loop
    nop
__mv_rt_read_float_exp_sign:
    jal __mv_rt_getchar
    nop
    move $s5, $v0
__mv_rt_read_float_exp_loop:
    addiu $t0, $s5, -48
    sltiu $t1, $t0, 10
    beq $t1, $zero, __mv_rt_read_float_exp_end
    nop
    li $t1, 10
    multu $s7, $t1
    mflo $s7
    addu $s7, $s7, $t0
    sltiu $t1, $s7, 64
    bne $t1, $zero, __mv_rt_read_float_exp_count
    nop
    li $s7, 64
__mv_rt_read_float_exp_count:
    lw $t0, 36($fp)
    addiu $t0, $t0, 1
    sw $t0, 36($fp)
    jal __mv_rt_getchar
    nop
    move $s5, $v0
    b __mv_rt_read_float_exp_loop
    nop
__mv_rt_read_float_exp_end:
    lw $t0, 36($fp)
    beq $t0, $zero, __mv_rt_read_float_failure
    nop
    lw $t0, 32($fp)
    beq $t0, $zero, __mv_rt_read_float_finish
    nop
    subu $s7, $zero, $s7
__mv_rt_read_float_finish:
    beq $s4, $zero, __mv_rt_read_float_failure
    nop
    sw $s5, __mv_rt_pushback
    subu $s7, $s7, $s2
    mtc1 $s1, $f0
    li $t0, 0x41200000
    mtc1 $t0, $f2
__mv_rt_read_float_scale:
    beq $s7, $zero, __mv_rt_read_float_store
    nop
    bltz $s7, __mv_rt_read_float_divide
    nop
    mul.s $f0, $f0, $f2
    addiu $s7, $s7, -1
    b __mv_rt_read_float_scale
    nop
__mv_rt_read_float_divide:
    div.s $f0, $f0, $f2
    addiu $s7, $s7, 1
    b __mv_rt_read_float_scale
    nop
__mv_rt_read_float_store:
    beq $s3, $zero, __mv_rt_read_float_write
    nop
    neg.s $f0, $f0
__mv_rt_read_float_write:
    swc1 $f0, 0($s0)
    li $v0, 1
    b __mv_rt_read_float_return
    nop
__mv_rt_read_float_failure:
    sw $s5, __mv_rt_pushback
    move $v0, $zero
)"); r.end();
}

// Bounded strings stop before whitespace and retain the following character.
void emitReadString(Runtime &r) {
    r.begin("read_string"); r.arg("$s0", 0); r.arg("$s1", 1);
    r.code(R"(    move $s2, $zero
    jal __mv_rt_skip_space
    nop
    move $s3, $v0
__mv_rt_read_string_loop:
    beq $s2, $s1, __mv_rt_read_string_done
    nop
    bltz $s3, __mv_rt_read_string_done
    nop
    li $t0, 32
    beq $s3, $t0, __mv_rt_read_string_done
    nop
    addiu $t0, $s3, -9
    sltiu $t0, $t0, 5
    bne $t0, $zero, __mv_rt_read_string_done
    nop
    sb $s3, 0($s0)
    addiu $s0, $s0, 1
    addiu $s2, $s2, 1
    jal __mv_rt_getchar
    nop
    move $s3, $v0
    b __mv_rt_read_string_loop
    nop
__mv_rt_read_string_done:
    sw $s3, __mv_rt_pushback
    move $v0, $zero
    beq $s2, $zero, __mv_rt_read_string_return
    nop
    sb $zero, 0($s0)
    li $v0, 1
)"); r.end();
}

// The format walker counts only assignments completed by a conversion.
void emitScanf(Runtime &r) {
    r.begin("scanf"); r.arg("$s0", 0);
    r.code(R"(    addiu $s1, $fp, 68
    move $s2, $zero
__mv_rt_scanf_loop:
    lbu $s5, 0($s0)
    beq $s5, $zero, __mv_rt_scanf_done
    nop
    li $t0, 32
    beq $s5, $t0, __mv_rt_scanf_space
    nop
    addiu $t0, $s5, -9
    sltiu $t0, $t0, 5
    bne $t0, $zero, __mv_rt_scanf_space
    nop
    li $t0, 37
    bne $s5, $t0, __mv_rt_scanf_literal
    nop
    addiu $s0, $s0, 1
    lbu $s5, 0($s0)
    beq $s5, $t0, __mv_rt_scanf_literal
    nop
    move $s4, $zero
__mv_rt_scanf_width:
    addiu $t0, $s5, -48
    sltiu $t1, $t0, 10
    beq $t1, $zero, __mv_rt_scanf_conversion
    nop
    li $t1, 10
    multu $s4, $t1
    mflo $s4
    addu $s4, $s4, $t0
    addiu $s0, $s0, 1
    lbu $s5, 0($s0)
    b __mv_rt_scanf_width
    nop
__mv_rt_scanf_conversion:
    li $t0, 115
    beq $s5, $t0, __mv_rt_scanf_string
    nop
    bne $s4, $zero, __mv_rt_format_error
    nop
    lw $s3, 0($s1)
    addiu $s1, $s1, 4
    li $t0, 99
    beq $s5, $t0, __mv_rt_scanf_char
    nop
    la $t3, __mv_rt_read_int
    li $t0, 100
    beq $s5, $t0, __mv_rt_scanf_action
    nop
    li $t0, 117
    beq $s5, $t0, __mv_rt_scanf_action
    nop
    la $t3, __mv_rt_read_float
    li $t0, 102
    bne $s5, $t0, __mv_rt_format_error
    nop
    b __mv_rt_scanf_action
    nop
__mv_rt_scanf_string:
    blez $s4, __mv_rt_format_error
    nop
    lw $s3, 0($s1)
    addiu $s1, $s1, 4
    la $t3, __mv_rt_read_string
__mv_rt_scanf_action:
    addiu $sp, $sp, -8
    sw $s3, 0($sp)
    sw $s4, 4($sp)
    jalr $t3
    nop
    addiu $sp, $sp, 8
    beq $v0, $zero, __mv_rt_scanf_done
    nop
    b __mv_rt_scanf_assigned
    nop
__mv_rt_scanf_char:
    jal __mv_rt_getchar
    nop
    bltz $v0, __mv_rt_scanf_done
    nop
    sb $v0, 0($s3)
__mv_rt_scanf_assigned:
    addiu $s2, $s2, 1
__mv_rt_scanf_advance:
    addiu $s0, $s0, 1
    b __mv_rt_scanf_loop
    nop
__mv_rt_scanf_space:
    jal __mv_rt_skip_space
    nop
    sw $v0, __mv_rt_pushback
    b __mv_rt_scanf_advance
    nop
__mv_rt_scanf_literal:
    jal __mv_rt_getchar
    nop
    beq $v0, $s5, __mv_rt_scanf_advance
    nop
    sw $v0, __mv_rt_pushback
__mv_rt_scanf_done:
    move $v0, $s2
)"); r.end();
}

}

std::string emitMIPSRuntime(const std::vector<std::string> &helpers) {
    if (helpers.empty()) return "";
    Runtime runtime(helpers);
    emitPrinting(runtime);
    emitPrintf(runtime);
    emitAllocator(runtime);
    emitHistory(runtime);
    emitInput(runtime);
    emitReadInteger(runtime);
    emitReadFloat(runtime);
    emitReadString(runtime);
    emitScanf(runtime);
    return runtime.str();
}
