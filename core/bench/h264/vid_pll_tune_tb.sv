// bench/h264/vid_pll_tune_tb.sv -- (mm) h264/vid_pll_tune.sv against a model of the reconfiguration
// core's Avalon slave (waitrequest mode: a read held 3 cycles, a write 2): it reads M and K0; with
// GL_EN and a trim it walks K to K0 - trim * ((M * 2^32 + K0) >> 16) in moves of at most a quarter
// step, each a DSM write then START, and back to K0 without GL_EN. Prints "PASS: vid_pll_tune_tb".
`timescale 1ns/1ps
module vid_pll_tune_tb;
reg clk = 0; always #10 clk = ~clk;
reg locked = 0, en = 0; reg [7:0] trim = 0;
wire ok, moving, mrst, mread, mwrite; wire [5:0] maddr; wire [31:0] mwdata;
reg  [31:0] mrdata;
reg  [1:0]  cnt = 0;
wire        mwait = (mread || mwrite) && cnt != 2'd3;
always @(posedge clk) cnt <= (mread || mwrite) ? (cnt == 2'd3 ? 2'd0 : cnt + 1'b1) : 2'd0;
localparam [15:0] M_RAW = 16'h0707;              // M = 14
localparam [31:0] K0 = 32'hDAFF_1234;
always @(*) begin
    mrdata = 32'hDEAD_BEEF;
    if (mread && maddr == 6'd4) mrdata = {16'h0, M_RAW};
    if (mread && maddr == 6'h20 && mwdata[5:0] == 6'h18) mrdata = {16'h0, K0[15:0]};
    if (mread && maddr == 6'h20 && mwdata[5:0] == 6'h19) mrdata = {16'h0, K0[31:16]};
end
vid_pll_tune #(.T_WAIT(28'd100), .T_TICK(20'd49)) dut (
    .clk(clk), .locked(locked), .gl_en(en), .gl_trim(trim), .ok(ok), .moving(moving), .mgmt_reset(mrst),
    .mgmt_waitrequest(mwait), .mgmt_read(mread), .mgmt_write(mwrite), .mgmt_readdata(mrdata),
    .mgmt_address(maddr), .mgmt_writedata(mwdata));
integer fails = 0, nk = 0, ngo = 0, bigmove = 0;
reg [31:0] k = K0, pend;
wire [40:0] mk = {9'd14, K0};
wire [24:0] step = mk[40:16];
always @(posedge clk) if (mwrite && !mwait) begin
    if (maddr == 6'd7) begin
        if ((mwdata > k ? mwdata - k : k - mwdata) > step / 4 + 1) bigmove = bigmove + 1;
        pend = mwdata; nk = nk + 1;
    end
    if (maddr == 6'd2) begin ngo = ngo + 1; k = pend; end
end
task expect_k(input [31:0] want, input [8*24-1:0] what);
    if (k !== want || moving) begin $display("FAIL %0s: K %h want %h (moving %b)", what, k, want, moving); fails++; end
    else $display("  ok   %0s: K %h", what, k);
endtask
initial begin
    repeat (10) @(posedge clk); locked = 1;
    repeat (300) @(posedge clk);
    if (!ok) begin $display("FAIL: K0 / M not read"); fails++; end
    if (nk) begin $display("FAIL: a write without GL_EN"); fails++; end
    en = 1; trim = 8'd10;                         // slower: K0 - 10 steps, 40 moves
    repeat (50 * 60) @(posedge clk);
    expect_k(K0 - 10 * step, "trim +10");
    trim = -8'sd7;                                // faster: K0 + 7 steps
    repeat (50 * 80) @(posedge clk);
    expect_k(K0 + 7 * step, "trim -7");
    en = 0;
    repeat (50 * 40) @(posedge clk);
    expect_k(K0, "GL_EN off");
    if (bigmove) begin $display("FAIL: %0d moves larger than a quarter step", bigmove); fails++; end
    if (nk != ngo) begin $display("FAIL: %0d K writes, %0d starts", nk, ngo); fails++; end
    if (fails) begin $display("FAIL: vid_pll_tune_tb"); $fatal(1); end
    $display("PASS: vid_pll_tune_tb (%0d moves; a step is %0d of K = %.1f ppm)", nk, step, 1e6 * step / (14.0 * 4294967296.0 + K0));
    $finish;
end
endmodule
