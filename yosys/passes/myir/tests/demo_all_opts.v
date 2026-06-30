// =====================================================================
//  demo_all_opts.v  --  single-module showcase for the MyIR optimizer
// =====================================================================
//  One coherent circuit that exercises every MyIR optimization pass.
//  The boolean "main" datapath is wired so the passes cascade in the
//  same order myir_opts runs them: a constant ripples in, which makes a
//  compare go constant, which makes a mux select go constant, which
//  collapses the mux, which exposes dead logic for the cleaner.  Two
//  short side branches cover the arithmetic (strength) and bus (concat)
//  rules that do not fit a boolean cascade.
//
//  Note on inputs: the boolean cascade uses scalar control bits
//  (a, b, c, d) rather than slices of a bus.  The arithmetic / concat
//  branches operate on the whole `din` bus, so no bit-selects are
//  needed anywhere -- keeping the example clean and fully verifiable.
//
//  Pass coverage (myir_opts pipeline order):
//    myir_opt_const    : k0=1 -> k1 -> en   (constant through alias chain)
//    myir_opt_expr     : msel & en(=1) -> msel
//    myir_opt_compare  : (din == din) -> 1
//    myir_opt_strength : din + din -> din << 1
//    myir_opt_concat   : {din, 2'b00, 2'b11} -> {din, 4'b0011}
//    myir_opt_const(2) : propagate eq(=1) into the mux select
//    myir_opt_muxtree  : (1 ? fac : dead) -> fac   (dead is orphaned)
//    myir_opt_expr(2)  : tidy any residual identities
//    myir_rewrite      : (a&b)|(a&c) -> a & (b|c)
//    myir_opt_share    : b&c computed twice -> shared once
//    myir_opt_clean    : remove the now-dead `dead` node
// =====================================================================
module demo_all_opts(
    input        a, b, c, d,   // scalar control bits for the boolean cascade
    input  [3:0] din,          // bus for the arithmetic + concat branches
    output [3:0] y_shift,      // strength side branch
    output [7:0] y_cat,        // concat   side branch
    output       y_main,       // boolean cascade output
    output       y_sh1,        // share demo
    output       y_sh2
);
    // ---- opt_const: constant propagates through an alias chain --------
    wire k0, k1, en;
    assign k0 = 1'b1;
    assign k1 = k0;
    assign en = k1;

    // ---- opt_compare: self-compare folds to 1, then drives a mux -----
    wire eq;
    assign eq = (din == din);

    // ---- rewrite: factorable AND-OR --------------------------------
    wire ab, ac, fac;
    assign ab  = a & b;
    assign ac  = a & c;
    assign fac = ab | ac;

    // ---- node that becomes dead once the mux collapses -> clean ------
    wire dead;
    assign dead = a & d;

    // ---- opt_muxtree: select becomes constant (after compare+const) --
    wire msel;
    assign msel = eq ? fac : dead;

    // ---- opt_expr: x & 1 -> x  (after en folds to 1) ----------------
    assign y_main = msel & en;

    // ---- opt_share: identical sub-expression computed twice ---------
    assign y_sh1 = b & c;
    assign y_sh2 = b & c;

    // ---- opt_strength: a + a -> a << 1 ------------------------------
    assign y_shift = din + din;

    // ---- opt_concat: merge adjacent constants ----------------------
    assign y_cat = {din, 2'b00, 2'b11};
endmodule
