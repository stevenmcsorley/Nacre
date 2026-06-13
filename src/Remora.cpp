// Remora — a 32-program stereo FX workhorse for VCV Rack, inspired by the
// Happy Nerding FX AID and its factory default program bank. Original DSP.
//
// All 32 default programs, same order and same Control 1/2/3 assignments as
// the factory FX list. Per the hardware conventions: "Tone" is bipolar
// (neutral at noon, CW cuts lows, CCW cuts highs); "Damping" is full CCW =
// most HF cut. Programs are selected with the prev/next buttons (bank +
// program LEDs) or from the context menu.

#include "plugin.hpp"
#include "layout_remora.hpp"

// ------------------------------------------------------------ FX table ----
struct FXDef {
	const char* name;
	const char* c1;
	const char* c2;
	const char* c3;
};
static const FXDef FXDEFS[32] = {
	{"Delay Tape (3 heads)", "Delay time", "Feedback", "Heads ratio"},
	{"Delay Ping-Pong", "Delay time", "Feedback", "Tone"},
	{"Delay HP (resonant 2-pole in loop)", "Delay time", "Feedback", "Cutoff"},
	{"Delay LP (resonant 4-pole in loop)", "Delay time", "Feedback", "Cutoff"},
	{"Delay Freq Shift (in loop)", "Delay time", "Feedback", "± Freq shift"},
	{"Delay Pitch Shift (in loop)", "Delay time", "Feedback", "± Pitch shift"},
	{"Delay Clock Sync (CV 1 = clock)", "(clock via CV 1)", "Feedback", "Divider"},
	{"Delay Comb", "Frequency", "Resonance", "± Feedforward"},
	{"Delay Magneto (4 heads)", "Delay time", "Feedback", "Chorus"},
	{"Delay into Reverb", "Delay time", "Feedback", "± Reverb time/amount"},
	{"Delay into Dual Shimmer", "Delay time", "Feedback", "± Shimmer time/amount"},
	{"Delay Vowel (pre-feedback filter)", "Delay time", "Feedback", "Vowel"},
	{"Filter Vowel", "± Formant shift", "Resonance", "Vowel"},
	{"Wave Folder", "Fold amount", "± Symmetry", "Stereo width"},
	{"Sample Rate Reducer", "Amount", "Pre-FX tone", "Post-FX tone"},
	{"Bit Crusher", "Amount", "Pre-FX tone", "Post-FX tone"},
	{"Flanger", "Rate (0 = manual)", "Range", "± Feedback"},
	{"Phaser Classic 12", "Rate (0 = manual)", "Range", "± Feedback"},
	{"Pitch Shifter (±2 oct)", "± Pitch shift", "± Feedback", "Tone"},
	{"Pitch Shifter Dual (semitones)", "± Pitch 1", "± Pitch 2", "1 <> 2 crossfade"},
	{"Freq Shifter Up-Dn", "± Freq shift", "± Feedback (- swaps L/R)", "Feedback delay"},
	{"Chorus into Reverb (8 voice)", "Spread", "Stereo width", "Reverb amount"},
	{"Reverb Spring", "Decay time", "± Resonance", "Damping"},
	{"Reverb Plate Classic", "Decay time", "Pre-delay", "Tone"},
	{"Reverb Room Stereo", "Decay time", "Chorus", "Tone"},
	{"Reverb Hall Chorus", "Decay time", "Chorus", "Tone"},
	{"Reverb Hall Medium", "Decay time", "Pre-delay", "Tone"},
	{"Reverb Black Hole", "Decay time", "Gravity", "Tone"},
	{"Reverb Cloud", "Decay time", "Chorus", "Diffusion"},
	{"Reverb Gray Hole Light", "Decay time", "± Gravity", "Tone"},
	{"Reverb Shimmer Input Dual", "Decay time", "Shimmer amount", "Up <> down"},
	{"Reverb Shimmer Dual (regen)", "Decay time", "Shimmer amount", "Up <> down"},
};

// ---------------------------------------------------------- DSP blocks ----
struct DLine {
	std::vector<float> buf;
	int pos = 0;
	void ensure(int n) {
		if ((int)buf.size() < n) {
			buf.assign(n, 0.f);
			pos = 0;
		}
	}
	void write(float x) {
		buf[pos] = x;
		if (++pos >= (int)buf.size())
			pos = 0;
	}
	float tap(float d) const {  // d in samples, before the latest write
		int n = buf.size();
		d = clamp(d, 1.f, n - 2.f);
		float p = pos - 1 - d;
		while (p < 0)
			p += n;
		int i = (int)p;
		float f = p - i;
		int j = i + 1 == n ? 0 : i + 1;
		return buf[i] + (buf[j] - buf[i]) * f;
	}
	void clear() { std::fill(buf.begin(), buf.end(), 0.f); pos = 0; }
};

