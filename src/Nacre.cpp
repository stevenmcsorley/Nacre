// Nacre — a granular audio processor for VCV Rack, inspired by the feature
// set of Mutable Instruments Beads. Original DSP implementation; the reverb
// stage uses Émilie Gillet's MIT-licensed Clouds reverb (see eurorack/).

#include "plugin.hpp"
#include "layout.hpp"
#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/reverb.h"
#include "plaits/resources.h"

static const float BUFFER_SECONDS = 8.f;
static const float MAX_DELAY_SECONDS = 6.5f;
static const float MIN_GRAIN_SECONDS = 0.03f;
static const float MAX_GRAIN_SECONDS = 4.f;
static const int MAX_GRAINS = 64;

// Rotary-head pitch shifter for the delay mode (dual sine-windowed heads).
struct TapShifter {
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

// Grain amplitude envelope: clicky rectangular fully CCW, hann around the
// middle, and increasingly slow exponential attacks (reversed-grain feel)
// toward fully CW — tuned so the knob is audible across its whole range.
static inline float grainEnv(float p, float shape) {
	if (shape < 0.45f) {
		// rectangular with widening raised-cosine edges
		float edge = 0.002f + 0.55f * shape;
		float x = std::min(p, 1.f - p) / edge;
		if (x > 1.f)
			x = 1.f;
		float s = std::sin(0.5f * M_PI * x);
		return s * s;
	}
	if (shape < 0.55f) {
		// hann window
		float s = std::sin(M_PI * p);
		return s * s;
	}
	// late peak with an increasingly exponential (slow) attack
	float t = (shape - 0.55f) / 0.45f;
	float peak = 0.6f + 0.38f * t;
	float x = (p < peak) ? (p / peak) : ((1.f - p) / (1.f - peak));
	float curve = 1.f + 3.f * t;
	return std::pow(std::max(x, 0.f), curve);
}

struct Nacre : Module {
	enum ParamId {
		TIME_PARAM,
		PITCH_PARAM,
		SIZE_PARAM,
		SHAPE_PARAM,
		TIME_RND_PARAM,
		PITCH_RND_PARAM,
		SIZE_RND_PARAM,
		SHAPE_RND_PARAM,
		DENSITY_PARAM,
		FEEDBACK_PARAM,
		DRYWET_PARAM,
		REVERB_PARAM,
		SEED_PARAM,
		FREEZE_PARAM,
		QUALITY_PARAM,
		ASSIGN_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TIME_INPUT,
		PITCH_INPUT,
		SIZE_INPUT,
		SHAPE_INPUT,
		DENSITY_INPUT,
		SEED_INPUT,
		MIX_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(QUALITY_LIGHT, 3),
		FREEZE_LIGHT,
		SEED_LIGHT,
		ASSIGN_FB_LIGHT,
		ASSIGN_DW_LIGHT,
		ASSIGN_RV_LIGHT,
		ENUMS(IN_LIGHT, 2),
		LIGHTS_LEN
	};

	struct Grain {
		bool active = false;
		double readPos = 0.0;
		double ratio = 1.0;
		double phase = 0.0;
		double duration = 1.0;
		float shape = 0.5f;
		float gainL = 0.7f;
		float gainR = 0.7f;
	};

	std::vector<float> bufL, bufR;
	int bufLen = 0;
	int writeIdx = 0;

	Grain grains[MAX_GRAINS];

	bool frozen = false;
	bool latched = true;
	int quality = 1; // 0 vintage digital, 1 clean, 2 tape
	int assign = 1;  // MIX CV destination: 0 FB, 1 D/W, 2 REV

	float schedTimer = 0.f;
	int clockDivCount = 0;

	float seedHold = 0.f;
	bool seedToggled = false;

	float prevWetL = 0.f, prevWetR = 0.f;
	float fbHpL = 0.f, fbHpR = 0.f;
	float heldL = 0.f, heldR = 0.f;
	bool decimToggle = false;
	double wowPhase1 = 0.0, wowPhase2 = 0.0;
	float inLevel = 0.f;

	// automatic input gain (0..+32 dB over ~5 s, like hardware)
	bool agc = true;
	float manualGainDb = 0.f;
	float agcGain = 1.f;
	float peakEnv = 0.f;

	// grain trigger on the R output (hardware [M]+[C] combo)
	bool rTrigger = false;
	dsp::PulseGenerator grainPulse;

	// delay mode (SIZE fully CW)
	float seedPeriod = 0.5f;
	float seedEdgeTimer = 1e9f;
	double tapDelay = 24000.0, tapOldDelay = 24000.0;
	float tapXf = 1.f;
	TapShifter shiftL, shiftR;

	// wavetable synth mode (both inputs unpatched for 10 s)
	float noInputTimer = 0.f;
	bool wtMode = false;
	int wtBank = -1;
	double oscPhase = 0.0;
	double tremPh = 0.0;
	double breathPh = 0.0;

