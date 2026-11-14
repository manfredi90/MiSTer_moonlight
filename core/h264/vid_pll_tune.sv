// h264/vid_pll_tune.sv -- (MiSTer Moonlight) the raster's frequency, finely, through vid_pll's dynamic
// reconfiguration: only the delta-sigma modulator's K changes (the fractional part of the feedback
// multiplier), then START -- what Main's HDMI low-latency mode does continuously to pll_hdmi
// (sys/pll_hdmi_adj.vhd): the clock glides to its new frequency, no relock (a CRT showed nothing at
// 480i with 0.1 % steps; 240p moved ~1 pixel at each abrupt step -- the set's horizontal AFC: hence
// the ramp below).
//
// THE GENLOCK (interface mm 1.5, word 34's GL_EN / GL_TRIM): K0 and M are read back from the PLL
// (DPRIO 0x18 / 0x19 and the M register) once vid_pll has locked; the target is
//   K = K0 - GL_TRIM x ((M x 2^32 + K0) >> 16)
// -- GL_TRIM (signed, -127..127) in 2^-16 of the frequency (15.3 ppm), positive SLOWER (longer
// refreshes) -- or K0 without GL_EN. K moves toward it every 10 ms by at most a quarter step
// (3.8 ppm: ~380 ppm a second), so the frequency glides; each move is a DSM write and START. A
// target past K's range is clamped (M never changes). `ok`: K0 and M were read.
//
// mgmt: the reconfiguration core's Avalon slave on CLK_50M (sys/pll_cfg/pll_cfg.v): waitrequest
// mode (the reset default), a write held until waitrequest drops. A READ IS NOT HELD: the core
// never raises waitrequest for one; it walks READ_IDLE -> READ_WAIT -> READ -> READ_POST_WAIT and
// its readdata holds the value for the single cycle 3 clocks after the read was seen (0 otherwise)
// -- so a read is a one-cycle pulse and the data is taken 3 cycles later (the first version took
// readdata at once: M read 0, `ok` never rose, nothing was ever written). Reads of ANY_DPRIO
// (0x20) take the DPRIO address from writedata[5:0]; M_REG (4) returns the M counter's {hi, lo}.

