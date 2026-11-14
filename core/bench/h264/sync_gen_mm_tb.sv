// bench/h264/sync_gen_mm_tb.sv -- MiSTer Moonlight's raster (h264/sync_gen.sv), scored against
// figures written out here independently of the module: every mode exactly 60.000 Hz on the
// 24.759 MHz dot (412,650 dots a refresh), 640 active pixels, constant line lengths.
//
//   +mode=0  480p: 786-dot lines, 525 lines, 640 x 480 active, hsync 62 dots from dot 656,
//            vsync 6 lines from line 489
//   +mode=1  480i: 1,572-dot lines (pixel repetition: 1,280 active dots), fields of 263 / 262 lines,
//            240 active a field, vsync edges exactly 412,650 dots apart (the half-line interlace),
//            hsync 124 dots from dot 1,312, ce_pix every other dot
//   +mode=2  240p: 1,575-dot lines, 262 lines, 1,280 x 240 active dots, vsync edges 412,650 apart
//
//   +gl=N    (mm, interface 1.5) the genlock on, trim N (lines per 64 refreshes, -64..64): every
//            line still the mode's length, every refresh the mode's or one line longer (N > 0) /
//            shorter (N < 0), |N| / 64 of them; the vsync to first-active-line distance unchanged
//            (the picture keeps its place); the active lines a refresh unchanged. 64 refreshes.
//
// Prints "PASS: sync_gen_mm_tb" or $fatal(1).
`timescale 1ns/1ps
module sync_gen_mm_tb;

reg clk = 0, rst_n = 0;
always #5 clk = ~clk;
integer mode = 0;
initial if (!$value$plusargs("mode=%d", mode)) mode = 0;
integer gl = 0, gl_on = 0;
initial if ($value$plusargs("gl=%d", gl)) gl_on = 1;

wire        dot, pixel_en, h_sync, v_sync, h_blank, v_blank, frame_tick, field, ce_pix, m_il, m_240;
wire [1:0]  m_std;
wire [11:0] h_pos, v_pos, lines, last_line;

sync_gen dut (
    .clk(clk), .clk_en(1'b1), .dot(dot), .rst_n(rst_n),
    .interlaced(mode == 1), .prog240(mode == 2), .std(2'd0), .gl_en(gl_on != 0), .gl_trim(gl[7:0]),
    .nat(1'b0), .g_w(12'd0), .g_h(12'd0), .g_l(12'd0), .g_nt(12'd0), .g_last(12'd0),
    .lines(lines), .last_line(last_line),
    .h_pos(h_pos), .v_pos(v_pos), .pixel_en(pixel_en), .h_sync(h_sync), .v_sync(v_sync),
    .h_blank(h_blank), .v_blank(v_blank), .frame_tick(frame_tick), .field(field), .ce_pix(ce_pix),
    .mode_il(m_il), .mode_std(m_std), .mode_240p(m_240)
);

integer fails = 0;
task check(input [8*64-1:0] what, input integer got, input integer want);
    if (got !== want) begin
        $display("  FAIL %0s: %0d, want %0d", what, got, want);
        fails = fails + 1;
    end else
        $display("  ok   %0s: %0d", what, got);
endtask

// the expected figures
integer W_LINE, W_ACT_DOTS, W_ACT_LINES, W_HS0, W_HSW, W_REFRESH;
initial begin
    W_REFRESH = 412650;                                  // 24,759,000 / 60
    case (mode)
        1:       begin W_LINE = 1572; W_ACT_DOTS = 1280; W_ACT_LINES = 240; W_HS0 = 1312; W_HSW = 124; end
        2:       begin W_LINE = 1575; W_ACT_DOTS = 1280; W_ACT_LINES = 240; W_HS0 = 1312; W_HSW = 124; end
        default: begin W_LINE = 786;  W_ACT_DOTS = 640;  W_ACT_LINES = 480; W_HS0 = 656;  W_HSW = 62;  end
    endcase
end

// measurements, from the outputs alone
integer t = 0;                      // dots since reset
integer hs_rise_prev = -1, line_len_bad = 0, line_len_n = 0, hs_w = 0, hs_w_bad = 0, hs_cur = 0;
integer vs_rise_prev = -1, vs_gap_n = 0, vs_gap_bad = 0;
integer act_dots_line = 0, act_line_bad = 0, act_lines = 0, act_lines_seen = -1, de_prev = 0;
integer ft_prev = -1, ft_n = 0, ft_gap_bad = 0;
integer hs_first = -1, de_first_h = -1, ce_bad = 0;
reg hs_q = 0, vs_q = 0, de_q = 0;
integer refreshes = 0;
integer vs_long = 0, vs_short = 0, vs_odd = 0;          // (gl) refreshes a line longer / shorter / other
integer v2f[0:1], v2f_set[0:1], v2f_bad = 0, vs_last_rise = -1;

reg     go = 0;                     // from the first refresh on (the mode's restart after reset is not scored)
integer gap_prev = 0;
always @(posedge clk) if (rst_n) begin
    t = t + 1;
    if (!go) begin
        // wait for the first refresh's first dot; the measurements start there
        if (frame_tick) begin go = 1; ft_prev = t; act_dots_line = pixel_en ? 1 : 0; act_lines = 0; end
        hs_q = h_sync; vs_q = v_sync; de_q = pixel_en;
    end else begin
    // hsync: period and width
    if (h_sync && !hs_q) begin
        if (hs_rise_prev >= 0) begin
            line_len_n = line_len_n + 1;
            if (t - hs_rise_prev != W_LINE) line_len_bad = line_len_bad + 1;
        end
        hs_rise_prev = t; hs_cur = 0;
        if (hs_first < 0) hs_first = h_pos;
    end
    if (h_sync) hs_cur = hs_cur + 1;
    if (!h_sync && hs_q && hs_rise_prev >= 0 && hs_cur != W_HSW) hs_w_bad = hs_w_bad + 1;
    // vsync: edges a refresh apart
    if (v_sync && !vs_q) begin
        if (vs_rise_prev >= 0) begin
            vs_gap_n = vs_gap_n + 1;
            if (gl_on) begin
                if (t - vs_rise_prev == W_REFRESH + W_LINE) vs_long = vs_long + 1;
                else if (t - vs_rise_prev == W_REFRESH - W_LINE) vs_short = vs_short + 1;
                else if (t - vs_rise_prev != W_REFRESH) begin vs_odd = vs_odd + 1; $display("  vsync gap %0d", t - vs_rise_prev); end
            end else if (t - vs_rise_prev != W_REFRESH) begin
                vs_gap_bad = vs_gap_bad + 1;
                $display("  vsync gap %0d", t - vs_rise_prev);
            end
        end
        vs_rise_prev = t;
        vs_last_rise = t;
    end
    // DE: active dots a line, active lines a refresh
    if (pixel_en) act_dots_line = act_dots_line + 1;
    if (!pixel_en && de_q) begin
        if (act_dots_line != W_ACT_DOTS) act_line_bad = act_line_bad + 1;
        act_dots_line = 0;
        act_lines = act_lines + 1;
    end
    // frame_tick: a refresh apart; active lines counted per refresh
    if (frame_tick && t != ft_prev) begin
        // (gl) the distance from the last vsync edge to this refresh's first dot, per field parity
        if (vs_last_rise >= 0) begin
            if (!v2f_set[field]) begin v2f[field] = t - vs_last_rise; v2f_set[field] = 1; end
            else if (v2f[field] != t - vs_last_rise) v2f_bad = v2f_bad + 1;
        end
        if (ft_prev >= 0 && !gl_on) begin
            ft_n = ft_n + 1;
            // 480i: the fields are 263 and 262 lines (their vsync edges, checked below, a refresh
            // apart): two consecutive fields make 2 x 412,650 dots
            if (mode == 1) begin
                if (ft_n % 2 == 0 && gap_prev + (t - ft_prev) != 2 * W_REFRESH) ft_gap_bad = ft_gap_bad + 1;
                if (t - ft_prev != 263 * W_LINE && t - ft_prev != 262 * W_LINE) ft_gap_bad = ft_gap_bad + 1;
                gap_prev = t - ft_prev;
            end else if (t - ft_prev != W_REFRESH) ft_gap_bad = ft_gap_bad + 1;
            if (act_lines != W_ACT_LINES) begin
                act_lines_seen = act_lines;
            end
        end
        ft_prev = t; act_lines = 0;
    end
    // ce_pix: every other dot on the 15 kHz rasters, every dot at 480p
    if ((mode != 0) && pixel_en && (ce_pix !== ~h_pos[0])) ce_bad = ce_bad + 1;
    if ((mode == 0) && pixel_en && (ce_pix !== 1'b1)) ce_bad = ce_bad + 1;
    hs_q = h_sync; vs_q = v_sync; de_q = pixel_en;
    end
end

initial begin
    v2f_set[0] = 0; v2f_set[1] = 0;
    repeat (5) @(posedge clk);
    rst_n = 1;
    if (gl_on) begin
        repeat (412650 * 66) @(posedge clk);
        $display("sync_gen_mm_tb, mode %0d (%0s), genlock trim %0d:", mode, mode == 1 ? "480i" : mode == 2 ? "240p" : "480p", gl);
        check("dots a line, every line (lines off)", line_len_bad, 0);
        check("hsync width, every line (lines off)", hs_w_bad, 0);
        check("active dots a line, every line (lines off)", act_line_bad, 0);
        check("active lines a refresh (-1 = all right)", act_lines_seen, -1);
        check("refreshes neither the mode's nor a line off", vs_odd, 0);
        check("refreshes a line longer: |trim| / 64 of them (+-1)",
              ((gl > 0 ? vs_long : vs_short) * 64 - (gl > 0 ? gl : -gl) * vs_gap_n) / 64 <= 1 &&
              ((gl > 0 ? vs_long : vs_short) * 64 - (gl > 0 ? gl : -gl) * vs_gap_n) / 64 >= -1 ? 1 : 0, 1);
        check("none the other way", gl > 0 ? vs_short : gl < 0 ? vs_long : vs_long + vs_short, 0);
        check("vsync to the first active dot: the same every refresh (off)", v2f_bad, 0);
        $display("  (%0d refreshes: %0d a line longer, %0d shorter)", vs_gap_n, vs_long, vs_short);
        if (fails) begin $display("FAIL: sync_gen_mm_tb (%0d)", fails); $fatal(1); end
        $display("PASS: sync_gen_mm_tb");
        $finish;
    end
    // five refreshes
    repeat (412650 * 5 + 100) @(posedge clk);
    $display("sync_gen_mm_tb, mode %0d (%0s):", mode, mode == 1 ? "480i" : mode == 2 ? "240p" : "480p");
    check("dots a line, every line (lines off)", line_len_bad, 0);
    check("hsync first dot", hs_first, W_HS0);
    check("hsync width, every line (lines off)", hs_w_bad, 0);
    check("active dots a line, every line (lines off)", act_line_bad, 0);
    check("refreshes seen (frame_tick)", ft_n >= 4 ? 1 : 0, 1);
    check("dots a refresh, every refresh (refreshes off): 60.000 Hz", ft_gap_bad, 0);
    check("active lines a refresh (-1 = all right)", act_lines_seen, -1);
    check("vsync edges seen", vs_gap_n >= 4 ? 1 : 0, 1);
    check("vsync edges a refresh apart (gaps off)", vs_gap_bad, 0);
    check("ce_pix pattern (dots off)", ce_bad, 0);
    check("mode_std reports 60.00 Hz", m_std, 1);
    if (fails) begin $display("FAIL: sync_gen_mm_tb (%0d)", fails); $fatal(1); end
    $display("PASS: sync_gen_mm_tb");
    $finish;
end

endmodule
