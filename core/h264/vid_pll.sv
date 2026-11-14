// h264/vid_pll.sv -- the raster's clock, clk_vid (docs/hd_raster.md S2, Phase 5c part 3)
//
// MISTER MOONLIGHT (2026-11): clk_vid is 74.277 MHz and clk_vid_dv 24.759 MHz -- the 640-pixel
// raster's dot (h264/sync_gen.sv), 412,650 dots = exactly 1/60 s, 450,000 clk_sys cycles: clk_vid /
// clk_sys = 2,751 / 1,000 exactly. Fractional-N (VCO 742.77 MHz = 50 x 14.8554); the K quantisation
// (2^-32) drifts the raster against disp_ctl's grid by well under a cycle an hour, and the grid
// re-aligns at every mode change. The text below describes the original 81 / 27 MHz clocks.
//
// Input:  50 MHz (FPGA_CLK2_50 via sys_top: the pin sys_pll runs from)
// Output: outclk_0  81 MHz  clk_vid -- CLK_VIDEO; the raster side (sync_gen, the reader's and the
//                                       overlay's read-outs, the output stage) on a dot enable:
//                                       one clock in three on the 27 MHz rasters (480i / 576i,
//                                       480p / 576p: the analog pins exactly as at 27 MHz), every
//                                       clock on the native one.
//
// Why a second PLL: 81 = 3 x 27 is not a divisor of sys_pll's 1,350 MHz VCO, and no VCO in range
// makes 81, 90 and ~96 together (1,620 is over the 1,600 limit; 1,215 costs clk_mem and clk_dec
// ~3 %). VCO 810 = 50 x 81 / 5, / 10. Integer-N from the same crystal as sys_pll: exactly three
// clk_vid cycles per 27 MHz dot, so the raster never drifts against disp_ctl's refresh grid on
// clk_sys -- but the PHASE is unrelated: every transfer goes through a crossing primitive
// (h264/raster_xing.sv) and sys/sys_top.sdc keeps this clock in the core PLL's timed group (an
// H264-FORK line), the crossings bounded by name in h264/h264.sdc.
//
// The instance MUST be named vid_pll: tools/fmax_check.sh gates clk_vid by the hierarchical name
// emu|vid_pll|altera_pll_i|general[0]..., and sys_top.sdc's group pattern names it.
// `ifdef QUARTUS as sys_pll; simulation passes refclk through (benches make their own clocks).
//
// outclk_1, 27 MHz, clk_vid_dv (dv27, 2026-10-29): with MiSTer.ini direct_video=1, sys_top clocks the
// HDMI transmitter from the core's clock (its hdmi_clk_sw), and on clk_vid that sent every 27 MHz
// dot three times: a 480i line of 5,148 pixels at 81 MHz, which HDMI-to-VGA DACs refuse (black on a
// Ugreen, scrambled on a GE; docs/hd_raster.md S11 had left it to confirm). The H264-FORK line in
// sys_top takes this clock for the transmitter instead, so the DAC sees the plain 27 MHz 480i / 480p
// (576i / 576p) raster: one dot per clock, 480i's pixel repetition as two clocks a pixel. The raster
// side stays on clk_vid; sys_top's dv_* registers (clk_vid) hold each dot for its three clocks, so
// the transmitter's 27 MHz register takes every dot exactly once at any fixed phase. The phase is
// shifted half a clk_vid period (6,173 ps) so no 27 MHz edge coincides with an 81 MHz one: the
// tightest clk_vid -> clk_vid_dv pair is then 6.2 ns, which TimeQuest times (same PLL, same group).

