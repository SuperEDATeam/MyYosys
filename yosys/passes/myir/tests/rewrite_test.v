// Test several rewrite rules
module rewrite_test(
    a, b, c,
    y_factor, y_absorb, y_idem, y_xor, y_xnor, y_not, y_or0, y_and1,
    y_demorgan, y_consensus, y_resub, y_deep_resub
);
    input  a, b, c;
    output y_factor, y_absorb, y_idem, y_xor, y_xnor, y_not, y_or0, y_and1;
    output y_demorgan, y_consensus, y_resub, y_deep_resub;

    wire ab, ac;
    wire and_absorb;
    wire nn0, n0;
    wire na, nb;
    wire xay, nxay;
    wire nna, nnaxby;

    assign ab = a & b;
    assign ac = a & c;
    assign y_factor = ab | ac;

    assign and_absorb = a & b;
    assign y_absorb = a | and_absorb;

    assign y_idem = a | a;
    assign y_xor = a ^ a;
    assign y_xnor = a ~^ a;

    assign n0 = ~a;
    assign nn0 = ~n0;
    assign y_not = nn0;

    assign y_or0 = a | 1'b0;
    assign y_and1 = a & 1'b1;

    assign na = ~a;
    assign nb = ~b;
    assign y_demorgan = na | nb;

    assign xay = a & b;
    assign nxay = na & b;
    assign y_consensus = xay | nxay;

    assign y_resub = a | nxay;

    assign nna = ~na;
    assign nnaxby = nna & b;
    assign y_deep_resub = a | nnaxby;
endmodule
