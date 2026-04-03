// Test for shared sub-expression elimination
module share_test(a, b, x, y);
    input  [3:0] a, b;
    output [3:0] x, y;
    // Both outputs use the same sub-expression (a & b)
    assign x = a & b;
    assign y = a & b;
endmodule
