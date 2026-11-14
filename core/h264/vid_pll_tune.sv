// h264/vid_pll_tune.sv -- (MiSTer Moonlight) the raster's frequency, finely, through vid_pll's dynamic
// reconfiguration: only the delta-sigma modulator's K changes (the fractional part of the feedback
// multiplier), then START -- what Main's HDMI low-latency mode does continuously to pll_hdmi
// (sys/pll_hdmi_adj.vhd), so the clock glides to its new frequency without a relock.
//
// THE PLL TEST (this version): K0 and M are read back from the PLL (DPRIO 0x18 / 0x19 and the M
// register) once vid_pll has locked; then, while `test` (the OSD's "PLL Test"), every 5 s the K
// alternates between K0 and K0 + (M * 2^32 + K0) / 1024: the raster 1/1024 faster (60.0586 Hz
// instead of 60.000), or slower if that K would overflow. Off: K0 again. `fast` reports the state
// (status word 45 [25], to see it in the client's log), `ok` that K0 was read.
//
// mgmt: the reconfiguration core's Avalon slave on CLK_50M (sys/pll_cfg/pll_cfg.v): waitrequest
// mode (the reset default), a write held until waitrequest drops; reads of ANY_DPRIO (0x20) take
// the DPRIO address from writedata[5:0], M_REG (4) returns the M counter's {hi, lo}.

`default_nettype none

module vid_pll_tune #(
    parameter [27:0] T_WAIT = 28'd50_000_000,      // 1 s after the lock (a bench sets it small)
    parameter [27:0] T_STEP = 28'd249_999_999      // 5 s a step, minus one
) (
    input  wire        clk,            // CLK_50M
    input  wire        locked,         // vid_pll's (async)
    input  wire        test,           // the OSD's PLL Test (async, quasi-static)
    output logic       fast,           // the raster runs at K0 + delta now
    output logic       ok,             // K0 / M were read
    // to sys/pll_cfg
    output logic       mgmt_reset,
    input  wire        mgmt_waitrequest,
    output logic       mgmt_read,
    output logic       mgmt_write,
    input  wire [31:0] mgmt_readdata,
    output logic [5:0] mgmt_address,
    output logic [31:0] mgmt_writedata
);

    logic [2:0] lk_s, ts_s;
    always_ff @(posedge clk) begin lk_s <= {lk_s[1:0], locked}; ts_s <= {ts_s[1:0], test}; end
    wire lk = lk_s[2], ts = ts_s[2];

    typedef enum logic [3:0] {S_RST, S_WAIT, S_RD_M, S_RD_KL, S_RD_KH, S_CALC, S_IDLE, S_WR_K, S_WR_GO, S_SETTLE} st_t;
    st_t st;
    logic [27:0] tmr;                   // 5 s at 50 MHz: 250,000,000 < 2^28
    logic [15:0] m_raw, k_lo;
    logic [31:0] k0, k_alt, k_now, k_tgt;
    logic        alt_on;                // the alternative K is in force
    logic [40:0] mk;                    // M * 2^32 + K0 (M < 512)
    wire  [8:0]  m_val = {1'b0, m_raw[15:8]} + {1'b0, m_raw[7:0]};
    wire         rd_done = mgmt_read && !mgmt_waitrequest;
    wire         wr_done = mgmt_write && !mgmt_waitrequest;

    always_ff @(posedge clk) begin
        if (!lk) begin
            st <= S_RST; tmr <= '0; mgmt_reset <= 1'b1; mgmt_write <= 1'b0; mgmt_read <= 1'b0;
            mgmt_address <= '0; mgmt_writedata <= '0; ok <= 1'b0; fast <= 1'b0; alt_on <= 1'b0;
        end else case (st)
            S_RST:  begin mgmt_reset <= 1'b0; tmr <= '0; st <= S_WAIT; end
            S_WAIT: begin
                tmr <= tmr + 1'b1;
                if (tmr == T_WAIT) begin                       // 1 s after the lock: read M
                    mgmt_address <= 6'd4; mgmt_writedata <= '0; mgmt_read <= 1'b1; st <= S_RD_M;
                end
            end
            S_RD_M: if (rd_done) begin
                m_raw <= mgmt_readdata[15:0]; mgmt_read <= 1'b0; st <= S_RD_KL;
            end
            S_RD_KL: if (!mgmt_read) begin                            // ANY_DPRIO 0x18: K[15:0]
                mgmt_address <= 6'h20; mgmt_writedata <= 32'h18; mgmt_read <= 1'b1;
            end else if (rd_done) begin
                k_lo <= mgmt_readdata[15:0]; mgmt_read <= 1'b0; st <= S_RD_KH;
            end
            S_RD_KH: if (!mgmt_read) begin                            // ANY_DPRIO 0x19: K[31:16]
                mgmt_address <= 6'h20; mgmt_writedata <= 32'h19; mgmt_read <= 1'b1;
            end else if (rd_done) begin
                k0 <= {mgmt_readdata[15:0], k_lo}; mgmt_read <= 1'b0; st <= S_CALC;
            end
            S_CALC: begin
                mk <= {m_val, k0};                                     // M * 2^32 + K0
                st <= S_SETTLE; tmr <= '0;
            end
            S_SETTLE: begin                                            // (one cycle for mk) the step
                // faster (K up) unless that would carry into M; then slower
                k_alt <= (({1'b0, k0} + {2'b00, mk[40:10]}) > 33'h0_FFFF_FFFF) ? k0 - {1'b0, mk[40:10]} : k0 + {1'b0, mk[40:10]};
                k_now <= k0; ok <= (m_val != 9'd0); st <= S_IDLE; tmr <= '0;
            end
            S_IDLE: begin
                tmr <= (tmr == T_STEP) ? 28'd0 : tmr + 1'b1;
                if (ok) begin
                    if (!ts && alt_on) begin
                        alt_on <= 1'b0; k_tgt <= k0; st <= S_WR_K;     // the test off: back to K0 at once
                    end else if (ts && tmr == T_STEP) begin
                        alt_on <= !alt_on; k_tgt <= alt_on ? k0 : k_alt; st <= S_WR_K;
                    end
                end
            end
            S_WR_K: if (!mgmt_write) begin                            // DSM_REG: the new K
                mgmt_address <= 6'd7; mgmt_writedata <= k_tgt; mgmt_write <= 1'b1;
            end else if (wr_done) begin
                mgmt_write <= 1'b0; st <= S_WR_GO;
            end
            S_WR_GO: if (!mgmt_write) begin                           // START
                mgmt_address <= 6'd2; mgmt_writedata <= 32'd1; mgmt_write <= 1'b1;
            end else if (wr_done) begin
                mgmt_write <= 1'b0; k_now <= k_tgt; fast <= (k_tgt != k0); st <= S_IDLE;
            end
            default: st <= S_RST;
        endcase
    end
endmodule

`default_nettype wire
