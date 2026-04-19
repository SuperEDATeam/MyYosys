// Test: if/else inside always block -> MyIR ProcBlock
module mux2(input a, b, sel, output reg y);
    always @(*) begin
        if (sel)
            y = b;
        else
            y = a;
    end
endmodule