`default_nettype none

module vid_pll_tune #(
    parameter [27:0] T_WAIT = 28'd50_000_000,      // 1 s after the lock (a bench sets it small)
    parameter [19:0] T_TICK = 20'd499_999           // 10 ms a move, minus one
) (
    input  wire        clk,            // CLK_50M
    input  wire        locked,         // vid_pll's (async)
    input  wire        gl_en,          // word 34's GL_EN (async, quasi-static)
    input  wire  [7:0] gl_trim,        // ... GL_TRIM (async, quasi-static; taken when stable)
    output logic       ok,             // K0 / M were read
    output logic       moving,         // K is not yet at its target
    output logic [31:0] k_out,         // the K last written (K0 before any write)
    // to sys/pll_cfg
    output logic       mgmt_reset,
    input  wire        mgmt_waitrequest,
    output logic       mgmt_read,
    output logic       mgmt_write,
    input  wire [31:0] mgmt_readdata,
    output logic [5:0] mgmt_address,
    output logic [31:0] mgmt_writedata
);

    logic [2:0] lk_s, en_s;
    logic [7:0] tr_s1, tr_s2, tr_q;
    always_ff @(posedge clk) begin
        lk_s <= {lk_s[1:0], locked}; en_s <= {en_s[1:0], gl_en};
        tr_s1 <= gl_trim; tr_s2 <= tr_s1;
        if (tr_s2 == tr_s1) tr_q <= tr_s2;          // a stable value only (it crosses clocks)
    end
    wire lk = lk_s[2], en = en_s[2];

    typedef enum logic [3:0] {S_RST, S_WAIT, S_RD_M, S_RD_KL, S_RD_KH, S_CALC, S_STEP, S_IDLE, S_WR_K, S_WR_GO} st_t;
    st_t st;
    logic [27:0] tmr;
    logic [2:0]  rcnt;                  // cycles since a read pulse
    logic [19:0] tick;
    logic [15:0] m_raw, k_lo;
    logic [31:0] k0, k_now;
    logic [40:0] mk;                    // M * 2^32 + K0 (M < 512)
    logic [24:0] step;                  // mk >> 16: 2^-16 of the frequency
    logic signed [34:0] tgt_s;          // the target, before clamping (signed, 35 bits)
    logic [31:0] k_tgt, k_wr;
    wire  [8:0]  m_val = {1'b0, m_raw[15:8]} + {1'b0, m_raw[7:0]};
    wire         rd_done = rcnt == 3'd3;   // the read's data is on mgmt_readdata this cycle
    wire         wr_done = mgmt_write && !mgmt_waitrequest;
    // the target: K0 - trim * step, clamped to K's range
    wire signed [8:0]  trim_s = en ? {tr_q[7], tr_q} : 9'sd0;
    wire signed [34:0] prod   = trim_s * $signed({1'b0, step});      // |prod| < 2^32
    wire signed [34:0] raw    = $signed({3'b000, k0}) - prod;
    wire [31:0]  clamped = raw < 0 ? 32'd0 : raw > 35'sd4294967295 ? 32'hFFFF_FFFF : raw[31:0];
    // a move of at most step / 4 toward it
    wire [24:0]  qstep = {2'b00, step[24:2]};
    wire [31:0]  k_next = (k_tgt > k_now) ? ((k_tgt - k_now > {7'd0, qstep}) ? k_now + {7'd0, qstep} : k_tgt)
                                          : ((k_now - k_tgt > {7'd0, qstep}) ? k_now - {7'd0, qstep} : k_tgt);

    always_ff @(posedge clk) begin
        if (!lk) begin
            st <= S_RST; tmr <= '0; tick <= '0; mgmt_reset <= 1'b1; mgmt_write <= 1'b0; mgmt_read <= 1'b0;
            mgmt_address <= '0; mgmt_writedata <= '0; ok <= 1'b0; moving <= 1'b0; rcnt <= '0;
        end else begin
            if (mgmt_read) mgmt_read <= 1'b0;                          // a read is one cycle
            rcnt <= mgmt_read ? 3'd1 : (rcnt != 3'd0 && rcnt != 3'd7) ? rcnt + 1'b1 : 3'd0;
            tick <= (tick == T_TICK) ? 20'd0 : tick + 1'b1;
            k_tgt <= clamped;
            case (st)
            S_RST:  begin mgmt_reset <= 1'b0; tmr <= '0; st <= S_WAIT; end
            S_WAIT: begin
                tmr <= tmr + 1'b1;
                if (tmr == T_WAIT) begin                                // 1 s after the lock: read M
                    mgmt_address <= 6'd4; mgmt_writedata <= '0; mgmt_read <= 1'b1; st <= S_RD_M;
                end
            end
            S_RD_M: if (rd_done) begin
                m_raw <= mgmt_readdata[15:0];                          // ANY_DPRIO 0x18: K[15:0]
                mgmt_address <= 6'h20; mgmt_writedata <= 32'h18; mgmt_read <= 1'b1; st <= S_RD_KL;
            end
            S_RD_KL: if (rd_done) begin
                k_lo <= mgmt_readdata[15:0];                           // ANY_DPRIO 0x19: K[31:16]
                mgmt_address <= 6'h20; mgmt_writedata <= 32'h19; mgmt_read <= 1'b1; st <= S_RD_KH;
            end
            S_RD_KH: if (rd_done) begin
                k0 <= {mgmt_readdata[15:0], k_lo}; st <= S_CALC;
            end
            S_CALC: begin mk <= {m_val, k0}; st <= S_STEP; end
            S_STEP: begin step <= mk[40:16]; k_now <= k0; ok <= (m_val != 9'd0); st <= S_IDLE; end
            S_IDLE: begin
                moving <= ok && (k_tgt != k_now);
                if (ok && tick == T_TICK && k_tgt != k_now) begin k_wr <= k_next; st <= S_WR_K; end
            end
            S_WR_K: if (!mgmt_write) begin                             // DSM_REG: one move
                mgmt_address <= 6'd7; mgmt_writedata <= k_wr; mgmt_write <= 1'b1;
            end else if (wr_done) begin
                mgmt_write <= 1'b0; st <= S_WR_GO;
            end
            S_WR_GO: if (!mgmt_write) begin                            // START
                mgmt_address <= 6'd2; mgmt_writedata <= 32'd1; mgmt_write <= 1'b1;
            end else if (wr_done) begin
                mgmt_write <= 1'b0; k_now <= k_wr; st <= S_IDLE;
            end
            default: st <= S_RST;
            endcase
        end
    end
    assign k_out = k_now;
endmodule

`default_nettype wire
