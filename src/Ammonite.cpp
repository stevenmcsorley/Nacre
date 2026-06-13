// Ammonite — a clocked multi-line stereo delay network for VCV Rack,
// inspired by the architecture of the Qu-Bit Nautilus (8 delay lines,
// clock div/mult, line spreading, per-line reversal, feedback-path
// coloration, shimmer modes, and feedback routing networks).
// Original DSP implementation.

#include "plugin.hpp"
#include "layout_ammonite.hpp"

static const float MAX_LINE_SECONDS = 12.f; // buffer per line
static const float MAX_DELAY_SECONDS = 10.f;
static const int LINES_PER_CH = 4;

// Clock div/mult steps in beats (clock = quarter note), 2 bars .. 512th note.
static const float RESOLUTION_BEATS[16] = {
	8.f, 4.f, 3.f, 2.f, 1.5f, 1.f, 0.75f, 0.5f,
	1.f / 3.f, 0.25f, 1.f / 6.f, 0.125f, 1.f / 16.f, 1.f / 32.f, 1.f / 64.f, 1.f / 128.f
};

// Granular pitch shifter (dual sine-windowed heads) for shimmer feedback
// and reverse playback.
struct GrainShifter {
	std::vector<float> buf;
	int size = 0;
	int w = 0;
	float ph = 0.f;

	void setSize(int n) {
		size = n;
		buf.assign(size, 0.f);
		w = 0;
		ph = 0.f;
	}

	float read(float delay) {
		float rp = (float)w - delay;
		while (rp < 0.f)
			rp += size;
		int i0 = (int)rp;
		float fr = rp - i0;
		int i1 = i0 + 1;
		if (i1 >= size)
			i1 -= size;
		return buf[i0] + (buf[i1] - buf[i0]) * fr;
	}

	// ratio 2 = +1 octave, 0.5 = -1 octave, window W in samples
	float process(float x, float ratio, float W) {
		buf[w] = x;
		ph += (1.f - ratio);
		if (ph < 0.f)
			ph += W;
		if (ph >= W)
			ph -= W;
		float d2 = ph + W * 0.5f;
		if (d2 >= W)
			d2 -= W;
		float g1 = std::sin(M_PI * ph / W);
		float g2 = std::sin(M_PI * d2 / W);
		float out = read(ph) * g1 + read(d2) * g2;
		w++;
		if (w >= size)
			w = 0;
		return out * 0.82f;
	}
};

// Feedback-path coloration effects ("color" knob). One instance per line.
struct ColorFx {
	float lp[4] = {};
	float hpLp[4] = {};
	float held = 0.f;
	int holdCount = 0;

	float onePoleChain(float* s, float x, float a) {
		for (int i = 0; i < 4; i++) {
			s[i] += a * (x - s[i]);
			x = s[i];
		}
		return x;
	}

	float process(float x, int effect, float depth, float sr) {
		switch (effect) {
			case 0: { // lowpass: closes with depth
				float fc = 18000.f * std::pow(120.f / 18000.f, depth);
				float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
				return onePoleChain(lp, x, a);
			}
			case 1: { // highpass: rises with depth
				float fc = 20.f * std::pow(4000.f / 20.f, depth);
				float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
				return x - onePoleChain(hpLp, x, a);
			}
			case 2: { // crush: bit depth + sample rate reduction
				int holdN = 1 + (int)(depth * 30.f);
				if (++holdCount >= holdN) {
					holdCount = 0;
					float bits = 12.f - depth * 9.f;
					float q = std::pow(2.f, bits);
					held = std::round(clamp(x, -1.f, 1.f) * q) / q;
				}
				return held;
			}
			case 3: { // saturate
				float g = 1.f + depth * 8.f;
				return std::tanh(x * g) / std::tanh(g * 0.4f + 0.6f);
			}
			case 4: { // wavefold
				float g = 1.f + depth * 6.f;
				float y = x * g;
				// triangle fold into [-1, 1]
				y = 4.f * (std::fabs(0.25f * y + 0.25f - std::round(0.25f * y + 0.25f)) - 0.25f);
				return y * 0.9f;
			}
			default: { // distort
				float g = 1.f + depth * 25.f;
				float y = clamp(x * g, -1.3f, 1.3f);
				return std::tanh(y * 1.5f) * 0.85f;
			}
		}
	}
};

