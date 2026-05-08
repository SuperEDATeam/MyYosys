module const_test(input x, output y1, y2, y3);
    wire a, b, c, d, e;

    assign a = 1'b1;
    assign b = a;
    assign c = b;
    assign y1 = c & x;

    assign d = 1'b0;
    assign e = d;
    assign y2 = e | x;

    assign y3 = c ^ e;
endmodule
