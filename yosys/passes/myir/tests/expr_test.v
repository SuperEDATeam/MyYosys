module expr_test(input a, b, s, output y1, y2, y3, y4, y5, y6, y7);
    assign y1 = a & 1'b1;
    assign y2 = b | 1'b0;
    assign y3 = a ^ 1'b0;
    assign y4 = a ~^ 1'b0;
    assign y5 = s ? 1'b0 : 1'b1;
    assign y6 = s ? a : a;
    assign y7 = (1'b1 & a) | 1'b0;
endmodule