`timescale 1ns/10ps
module vid_pll (
	input  wire refclk,
	input  wire rst,
	output wire outclk_0, // 74.277 MHz -- clk_vid
	output wire outclk_1, // 24.759 MHz -- clk_vid_dv: the HDMI transmitter's clock on direct_video (below)
	output wire locked,
	// (mm) dynamic reconfiguration (h264/vid_pll_tune.sv through sys/pll_cfg): the DSM's K -- the
	// raster's frequency in steps of 2^-32 of the multiplier, as Main's HDMI low-latency mode does
	// with pll_hdmi (sys/pll_hdmi_adj.vhd). THE PLL IS WRITTEN OUT PHYSICALLY ("Cyclone V", every
	// counter given -- the form of sys/pll_hdmi/pll_hdmi_0002.v, whose VCO it shares): as "General"
	// (frequencies only, the Fitter choosing) the reconfiguration ports were left unconnected on
	// hardware -- every read 0, waitrequest high for good. The values are the ones the Fitter chose
	// then (H264.fit.rpt): VCO 445.661998 MHz = 50 x (8 + 3922335847 / 2^32), N bypassed, M 4 + 4,
	// C0 3 + 3 (74.277), C1 9 + 9 with PRST 4 (24.759, 60 degrees = 6732 ps)
	input  wire [63:0] reconfig_to_pll,
	output wire [63:0] reconfig_from_pll
);

`ifdef QUARTUS

	altera_pll #(
		.fractional_vco_multiplier("true"),
		.reference_clock_frequency("50.0 MHz"),
		.pll_fractional_cout(32),
		.pll_dsm_out_sel("1st_order"),
		.operation_mode("direct"),
		.number_of_clocks(2),
		.output_clock_frequency0("74.276999 MHz"),
		.phase_shift0("0 ps"),
		.duty_cycle0(50),
		.output_clock_frequency1("24.758999 MHz"),
		.phase_shift1("6732 ps"),
		.duty_cycle1(50),
		.output_clock_frequency2("0 MHz"),
		.phase_shift2("0 ps"),
		.duty_cycle2(50),
		.output_clock_frequency3("0 MHz"),
		.phase_shift3("0 ps"),
		.duty_cycle3(50),
		.output_clock_frequency4("0 MHz"),
		.phase_shift4("0 ps"),
		.duty_cycle4(50),
		.output_clock_frequency5("0 MHz"),
		.phase_shift5("0 ps"),
		.duty_cycle5(50),
		.output_clock_frequency6("0 MHz"),
		.phase_shift6("0 ps"),
		.duty_cycle6(50),
		.output_clock_frequency7("0 MHz"),
		.phase_shift7("0 ps"),
		.duty_cycle7(50),
		.output_clock_frequency8("0 MHz"),
		.phase_shift8("0 ps"),
		.duty_cycle8(50),
		.output_clock_frequency9("0 MHz"),
		.phase_shift9("0 ps"),
		.duty_cycle9(50),
		.output_clock_frequency10("0 MHz"),
		.phase_shift10("0 ps"),
		.duty_cycle10(50),
		.output_clock_frequency11("0 MHz"),
		.phase_shift11("0 ps"),
		.duty_cycle11(50),
		.output_clock_frequency12("0 MHz"),
		.phase_shift12("0 ps"),
		.duty_cycle12(50),
		.output_clock_frequency13("0 MHz"),
		.phase_shift13("0 ps"),
		.duty_cycle13(50),
		.output_clock_frequency14("0 MHz"),
		.phase_shift14("0 ps"),
		.duty_cycle14(50),
		.output_clock_frequency15("0 MHz"),
		.phase_shift15("0 ps"),
		.duty_cycle15(50),
		.output_clock_frequency16("0 MHz"),
		.phase_shift16("0 ps"),
		.duty_cycle16(50),
		.output_clock_frequency17("0 MHz"),
		.phase_shift17("0 ps"),
		.duty_cycle17(50),
		.pll_type("Cyclone V"),
		.pll_subtype("Reconfigurable"),
		.m_cnt_hi_div(4),
		.m_cnt_lo_div(4),
		.n_cnt_hi_div(256),
		.n_cnt_lo_div(256),
		.m_cnt_bypass_en("false"),
		.n_cnt_bypass_en("true"),
		.m_cnt_odd_div_duty_en("false"),
		.n_cnt_odd_div_duty_en("false"),
		.c_cnt_hi_div0(3),
		.c_cnt_lo_div0(3),
		.c_cnt_prst0(1),
		.c_cnt_ph_mux_prst0(0),
		.c_cnt_in_src0("ph_mux_clk"),
		.c_cnt_bypass_en0("false"),
		.c_cnt_odd_div_duty_en0("false"),
		.c_cnt_hi_div1(9),
		.c_cnt_lo_div1(9),
		.c_cnt_prst1(4),
		.c_cnt_ph_mux_prst1(0),
		.c_cnt_in_src1("ph_mux_clk"),
		.c_cnt_bypass_en1("false"),
		.c_cnt_odd_div_duty_en1("false"),
		.c_cnt_hi_div2(1),
		.c_cnt_lo_div2(1),
		.c_cnt_prst2(1),
		.c_cnt_ph_mux_prst2(0),
		.c_cnt_in_src2("ph_mux_clk"),
		.c_cnt_bypass_en2("true"),
		.c_cnt_odd_div_duty_en2("false"),
		.c_cnt_hi_div3(1),
		.c_cnt_lo_div3(1),
		.c_cnt_prst3(1),
		.c_cnt_ph_mux_prst3(0),
		.c_cnt_in_src3("ph_mux_clk"),
		.c_cnt_bypass_en3("true"),
		.c_cnt_odd_div_duty_en3("false"),
		.c_cnt_hi_div4(1),
		.c_cnt_lo_div4(1),
		.c_cnt_prst4(1),
		.c_cnt_ph_mux_prst4(0),
		.c_cnt_in_src4("ph_mux_clk"),
		.c_cnt_bypass_en4("true"),
		.c_cnt_odd_div_duty_en4("false"),
		.c_cnt_hi_div5(1),
		.c_cnt_lo_div5(1),
		.c_cnt_prst5(1),
		.c_cnt_ph_mux_prst5(0),
		.c_cnt_in_src5("ph_mux_clk"),
		.c_cnt_bypass_en5("true"),
		.c_cnt_odd_div_duty_en5("false"),
		.c_cnt_hi_div6(1),
		.c_cnt_lo_div6(1),
		.c_cnt_prst6(1),
		.c_cnt_ph_mux_prst6(0),
		.c_cnt_in_src6("ph_mux_clk"),
		.c_cnt_bypass_en6("true"),
		.c_cnt_odd_div_duty_en6("false"),
		.c_cnt_hi_div7(1),
		.c_cnt_lo_div7(1),
		.c_cnt_prst7(1),
		.c_cnt_ph_mux_prst7(0),
		.c_cnt_in_src7("ph_mux_clk"),
		.c_cnt_bypass_en7("true"),
		.c_cnt_odd_div_duty_en7("false"),
		.c_cnt_hi_div8(1),
		.c_cnt_lo_div8(1),
		.c_cnt_prst8(1),
		.c_cnt_ph_mux_prst8(0),
		.c_cnt_in_src8("ph_mux_clk"),
		.c_cnt_bypass_en8("true"),
		.c_cnt_odd_div_duty_en8("false"),
		.c_cnt_hi_div9(1),
		.c_cnt_lo_div9(1),
		.c_cnt_prst9(1),
		.c_cnt_ph_mux_prst9(0),
		.c_cnt_in_src9("ph_mux_clk"),
		.c_cnt_bypass_en9("true"),
		.c_cnt_odd_div_duty_en9("false"),
		.c_cnt_hi_div10(1),
		.c_cnt_lo_div10(1),
		.c_cnt_prst10(1),
		.c_cnt_ph_mux_prst10(0),
		.c_cnt_in_src10("ph_mux_clk"),
		.c_cnt_bypass_en10("true"),
		.c_cnt_odd_div_duty_en10("false"),
		.c_cnt_hi_div11(1),
		.c_cnt_lo_div11(1),
		.c_cnt_prst11(1),
		.c_cnt_ph_mux_prst11(0),
		.c_cnt_in_src11("ph_mux_clk"),
		.c_cnt_bypass_en11("true"),
		.c_cnt_odd_div_duty_en11("false"),
		.c_cnt_hi_div12(1),
		.c_cnt_lo_div12(1),
		.c_cnt_prst12(1),
		.c_cnt_ph_mux_prst12(0),
		.c_cnt_in_src12("ph_mux_clk"),
		.c_cnt_bypass_en12("true"),
		.c_cnt_odd_div_duty_en12("false"),
		.c_cnt_hi_div13(1),
		.c_cnt_lo_div13(1),
		.c_cnt_prst13(1),
		.c_cnt_ph_mux_prst13(0),
		.c_cnt_in_src13("ph_mux_clk"),
		.c_cnt_bypass_en13("true"),
		.c_cnt_odd_div_duty_en13("false"),
		.c_cnt_hi_div14(1),
		.c_cnt_lo_div14(1),
		.c_cnt_prst14(1),
		.c_cnt_ph_mux_prst14(0),
		.c_cnt_in_src14("ph_mux_clk"),
		.c_cnt_bypass_en14("true"),
		.c_cnt_odd_div_duty_en14("false"),
		.c_cnt_hi_div15(1),
		.c_cnt_lo_div15(1),
		.c_cnt_prst15(1),
		.c_cnt_ph_mux_prst15(0),
		.c_cnt_in_src15("ph_mux_clk"),
		.c_cnt_bypass_en15("true"),
		.c_cnt_odd_div_duty_en15("false"),
		.c_cnt_hi_div16(1),
		.c_cnt_lo_div16(1),
		.c_cnt_prst16(1),
		.c_cnt_ph_mux_prst16(0),
		.c_cnt_in_src16("ph_mux_clk"),
		.c_cnt_bypass_en16("true"),
		.c_cnt_odd_div_duty_en16("false"),
		.c_cnt_hi_div17(1),
		.c_cnt_lo_div17(1),
		.c_cnt_prst17(1),
		.c_cnt_ph_mux_prst17(0),
		.c_cnt_in_src17("ph_mux_clk"),
		.c_cnt_bypass_en17("true"),
		.c_cnt_odd_div_duty_en17("false"),
		.pll_vco_div(2),
		.pll_cp_current(20),
		.pll_bwctrl(4000),
		.pll_output_clk_frequency("445.661998 MHz"),
		.pll_fractional_division("3922335847"),
		.mimic_fbclk_type("none"),
		.pll_fbclk_mux_1("glb"),
		.pll_fbclk_mux_2("m_cnt"),
		.pll_m_cnt_in_src("ph_mux_clk"),
		.pll_slf_rst("true")
	) altera_pll_i (
		.rst      (rst),
		.outclk   ({outclk_1, outclk_0}),
		.locked   (locked),
		.reconfig_to_pll   (reconfig_to_pll),
		.reconfig_from_pll (reconfig_from_pll),
		.fboutclk ( ),
		.fbclk    (1'b0),
		.refclk   (refclk)
	);

`else
	assign outclk_0 = refclk;
	assign outclk_1 = refclk;
	assign locked   = 1'b1;
	assign reconfig_from_pll = 64'd0;
`endif

endmodule