	clouds::Reverb reverb;
	uint16_t reverbBuffer[16384] = {};

	dsp::SchmittTrigger seedInTrigger;
	dsp::BooleanTrigger seedBtn, freezeBtn, qualBtn, assignBtn;
	dsp::ClockDivider lightDivider;

	Nacre() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(TIME_PARAM, 0.f, 1.f, 0.15f, "Time (buffer position)", "%", 0.f, 100.f);
		configParam(PITCH_PARAM, -24.f, 24.f, 0.f, "Pitch", " st");
		configParam(SIZE_PARAM, 0.f, 1.f, 0.75f, "Size");
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.4f, "Shape");
		configParam(TIME_RND_PARAM, -1.f, 1.f, 0.f, "Time CV / randomize");
		configParam(PITCH_RND_PARAM, -1.f, 1.f, 0.f, "Pitch CV / randomize");
		configParam(SIZE_RND_PARAM, -1.f, 1.f, 0.f, "Size CV / randomize");
		configParam(SHAPE_RND_PARAM, -1.f, 1.f, 0.f, "Shape CV / randomize");
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.32f, "Density (CCW steady / CW random; silent at noon)");
		configParam(FEEDBACK_PARAM, 0.f, 1.f, 0.f, "Feedback", "%", 0.f, 100.f);
		configParam(DRYWET_PARAM, 0.f, 1.f, 0.5f, "Dry/wet", "%", 0.f, 100.f);
		configParam(REVERB_PARAM, 0.f, 1.f, 0.2f, "Reverb", "%", 0.f, 100.f);
		configButton(SEED_PARAM, "Seed (hold 2 s: toggle latched/gated)");
		configButton(FREEZE_PARAM, "Freeze");
		configButton(QUALITY_PARAM, "Quality (blue digital / green clean / red tape)");
		configButton(ASSIGN_PARAM, "Mix CV destination");

		configInput(TIME_INPUT, "Time CV");
		configInput(PITCH_INPUT, "Pitch (1V/oct via attenurandomizer)");
		configInput(SIZE_INPUT, "Size CV");
		configInput(SHAPE_INPUT, "Shape CV");
		configInput(DENSITY_INPUT, "Density CV");
		configInput(SEED_INPUT, "Seed trigger");
		configInput(MIX_INPUT, "Mix CV (assignable)");
		configInput(IN_L_INPUT, "Left audio");
		configInput(IN_R_INPUT, "Right audio");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);

		configLight(SEED_LIGHT, "Latched mode");
		configLight(FREEZE_LIGHT, "Frozen");

		lightDivider.setDivision(64);
		reverb.Init(reverbBuffer);
		reverb.set_diffusion(0.7f);
		allocateBuffer(44100.f);
	}

	void allocateBuffer(float sr) {
		bufLen = (int)(sr * BUFFER_SECONDS);
		bufL.assign(bufLen, 0.f);
		bufR.assign(bufLen, 0.f);
		writeIdx = 0;
		for (Grain& g : grains)
			g.active = false;
		shiftL.setSize((int)(sr * 0.12f));
		shiftR.setSize((int)(sr * 0.12f));
		wtBank = -1; // refill wavetable if active
	}

	// Fill the buffer with raw waveforms decoded from Plaits' wavetable
	// data (vendored, MIT) — 8 waves laid out across the buffer so TIME
	// scans through them, each resampled to a C2 root.
	void fillWavetable(float sr, int bank) {
		const float rootHz = 65.41f;
		int seg = bufLen / 8;
		for (int wv = 0; wv < 8; wv++) {
			const int16_t* w = plaits::wav_integrated_waves + (bank * 24 + wv * 3) * 260;
			// decode the integrated wave (first difference) and normalize
			float raw[256];
			float peak = 1.f;
			for (int i = 0; i < 256; i++) {
				raw[i] = (float)(w[i + 1] - w[i]);
				peak = std::max(peak, std::fabs(raw[i]));
			}
			float norm = 0.8f / peak;
			double step = 256.0 * rootHz / sr;
			double ph = 0.0;
			for (int i = 0; i < seg; i++) {
				int i0 = (int)ph;
				float fr = (float)(ph - i0);
				float v = (raw[i0 & 255] + (raw[(i0 + 1) & 255] - raw[i0 & 255]) * fr) * norm;
				bufL[wv * seg + i] = v;
				bufR[wv * seg + i] = v;
				ph += step;
				if (ph >= 256.0)
					ph -= 256.0;
			}
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocateBuffer(e.sampleRate);
	}

	void onReset() override {
		frozen = false;
		latched = true;
		quality = 1;
		assign = 1;
		allocateBuffer(APP->engine->getSampleRate());
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "frozen", json_boolean(frozen));
		json_object_set_new(root, "latched", json_boolean(latched));
		json_object_set_new(root, "quality", json_integer(quality));
		json_object_set_new(root, "assign", json_integer(assign));
		json_object_set_new(root, "agc", json_boolean(agc));
		json_object_set_new(root, "manualGainDb", json_real(manualGainDb));
		json_object_set_new(root, "rTrigger", json_boolean(rTrigger));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "frozen")))
			frozen = json_boolean_value(j);
		if ((j = json_object_get(root, "latched")))
			latched = json_boolean_value(j);
		if ((j = json_object_get(root, "quality")))
			quality = json_integer_value(j);
		if ((j = json_object_get(root, "assign")))
			assign = json_integer_value(j);
		if ((j = json_object_get(root, "agc")))
			agc = json_boolean_value(j);
		if ((j = json_object_get(root, "manualGainDb")))
			manualGainDb = json_real_value(j);
		if ((j = json_object_get(root, "rTrigger")))
			rTrigger = json_boolean_value(j);
	}

	float readBuf(const std::vector<float>& b, double pos) {
		double fl = std::floor(pos);
		float frac = (float)(pos - fl);
		int i = (int)fl % bufLen;
		if (i < 0)
			i += bufLen;
		int im1 = (i == 0) ? bufLen - 1 : i - 1;
		int i1 = (i + 1 >= bufLen) ? i + 1 - bufLen : i + 1;
		int i2 = (i + 2 >= bufLen) ? i + 2 - bufLen : i + 2;
		float xm1 = b[im1], x0 = b[i], x1 = b[i1], x2 = b[i2];
		// 4-point Hermite interpolation
		float c = (x1 - xm1) * 0.5f;
		float v = x0 - x1;
		float w = c + v;
		float a = w + v + (x2 - x0) * 0.5f;
		float bCoef = w + a;
		return ((a * frac - bCoef) * frac + c) * frac + x0;
	}

	// Attenurandomizer, per the hardware: with CV patched, CW of center
	// scales the CV, CCW makes the CV control the randomization amount.
	// Without CV, CW applies uniform randomization and CCW a "peaky"
	// distribution clustered toward the center. Sampled once per grain.
	float attnRand(int inputId, int attnParamId, bool& randomized) {
		float a = params[attnParamId].getValue();
		if (inputs[inputId].isConnected()) {
			if (a >= 0.f)
				return clamp(inputs[inputId].getVoltage() * 0.2f * a, -1.f, 1.f);
			// CCW: CV sets how much randomization is applied
			float amt = std::fabs(a) * clamp(std::fabs(inputs[inputId].getVoltage()) / 5.f, 0.f, 1.f);
			if (amt > 0.01f)
				randomized = true;
			return (random::uniform() * 2.f - 1.f) * amt;
		}
		float aa = std::fabs(a);
		if (aa > 0.01f)
			randomized = true;
		float u;
		if (a >= 0.f)
			u = random::uniform() * 2.f - 1.f; // uniform
		else
			u = (random::uniform() + random::uniform() + random::uniform()) * (2.f / 3.f) - 1.f; // peaky
		return u * aa;
	}

	void spawnGrain(float sr, bool randomRate = false) {
		Grain* g = NULL;
		for (Grain& gr : grains) {
			if (!gr.active) {
				g = &gr;
				break;
			}
		}
		if (!g) {
			// steal the most progressed grain
			double best = -1.0;
			for (Grain& gr : grains) {
				double pr = gr.phase / gr.duration;
				if (pr > best) {
					best = pr;
					g = &gr;
				}
			}
		}

		bool randomized = randomRate;
		float t = clamp(params[TIME_PARAM].getValue() + attnRand(TIME_INPUT, TIME_RND_PARAM, randomized) * 0.5f, 0.f, 1.f);

		float sk = clamp(params[SIZE_PARAM].getValue() + attnRand(SIZE_INPUT, SIZE_RND_PARAM, randomized) * 0.5f, 0.f, 1.f);
		float s2 = sk * 2.f - 1.f;
		float durSec = MIN_GRAIN_SECONDS * std::pow(MAX_GRAIN_SECONDS / MIN_GRAIN_SECONDS, std::fabs(s2));
		bool reverse = s2 < 0.f;

		float knob = params[PITCH_PARAM].getValue();
		float q = std::round(knob);
		if (std::fabs(knob - q) < 0.15f)
			knob = q; // virtual notches at semitones
		float semis = knob;
		float pa = params[PITCH_RND_PARAM].getValue();
		if (wtMode) {
			// wavetable mode: PITCH CV is always 1V/oct; the attenurandomizer
			// only controls pitch randomization
			semis += inputs[PITCH_INPUT].getVoltage() * 12.f;
			if (std::fabs(pa) > 0.01f) {
				randomized = true;
				semis += (random::uniform() * 2.f - 1.f) * std::fabs(pa) * 12.f;
			}
		}
		else if (inputs[PITCH_INPUT].isConnected() && pa >= 0.f) {
			semis += inputs[PITCH_INPUT].getVoltage() * 12.f * pa;
		}
		else {
			float amt = std::fabs(pa);
			if (inputs[PITCH_INPUT].isConnected())
				amt *= clamp(std::fabs(inputs[PITCH_INPUT].getVoltage()) / 5.f, 0.f, 1.f);
			if (amt > 0.01f)
				randomized = true;
			float u;
			if (pa >= 0.f)
				u = random::uniform() * 2.f - 1.f;
			else
				u = (random::uniform() + random::uniform() + random::uniform()) * (2.f / 3.f) - 1.f;
			semis += u * amt * 12.f;
		}
		semis = clamp(semis, -48.f, 48.f);

		float shape = clamp(params[SHAPE_PARAM].getValue() + attnRand(SHAPE_INPUT, SHAPE_RND_PARAM, randomized) * 0.5f, 0.f, 1.f);

		float delaySec = 0.02f + t * MAX_DELAY_SECONDS;
		double start;
		if (wtMode) {
			// TIME scans across the 8 stored waveforms
			start = (double)t * (bufLen - 8);
		}
		else {
			start = (double)writeIdx - (double)delaySec * sr;
			start = std::fmod(start, (double)bufLen);
			if (start < 0.0)
				start += bufLen;
		}

		g->active = true;
		g->phase = 0.0;
		g->duration = std::max(32.0, (double)durSec * sr);
		g->readPos = start;
		g->ratio = std::exp2(semis / 12.f) * (reverse ? -1.0 : 1.0);
		g->shape = shape;
		// pan is only randomized when some other parameter is randomized
		// (hardware rule); otherwise the stereo image is preserved
		float pan = randomized ? 0.5f + (random::uniform() - 0.5f) * 0.8f : 0.5f;
		g->gainL = std::cos(pan * 0.5f * M_PI) * 0.85f;
		g->gainR = std::sin(pan * 0.5f * M_PI) * 0.85f;
		grainPulse.trigger(0.002f);
	}

	void process(const ProcessArgs& args) override {
		if (bufLen == 0)
			return;
		float sr = args.sampleRate;

		// ---- buttons ----
		if (freezeBtn.process(params[FREEZE_PARAM].getValue() > 0.f))
			frozen = !frozen;
		if (qualBtn.process(params[QUALITY_PARAM].getValue() > 0.f))
			quality = (quality + 1) % 3;
		if (assignBtn.process(params[ASSIGN_PARAM].getValue() > 0.f))
			assign = (assign + 1) % 3;

		bool seedPressed = params[SEED_PARAM].getValue() > 0.f;
		if (seedBtn.process(seedPressed)) {
			spawnGrain(sr);
			seedHold = 0.f;
			seedToggled = false;
		}
		if (seedPressed) {
			seedHold += args.sampleTime;
			if (seedHold > 2.f && !seedToggled) {
				latched = !latched;
				seedToggled = true;
			}
		}

		// ---- input + auto gain (0..+32 dB over ~5 s, like hardware) ----
		bool lCon = inputs[IN_L_INPUT].isConnected();
		bool rCon = inputs[IN_R_INPUT].isConnected();
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float inR = rCon ? inputs[IN_R_INPUT].getVoltage() / 5.f : inL;
		if (!lCon && rCon)
			inL = inR;
		float rawPeak = std::max(std::fabs(inL), std::fabs(inR));
		peakEnv = std::max(rawPeak, peakEnv * (1.f - args.sampleTime / 2.f));
		if (agc) {
			float target = (peakEnv > 0.005f) ? clamp(0.7f / peakEnv, 1.f, 40.f) : agcGain;
			float k = args.sampleTime / 5.f * 39.f; // full swing in ~5 s
			agcGain += clamp(target - agcGain, -k, k);
		}
		else {
			agcGain = std::pow(10.f, manualGainDb / 20.f);
		}
		inL *= agcGain;
		inR *= agcGain;
		inLevel = std::max(std::max(std::fabs(inL), std::fabs(inR)), inLevel * 0.9995f);

		// ---- wavetable synth mode: both inputs unpatched for 10 s ----
		if (!lCon && !rCon) {
			noInputTimer += args.sampleTime;
			if (noInputTimer > 10.f && !wtMode) {
				wtMode = true;
				wtBank = -1;
			}
		}
		else {
			noInputTimer = 0.f;
			if (wtMode) {
				wtMode = false;
				std::fill(bufL.begin(), bufL.end(), 0.f);
				std::fill(bufR.begin(), bufR.end(), 0.f);
			}
		}

		// ---- mix section values (MIX CV applies to the assigned one) ----
		float fb = params[FEEDBACK_PARAM].getValue();
		float dw = params[DRYWET_PARAM].getValue();
		float rv = params[REVERB_PARAM].getValue();
		if (inputs[MIX_INPUT].isConnected()) {
			float mcv = inputs[MIX_INPUT].getVoltage() / 5.f;
			if (assign == 0)
				fb += mcv;
			else if (assign == 1)
				dw += mcv;
			else
				rv += mcv;
		}
		fb = clamp(fb, 0.f, 1.f);
		dw = clamp(dw, 0.f, 1.f);
		rv = clamp(rv, 0.f, 1.f);

		// in wavetable mode the feedback knob selects the waveform bank
		if (wtMode) {
			int bank = clamp((int)(fb * 7.99f), 0, 7);
			if (bank != wtBank) {
				wtBank = bank;
				fillWavetable(sr, bank);
			}
		}

		// ---- record into the buffer (feedback + quality character) ----
		float fbCut = 2.f * M_PI * 20.f / sr;
		fbHpL += fbCut * (prevWetL - fbHpL);
		fbHpR += fbCut * (prevWetR - fbHpR);
		float wl = inL + fb * 0.95f * (prevWetL - fbHpL);
		float wr = inR + fb * 0.95f * (prevWetR - fbHpR);
		switch (quality) {
			case 0: { // vintage digital: half-rate sample-hold + 12-bit
				decimToggle = !decimToggle;
				if (decimToggle) {
					heldL = std::floor(clamp(wl, -1.f, 1.f) * 2048.f) / 2048.f;
					heldR = std::floor(clamp(wr, -1.f, 1.f) * 2048.f) / 2048.f;
				}
				wl = heldL;
				wr = heldR;
			} break;
			case 1: // clean, gentle safety limiting
				wl = clamp(wl, -2.f, 2.f);
				wr = clamp(wr, -2.f, 2.f);
				break;
			case 2: { // tape: saturation on record, wow/flutter on playback
				const float norm = 1.f / std::tanh(1.5f);
				wl = std::tanh(wl * 1.5f) * norm * 0.9f;
				wr = std::tanh(wr * 1.5f) * norm * 0.9f;
			} break;
		}
		if (!frozen && !wtMode) {
			bufL[writeIdx] = wl;
			bufR[writeIdx] = wr;
			writeIdx++;
			if (writeIdx >= bufLen)
				writeIdx = 0;
		}

		// ---- delay mode: SIZE fully clockwise turns grains into delay taps ----
		bool delayMode = params[SIZE_PARAM].getValue() >= 0.99f && !wtMode;

		// seed clock measurement (for clocked density / delay sync)
		seedEdgeTimer += args.sampleTime;
		bool seedTick = seedInTrigger.process(inputs[SEED_INPUT].getVoltage(), 0.1f, 1.f);
		if (seedTick) {
			if (seedEdgeTimer < 8.f && seedEdgeTimer > 0.001f)
				seedPeriod = clamp(seedEdgeTimer, 0.001f, 8.f);
			seedEdgeTimer = 0.f;
		}

		// ---- grain scheduling (skipped in delay mode) ----
		float dKnob = params[DENSITY_PARAM].getValue() * 2.f - 1.f;
		float dCv = inputs[DENSITY_INPUT].getVoltage();
		bool clocked = inputs[SEED_INPUT].isConnected();
		bool freezeSpawnBlock = wtMode && frozen; // wavetable freeze halts grains
		if (delayMode) {
			// no granular scheduling; the tap renderer below takes over
		}
		else if (clocked) {
			// clocked: CCW = clock divider /1../16, CW = probability gate
			float dx = clamp(dKnob + dCv / 5.f, -1.f, 1.f);
			if (seedTick && !freezeSpawnBlock) {
				if (dx <= 0.02f) {
					int div = 1 + (int)(-dx * 15.f + 0.5f);
					if (++clockDivCount >= div) {
						clockDivCount = 0;
						spawnGrain(sr);
					}
				}
				else if (random::uniform() > dx * 0.95f) {
					spawnGrain(sr, true);
				}
			}
		}
		else if (latched) {
			// latched: silent at noon; CCW = constant metronomic rate,
			// CW = randomly modulated rate. Density CV = exponential 1V/oct.
			float dx = dKnob;
			if (std::fabs(dx) > 0.04f && !freezeSpawnBlock) {
				float rate = 0.25f * std::exp2(std::fabs(dx) * 10.5f) * std::exp2(clamp(dCv, -5.f, 5.f));
				rate = std::min(rate, 400.f);
				schedTimer -= args.sampleTime;
				if (schedTimer <= 0.f) {
					bool randSide = dx > 0.f;
					spawnGrain(sr, randSide);
					float period = 1.f / rate;
					schedTimer = randSide ? -std::log(1.f - random::uniform() * 0.999f) * period : period;
				}
			}
			else {
				schedTimer = 0.f;
			}
		}
		else {
			// gated mode: grains repeat at the density rate only while the
			// SEED button is held
			if (seedPressed && std::fabs(dKnob) > 0.04f && !freezeSpawnBlock) {
				float rate = 0.25f * std::exp2(std::fabs(dKnob) * 10.5f) * std::exp2(clamp(dCv, -5.f, 5.f));
				rate = std::min(rate, 400.f);
				schedTimer -= args.sampleTime;
				if (schedTimer <= 0.f) {
					spawnGrain(sr, dKnob > 0.f);
					schedTimer = 1.f / rate;
				}
			}
			else if (!seedPressed) {
				schedTimer = 0.f;
			}
		}

		// ---- render grains ----
		double wow = 0.0;
		if (quality == 2) {
			wowPhase1 += 2.0 * M_PI * 0.9 * args.sampleTime;
			wowPhase2 += 2.0 * M_PI * 6.3 * args.sampleTime;
			if (wowPhase1 > 2.0 * M_PI)
				wowPhase1 -= 2.0 * M_PI;
			if (wowPhase2 > 2.0 * M_PI)
				wowPhase2 -= 2.0 * M_PI;
			wow = (std::sin(wowPhase1) * 0.0012 + std::sin(wowPhase2) * 0.00018) * sr;
		}
		float wetL = 0.f, wetR = 0.f;
		int activeGrains = 0;
		for (Grain& g : grains) {
			if (!g.active)
				continue;
			float p = (float)(g.phase / g.duration);
			if (p >= 1.f) {
				g.active = false;
				continue;
			}
			activeGrains++;
			float env = grainEnv(p, g.shape);
			double rp = g.readPos + wow;
			wetL += readBuf(bufL, rp) * env * g.gainL;
			wetR += readBuf(bufR, rp) * env * g.gainR;
			g.readPos += g.ratio;
			if (g.readPos >= bufLen)
				g.readPos -= bufLen;
			else if (g.readPos < 0.0)
				g.readPos += bufLen;
			g.phase += 1.0;
		}
		// overlap normalization: dense clouds stay lush instead of slamming
		// into the limiter (the hardware compensates similarly)
		if (activeGrains > 2) {
			float norm = std::min(1.f, 1.7f / std::sqrt((float)activeGrains));
			wetL *= norm;
			wetR *= norm;
		}

		// ---- delay-tap rendering (SIZE fully CW) ----
		if (delayMode) {
			float dxd = clamp(dKnob + dCv / 5.f, -1.f, 1.f);
			double base;
			bool extraTap = false;
			if (clocked) {
				// clock sets the base time; DENSITY picks the subdivision
				static const float binDiv[5] = {1.f, 0.5f, 0.25f, 0.125f, 0.0625f};
				static const float wideDiv[6] = {1.f, 0.75f, 2.f / 3.f, 0.5f, 1.f / 3.f, 0.25f};
				float div = (dxd <= 0.f) ? binDiv[clamp((int)(-dxd * 4.99f), 0, 4)]
				                         : wideDiv[clamp((int)(dxd * 5.99f), 0, 5)];
				base = (double)seedPeriod * div * sr;
			}
			else {
				// knob: full buffer at noon, audio-rate flanging fully CCW;
				// past noon an extra unevenly-spaced tap appears
				float k = clamp(dxd * 0.5f + 0.5f, 0.f, 1.f);
				if (k <= 0.5f)
					base = (double)(MAX_DELAY_SECONDS * std::pow(0.002f / MAX_DELAY_SECONDS, (0.5f - k) * 2.f)) * sr;
				else {
					base = (double)MAX_DELAY_SECONDS * sr;
					extraTap = true;
				}
			}
			float t = clamp(params[TIME_PARAM].getValue() + inputs[TIME_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			float tapL, tapR;
			if (frozen) {
				// frozen: loop a slice; TIME selects which slice
				double sliceLen = std::fmin(std::fmax(base, 64.0), (double)bufLen - 64.0);
				double sliceStart = (double)t * ((double)bufLen - sliceLen);
				tapDelay += 1.0; // loop phase advances with time
				if (tapDelay >= sliceLen)
					tapDelay = std::fmod(tapDelay, sliceLen);
				double rp = sliceStart + tapDelay;
				tapL = readBuf(bufL, rp);
				tapR = readBuf(bufR, rp);
			}
			else {
				// delay: TIME selects the delay time as a multiple of base
				int mult = 1 + (int)(t * 3.99f);
				double want = std::fmin(std::fmax(base * mult, 64.0), (double)MAX_DELAY_SECONDS * sr);
				if (tapXf >= 1.f && std::fabs(want - tapDelay) > tapDelay * 0.01 + 8.0) {
					tapOldDelay = tapDelay;
					tapDelay = want;
					tapXf = 0.f;
				}
				auto tapRead = [&](const std::vector<float>& b, double d) {
					double rp = (double)writeIdx - d;
					return readBuf(b, rp);
				};
				if (tapXf < 1.f) {
					tapXf = std::min(1.f, tapXf + args.sampleTime / 0.05f);
					float gNew = std::sin(tapXf * M_PI_2);
					float gOld = std::cos(tapXf * M_PI_2);
					tapL = tapRead(bufL, tapDelay) * gNew + tapRead(bufL, tapOldDelay) * gOld;
					tapR = tapRead(bufR, tapDelay) * gNew + tapRead(bufR, tapOldDelay) * gOld;
				}
				else {
					tapL = tapRead(bufL, tapDelay);
					tapR = tapRead(bufR, tapDelay);
				}
				if (extraTap) {
					double d2 = tapDelay * 0.618;
					tapL += tapRead(bufL, d2) * 0.7f;
					tapR += tapRead(bufR, d2) * 0.7f;
				}
			}
			// rotary-head pitch shifting, bypassed at noon
			float knob = params[PITCH_PARAM].getValue();
			float qn = std::round(knob);
			if (std::fabs(knob - qn) < 0.15f)
				knob = qn;
			if (std::fabs(knob) > 0.2f) {
				float ratio = std::exp2(knob / 12.f);
				tapL = shiftL.process(tapL, ratio, 0.1f * sr);
				tapR = shiftR.process(tapR, ratio, 0.1f * sr);
			}
			// SHAPE: synchronized envelope on the repeats
			float shp = params[SHAPE_PARAM].getValue();
			if (shp > 0.55f && base > 64.0) {
				tremPh += 2.0 * M_PI * args.sampleTime * sr / base;
				if (tremPh > 2.0 * M_PI)
					tremPh -= 2.0 * M_PI;
				float depth = (shp - 0.55f) * 2.2f;
				float gEnv = 1.f - depth * (0.5f - 0.5f * std::cos((float)tremPh));
				tapL *= gEnv;
				tapR *= gEnv;
			}
			wetL += tapL;
			wetR += tapR;
		}

		wetL = std::tanh(wetL);
		wetR = std::tanh(wetR);
		prevWetL = wetL;
		prevWetR = wetR;

		// wavetable mode: the "dry" signal is a continuous oscillator
		// playing the TIME-selected waveform
		if (wtMode) {
			float t = clamp(params[TIME_PARAM].getValue() + inputs[TIME_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			int seg = clamp((int)(t * 7.99f), 0, 7);
			double cycle = sr / 65.41;
			float semis = params[PITCH_PARAM].getValue() + inputs[PITCH_INPUT].getVoltage() * 12.f;
			oscPhase += std::exp2(clamp(semis, -48.f, 48.f) / 12.f);
			if (oscPhase >= cycle)
				oscPhase = std::fmod(oscPhase, cycle);
			float osc = readBuf(bufL, (double)seg * (bufLen / 8) + oscPhase);
			inL = osc;
			inR = osc;
		}

		// ---- dry/wet and reverb ----
		float dryGain = std::cos(dw * 0.5f * M_PI);
		float wetGain = std::sin(dw * 0.5f * M_PI);
		float outL = inL * dryGain + wetL * wetGain;
		float outR = inR * dryGain + wetR * wetGain;

		reverb.set_amount(rv * 0.54f);
		reverb.set_time(0.35f + 0.63f * rv);
		reverb.set_input_gain(0.2f);
		reverb.set_lp(0.6f + 0.37f * rv);
		clouds::FloatFrame frame;
		frame.l = outL;
		frame.r = outR;
		reverb.Process(&frame, 1);

		bool pulse = grainPulse.process(args.sampleTime);
		if (rTrigger) {
			// hardware option: R carries the grain trigger, audio sums to L
			outputs[OUT_L_OUTPUT].setVoltage(clamp((frame.l + frame.r) * 0.7f * 5.f, -12.f, 12.f));
			outputs[OUT_R_OUTPUT].setVoltage(pulse ? 5.f : 0.f);
		}
		else {
			outputs[OUT_L_OUTPUT].setVoltage(clamp(frame.l * 5.f, -12.f, 12.f));
			outputs[OUT_R_OUTPUT].setVoltage(clamp(frame.r * 5.f, -12.f, 12.f));
		}

		// ---- lights ----
		if (lightDivider.process()) {
			lights[QUALITY_LIGHT + 0].setBrightness(quality == 2);
			lights[QUALITY_LIGHT + 1].setBrightness(quality == 1);
			lights[QUALITY_LIGHT + 2].setBrightness(quality == 0);
			lights[FREEZE_LIGHT].setBrightness(frozen);
			// latched: slowly breathing, like the hardware
			breathPh += 1.4 * 64.0 * args.sampleTime;
			if (breathPh > 2.0 * M_PI)
				breathPh -= 2.0 * M_PI;
			lights[SEED_LIGHT].setBrightness(latched ? 0.55f + 0.45f * (float)std::sin(breathPh) : 0.15f);
			lights[ASSIGN_FB_LIGHT].setBrightness(assign == 0);
			lights[ASSIGN_DW_LIGHT].setBrightness(assign == 1);
			lights[ASSIGN_RV_LIGHT].setBrightness(assign == 2);
			lights[IN_LIGHT + 0].setBrightness(clamp(inLevel, 0.f, 1.f) * (inLevel <= 1.f));
			lights[IN_LIGHT + 1].setBrightness(inLevel > 1.f);
		}
	}
};

struct NacreWidget : ModuleWidget {
	NacreWidget(Nacre* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Nacre.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace layout;

		// grain parameter columns
		static const int bigParams[4] = {Nacre::TIME_PARAM, Nacre::PITCH_PARAM, Nacre::SIZE_PARAM, Nacre::SHAPE_PARAM};
		static const int rndParams[4] = {Nacre::TIME_RND_PARAM, Nacre::PITCH_RND_PARAM, Nacre::SIZE_RND_PARAM, Nacre::SHAPE_RND_PARAM};
		static const int cvInputs[4] = {Nacre::TIME_INPUT, Nacre::PITCH_INPUT, Nacre::SIZE_INPUT, Nacre::SHAPE_INPUT};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(C[i], BIG_Y)), module, bigParams[i]));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(C[i], ATTN_Y)), module, rndParams[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[i], CV_Y)), module, cvInputs[i]));
		}

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(DENS_X, DENS_Y)), module, Nacre::DENSITY_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(SEED_BTN_X, SEED_BTN_Y)), module, Nacre::SEED_PARAM));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(SEED_LED_X, SEED_LED_Y)), module, Nacre::SEED_LIGHT));

		static const int mixParams[3] = {Nacre::FEEDBACK_PARAM, Nacre::DRYWET_PARAM, Nacre::REVERB_PARAM};
		static const int mixLights[3] = {Nacre::ASSIGN_FB_LIGHT, Nacre::ASSIGN_DW_LIGHT, Nacre::ASSIGN_RV_LIGHT};
		for (int i = 0; i < 3; i++) {
			addParam(createParamCentered<Trimpot>(mm2px(Vec(MIX_X[i], MIX_KNOB_Y)), module, mixParams[i]));
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(MIX_X[i], MIX_LED_Y)), module, mixLights[i]));
		}

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[0], ROW2_Y)), module, Nacre::DENSITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[1], ROW2_Y)), module, Nacre::SEED_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(MIX_X[0], ROW2_Y)), module, Nacre::ASSIGN_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(MIX_X[2], ROW2_Y)), module, Nacre::MIX_INPUT));

		// lights live inside the buttons (keeps the header clear)
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedGreenBlueLight>>>(mm2px(Vec(QUAL_BTN_X, QUAL_BTN_Y)), module, Nacre::QUALITY_PARAM, Nacre::QUALITY_LIGHT));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedLight>>>(mm2px(Vec(FRZ_BTN_X, FRZ_BTN_Y)), module, Nacre::FREEZE_PARAM, Nacre::FREEZE_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN_L_X, IO_Y)), module, Nacre::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN_R_X, IO_Y)), module, Nacre::IN_R_INPUT));
		addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(PEAK_X, PEAK_Y)), module, Nacre::IN_LIGHT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_L_X, IO_Y)), module, Nacre::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_R_X, IO_Y)), module, Nacre::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Nacre* module = getModule<Nacre>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Quality",
			{"Vintage digital", "Clean", "Tape"}, &module->quality));
		menu->addChild(createIndexPtrSubmenuItem("Mix CV destination",
			{"Feedback", "Dry/wet", "Reverb"}, &module->assign));
		menu->addChild(createBoolPtrMenuItem("Latched seed mode", "", &module->latched));
		menu->addChild(createBoolPtrMenuItem("Freeze", "", &module->frozen));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Automatic input gain (0..+32 dB)", "", &module->agc));
		{
			struct GainQuantity : Quantity {
				Nacre* m;
				void setValue(float v) override { m->manualGainDb = clamp(v, -12.f, 32.f); }
				float getValue() override { return m->manualGainDb; }
				float getMinValue() override { return -12.f; }
				float getMaxValue() override { return 32.f; }
				float getDefaultValue() override { return 0.f; }
				std::string getLabel() override { return "Manual input gain (when AGC off)"; }
				std::string getUnit() override { return " dB"; }
				int getDisplayPrecision() override { return 3; }
			};
			ui::Slider* s = new ui::Slider;
			GainQuantity* q = new GainQuantity;
			q->m = module;
			s->quantity = q;
			s->box.size.x = 240.f;
			menu->addChild(s);
		}
		menu->addChild(createBoolPtrMenuItem("R output sends grain triggers", "", &module->rTrigger));
	}
};

Model* modelNacre = createModel<Nacre, NacreWidget>("Nacre");
