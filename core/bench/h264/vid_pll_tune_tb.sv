// bench/h264/vid_pll_tune_tb.sv -- (mm) h264/vid_pll_tune.sv against a model of the reconfiguration
// core's Avalon slave (waitrequest mode: a read held 3 cycles, a write 2): it reads M and K0, then
// with `test` writes K0 + (M * 2^32 + K0) >> 10 and START, back to K0 a step later, and K0 at once
// when the test goes off. Prints "PASS: vid_pll_tune_tb" or $fatal(1).
`timescale 1ns/1ps
module vid_pll_tune_tb;
reg clk = 0; always #10 clk = ~clk;
reg locked = 0, test = 0;
wire fast, ok, mrst, mread, mwrite; wire [5:0] maddr; wire [31:0] mwdata;
reg  [31:0] mrdata = 0; reg wreq_r;
reg  [1:0]  cnt = 0;
wire        busy = (mread || mwrite) && cnt != 2'd3;
wire        mwait = busy;
always @(posedge clk) cnt <= (mread || mwrite) ? (cnt == 2'd3 ? 2'd0 : cnt + 1'b1) : 2'd0;
localparam [15:0] M_RAW = 16'h0707;              // M = 14
localparam [31:0] K0 = 32'hDAFF_1234;
always @(*) begin
    mrdata = 32'hDEAD_BEEF;
    if (mread && maddr == 6'd4) mrdata = {16'h0, M_RAW};
    if (mread && maddr == 6'h20 && mwdata[5:0] == 6'h18) mrdata = {16'h0, K0[15:0]};
    if (mread && maddr == 6'h20 && mwdata[5:0] == 6'h19) mrdata = {16'h0, K0[31:16]};
end
vid_pll_tune #(.T_WAIT(28'd100), .T_STEP(28'd999)) dut (
    .clk(clk), .locked(locked), .test(test), .fast(fast), .ok(ok), .mgmt_reset(mrst),
    .mgmt_waitrequest(mwait), .mgmt_read(mread), .mgmt_write(mwrite), .mgmt_readdata(mrdata),
    .mgmt_address(maddr), .mgmt_writedata(mwdata));
integer fails = 0, nk = 0, ngo = 0;
reg [31:0] lastk;
always @(posedge clk) if (mwrite && !mwait) begin
    if (maddr == 6'd7) begin lastk = mwdata; nk = nk + 1; end
    if (maddr == 6'd2) ngo = ngo + 1;
end
// the expected step: (14 * 2^32 + K0) >> 10
wire [40:0] mk = {9'd14, K0};
wire [31:0] want = K0 + mk[40:10];
initial begin
    repeat (10) @(posedge clk); locked = 1;
    repeat (300) @(posedge clk);
    if (!ok) begin $display("FAIL: K0 / M not read"); fails++; end
    if (nk != 0) begin $display("FAIL: a write before the test"); fails++; end
    test = 1;
    repeat (1100) @(posedge clk);
    if (nk != 1 || lastk !== want || ngo != 1 || !fast) begin $display("FAIL: first step: %0d writes, K %h want %h, %0d starts, fast %b", nk, lastk, want, ngo, fast); fails++; end
    repeat (1000) @(posedge clk);
    if (nk != 2 || lastk !== K0 || fast) begin $display("FAIL: second step: %0d writes, K %h want %h, fast %b", nk, lastk, K0, fast); fails++; end
    repeat (1000) @(posedge clk);
    if (nk != 3 || lastk !== want) begin $display("FAIL: third step"); fails++; end
    test = 0;
    repeat (50) @(posedge clk);
    if (nk != 4 || lastk !== K0 || fast) begin $display("FAIL: test off: %0d writes, K %h, fast %b", nk, lastk, fast); fails++; end
    repeat (3000) @(posedge clk);
    if (nk != 4) begin $display("FAIL: writes with the test off"); fails++; end
    if (fails) begin $display("FAIL: vid_pll_tune_tb"); $fatal(1); end
    $display("PASS: vid_pll_tune_tb (K0 %h, step to %h: %.4f %%)", K0, want, 100.0 * (want - K0) / (14.0 * 4294967296.0 + K0));
    $finish;
end
endmodule
