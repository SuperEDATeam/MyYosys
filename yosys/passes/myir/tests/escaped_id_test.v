// Round-trip test for escaped identifiers ( \name[i] style ).
// Mirrors how EPFL combinational benchmarks name their ports/wires.
//
// Three sanity criteria:
//   - port \a[0]   , \a[1]   , \y[0]    survive AST import & export
//     under the same canonical form throughout the pipeline.
//   - intermediate scalar wires keep their bracketed-but-scalar names.
//   - downstream myir_opt_clean must NOT delete any of these wires
//     (it would, if the AST_IDENTIFIER / AST_WIRE name representations
//      were mismatched and one side was treated as unused).

module top( \a[0] , \a[1] , \y[0] );
  input  \a[0] , \a[1] ;
  output \y[0] ;
  wire n1;
  assign n1     = \a[0]  & \a[1] ;
  assign \y[0]  = ~n1;
endmodule
