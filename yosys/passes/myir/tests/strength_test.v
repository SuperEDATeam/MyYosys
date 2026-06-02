// Test myir_opt_strength rules.
//
// Expected after `myir_opt_strength`:
//   y_mul2   : Mul(x, 2)      -> Shl(x, 1)
//   y_mul8   : Mul(x, 8)      -> Shl(x, 3)
//   y_mul1   : Mul(x, 1)      (left alone -- handled by myir_opt_expr)
//   y_mul3   : Mul(x, 3)      (3 is NOT a power of two; left alone)
//   y_add2x  : Add(x, x)      -> Shl(x, 1)
//   y_zerox  : Sub(0, x)      -> Neg(x)
//   y_negneg : Neg(Neg(x))    -> Buf x  (i.e. y_negneg directly drives x)
module strength_test(
    x,
    y_mul2, y_mul8, y_mul1, y_mul3,
    y_add2x, y_zerox, y_negneg
);
    input  [7:0] x;
    output [7:0] y_mul2, y_mul8, y_mul1, y_mul3;
    output [7:0] y_add2x, y_zerox, y_negneg;

    assign y_mul2   = x * 8'd2;
    assign y_mul8   = x * 8'd8;
    assign y_mul1   = x * 8'd1;       // shouldn't hit strength rules
    assign y_mul3   = x * 8'd3;       // shouldn't hit strength rules

    assign y_add2x  = x + x;
    assign y_zerox  = 8'd0 - x;

    wire [7:0] nx;
    assign nx = -x;
    assign y_negneg = -nx;
endmodule
