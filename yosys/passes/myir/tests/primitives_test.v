// Round-trip test for AST_PRIMITIVE handling.
// Uses Verilog gate primitives (and, or, not, xor, nand, nor, xnor, buf)
// directly so the AST contains AST_PRIMITIVE nodes that
// myir_ast_import::handle_primitive() must lower into MyIR::Expr
// chains.
//
// Note: EPFL benchmarks do not use this form; this test exists
// purely to make sure myir_ast_import does not regress when a
// 3rd-party netlist mixes primitives with continuous assigns.

module top(
    input  a, b, c,
    output o_and, o_or, o_xor,
    output o_nand, o_nor, o_xnor,
    output o_not, o_buf
);

    and  g_and  (o_and,  a, b, c);
    or   g_or   (o_or,   a, b, c);
    xor  g_xor  (o_xor,  a, b, c);
    nand g_nand (o_nand, a, b, c);
    nor  g_nor  (o_nor,  a, b, c);
    xnor g_xnor (o_xnor, a, b, c);
    not  g_not  (o_not,  a);
    buf  g_buf  (o_buf,  a);

endmodule
