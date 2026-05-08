// Test mux-tree optimization on nested muxes with repeated selectors.
module muxtree_test(input a, b, c, d, output y1, y2, y3, y4, y5, y6);
    wire na;
    wire t1, t2;
    assign na = ~a;

    assign y1 = a ? (a ? b : c) : d;
    assign y2 = a ? b : (a ? c : d);
    assign y3 = 1'b0 ? b : c;
    assign y4 = a ? (na ? b : c) : d;
    assign y5 = a ? b : b;
    assign t1 = a ? b : c;
    assign t2 = t1;
    assign y6 = a ? d : t2;
endmodule