// Bipolar tone: t=0 neutral, t>0 cuts lows (1-pole HP), t<0 cuts highs.
struct Tone {
	float s = 0.f;
	float process(float x, float t, float sr) {
		float a = std::fabs(t);
		if (a < 0.03f)
			return x;
		if (t > 0.f) {
			float fc = 30.f * std::pow(40.f, a);  // 30..1200 Hz
			float k = 1.f - std::exp(-2.f * M_PI * fc / sr);
			s += k * (x - s);
			return x - s;
		}
		float fc = 18000.f * std::pow(0.014f, a);  // 18k..250 Hz
		float k = 1.f - std::exp(-2.f * M_PI * fc / sr);
		s += k * (x - s);
		return s;
	}
};

// Damping: d=0 most HF cut, d=1 none.
struct DampLP {
	float s = 0.f;
	float process(float x, float d, float sr) {
		float fc = 400.f * std::pow(45.f, clamp(d, 0.f, 1.f));  // 400..18k
		float k = 1.f - std::exp(-2.f * M_PI * fc / sr);
		s += k * (x - s);
		return s;
	}
};

struct SVF2 {
	float ic1 = 0.f, ic2 = 0.f;
	void set(float fc, float Q, float sr, float& g, float& R) {
		g = std::tan(M_PI * clamp(fc, 10.f, sr * 0.45f) / sr);
		R = 1.f / (2.f * std::max(Q, 0.1f));
	}
	void process(float in, float g, float R, float& lp, float& bp, float& hp) {
		float a1 = 1.f / (1.f + g * (g + 2.f * R));
		hp = (in - (2.f * R + g) * ic1 - ic2) * a1;
		float v1 = g * hp;
		bp = v1 + ic1;
		ic1 = bp + v1;
		float v2 = g * bp;
		lp = v2 + ic2;
		ic2 = lp + v2;
	}
};

// Granular dual-head pitch shifter.
struct Shifter {
	DLine d;
	float ph = 0.f;
	float process(float in, float ratio, float sr) {
		d.ensure((int)(0.12f * sr));
		d.write(in);
		float win = 0.05f * sr;
		ph += (1.f - ratio);
		while (ph >= win) ph -= win;
		while (ph < 0.f) ph += win;
		float p2 = ph + win * 0.5f;
		if (p2 >= win) p2 -= win;
		float g1 = std::sin(M_PI * ph / win);
		float g2 = std::sin(M_PI * p2 / win);
		return d.tap(1.f + ph) * g1 * g1 + d.tap(1.f + p2) * g2 * g2;
	}
};

// Hilbert pair (Olli Niemitalo's 2x4 allpass design) + quadrature osc.
struct AP2 {
	float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	float process(float x, float a2) {
		float y = a2 * (x + y2) - x2;
		x2 = x1; x1 = x;
		y2 = y1; y1 = y;
		return y;
	}
};
struct Hilbert {
	AP2 i[4], q[4];
	float z = 0.f;  // one-sample delay for the I path
	void process(float x, float& I, float& Q) {
		static const float ai[4] = {0.4794008655888399f, 0.8767309966775711f, 0.9764082906845529f, 0.9974832836230965f};
		static const float aq[4] = {0.1617584983677011f, 0.7330289323414904f, 0.9457288979959557f, 0.9905856600653735f};
		float vi = x, vq = x;
		for (int k = 0; k < 4; k++) {
			vi = i[k].process(vi, ai[k] * ai[k]);
			vq = q[k].process(vq, aq[k] * aq[k]);
		}
		I = z;  // delay I by one sample to align
		z = vi;
		Q = vq;
	}
};

