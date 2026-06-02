// Test myir_opt_compare rules.
//
// Expected after `myir_opt_compare`:
//   y_eq_self  : a == a            ->  1'b1
//   y_ne_self  : a != a            ->  1'b0
//   y_lt_self  : a <  a            ->  1'b0
//   y_le_self  : a <= a            ->  1'b1
//   y_ge_self  : a >= a            ->  1'b1
//   y_gt_self  : a >  a            ->  1'b0
//   y_u_lt0    : unsigned_a <  0   ->  1'b0
//   y_u_ge0    : unsigned_a >= 0   ->  1'b1
//
// And a follow-up `myir_opt_muxtree` should then collapse the mux below
// because the select went constant.
//   y_eq_mux   : (a == a) ? c : d  ->  c
module compare_test(
    a, c, d,
    y_eq_self, y_ne_self, y_lt_self, y_le_self,
    y_ge_self, y_gt_self,
    y_u_lt0, y_u_ge0,
    y_eq_mux
);
    input  [7:0] a;          // unsigned by default
    input        c, d;
    output       y_eq_self, y_ne_self, y_lt_self, y_le_self;
    output       y_ge_self, y_gt_self;
    output       y_u_lt0, y_u_ge0;
    output       y_eq_mux;

    assign y_eq_self = (a == a);
    assign y_ne_self = (a != a);
    assign y_lt_self = (a <  a);
    assign y_le_self = (a <= a);
    assign y_ge_self = (a >= a);
    assign y_gt_self = (a >  a);

    assign y_u_lt0   = (a <  8'd0);
    assign y_u_ge0   = (a >= 8'd0);

    assign y_eq_mux  = (a == a) ? c : d;
endmodule
