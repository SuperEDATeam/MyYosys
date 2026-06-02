// Test myir_opt_concat rules.
//
// Expected after `myir_opt_concat`:
//   y_const  : {4'hA, 4'h3}        ->  8'hA3      (all_const)
//   y_single : {a}                 ->  Buf a      (single_op)
//   y_adj    : {a, 2'b00, 2'b11}   ->  Concat(a, 4'b0011)   (adj_const)
//   y_nest   : {a, {b, c}}         ->  Concat(a, b, c)      (flatten)
module concat_test(
    a, b, c,
    y_const, y_single,
    y_adj, y_nest
);
    input  [3:0] a;
    input  [1:0] b;
    input  [1:0] c;
    output [7:0] y_const;
    output [3:0] y_single;
    output [7:0] y_adj;
    output [7:0] y_nest;

    assign y_const  = {4'hA, 4'h3};
    assign y_single = {a};
    assign y_adj    = {a, 2'b00, 2'b11};

    wire [3:0] bc;
    assign bc = {b, c};
    assign y_nest = {a, bc};
endmodule