// Dattorro-style plate tank with size scaling, tank modulation (chorus),
// variable diffusion, and an external feedback insert point for shimmer.
struct Verb {
	DLine pre, inAp[4], tAp[4], tDel[4];
	float lp1 = 0.f, lp2 = 0.f;
	float lfo = 0.f;
	float fb1 = 0.f, fb2 = 0.f;
	static constexpr float REF = 29761.f;
	void ensure(float sr) {
		float s = sr / REF * 1.45f;  // headroom for size > 1
		pre.ensure((int)(0.16f * sr) + 4);
		static const int ia[4] = {142, 107, 379, 277};
		static const int ta[4] = {672, 1800, 908, 2656};
		static const int td[4] = {4453, 3720, 4217, 3163};
		for (int k = 0; k < 4; k++) {
			inAp[k].ensure((int)(ia[k] * sr / REF) + 4);
			tAp[k].ensure((int)(ta[k] * s) + 64);
			tDel[k].ensure((int)(td[k] * s) + 64);
		}
	}
	void clear() {
		pre.clear();
		for (int k = 0; k < 4; k++) { inAp[k].clear(); tAp[k].clear(); tDel[k].clear(); }
		lp1 = lp2 = fb1 = fb2 = 0.f;
	}
	float apTick(DLine& l, float x, float n, float coef) {
		float v = l.tap(n);
		float w = x - coef * v;
		l.write(w);
		return v + coef * w;
	}
	// returns tank cross-feed (for shimmer pickoff) via tankOut
	void process(float in, float sr, float decay, float size, float damp, float mod,
	             float diff, float preDelay, float shimIn, float& outL, float& outR, float& tankOut) {
		ensure(sr);
		float k = sr / REF;
		pre.write(in);
		float x = pre.tap(std::max(1.f, preDelay * sr));
		x = apTick(inAp[0], x, 142 * k, 0.75f * diff);
		x = apTick(inAp[1], x, 107 * k, 0.75f * diff);
		x = apTick(inAp[2], x, 379 * k, 0.625f * diff);
		x = apTick(inAp[3], x, 277 * k, 0.625f * diff);
		x += shimIn;

		lfo += 0.9f / sr;
		if (lfo >= 1.f) lfo -= 1.f;
		float m1 = std::sin(2.f * M_PI * lfo) * mod * 12.f * k;
		float m2 = std::cos(2.f * M_PI * lfo) * mod * 12.f * k;

		// branch 1
		float b1 = x + fb2 * decay;
		b1 = apTick(tAp[0], b1, 672 * k * size + m1, -0.7f * diff);
		tDel[0].write(b1);
		float d1 = tDel[0].tap(4453 * k * size);
		lp1 += (1.f - std::exp(-2.f * M_PI * (1000.f + damp * 9000.f) / sr)) * (d1 - lp1);
		float c1 = apTick(tAp[1], lp1 * decay, 1800 * k * size, 0.5f * diff);
		tDel[1].write(c1);
		fb1 = tDel[1].tap(3720 * k * size);
		// branch 2
		float b2 = x + fb1 * decay;
		b2 = apTick(tAp[2], b2, 908 * k * size + m2, -0.7f * diff);
		tDel[2].write(b2);
		float d2 = tDel[2].tap(4217 * k * size);
		lp2 += (1.f - std::exp(-2.f * M_PI * (1000.f + damp * 9000.f) / sr)) * (d2 - lp2);
		float c2 = apTick(tAp[3], lp2 * decay, 2656 * k * size, 0.5f * diff);
		tDel[3].write(c2);
		fb2 = tDel[3].tap(3163 * k * size);

		outL = 0.6f * (tDel[0].tap(266 * k * size) + tDel[0].tap(2974 * k * size)
		             - tAp[3].tap(1913 * k * size) + tDel[3].tap(1996 * k * size));
		outR = 0.6f * (tDel[2].tap(353 * k * size) + tDel[2].tap(3627 * k * size)
		             - tAp[1].tap(1228 * k * size) + tDel[1].tap(2673 * k * size));
		tankOut = fb1 + fb2;
	}
};

// vowel formant tables (F1/F2/F3)
static const float VOWELS10[10][3] = {
	{730, 1090, 2440},  // AH
	{530, 1840, 2480},  // E
	{270, 2290, 3010},  // EE
	{300, 870, 2240},   // U
	{450, 800, 2830},   // O
	{390, 1990, 2550},  // I
	{660, 1720, 2410},  // AE
	{570, 840, 2410},   // AW
	{640, 1190, 2390},  // A
	{490, 1350, 1690},  // ER
};
// 8-vowel set for the delay (AH-E-EE-O-I-AE-AW-A)
static const int V8[8] = {0, 1, 2, 4, 5, 6, 7, 8};

struct Remora : Module {
	enum ParamId { C1_PARAM, C2_PARAM, C3_PARAM, DW_PARAM, PREV_PARAM, NEXT_PARAM, PARAMS_LEN };
	enum InputId { CV1_INPUT, CV2_INPUT, CV3_INPUT, DWCV_INPUT, INL_INPUT, INR_INPUT, INPUTS_LEN };
	enum OutputId { OUTL_OUTPUT, OUTR_OUTPUT, OUTPUTS_LEN };
	enum LightId { ENUMS(LED_LIGHT, 8), LIGHTS_LEN };

	int fx = 0;
	int lastFx = -1;

	// shared state
	DLine dl[2];
	float fbL = 0.f, fbR = 0.f;
	Tone tone[4];
	DampLP damp[2];
	SVF2 svf[2][3];
	Shifter shA, shB;
	Hilbert hilb[2];
	float oscPh = 0.f;
	Verb verb;
	float shimFb = 0.f;
	float lfoPh = 0.f;
	float apz[2][13] = {};
	float holdV[2] = {};
	float holdCnt = 0.f;
	// clock sync
	dsp::SchmittTrigger clkTrig;
	float clkPeriod = 0.5f, clkTimer = 0.f;
	dsp::SchmittTrigger prevTrig, nextTrig;
	dsp::ClockDivider lightDivider;

