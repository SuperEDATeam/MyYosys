module test_opt(input a, input b, input c, output out1, output out2);
    wire w1, w2;

    // 1. 常量传播 (Constant Propagation)
    // 如果 a 被强制赋值为 1，那么 w1 = 1 & b 应该直接变成 w1 = b
    assign w1 = 1 & b; 

    // 2. 公共子表达式 (Common Subexpression - CSE)
    // w1 和 w2 的逻辑完全一样，IR 应该合并它们，或者在计算 out2 时复用 w1
    assign w2 = 1 & b; 

    // 3. 死代码 (Dead Code / Redundancy)
    // 如果 out1 最终只连接到一个固定的逻辑，或者某些中间变量没被用到
    assign out1 = w1 | (c & 0); // c & 0 应该被优化掉，变成 out1 = w1
    assign out2 = w2;

endmodule