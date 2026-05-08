// Test local rewrite: (a & b) | (a & c) -> a & (b | c)
module rewrite_test(a, b, c, y);
    input  a, b, c;
    output y;

    wire ab, ac;
    assign ab = a & b;
    assign ac = a & c;
    assign y = ab | ac;
endmodule