struct DelayLine {
	std::vector<float> buf;
	int size = 0;
	int w = 0;
	// time behavior
	float curDelay = 4800.f;
	float oldDelay = 4800.f;
	float xfade = 1.f; // 1 = settled
	float revPh = 0.f;
	float gain = 0.f;       // sensor activation ramp
	float frozenLen = 0.f;
	float freezePh = 0.f;
	GrainShifter shifter;
	ColorFx fx;

	void setSize(int n) {
		size = n;
		buf.assign(size, 0.f);
		w = 0;
		xfade = 1.f;
		revPh = 0.f;
	}

	void clear() {
		std::fill(buf.begin(), buf.end(), 0.f);
		std::fill(std::begin(fx.lp), std::end(fx.lp), 0.f);
		std::fill(std::begin(fx.hpLp), std::end(fx.hpLp), 0.f);
		std::fill(shifter.buf.begin(), shifter.buf.end(), 0.f);
	}

	float read(float delay) {
		delay = clamp(delay, 1.f, (float)(size - 4));
		float rp = (float)w - delay;
		while (rp < 0.f)
			rp += size;
		int i0 = (int)rp;
		float fr = rp - i0;
		int i1 = i0 + 1;
		if (i1 >= size)
			i1 -= size;
		return buf[i0] + (buf[i1] - buf[i0]) * fr;
	}

	void write(float x) {
		buf[w] = x;
		w++;
		if (w >= size)
			w = 0;
	}
};

struct Ammonite : Module {
	enum ParamId {
		TAPS_PARAM,
		DIV_PARAM,
		COLOR_PARAM,
		DEPTH_PARAM,
		SPREAD_PARAM,
		REVERSE_PARAM,
		FDBK_PARAM,
		MIX_PARAM,
		TAP_PARAM,
		FREEZE_PARAM,
		PURGE_PARAM,
		MODE_PARAM,
		ROUTE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TAPS_INPUT,
		DIV_INPUT,
		COLOR_INPUT,
		DEPTH_INPUT,
		SPREAD_INPUT,
		REVERSE_INPUT,
		FDBK_INPUT,
		MIX_INPUT,
		CLOCK_INPUT,
		FREEZE_INPUT,
		PURGE_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		SONAR_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		FREEZE_LIGHT,
		ENUMS(MODE_LIGHT, 3),
		ENUMS(ROUTE_LIGHT, 3),
		ENUMS(COLOR_LIGHT, 3),
		CLOCK_LIGHT,
		LIGHTS_LEN
	};

	DelayLine lines[2][LINES_PER_CH];

	int delayMode = 0;  // 0 fade, 1 doppler, 2 shimmer, 3 de-shimmer
	int routeMode = 0;  // 0 normal, 1 ping pong, 2 cascade, 3 adrift
	bool frozen = false;
	int sonarMode = 0;  // 0 stepped CV, 1 tap pings, 2 clock passthrough

	float clockPeriod = 0.5f; // 120 BPM
	float clockPhase = 0.f;
	float tapTimer = 1e9f;
	float lastExtEdge = 1e9f;
	float purgeRamp = 1.f;
	// sonar: stepped CV / pings driven by the delay taps
	float sonarTickPh[2][LINES_PER_CH] = {};
	float sonarStep = 0.4f;
	float sonarPing = 0.f;
	// configurable input gain and CV attenuverters (hardware: assignable
	// attenuverters + Tap-combo input level, here via the context menu)
	float inputGainDb = 0.f;       // -12..+12 dB
	float spreadCvAmt = 1.f;       // -1..+1
	float fdbkCvAmt = 1.f;         // -1..+1

	dsp::SchmittTrigger clockTrig, purgeTrig;
	dsp::BooleanTrigger tapBtn, freezeBtn, purgeBtn, modeBtn, routeBtn;
	dsp::ClockDivider lightDivider;