	Remora() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		struct CtlQuantity : ParamQuantity {
			int idx;
			std::string getLabel() override {
				Remora* m = dynamic_cast<Remora*>(module);
				if (!m)
					return name;
				const FXDef& d = FXDEFS[m->fx];
				return string::f("%s", idx == 0 ? d.c1 : idx == 1 ? d.c2 : d.c3);
			}
		};
		for (int k = 0; k < 3; k++) {
			CtlQuantity* q = configParam<CtlQuantity>(C1_PARAM + k, 0.f, 1.f, 0.5f, string::f("Control %d", k + 1));
			q->idx = k;
		}
		configParam(DW_PARAM, 0.f, 1.f, 0.5f, "Dry/wet");
		configButton(PREV_PARAM, "Previous program");
		configButton(NEXT_PARAM, "Next program");
		configInput(CV1_INPUT, "Control 1 CV (clock for Delay Clock Sync)");
		configInput(CV2_INPUT, "Control 2 CV");
		configInput(CV3_INPUT, "Control 3 CV");
		configInput(DWCV_INPUT, "Dry/wet CV");
		configInput(INL_INPUT, "Left audio");
		configInput(INR_INPUT, "Right audio (normalled to left)");
		configOutput(OUTL_OUTPUT, "Left");
		configOutput(OUTR_OUTPUT, "Right");
		lightDivider.setDivision(64);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "fx", json_integer(fx));
		return rootJ;
	}
	void dataFromJson(json_t* rootJ) override {
		json_t* fxJ = json_object_get(rootJ, "fx");
		if (fxJ)
			fx = clamp((int)json_integer_value(fxJ), 0, 31);
	}

	void resetState() {
		dl[0].clear();
		dl[1].clear();
		verb.clear();
		fbL = fbR = shimFb = 0.f;
		for (int c = 0; c < 2; c++)
			for (int k = 0; k < 13; k++)
				apz[c][k] = 0.f;
	}

	float ctl(int k) {
		return clamp(params[C1_PARAM + k].getValue() + inputs[CV1_INPUT + k].getVoltage() / 10.f, 0.f, 1.f);
	}

	void process(const ProcessArgs& args) override {
		if (prevTrig.process(params[PREV_PARAM].getValue()))
			fx = (fx + 31) % 32;
		if (nextTrig.process(params[NEXT_PARAM].getValue()))
			fx = (fx + 1) % 32;
		if (fx != lastFx) {
			resetState();
			lastFx = fx;
		}

		float sr = args.sampleRate;
		float inL = inputs[INL_INPUT].getVoltage();
		float inR = inputs[INR_INPUT].isConnected() ? inputs[INR_INPUT].getVoltage() : inL;
		float mono = (inputs[INR_INPUT].isConnected() ? (inL + inR) * 0.5f : inL) / 5.f;
		float xL = inL / 5.f, xR = inR / 5.f;
		float wetL = 0.f, wetR = 0.f;

		float c1 = ctl(0), c2 = ctl(1), c3 = ctl(2);
		float b1 = (c1 - 0.5f) * 2.f, b2 = (c2 - 0.5f) * 2.f, b3 = (c3 - 0.5f) * 2.f;

		// generic delay-time mapping 10 ms .. 1.5 s
		float dTime = 0.01f * std::pow(150.f, c1);
		float dSamp = dTime * sr;
		float fb = c2 * 1.05f;

		switch (fx) {
			case 0: {  // Delay Tape: 3 heads, 1&2 -> L, 1&3 -> R
				dl[0].ensure((int)(2.f * sr));
				float h1 = dl[0].tap(dSamp);
				float h2 = dl[0].tap(dSamp * (0.3f + 0.45f * c3));
				float h3 = dl[0].tap(dSamp * (0.92f - 0.35f * c3));
				dl[0].write(mono + damp[0].process(h1, 0.55f, sr) * fb);
				wetL = (h1 + h2) * 0.7f;
				wetR = (h1 + h3) * 0.7f;
			} break;
			case 1: {  // Delay Ping-Pong: right first, left second
				dl[0].ensure((int)(2.f * sr));
				dl[1].ensure((int)(2.f * sr));
				float r = dl[0].tap(dSamp);
				float l = dl[1].tap(dSamp);
				dl[0].write(mono + tone[0].process(l, b3, sr) * fb);
				dl[1].write(r);
				wetR = r;
				wetL = l;
			} break;
			case 2:    // Delay HP: resonant 2-pole HP in loop
			case 3: {  // Delay LP: resonant 4-pole LP in loop
				dl[0].ensure((int)(2.f * sr));
				float t = dl[0].tap(dSamp);
				float fc = 40.f * std::pow(250.f, c3);
				float g, R;
				svf[0][0].set(fc, 2.6f, sr, g, R);
				float lp, bp, hp;
				svf[0][0].process(t, g, R, lp, bp, hp);
				float f1 = (fx == 2) ? hp : lp;
				if (fx == 3) {
					svf[0][1].process(f1, g, R, lp, bp, hp);
					f1 = lp;
				}
				dl[0].write(mono + clamp(f1 * fb, -3.f, 3.f));
				wetL = t;
				wetR = t;
			} break;
			case 4: {  // Delay Freq Shift in loop (±650 Hz)
				dl[0].ensure((int)(2.f * sr));
				float t = dl[0].tap(dSamp);
				float I, Q;
				hilb[0].process(t, I, Q);
				oscPh += b3 * 650.f / sr;
				oscPh -= std::floor(oscPh);
				float sh = I * std::cos(2.f * M_PI * oscPh) + Q * std::sin(2.f * M_PI * oscPh);
				dl[0].write(mono + sh * fb);
				wetL = t;
				wetR = t;
			} break;
			case 5: {  // Delay Pitch Shift in loop (±1 oct smooth)
				dl[0].ensure((int)(2.f * sr));
				float t = dl[0].tap(dSamp);
				float sh = shA.process(t, std::pow(2.f, b3), sr);
				dl[0].write(mono + sh * fb);
				wetL = t;
				wetR = t;
			} break;
			case 6: {  // Delay Clock Sync: CV1 = square clock, C3 divider
				dl[0].ensure((int)(2.f * sr));
				clkTimer += 1.f / sr;
				if (clkTrig.process(inputs[CV1_INPUT].getVoltage(), 0.1f, 1.f)) {
					if (clkTimer > 0.005f && clkTimer < 2.f)
						clkPeriod = clkTimer;
					clkTimer = 0.f;
				}
				static const float DIVS[8] = {4.f, 3.f, 2.f, 1.5f, 1.f, 0.75f, 0.5f, 0.25f};
				float d = clamp(clkPeriod / DIVS[(int)(c3 * 7.999f)], 0.01f, 1.9f) * sr;
				float t = dl[0].tap(d);
				dl[0].write(mono + damp[0].process(t, 0.7f, sr) * fb);
				wetL = t;
				wetR = t;
			} break;
			case 7: {  // Delay Comb (stereo): feedforward & feedback
				dl[0].ensure((int)(0.2f * sr));
				dl[1].ensure((int)(0.2f * sr));
				float f = 25.f * std::pow(100.f, c1);  // 25 Hz .. 2.5 kHz
				float d = sr / f;
				float tL = dl[0].tap(d), tR = dl[1].tap(d);
				dl[0].write(xL + tL * c2 * 0.95f);
				dl[1].write(xR + tR * c2 * 0.95f);
				wetL = xL * 0.5f + tL * b3;
				wetR = xR * 0.5f + tR * b3;
			} break;
			case 8: {  // Delay Magneto: 4 heads even spacing + chorus
				dl[0].ensure((int)(2.f * sr));
				lfoPh += 0.5f / sr;
				lfoPh -= std::floor(lfoPh);
				float wob = c3 * 0.0015f * sr;
				float h[4];
				for (int k = 0; k < 4; k++) {
					float m = std::sin(2.f * M_PI * (lfoPh + k * 0.25f)) * wob;
					h[k] = dl[0].tap(dSamp * (k + 1) * 0.25f + m);
				}
				dl[0].write(mono + damp[0].process(h[3], 0.6f, sr) * fb);
				wetL = (h[0] + h[2]) * 0.7f;
				wetR = (h[1] + h[3]) * 0.7f;
			} break;
			case 9:
			case 10: {  // Delay into Reverb / Dual Shimmer (outside loop)
				dl[0].ensure((int)(2.f * sr));
				float t = dl[0].tap(dSamp);
				dl[0].write(mono + damp[0].process(t, 0.7f, sr) * fb);
				float amt = std::fabs(b3);
				float decay = (b3 < 0.f) ? 0.45f : 0.62f + 0.3f * amt;
				float vl, vr;
				float shim = 0.f;
				if (fx == 10) {
					float up = shA.process(shimFb, 2.f, sr);
					float dn = shB.process(shimFb, 0.5f, sr);
					shim = (up + dn) * 0.35f * amt;
				}
				verb.process(t * amt, sr, decay, 0.9f, 0.6f, 0.15f, 0.7f, 0.f, shim, vl, vr, shimFb);
				wetL = t + vl;
				wetR = t + vr;
			} break;
			case 11: {  // Delay Vowel: pre-feedback vowel filter (8 vowels)
				dl[0].ensure((int)(2.f * sr));
				float t = dl[0].tap(dSamp);
				float vp = c3 * 6.999f;
				int v0 = (int)vp;
				float vf = vp - v0;
				float y = 0.f;
				float x = mono + t * fb;
				for (int f = 0; f < 3; f++) {
					float fc = crossfade(VOWELS10[V8[v0]][f], VOWELS10[V8[v0 + 1]][f], vf);
					float g, R;
					svf[0][f].set(fc, 7.f, sr, g, R);
					float lp, bp, hp;
					svf[0][f].process(x, g, R, lp, bp, hp);
					y += bp * (f == 0 ? 1.f : f == 1 ? 0.7f : 0.35f);
				}
				dl[0].write(y * 1.4f);
				wetL = t;
				wetR = t;
			} break;
			case 12: {  // Filter Vowel (10 vowels, formant shift, mono)
				float vp = c3 * 8.999f;
				int v0 = (int)vp;
				float vf = vp - v0;
				float shift = std::pow(2.f, b1);
				float Q = 4.f + c2 * 14.f;
				float y = 0.f;
				for (int f = 0; f < 3; f++) {
					float fc = crossfade(VOWELS10[v0][f], VOWELS10[v0 + 1][f], vf) * shift;
					float g, R;
					svf[0][f].set(fc, Q, sr, g, R);
					float lp, bp, hp;
					svf[0][f].process(mono, g, R, lp, bp, hp);
					y += bp * (f == 0 ? 1.f : f == 1 ? 0.7f : 0.35f);
				}
				wetL = y * 1.6f;
				wetR = wetL;
			} break;
			case 13: {  // Wave Folder
				auto fold = [](float x, float a, float sym) {
					x = x * (1.f + a * 7.f) + sym * a * 2.f;
					x = std::fmod(x + 3.f, 4.f);
					if (x < 0.f)
						x += 4.f;
					return std::fabs(x - 2.f) - 1.f;
				};
				wetL = fold(xL, c1, b2);
				wetR = fold(xR, c1 * (1.f - c3 * 0.35f), b2 * (1.f - c3 * 0.7f));
			} break;
			case 14:    // Sample Rate Reducer
			case 15: {  // Bit Crusher
				float pL = tone[0].process(xL, b2, sr);
				float pR = tone[1].process(xR, b2, sr);
				if (fx == 14) {
					holdCnt += 1.f;
					float step = 1.f + c1 * c1 * 220.f;
					if (holdCnt >= step) {
						holdCnt -= step;
						holdV[0] = pL;
						holdV[1] = pR;
					}
				}
				else {
					float bits = 16.f - c1 * 14.5f;
					float lv = std::pow(2.f, bits);
					holdV[0] = std::round(pL * lv) / lv;
					holdV[1] = std::round(pR * lv) / lv;
				}
				wetL = tone[2].process(holdV[0], b3, sr);
				wetR = tone[3].process(holdV[1], b3, sr);
			} break;
			case 16: {  // Flanger (stereo, rate 0 = manual via Range/CV)
				dl[0].ensure((int)(0.05f * sr));
				dl[1].ensure((int)(0.05f * sr));
				float pos;
				if (c1 < 0.02f)
					pos = c2;
				else {
					lfoPh += 0.05f * std::pow(160.f, c1) / sr;
					lfoPh -= std::floor(lfoPh);
					pos = 0.5f - 0.5f * std::cos(2.f * M_PI * lfoPh);
					pos = 0.5f + (pos - 0.5f) * c2;
				}
				float d = (0.0004f + pos * 0.008f) * sr;
				float tL = dl[0].tap(d);
				float tR = dl[1].tap(d * 1.02f);
				dl[0].write(xL + tL * b3 * 0.85f);
				dl[1].write(xR + tR * b3 * 0.85f);
				wetL = (xL + tL) * 0.7f;
				wetR = (xR + tR) * 0.7f;
			} break;
			case 17: {  // Phaser Classic 12 (mono in, stereo out)
				float pos;
				if (c1 < 0.02f)
					pos = c2;
				else {
					lfoPh += 0.04f * std::pow(120.f, c1) / sr;
					lfoPh -= std::floor(lfoPh);
					pos = 0.5f - 0.5f * std::cos(2.f * M_PI * lfoPh);
					pos = 0.5f + (pos - 0.5f) * c2;
				}
				for (int c = 0; c < 2; c++) {
					float p = (c == 0) ? pos : 1.f - pos;
					float fc = 200.f * std::pow(20.f, p);
					float a = (std::tan(M_PI * fc / sr) - 1.f) / (std::tan(M_PI * fc / sr) + 1.f);
					float x = mono + apz[c][12] * b3 * 0.8f;
					for (int s = 0; s < 12; s++) {
						float y = a * x + apz[c][s];
						apz[c][s] = x - a * y;
						x = y;
					}
					apz[c][12] = x;
					(c == 0 ? wetL : wetR) = (mono + x) * 0.7f;
				}
			} break;
			case 18: {  // Pitch Shifter ±2 oct, feedback, tone
				float x = mono + fbL * b2 * 0.85f;
				float sh = shA.process(x, std::pow(2.f, b1 * 2.f), sr);
				fbL = sh;
				wetL = tone[0].process(sh, b3, sr);
				wetR = wetL;
			} break;
			case 19: {  // Pitch Shifter Dual: semitone steps ±12, xfade
				float r1 = std::pow(2.f, std::round(b1 * 12.f) / 12.f);
				float r2 = std::pow(2.f, std::round(b2 * 12.f) / 12.f);
				float s1 = shA.process(xL, r1, sr);
				float s2 = shB.process(xR, r2, sr);
				wetL = crossfade(s1, s2, c3);
				wetR = wetL;
			} break;
			case 20: {  // Freq Shifter Up-Dn: L up / R down, fb w/ delay
				dl[0].ensure((int)(0.6f * sr));
				dl[1].ensure((int)(0.6f * sr));
				float fbd = (0.005f + c3 * 0.5f) * sr;
				float xl = xL + dl[0].tap(fbd) * std::fabs(b2) * 0.8f;
				float xr = xR + dl[1].tap(fbd) * std::fabs(b2) * 0.8f;
				float I, Q, I2, Q2;
				hilb[0].process(xl, I, Q);
				hilb[1].process(xr, I2, Q2);
				oscPh += std::fabs(b1) * 650.f / sr;
				oscPh -= std::floor(oscPh);
				float co = std::cos(2.f * M_PI * oscPh), sn = std::sin(2.f * M_PI * oscPh);
				float up = I * co + Q * sn;
				float dn = I2 * co - Q2 * sn;
				if (b1 < 0.f)
					std::swap(up, dn);
				bool swapLR = b2 < 0.f;
				wetL = swapLR ? dn : up;
				wetR = swapLR ? up : dn;
				dl[0].write(wetL);
				dl[1].write(wetR);
			} break;
			case 21: {  // Chorus into Reverb: 8 voices
				dl[0].ensure((int)(0.08f * sr));
				dl[0].write(mono);
				lfoPh += 0.35f / sr;
				lfoPh -= std::floor(lfoPh);
				float sumL = 0.f, sumR = 0.f;
				for (int v = 0; v < 8; v++) {
					float m = std::sin(2.f * M_PI * (lfoPh * (1.f + v * 0.11f) + v * 0.125f));
					float d = (0.008f + v * 0.0035f + m * 0.002f * (0.2f + c1)) * sr;
					float t = dl[0].tap(d);
					float pan = ((v & 1) ? 1.f : -1.f) * c2;
					sumL += t * (1.f - pan) * 0.5f;
					sumR += t * (1.f + pan) * 0.5f;
				}
				sumL *= 0.45f;
				sumR *= 0.45f;
				float vl, vr, tk;
				verb.process((sumL + sumR) * 0.5f, sr, 0.75f, 0.95f, 0.6f, 0.2f, 0.7f, 0.f, 0.f, vl, vr, tk);
				wetL = sumL + vl * c3 * 1.4f;
				wetR = sumR + vr * c3 * 1.4f;
			} break;
			default: {  // 22..31: the reverb family
				float decay = 0.45f + c1 * 0.53f;
				float size = 0.85f, dampK = 0.55f, mod = 0.f, diff = 0.7f, preD = 0.f;
				float toneV = 0.f, shimAmt = 0.f, shimX = 0.f;
				switch (fx) {
					case 22:  // Spring
						size = 0.45f;
						dampK = c3;
						diff = 0.62f;
						break;
					case 23:  // Plate classic
						size = 0.8f;
						dampK = 0.75f;
						preD = c2 * 0.12f;
						toneV = b3;
						break;
					case 24:  // Room stereo
						size = 0.55f;
						mod = c2 * 0.5f;
						toneV = b3;
						break;
					case 25:  // Hall chorus
						size = 1.05f;
						mod = c2;
						toneV = b3;
						break;
					case 26:  // Hall medium
						size = 0.9f;
						preD = c2 * 0.15f;
						toneV = b3;
						break;
					case 27:  // Black hole
						size = 1.15f + c2 * 0.3f;
						decay = 0.6f + c1 * 0.395f;
						diff = 0.75f + c2 * 0.2f;
						mod = 0.25f + c2 * 0.3f;
						toneV = b3;
						break;
					case 28:  // Cloud
						size = 0.95f;
						mod = c2;
						diff = 0.3f + c3 * 0.68f;
						break;
					case 29:  // Gray hole light
						size = clamp(1.0f + b2 * 0.4f, 0.5f, 1.4f);
						decay = 0.55f + c1 * 0.42f;
						diff = clamp(0.7f + b2 * 0.25f, 0.3f, 0.95f);
						toneV = b3;
						break;
					case 30:  // Shimmer input dual
					case 31:  // Shimmer dual (regen)
						size = 1.0f;
						decay = 0.55f + c1 * 0.42f;
						shimAmt = c2;
						shimX = c3;
						break;
				}
				float x = mono;
				if (fx == 22) {
					// spring chirp: short allpass cascade + resonant low boing
					float a = 0.6f;
					for (int s = 0; s < 6; s++) {
						float y = a * x + apz[0][s];
						apz[0][s] = x - a * y;
						x = y;
					}
					float g, R;
					svf[0][0].set(110.f, 3.f + std::fabs(b2) * 12.f, sr, g, R);
					float lp, bp, hp;
					svf[0][0].process(x, g, R, lp, bp, hp);
					x += bp * b2 * 1.5f;
				}
				if (fx == 30) {  // shimmer applied at the input
					float up = shA.process(mono, 2.f, sr);
					float dn = shB.process(mono, 0.5f, sr);
					x = mono + (up * shimX + dn * (1.f - shimX)) * shimAmt * 1.2f;
				}
				float shimIn = 0.f;
				if (fx == 31) {  // shimmer regenerating inside the tank
					float up = shA.process(shimFb, 2.f, sr);
					float dn = shB.process(shimFb, 0.5f, sr);
					shimIn = (up * shimX + dn * (1.f - shimX)) * shimAmt * 0.5f;
				}
				if (std::fabs(toneV) > 0.03f)
					x = tone[0].process(x, toneV, sr);
				float vl, vr;
				verb.process(x, sr, decay, size, dampK, mod, diff, preD, shimIn, vl, vr, shimFb);
				if (fx == 22) {
					vl = damp[0].process(vl, dampK, sr);
					vr = damp[1].process(vr, dampK, sr);
				}
				wetL = vl * 1.4f;
				wetR = vr * 1.4f;
			} break;
		}

		float dw = clamp(params[DW_PARAM].getValue() + inputs[DWCV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float outL = crossfade(xL, wetL, dw) * 5.f;
		float outR = crossfade(xR, wetR, dw) * 5.f;
		outputs[OUTL_OUTPUT].setVoltage(clamp(outL, -11.f, 11.f));
		outputs[OUTR_OUTPUT].setVoltage(clamp(outR, -11.f, 11.f));

		if (lightDivider.process()) {
			int bank = fx / 8, prog = fx % 8;
			for (int k = 0; k < 4; k++) {
				lights[LED_LIGHT + k].setBrightness(k == bank ? 1.f : 0.f);
				lights[LED_LIGHT + 4 + k].setBrightness(k == (prog % 4) ? (prog < 4 ? 1.f : 0.3f) : 0.f);
			}
		}
	}
};

struct RemoraWidget : ModuleWidget {
	RemoraWidget(Remora* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Remora.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace rmlayout;
		addParam(createParamCentered<TL1105>(mm2px(Vec(BTN_PREV_X, BTN_PREV_Y)), module, Remora::PREV_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(BTN_NEXT_X, BTN_NEXT_Y)), module, Remora::NEXT_PARAM));
		for (int k = 0; k < 4; k++) {
			addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(LED_X[k], LED_Y1)), module, Remora::LED_LIGHT + k));
			addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(LED_X[k], LED_Y2)), module, Remora::LED_LIGHT + 4 + k));
		}
		for (int k = 0; k < 3; k++) {
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KNOB_X, ROW_Y[k])), module, Remora::C1_PARAM + k));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CV_X, ROW_Y[k])), module, Remora::CV1_INPUT + k));
		}
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KNOB_X, ROW_Y[3])), module, Remora::DW_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CV_X, ROW_Y[3])), module, Remora::DWCV_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[0], R1)), module, Remora::INL_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[1], R1)), module, Remora::INR_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J_X[0], R2)), module, Remora::OUTL_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J_X[1], R2)), module, Remora::OUTR_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Remora* module = getModule<Remora>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel(string::f("Program %d: %s", module->fx + 1, FXDEFS[module->fx].name)));
		menu->addChild(createSubmenuItem("Select program", "", [=](Menu* sub) {
			for (int b = 0; b < 4; b++) {
				sub->addChild(createMenuLabel(string::f("Bank %d", b + 1)));
				for (int p = 0; p < 8; p++) {
					int i = b * 8 + p;
					sub->addChild(createCheckMenuItem(
						string::f("%d. %s", i + 1, FXDEFS[i].name), "",
						[=]() { return module->fx == i; },
						[=]() { module->fx = i; }));
				}
			}
		}));
	}
};

Model* modelRemora = createModel<Remora, RemoraWidget>("Remora");