	Ammonite() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TAPS_PARAM, 0.f, 1.f, 0.25f, "Taps (delay lines per channel)");
		configParam(DIV_PARAM, 0.f, 1.f, 0.33f, "Clock div/mult");
		configParam(COLOR_PARAM, 0.f, 1.f, 0.f, "Color (feedback effect select)");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.f, "Depth (effect amount)", "%", 0.f, 100.f);
		configParam(SPREAD_PARAM, 0.f, 1.f, 0.2f, "Spread (line spacing)", "%", 0.f, 100.f);
		configParam(REVERSE_PARAM, 0.f, 1.f, 0.f, "Reverse (lines played backwards)");
		configParam(FDBK_PARAM, 0.f, 1.f, 0.4f, "Feedback", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Dry/wet mix", "%", 0.f, 100.f);
		configButton(TAP_PARAM, "Tap tempo");
		configButton(FREEZE_PARAM, "Freeze");
		configButton(PURGE_PARAM, "Purge (clear delay lines)");
		configButton(MODE_PARAM, "Delay mode (fade/doppler/shimmer/de-shimmer)");
		configButton(ROUTE_PARAM, "Feedback routing (normal/ping-pong/cascade/adrift)");
		configInput(TAPS_INPUT, "Taps CV");
		configInput(DIV_INPUT, "Div/mult CV");
		configInput(COLOR_INPUT, "Color CV");
		configInput(DEPTH_INPUT, "Depth CV");
		configInput(SPREAD_INPUT, "Spread CV");
		configInput(REVERSE_INPUT, "Reverse CV");
		configInput(FDBK_INPUT, "Feedback CV");
		configInput(MIX_INPUT, "Mix CV");
		configInput(CLOCK_INPUT, "Clock");
		configInput(FREEZE_INPUT, "Freeze gate");
		configInput(PURGE_INPUT, "Purge trigger");
		configInput(IN_L_INPUT, "Left audio");
		configInput(IN_R_INPUT, "Right audio (normalled to left)");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		configOutput(SONAR_OUTPUT, "Sonar (envelope follower / gate)");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		lightDivider.setDivision(64);
		allocate(44100.f);
	}

	void allocate(float sr) {
		int n = (int)(sr * MAX_LINE_SECONDS);
		for (int c = 0; c < 2; c++) {
			for (int i = 0; i < LINES_PER_CH; i++) {
				lines[c][i].setSize(n);
				lines[c][i].shifter.setSize((int)(sr * 0.12f));
			}
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	void onReset() override {
		delayMode = 0;
		routeMode = 0;
		frozen = false;
		sonarMode = 0;
		clockPeriod = 0.5f;
		inputGainDb = 0.f;
		spreadCvAmt = 1.f;
		fdbkCvAmt = 1.f;
		purge();
	}

	void purge() {
		for (int c = 0; c < 2; c++)
			for (int i = 0; i < LINES_PER_CH; i++)
				lines[c][i].clear();
		purgeRamp = 0.f;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "delayMode", json_integer(delayMode));
		json_object_set_new(root, "routeMode", json_integer(routeMode));
		json_object_set_new(root, "frozen", json_boolean(frozen));
		json_object_set_new(root, "sonarMode", json_integer(sonarMode));
		json_object_set_new(root, "inputGainDb", json_real(inputGainDb));
		json_object_set_new(root, "spreadCvAmt", json_real(spreadCvAmt));
		json_object_set_new(root, "fdbkCvAmt", json_real(fdbkCvAmt));
		json_object_set_new(root, "clockPeriod", json_real(clockPeriod));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "delayMode")))
			delayMode = json_integer_value(j);
		if ((j = json_object_get(root, "routeMode")))
			routeMode = json_integer_value(j);
		if ((j = json_object_get(root, "frozen")))
			frozen = json_boolean_value(j);
		if ((j = json_object_get(root, "sonarMode")))
			sonarMode = json_integer_value(j);
		if ((j = json_object_get(root, "inputGainDb")))
			inputGainDb = json_real_value(j);
		if ((j = json_object_get(root, "spreadCvAmt")))
			spreadCvAmt = json_real_value(j);
		if ((j = json_object_get(root, "fdbkCvAmt")))
			fdbkCvAmt = json_real_value(j);
		if ((j = json_object_get(root, "clockPeriod")))
			clockPeriod = json_real_value(j);
	}

	float cvParam(int paramId, int inputId) {
		return clamp(params[paramId].getValue() + inputs[inputId].getVoltage() / 10.f, 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;
		if (lines[0][0].size == 0)
			return;

		// ---- buttons / gates ----
		if (modeBtn.process(params[MODE_PARAM].getValue() > 0.f))
			delayMode = (delayMode + 1) % 4;
		if (routeBtn.process(params[ROUTE_PARAM].getValue() > 0.f))
			routeMode = (routeMode + 1) % 4;
		if (freezeBtn.process(params[FREEZE_PARAM].getValue() > 0.f))
			frozen = !frozen;
		bool gateFrozen = inputs[FREEZE_INPUT].isConnected() && inputs[FREEZE_INPUT].getVoltage() > 0.5f;
		bool isFrozen = frozen || gateFrozen;
		if (purgeBtn.process(params[PURGE_PARAM].getValue() > 0.f) ||
		    purgeTrig.process(inputs[PURGE_INPUT].getVoltage(), 0.1f, 1.f))
			purge();
		purgeRamp = std::min(1.f, purgeRamp + dt * 50.f);

		// ---- clock: tap tempo + external ----
		tapTimer += dt;
		if (tapBtn.process(params[TAP_PARAM].getValue() > 0.f)) {
			if (tapTimer < 4.f && tapTimer > 0.001f)
				clockPeriod = clamp(tapTimer, 0.001f, 4.f);
			tapTimer = 0.f;
		}
		lastExtEdge += dt;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 0.4f)) {
			if (lastExtEdge < 4.f && lastExtEdge > 0.001f)
				clockPeriod = clamp(lastExtEdge, 0.001f, 4.f);
			lastExtEdge = 0.f;
			clockPhase = 0.f;
		}
		clockPhase += dt / clockPeriod;
		if (clockPhase >= 1.f)
			clockPhase -= 1.f;

		// ---- parameters ----
		float taps = cvParam(TAPS_PARAM, TAPS_INPUT);
		int numLines = 1 + (int)(taps * 3.f + 0.5f);
		int divIdx = clamp((int)std::round(cvParam(DIV_PARAM, DIV_INPUT) * 15.f), 0, 15);
		float baseDelay = clamp(clockPeriod * RESOLUTION_BEATS[divIdx], 0.001f, MAX_DELAY_SECONDS);
		// spread/feedback CV pass through assignable attenuverters
		float spread = clamp(params[SPREAD_PARAM].getValue() + spreadCvAmt * inputs[SPREAD_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float reverse = cvParam(REVERSE_PARAM, REVERSE_INPUT);
		int numReversed = clamp((int)std::round(reverse * (2 * numLines)), 0, 2 * numLines);
		float fdbk = clamp(params[FDBK_PARAM].getValue() + fdbkCvAmt * inputs[FDBK_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float mix = cvParam(MIX_PARAM, MIX_INPUT);
		float colorV = cvParam(COLOR_PARAM, COLOR_INPUT);
		int colorFx = clamp((int)(colorV * 6.f), 0, 5);
		float depth = cvParam(DEPTH_PARAM, DEPTH_INPUT);
		float shiftRatio = (delayMode == 2) ? 2.f : (delayMode == 3) ? 0.5f : 1.f;
		// serial modes always run the whole chain; taps picks which line
		// outputs are audible
		bool serial = (routeMode >= 2);

		float inGain = std::pow(10.f, inputGainDb / 20.f);
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f * inGain;
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f * inGain : inL;
		float ins[2] = {inL, inR};

		// ---- read all line outputs ----
		float outs[2][LINES_PER_CH];
		float wet[2] = {};
		int active[2] = {};
		for (int c = 0; c < 2; c++) {
			for (int i = 0; i < LINES_PER_CH; i++) {
				DelayLine& dl = lines[c][i];
				// line spacing; right channel slightly offset for width
				float t = baseDelay * (1.f + spread * i);
				if (c == 1)
					t *= 1.f + spread * 0.02f;
				t = clamp(t, 0.001f, MAX_DELAY_SECONDS);
				float targetSamp = t * sr;

				// sensor activation ramp. In the serial modes (cascade and
				// adrift) the full chain always runs and taps only selects
				// which line outputs feed the wet mix.
				float targetGain = (serial || i < numLines) ? 1.f : 0.f;
				dl.gain += clamp(targetGain - dl.gain, -dt * 100.f, dt * 100.f);

				// reversal order: 1L, 1R, 2L, 2R, ...
				bool reversed = (2 * i + c) < numReversed;

				float y = 0.f;
				if (dl.gain > 0.001f) {
					float useDelay;
					if (isFrozen) {
						// beat repeat: the frozen slice follows the current
						// resolution, so DIV re-slices the frozen buffer in sync
						dl.frozenLen = clamp(targetSamp, 32.f, MAX_DELAY_SECONDS * sr);
						dl.freezePh += 1.f;
						if (dl.freezePh >= dl.frozenLen)
							dl.freezePh -= dl.frozenLen;
						useDelay = dl.frozenLen + 1.f - dl.freezePh;
						y = dl.read(useDelay);
					}
					else {
						dl.frozenLen = 0.f;
						if (delayMode == 1) {
							// doppler: slew the delay time, pitch artifacts
							dl.curDelay += (targetSamp - dl.curDelay) * (1.f - std::exp(-dt / 0.25f));
							y = dl.read(dl.curDelay);
						}
						else {
							// fade: crossfade between old and new times
							if (dl.xfade >= 1.f && std::fabs(targetSamp - dl.curDelay) > dl.curDelay * 0.01f + 4.f) {
								dl.oldDelay = dl.curDelay;
								dl.curDelay = targetSamp;
								dl.xfade = 0.f;
							}
							if (dl.xfade < 1.f) {
								dl.xfade = std::min(1.f, dl.xfade + dt / 0.05f);
								float gNew = std::sin(dl.xfade * M_PI_2);
								float gOld = std::cos(dl.xfade * M_PI_2);
								y = dl.read(dl.curDelay) * gNew + dl.read(dl.oldDelay) * gOld;
							}
							else {
								y = dl.read(dl.curDelay);
							}
						}
						if (reversed) {
							// dual-head reverse read over a 2x window
							float W = clamp(dl.curDelay, 0.05f * sr, 5.f * sr);
							dl.revPh += 2.f;
							if (dl.revPh >= 2.f * W)
								dl.revPh -= 2.f * W;
							float d1 = dl.revPh;
							float d2 = dl.revPh + W;
							if (d2 >= 2.f * W)
								d2 -= 2.f * W;
							float g1 = std::sin(M_PI * d1 / (2.f * W));
							float g2 = std::sin(M_PI * d2 / (2.f * W));
							y = dl.read(d1 + 1.f) * g1 + dl.read(d2 + 1.f) * g2;
						}
					}
					y *= dl.gain;
				}
				outs[c][i] = y;
				// wet mix: serial modes tap only the first `numLines` outputs
				if (dl.gain > 0.001f && (!serial || i < numLines)) {
					wet[c] += y;
					active[c]++;
				}
			}
		}
		for (int c = 0; c < 2; c++)
			wet[c] *= purgeRamp / std::sqrt((float)std::max(1, active[c]));

		// ---- feedback routing + write ----
		if (!isFrozen) {
			for (int c = 0; c < 2; c++) {
				for (int i = 0; i < LINES_PER_CH; i++) {
					DelayLine& dl = lines[c][i];
					float w;
					if (serial) {
						// cascade: 1L->2L->3L->4L->(fdbk)->1L per channel.
						// adrift: same chain but every hop crosses to the
						// opposite stereo channel. Dry input enters the first
						// line only; feedback and shimmer apply once per loop.
						int oc = (routeMode == 2) ? c : 1 - c;
						if (i == 0) {
							float fb = outs[oc][LINES_PER_CH - 1] * fdbk;
							if (shiftRatio != 1.f)
								fb = dl.shifter.process(fb, shiftRatio, 0.1f * sr);
							w = ins[c] + fb;
						}
						else {
							w = outs[oc][i - 1];
						}
					}
					else {
						float src = (routeMode == 1) ? outs[1 - c][i] : outs[c][i];
						float fb = src * fdbk;
						if (shiftRatio != 1.f)
							fb = dl.shifter.process(fb, shiftRatio, 0.1f * sr);
						w = ins[c] + fb;
					}
					w = dl.fx.process(w, colorFx, depth, sr);
					// keep runaway feedback civilized
					w = std::tanh(w * 0.9f) * 1.111f;
					dl.write(w);
				}
			}
		}

		// ---- mix + outputs ----
		float dryGain = std::cos(mix * M_PI_2);
		float wetGain = std::sin(mix * M_PI_2);
		float outL = inL * dryGain + wet[0] * wetGain;
		float outR = inR * dryGain + wet[1] * wetGain;
		outputs[OUT_L_OUTPUT].setVoltage(clamp(outL * 5.f, -12.f, 12.f));
		outputs[OUT_R_OUTPUT].setVoltage(clamp(outR * 5.f, -12.f, 12.f));

		// ---- sonar: algorithmic CV from the delay network. Every active
		// line "pings" once per its own delay period; the overlapping pings
		// build an ever-evolving stepped CV sequence (or fire gates). ----
		sonarPing = std::max(0.f, sonarPing - dt);
		for (int c = 0; c < 2; c++) {
			for (int i = 0; i < LINES_PER_CH; i++) {
				if (lines[c][i].gain < 0.5f || i >= numLines)
					continue;
				float period = std::max(0.005f, baseDelay * (1.f + spread * i) * (c == 1 ? 1.f + spread * 0.02f : 1.f));
				sonarTickPh[c][i] += dt / period;
				if (sonarTickPh[c][i] >= 1.f) {
					sonarTickPh[c][i] -= 1.f;
					// additive stepped CV: each line nudges the sequence by
					// its own golden-ratio stride, so taps interleave into
					// evolving but deterministic patterns
					sonarStep += 0.6180339887f * (i + 1) / (1.f + c * 0.5f);
					sonarStep -= std::floor(sonarStep);
					sonarPing = std::max(sonarPing, (float)std::min(0.5 * (double)period, 0.02));
				}
			}
		}
		switch (sonarMode) {
			case 1: // tap pings (gates, +5V)
				outputs[SONAR_OUTPUT].setVoltage(sonarPing > 0.f ? 5.f : 0.f);
				break;
			case 2: // clock passthrough, 50% duty
				outputs[SONAR_OUTPUT].setVoltage(clockPhase < 0.5f ? 5.f : 0.f);
				break;
			default: // evolving stepped CV, 0..5V
				outputs[SONAR_OUTPUT].setVoltage(sonarStep * 5.f);
				break;
		}

		// ---- lights ----
		if (lightDivider.process()) {
			static const float modeColors[4][3] = {
				{0.f, 0.2f, 1.f},   // fade: blue
				{0.f, 1.f, 0.1f},   // doppler: green
				{1.f, 0.45f, 0.f},  // shimmer: orange
				{1.f, 0.f, 1.f},    // de-shimmer/adrift: clear magenta-purple
			};
			static const float colorColors[6][3] = {
				{0.f, 0.2f, 1.f},  // lowpass: blue
				{0.f, 1.f, 0.1f},  // highpass: green
				{0.6f, 0.f, 1.f},  // crush: purple
				{1.f, 0.45f, 0.f}, // saturate: orange
				{0.f, 1.f, 1.f},   // fold: cyan
				{1.f, 0.f, 0.f},   // distort: red
			};
			for (int k = 0; k < 3; k++) {
				lights[MODE_LIGHT + k].setBrightness(modeColors[delayMode][k]);
				lights[ROUTE_LIGHT + k].setBrightness(modeColors[routeMode][k]);
				lights[COLOR_LIGHT + k].setBrightness(colorColors[colorFx][k] * (0.25f + 0.75f * depth));
			}
			lights[FREEZE_LIGHT].setBrightness(isFrozen);
			lights[CLOCK_LIGHT].setBrightness(clockPhase < 0.1f ? 1.f : 0.f);
		}
	}

};

struct AmmoniteWidget : ModuleWidget {
	AmmoniteWidget(Ammonite* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Ammonite.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace mlayout;

		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[0], BTN_Y)), module, Ammonite::FREEZE_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(C[0] + 5.6f, BTN_Y)), module, Ammonite::FREEZE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[1], BTN_Y)), module, Ammonite::MODE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(C[1] + 5.6f, BTN_Y)), module, Ammonite::MODE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[2], BTN_Y)), module, Ammonite::ROUTE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(C[2] + 5.6f, BTN_Y)), module, Ammonite::ROUTE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[3], BTN_Y)), module, Ammonite::PURGE_PARAM));

		static const int knobsR1[4] = {Ammonite::TAPS_PARAM, Ammonite::DIV_PARAM, Ammonite::COLOR_PARAM, Ammonite::DEPTH_PARAM};
		static const int knobsR2[4] = {Ammonite::SPREAD_PARAM, Ammonite::REVERSE_PARAM, Ammonite::FDBK_PARAM, Ammonite::MIX_PARAM};
		static const int cvR3[4] = {Ammonite::TAPS_INPUT, Ammonite::DIV_INPUT, Ammonite::COLOR_INPUT, Ammonite::DEPTH_INPUT};
		static const int cvR4[4] = {Ammonite::SPREAD_INPUT, Ammonite::REVERSE_INPUT, Ammonite::FDBK_INPUT, Ammonite::MIX_INPUT};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(C[i], K1_Y)), module, knobsR1[i]));
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(C[i], K2_Y)), module, knobsR2[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[i], R3)), module, cvR3[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[i], R4)), module, cvR4[i]));
		}
		addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(CHROMA_LED_X, CHROMA_LED_Y)), module, Ammonite::COLOR_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J5[0], R5)), module, Ammonite::CLOCK_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(J5[1], R5)), module, Ammonite::TAP_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J5[2], R5)), module, Ammonite::FREEZE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J5[3], R5)), module, Ammonite::PURGE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J5[4], R5)), module, Ammonite::SONAR_OUTPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J5[0], R6)), module, Ammonite::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J5[1], R6)), module, Ammonite::IN_R_INPUT));
		addChild(createLightCentered<SmallLight<WhiteLight>>(mm2px(Vec(CLK_LED_X, CLK_LED_Y)), module, Ammonite::CLOCK_LIGHT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J5[3], R6)), module, Ammonite::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J5[4], R6)), module, Ammonite::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Ammonite* module = getModule<Ammonite>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Delay mode",
			{"Fade", "Doppler", "Shimmer", "De-shimmer"}, &module->delayMode));
		menu->addChild(createIndexPtrSubmenuItem("Feedback routing",
			{"Normal", "Ping pong", "Cascade", "Adrift"}, &module->routeMode));
		menu->addChild(createIndexPtrSubmenuItem("Sonar output",
			{"Stepped CV (evolving)", "Tap pings (gates)", "Clock passthrough"}, &module->sonarMode));
		menu->addChild(createBoolPtrMenuItem("Freeze", "", &module->frozen));
		menu->addChild(new MenuSeparator);
		{
			struct GainQuantity : Quantity {
				Ammonite* m;
				void setValue(float v) override { m->inputGainDb = clamp(v, -12.f, 12.f); }
				float getValue() override { return m->inputGainDb; }
				float getMinValue() override { return -12.f; }
				float getMaxValue() override { return 12.f; }
				float getDefaultValue() override { return 0.f; }
				std::string getLabel() override { return "Input level"; }
				std::string getUnit() override { return " dB"; }
				int getDisplayPrecision() override { return 3; }
			};
			ui::Slider* s = new ui::Slider;
			GainQuantity* q = new GainQuantity;
			q->m = module;
			s->quantity = q;
			s->box.size.x = 220.f;
			menu->addChild(s);
		}
		{
			struct CvAmtQuantity : Quantity {
				float* v;
				std::string label;
				void setValue(float x) override { *v = clamp(x, -1.f, 1.f); }
				float getValue() override { return *v; }
				float getMinValue() override { return -1.f; }
				float getMaxValue() override { return 1.f; }
				float getDefaultValue() override { return 1.f; }
				std::string getLabel() override { return label; }
				std::string getUnit() override { return "%"; }
				float getDisplayValue() override { return *v * 100.f; }
				void setDisplayValue(float x) override { setValue(x / 100.f); }
				int getDisplayPrecision() override { return 3; }
			};
			ui::Slider* s1 = new ui::Slider;
			CvAmtQuantity* q1 = new CvAmtQuantity;
			q1->v = &module->spreadCvAmt;
			q1->label = "Spread CV attenuverter";
			s1->quantity = q1;
			s1->box.size.x = 220.f;
			menu->addChild(s1);
			ui::Slider* s2 = new ui::Slider;
			CvAmtQuantity* q2 = new CvAmtQuantity;
			q2->v = &module->fdbkCvAmt;
			q2->label = "Feedback CV attenuverter";
			s2->quantity = q2;
			s2->box.size.x = 220.f;
			menu->addChild(s2);
		}
	}
};

Model* modelAmmonite = createModel<Ammonite, AmmoniteWidget>("Ammonite");
