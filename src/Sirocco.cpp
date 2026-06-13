// Sirocco — a live granular processor for VCV Rack with melodic and
// rhythmic grain controls, inspired by the architecture of the Qu-Bit
// Mojave (clocked/quantized grain generation, zoned pitch randomness with
// scale quantization, buffer wandering, morphing grain windows, and a
// feedback/reverb macro). Original DSP implementation; the reverb stage
// uses Émilie Gillet's MIT-licensed Clouds reverb (see eurorack/).

#include "plugin.hpp"
#include "layout_sirocco.hpp"
#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/reverb.h"

static const float BUFFER_SECONDS = 16.f;
static const int MAX_GRAINS = 48;

// scale degrees (semitones within an octave); -1 terminated
static const int SCALES[4][13] = {
	{0, 2, 4, 5, 7, 9, 11, -1},                        // dawn: major
	{0, 2, 3, 5, 7, 8, 10, -1},                        // day: minor
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1},        // dusk: chromatic
	{-1},                                              // twilight: unquantized
};

// quantized clock-mode rate multipliers
static const float RATE_MULTS[11] = {0.125f, 0.25f, 1.f / 3.f, 0.5f, 1.f / 1.5f, 1.f, 1.5f, 2.f, 3.f, 4.f, 8.f};

// Grain window: morphs across hamming, up ramp, triangle, exp down, square.
static inline float grainWindow(float p, float shape) {
	float pos = clamp(shape, 0.f, 1.f) * 4.f;
	int a = (int)pos;
	float fr = pos - a;
	float wa = 0.f, wb = 0.f;
	for (int k = 0; k < 2; k++) {
		int idx = clamp(a + k, 0, 4);
		float v;
		float edge = std::min(1.f, std::min(p, 1.f - p) / 0.02f);
		switch (idx) {
			case 0: v = 0.54f - 0.46f * std::cos(2.f * M_PI * p); break;
			case 1: v = p * edge; break;
			case 2: v = 1.f - std::fabs(2.f * p - 1.f); break;
			case 3: v = std::exp(-5.f * p) * edge; break;
			default: v = edge; break;
		}
		if (k == 0)
			wa = v;
		else
			wb = v;
	}
	return wa + (wb - wa) * fr;
}

struct Sirocco : Module {
	enum ParamId {
		FX_PARAM,
		SIZE_PARAM,
		SCATTER_PARAM,
		MELODY_PARAM,
		WANDER_PARAM,
		RATE_PARAM,
		SPIN_PARAM,
		MIX_PARAM,
		POS_PARAM,
		PITCH_PARAM,
		SHAPE_PARAM,
		GEN_BTN_PARAM,
		TAP_PARAM,
		GENMODE_PARAM,
		CLKMODE_PARAM,
		SCALE_PARAM,
		LOCK_PARAM,
		FREEZE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		FX_INPUT,
		SCATTER_INPUT,
		MELODY_INPUT,
		SIZE_INPUT,
		RATE_INPUT,
		WANDER_INPUT,
		POS_INPUT,
		PITCH_INPUT,
		SHAPE_INPUT,
		SPIN_INPUT,
		MIX_INPUT,
		GEN_INPUT,
		CLOCK_INPUT,
		LOCK_INPUT,
		FREEZE_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		CV_OUTPUT,
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(GENMODE_LIGHT, 3),
		ENUMS(CLKMODE_LIGHT, 3),
		ENUMS(SCALE_LIGHT, 3),
		LOCK_LIGHT,
		FREEZE_LIGHT,
		LIGHTS_LEN
	};

	struct Grain {
		bool active = false;
		double readPos = 0.0;
		double ratio = 1.0;
		double phase = 0.0;
		double duration = 1.0;
		float shape = 0.f;
		bool reverse = false;
		float gainL = 0.7f, gainR = 0.7f;
	};

	struct GrainSpec {
		float delaySamp = 4800.f;
		float durSamp = 960.f;
		float semis = 0.f;
		float shape = 0.f;
		bool reverse = false;
		float pan = 0.5f;
	};

	std::vector<float> bufL, bufR;
	int bufLen = 0;
	int w = 0;

	Grain grains[MAX_GRAINS];
	GrainSpec frozenSpec;

	int genMode = 0;    // 0 erode (clock+gen), 1 shear (audio threshold), 2 chisel (gen only)
	bool quantized = false;
	int scaleMode = 0;  // dawn, day, dusk, twilight
	bool locked = false;
	bool frozen = false;
	int cvMode = 0;     // 0 morphing CV, 1 grain gates, 2 clock pulses

	float clockPeriod = 0.5f;
	float lastEdge = 1e9f;
	float tapTimer = 1e9f;
	float schedTimer = 0.f;
	int burstCount = 0;
	float burstTimer = 0.f, burstInterval = 0.05f;
	// quantized-mode lock to external clock edges
	int divCount = 0;
	int subsLeft = 0;
	float subTimer = 0.f;
	bool whirlReverse = false; // Whirl also flips grain direction (Narwhal option)

	// melodic events
	int eventRemaining = 0;
	int eventStep = 0;
	int eventDir = 1;
	int eventType = 0;
	int eventDegree = 0;

	// shear envelope follower
	float shearEnv = 0.f;
	bool shearArmed = true;

	// dune CV
	float dunePhase = 0.f;
	bool duneDescending = false;
	float dunePulse = 0.f;

	float prevWetL = 0.f, prevWetR = 0.f;
	float fbHpL = 0.f, fbHpR = 0.f;

	clouds::Reverb reverb;
	uint16_t reverbBuffer[16384] = {};

	dsp::SchmittTrigger clockTrig, genTrig, lockTrig, freezeTrig;
	dsp::BooleanTrigger genBtn, tapBtn, genModeBtn, clkModeBtn, scaleBtn, lockBtn, freezeBtn;
	dsp::ClockDivider lightDivider;

	Sirocco() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FX_PARAM, 0.f, 1.f, 0.5f, "FX (feedback < center > reverb)");
		configParam(SIZE_PARAM, 0.f, 1.f, 0.62f, "Size (reverse < 20 ms > forward)");
		configParam(SCATTER_PARAM, 0.f, 1.f, 0.1f, "Scatter (rhythmic variation)", "%", 0.f, 100.f);
		configParam(MELODY_PARAM, 0.f, 1.f, 0.f, "Melody (random pitch zones)");
		configParam(WANDER_PARAM, 0.f, 1.f, 0.1f, "Wander (random buffer position)", "%", 0.f, 100.f);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate (grain frequency / clock div-mult)");
		configParam(SPIN_PARAM, 0.f, 1.f, 0.3f, "Spin (random stereo pan)", "%", 0.f, 100.f);
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Dry/wet mix", "%", 0.f, 100.f);
		configParam(POS_PARAM, 0.f, 1.f, 0.f, "Position (buffer zone; scrub when locked)");
		configParam(PITCH_PARAM, -24.f, 24.f, 0.f, "Pitch", " st");
		configParam(SHAPE_PARAM, 0.f, 1.f, 0.f, "Shape (window: hamming/ramp/tri/exp/square)");
		configButton(GEN_BTN_PARAM, "Generate grain");
		configButton(TAP_PARAM, "Tap tempo");
		configButton(GENMODE_PARAM, "Gen mode (clock+gen / audio threshold / gen only)");
		configButton(CLKMODE_PARAM, "Clock mode (free/quantized)");
		configButton(SCALE_PARAM, "Scale (major/minor/chromatic/free)");
		configButton(LOCK_PARAM, "Lock buffer (position scrubs)");
		configButton(FREEZE_PARAM, "Freeze grains");
		configInput(FX_INPUT, "FX CV");
		configInput(SCATTER_INPUT, "Scatter CV");
		configInput(MELODY_INPUT, "Melody CV");
		configInput(SIZE_INPUT, "Size CV");
		configInput(RATE_INPUT, "Rate CV");
		configInput(WANDER_INPUT, "Wander CV");
		configInput(POS_INPUT, "Position CV");
		configInput(PITCH_INPUT, "Pitch (1V/oct)");
		configInput(SHAPE_INPUT, "Shape CV");
		configInput(SPIN_INPUT, "Spin CV");
		configInput(MIX_INPUT, "Mix CV");
		configInput(GEN_INPUT, "Gen gate");
		configInput(CLOCK_INPUT, "Clock");
		configInput(LOCK_INPUT, "Lock gate");
		configInput(FREEZE_INPUT, "Freeze gate");
		configInput(IN_L_INPUT, "Left audio (normalled to right)");
		configInput(IN_R_INPUT, "Right audio");
		configOutput(CV_OUTPUT, "Algorithmic CV (morphing / grain gates / clock)");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		lightDivider.setDivision(64);
		reverb.Init(reverbBuffer);
		reverb.set_diffusion(0.7f);
		allocate(44100.f);
	}

	void allocate(float sr) {
		bufLen = (int)(sr * BUFFER_SECONDS);
		bufL.assign(bufLen, 0.f);
		bufR.assign(bufLen, 0.f);
		w = 0;
		for (Grain& g : grains)
			g.active = false;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	void onReset() override {
		genMode = 0;
		quantized = false;
		scaleMode = 0;
		locked = false;
		frozen = false;
		cvMode = 0;
		clockPeriod = 0.5f;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "genMode", json_integer(genMode));
		json_object_set_new(root, "quantized", json_boolean(quantized));
		json_object_set_new(root, "scaleMode", json_integer(scaleMode));
		json_object_set_new(root, "locked", json_boolean(locked));
		json_object_set_new(root, "frozen", json_boolean(frozen));
		json_object_set_new(root, "cvMode", json_integer(cvMode));
		json_object_set_new(root, "whirlReverse", json_boolean(whirlReverse));
		json_object_set_new(root, "clockPeriod", json_real(clockPeriod));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "genMode"))) genMode = json_integer_value(j);
		if ((j = json_object_get(root, "quantized"))) quantized = json_boolean_value(j);
		if ((j = json_object_get(root, "scaleMode"))) scaleMode = json_integer_value(j);
		if ((j = json_object_get(root, "locked"))) locked = json_boolean_value(j);
		if ((j = json_object_get(root, "frozen"))) frozen = json_boolean_value(j);
		if ((j = json_object_get(root, "cvMode"))) cvMode = json_integer_value(j);
		if ((j = json_object_get(root, "whirlReverse"))) whirlReverse = json_boolean_value(j);
		if ((j = json_object_get(root, "clockPeriod"))) clockPeriod = json_real_value(j);
	}

	float readBuf(const std::vector<float>& b, double pos) {
		double fl = std::floor(pos);
		float frac = (float)(pos - fl);
		int i = (int)fl % bufLen;
		if (i < 0)
			i += bufLen;
		int i1 = (i + 1 >= bufLen) ? 0 : i + 1;
		return b[i] + (b[i1] - b[i]) * frac;
	}

	float cvParam(int paramId, int inputId) {
		return clamp(params[paramId].getValue() + inputs[inputId].getVoltage() / 10.f, 0.f, 1.f);
	}

	float quantizeToScale(float semis) {
		const int* scale = SCALES[scaleMode];
		if (scale[0] < 0)
			return semis; // twilight: unquantized
		int n = 0;
		while (scale[n] >= 0)
			n++;
		float oct = std::floor(semis / 12.f);
		float in = semis - oct * 12.f;
		float best = scale[0];
		float bestD = 1e9f;
		for (int i = 0; i < n; i++) {
			float d = std::fabs(in - scale[i]);
			if (d < bestD) {
				bestD = d;
				best = scale[i];
			}
		}
		// also consider the octave above
		if (std::fabs(in - 12.f) < bestD)
			return (oct + 1.f) * 12.f;
		return oct * 12.f + best;
	}

	// Melody zones: none / semitones / +octaves / octaves / scale notes / events
	float melodyOffset(float m) {
		if (m < 0.01f) {
			eventRemaining = 0;
			return 0.f;
		}
		if (eventRemaining > 0) {
			eventRemaining--;
			eventStep++;
			int deg;
			if (eventType == 2)
				deg = (eventStep & 1) ? eventDegree + 1 : eventDegree; // trill
			else
				deg = eventDegree + eventStep * eventDir;
			const int* scale = SCALES[scaleMode];
			int n = 0;
			while (scale[n] >= 0)
				n++;
			if (n == 0)
				return deg * 1.f;
			int oct = deg >= 0 ? deg / n : (deg - n + 1) / n;
			int idx = deg - oct * n;
			return oct * 12.f + scale[idx];
		}
		if (m < 0.33f) {
			// semitone shifts on every grain (super-saw zone); the spread of
			// available semitones widens across the zone
			float p = m / 0.33f;
			int r = 1 + (int)(p * 1.99f);
			return (float)((int)(random::u32() % (2 * r + 1)) - r);
		}
		if (m < 0.45f) {
			float u = random::uniform();
			if (u < 0.4f)
				return (float)((int)(random::u32() % 5) - 2);
			if (u < 0.7f)
				return (random::u32() & 1) ? 12.f : -12.f;
			return 0.f;
		}
		if (m < 0.55f) {
			int o = (int)(random::u32() % 3) - 1;
			return o * 12.f;
		}
		if (m < 0.65f) {
			return (float)((int)(random::u32() % 25) - 12);
		}
		// melodic events: arps and trills
		float p = (m - 0.65f) / 0.35f;
		if (random::uniform() < p * 0.6f) {
			eventType = random::u32() % 3; // 0 run up, 1 run down, 2 trill
			eventDir = (eventType == 1) ? -1 : 1;
			eventRemaining = 3 + random::u32() % 5;
			eventStep = 0;
			eventDegree = (int)(random::u32() % 5) - 2;
		}
		return (float)((int)(random::u32() % 25) - 12);
	}

	GrainSpec makeSpec(float sr, float ratePeriod) {
		GrainSpec s;
		float windowLen = clamp(8.f * clockPeriod, 0.25f, 12.f) * sr;
		float pos = cvParam(POS_PARAM, POS_INPUT);
		float wander = cvParam(WANDER_PARAM, WANDER_INPUT);
		float offset = pos * windowLen;
		if (wander > 0.003f) {
			float r = random::uniform() * wander * windowLen;
			if (quantized) {
				float beat = clockPeriod * sr;
				r = std::round(r / beat) * beat;
			}
			offset += r;
		}
		s.delaySamp = clamp(offset + 64.f, 64.f, (float)bufLen - 4.f);

		float sizeV = cvParam(SIZE_PARAM, SIZE_INPUT);
		float s2 = sizeV * 2.f - 1.f;
		s.reverse = s2 < 0.f;
		float m = std::fabs(s2);
		float maxDur = clamp(ratePeriod * 10.f, 0.02f, 4.f);
		s.durSamp = clamp(0.02f * std::pow(maxDur / 0.02f, m), 0.02f, 4.f) * sr;

		float semis = params[PITCH_PARAM].getValue() + inputs[PITCH_INPUT].getVoltage() * 12.f;
		semis += melodyOffset(cvParam(MELODY_PARAM, MELODY_INPUT));
		s.semis = quantizeToScale(clamp(semis, -48.f, 48.f));

		s.shape = cvParam(SHAPE_PARAM, SHAPE_INPUT);
		float spin = cvParam(SPIN_PARAM, SPIN_INPUT);
		s.pan = 0.5f + (random::uniform() - 0.5f) * spin;
		if (whirlReverse && random::uniform() < spin * 0.4f)
			s.reverse = !s.reverse;
		return s;
	}

	void spawnGrain(const GrainSpec& s) {
		Grain* g = NULL;
		for (Grain& gr : grains) {
			if (!gr.active) {
				g = &gr;
				break;
			}
		}
		if (!g) {
			double best = -1.0;
			for (Grain& gr : grains) {
				double pr = gr.phase / gr.duration;
				if (pr > best) {
					best = pr;
					g = &gr;
				}
			}
		}
		double start = (double)w - (double)s.delaySamp;
		while (start < 0.0)
			start += bufLen;
		g->active = true;
		g->phase = 0.0;
		g->duration = std::max(32.f, s.durSamp);
		g->readPos = start;
		g->ratio = std::exp2(s.semis / 12.f) * (s.reverse ? -1.0 : 1.0);
		g->reverse = s.reverse;
		g->shape = s.shape;
		g->gainL = std::cos(s.pan * 0.5f * M_PI) * 0.8f;
		g->gainR = std::sin(s.pan * 0.5f * M_PI) * 0.8f;
		dunePulse = 0.003f;
	}

	void triggerGrain(float sr, float ratePeriod) {
		if (frozen)
			spawnGrain(frozenSpec);
		else {
			frozenSpec = makeSpec(sr, ratePeriod);
			spawnGrain(frozenSpec);
		}
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;
		if (bufLen == 0)
			return;

		// ---- buttons ----
		if (genModeBtn.process(params[GENMODE_PARAM].getValue() > 0.f))
			genMode = (genMode + 1) % 3;
		if (clkModeBtn.process(params[CLKMODE_PARAM].getValue() > 0.f))
			quantized = !quantized;
		if (scaleBtn.process(params[SCALE_PARAM].getValue() > 0.f))
			scaleMode = (scaleMode + 1) % 4;
		if (lockBtn.process(params[LOCK_PARAM].getValue() > 0.f) ||
		    lockTrig.process(inputs[LOCK_INPUT].getVoltage(), 0.1f, 1.f))
			locked = !locked;
		if (freezeBtn.process(params[FREEZE_PARAM].getValue() > 0.f) ||
		    freezeTrig.process(inputs[FREEZE_INPUT].getVoltage(), 0.1f, 1.f))
			frozen = !frozen;

		// ---- clock ----
		tapTimer += dt;
		if (tapBtn.process(params[TAP_PARAM].getValue() > 0.f)) {
			if (tapTimer < 4.f && tapTimer > 0.001f)
				clockPeriod = clamp(tapTimer, 0.001f, 4.f);
			tapTimer = 0.f;
		}
		lastEdge += dt;
		bool clockTick = false;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (lastEdge < 4.f && lastEdge > 0.001f)
				clockPeriod = clamp(lastEdge, 0.001f, 4.f);
			lastEdge = 0.f;
			clockTick = true;
		}

		// ---- rate / scheduling period ----
		float rateV = cvParam(RATE_PARAM, RATE_INPUT);
		float ratePeriod;
		float rateMult = 1.f;
		if (quantized) {
			int idx = clamp((int)(rateV * 10.999f), 0, 10);
			rateMult = RATE_MULTS[idx];
			ratePeriod = clockPeriod / rateMult;
		}
		else {
			float hz = 0.25f * std::exp2(rateV * 12.f); // 0.25 Hz .. ~1 kHz (audio rate)
			ratePeriod = 1.f / hz;
		}
		ratePeriod = clamp(ratePeriod, 0.001f, 8.f);

		// ---- input + recording (with feedback) ----
		bool lCon = inputs[IN_L_INPUT].isConnected();
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f : inL;
		if (!lCon && inputs[IN_R_INPUT].isConnected())
			inL = inR;

		// gust macro: feedback left of center, reverb right, dead zone at center
		float fx = cvParam(FX_PARAM, FX_INPUT);
		float fbAmt = clamp((0.47f - fx) / 0.47f, 0.f, 1.f);
		float rvAmt = clamp((fx - 0.53f) / 0.47f, 0.f, 1.f);

		float fbCut = 2.f * M_PI * 20.f / sr;
		fbHpL += fbCut * (prevWetL - fbHpL);
		fbHpR += fbCut * (prevWetR - fbHpR);
		if (!locked) {
			bufL[w] = std::tanh(inL + fbAmt * 0.9f * (prevWetL - fbHpL));
			bufR[w] = std::tanh(inR + fbAmt * 0.9f * (prevWetR - fbHpR));
			w++;
			if (w >= bufLen)
				w = 0;
		}

		// ---- grain generation ----
		bool gen = genBtn.process(params[GEN_BTN_PARAM].getValue() > 0.f) ||
		           genTrig.process(inputs[GEN_INPUT].getVoltage(), 0.1f, 1.f);
		if (gen)
			triggerGrain(sr, ratePeriod);

		if (genMode == 0) { // erode: scheduler runs
			float scatter = cvParam(SCATTER_PARAM, SCATTER_INPUT);
			// quantized grid event: straight grain, clock-synced rest, or ratchet
			auto quantEvent = [&]() {
				float u = random::uniform();
				if (u < scatter * 0.35f) {
					// rest: skip this grain
				}
				else if (u < scatter * 0.65f) {
					int nr = 2 + (int)(random::u32() % 3);
					burstCount = nr - 1;
					burstInterval = ratePeriod / nr;
					burstTimer = burstInterval;
					triggerGrain(sr, ratePeriod);
				}
				else {
					triggerGrain(sr, ratePeriod);
				}
			};
			bool extClock = inputs[CLOCK_INPUT].isConnected();
			if (quantized && extClock) {
				// lock the grid to the incoming clock edges: divisions count
				// pulses, multiples schedule substeps inside the period
				if (clockTick) {
					if (rateMult < 0.999f) {
						int everyN = (int)std::round(1.f / rateMult);
						if (++divCount >= everyN) {
							divCount = 0;
							quantEvent();
						}
					}
					else {
						quantEvent();
						subsLeft = (int)std::round(rateMult) - 1;
						subTimer = ratePeriod;
					}
				}
				else if (subsLeft > 0) {
					subTimer -= dt;
					if (subTimer <= 0.f) {
						subsLeft--;
						subTimer = ratePeriod;
						quantEvent();
					}
				}
			}
			else {
				if (!quantized && clockTick)
					schedTimer = 0.f; // free mode: clock input resyncs the rate
				schedTimer -= dt;
				if (schedTimer <= 0.f) {
					if (quantized) {
						quantEvent();
						schedTimer = ratePeriod;
					}
					else {
						triggerGrain(sr, ratePeriod);
						// free variations: exponential jitter blended in by scatter
						float jitter = -std::log(1.f - random::uniform() * 0.999f);
						schedTimer = ratePeriod * (1.f - scatter + scatter * jitter);
					}
				}
			}
			if (burstCount > 0) {
				burstTimer -= dt;
				if (burstTimer <= 0.f) {
					triggerGrain(sr, ratePeriod);
					burstCount--;
					burstTimer = burstInterval;
				}
			}
		}
		else if (genMode == 1) { // shear: audio threshold
			float lvl = std::fabs(inL) * 0.6f + std::fabs(inR) * 0.4f;
			shearEnv = std::max(lvl, shearEnv * std::exp(-dt / 0.05f));
			if (shearArmed && shearEnv > 0.2f) {
				shearArmed = false;
				triggerGrain(sr, ratePeriod);
			}
			else if (!shearArmed && shearEnv < 0.08f) {
				shearArmed = true;
			}
		}
		// chisel: gen only (handled above)

		// ---- render grains ----
		float wetL = 0.f, wetR = 0.f;
		for (Grain& g : grains) {
			if (!g.active)
				continue;
			float p = (float)(g.phase / g.duration);
			if (p >= 1.f) {
				g.active = false;
				continue;
			}
			float env = grainWindow(g.reverse ? 1.f - p : p, g.shape);
			wetL += readBuf(bufL, g.readPos) * env * g.gainL;
			wetR += readBuf(bufR, g.readPos) * env * g.gainR;
			g.readPos += g.ratio;
			if (g.readPos >= bufLen)
				g.readPos -= bufLen;
			else if (g.readPos < 0.0)
				g.readPos += bufLen;
			g.phase += 1.0;
		}
		wetL = std::tanh(wetL);
		wetR = std::tanh(wetR);
		prevWetL = wetL;
		prevWetR = wetR;

		// ---- mix + reverb ----
		float mix = cvParam(MIX_PARAM, MIX_INPUT);
		float dryGain = std::cos(mix * 0.5f * M_PI);
		float wetGain = std::sin(mix * 0.5f * M_PI);
		float outL = inL * dryGain + wetL * wetGain;
		float outR = inR * dryGain + wetR * wetGain;

		reverb.set_amount(rvAmt * 0.54f);
		reverb.set_time(0.35f + 0.6f * rvAmt);
		reverb.set_input_gain(0.2f);
		reverb.set_lp(0.6f + 0.35f * rvAmt);
		clouds::FloatFrame frame;
		frame.l = outL;
		frame.r = outR;
		reverb.Process(&frame, 1);
		outputs[OUT_L_OUTPUT].setVoltage(clamp(frame.l * 5.f, -12.f, 12.f));
		outputs[OUT_R_OUTPUT].setVoltage(clamp(frame.r * 5.f, -12.f, 12.f));

		// ---- algorithmic CV output ----
		switch (cvMode) {
			case 1: // grain gates (hardware levels: +5V)
				dunePulse = std::max(0.f, dunePulse - dt);
				outputs[CV_OUTPUT].setVoltage(dunePulse > 0.f ? 5.f : 0.f);
				break;
			case 2: // clock pulses
				outputs[CV_OUTPUT].setVoltage((clockTick || lastEdge < 0.005f || std::fmod(tapTimer, clockPeriod) < 0.005f) ? 5.f : 0.f);
				break;
			default: {
				float spin = cvParam(SPIN_PARAM, SPIN_INPUT);
				float speed = 0.03f * std::exp2(spin * 9.f); // 0.03..~15 Hz
				dunePhase += dt * speed;
				if (dunePhase >= 1.f) {
					dunePhase -= 1.f;
					float wander = cvParam(WANDER_PARAM, WANDER_INPUT);
					duneDescending = random::uniform() < wander;
				}
				float v = duneDescending ? 1.f - dunePhase : dunePhase;
				float scatter = cvParam(SCATTER_PARAM, SCATTER_INPUT);
				if (scatter > 0.01f) {
					float n = std::max(2.f, 16.f - scatter * 14.f);
					float stepped = std::floor(v * n) / (n - 1.f);
					v += (clamp(stepped, 0.f, 1.f) - v) * std::min(1.f, scatter * 4.f);
				}
				outputs[CV_OUTPUT].setVoltage(v * 5.f);
			} break;
		}

		// ---- lights ----
		if (lightDivider.process()) {
			static const float genColors[3][3] = {{0.f, 0.2f, 1.f}, {0.f, 1.f, 0.1f}, {1.f, 0.7f, 0.f}};
			static const float scaleColors[4][3] = {{0.f, 0.4f, 1.f}, {0.f, 1.f, 0.1f}, {1.f, 0.7f, 0.f}, {0.8f, 0.f, 1.f}};
			for (int k = 0; k < 3; k++) {
				lights[GENMODE_LIGHT + k].setBrightness(genColors[genMode][k]);
				lights[SCALE_LIGHT + k].setBrightness(scaleColors[scaleMode][k]);
			}
			// clock mode: blue free / gold quantized, blinking with the clock
			float blink = (std::fmod(quantized ? tapTimer : (float)args.frame * dt, clockPeriod) < clockPeriod * 0.2f) ? 1.f : 0.35f;
			lights[CLKMODE_LIGHT + 0].setBrightness(quantized ? blink : 0.f);
			lights[CLKMODE_LIGHT + 1].setBrightness(quantized ? blink * 0.7f : 0.15f * blink);
			lights[CLKMODE_LIGHT + 2].setBrightness(quantized ? 0.f : blink);
			lights[LOCK_LIGHT].setBrightness(locked);
			lights[FREEZE_LIGHT].setBrightness(frozen);
		}
	}
};

struct SiroccoWidget : ModuleWidget {
	SiroccoWidget(Sirocco* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Sirocco.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace wlayout;

		// row A: FX knob, three mode buttons, SIZE knob
		addParam(createParamCentered<Trimpot>(mm2px(Vec(C[0], YA)), module, Sirocco::FX_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[1], YA)), module, Sirocco::GENMODE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(C[1] + 5.6f, YA)), module, Sirocco::GENMODE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[2], YA)), module, Sirocco::CLKMODE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(C[2] + 5.6f, YA)), module, Sirocco::CLKMODE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[3], YA)), module, Sirocco::SCALE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(C[3] + 5.6f, YA)), module, Sirocco::SCALE_LIGHT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(C[4], YA)), module, Sirocco::SIZE_PARAM));

		// row B: GEN button, SCATTER, MELODY, TAP button
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[0], YB)), module, Sirocco::GEN_BTN_PARAM));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(C[1], YB)), module, Sirocco::SCATTER_PARAM));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(C[3], YB)), module, Sirocco::MELODY_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[4], YB)), module, Sirocco::TAP_PARAM));

		// row C: WANDER, LOCK btn, RATE, FREEZE btn, SPIN
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(C[0], YC)), module, Sirocco::WANDER_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[1], YC)), module, Sirocco::LOCK_PARAM));
		addChild(createLightCentered<MediumLight<WhiteLight>>(mm2px(Vec(C[1], YC - 6.5f)), module, Sirocco::LOCK_LIGHT));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(C[2], YC)), module, Sirocco::RATE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C[3], YC)), module, Sirocco::FREEZE_PARAM));
		addChild(createLightCentered<MediumLight<WhiteLight>>(mm2px(Vec(C[3], YC - 6.5f)), module, Sirocco::FREEZE_LIGHT));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(C[4], YC)), module, Sirocco::SPIN_PARAM));

		// row D: MIX, POS, PITCH, SHAPE (small knobs like the hardware)
		addParam(createParamCentered<Trimpot>(mm2px(Vec(C[0], YD)), module, Sirocco::MIX_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(C[1], YD)), module, Sirocco::POS_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(C[2], YD)), module, Sirocco::PITCH_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(C[3], YD)), module, Sirocco::SHAPE_PARAM));

		// jacks
		static const int r1[5] = {Sirocco::FX_INPUT, Sirocco::SCATTER_INPUT, Sirocco::MELODY_INPUT, Sirocco::SIZE_INPUT, Sirocco::RATE_INPUT};
		static const int r2[5] = {Sirocco::WANDER_INPUT, Sirocco::POS_INPUT, Sirocco::PITCH_INPUT, Sirocco::SHAPE_INPUT, Sirocco::SPIN_INPUT};
		static const int r3[5] = {Sirocco::MIX_INPUT, Sirocco::GEN_INPUT, Sirocco::CLOCK_INPUT, Sirocco::LOCK_INPUT, Sirocco::FREEZE_INPUT};
		for (int i = 0; i < 5; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[i], R1)), module, r1[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[i], R2)), module, r2[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[i], R3)), module, r3[i]));
		}
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[0], R4)), module, Sirocco::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C[1], R4)), module, Sirocco::IN_R_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C[2], R4)), module, Sirocco::CV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C[3], R4)), module, Sirocco::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C[4], R4)), module, Sirocco::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Sirocco* module = getModule<Sirocco>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Gen mode",
			{"Erode (clock + gen)", "Shear (audio threshold)", "Chisel (gen only)"}, &module->genMode));
		menu->addChild(createIndexPtrSubmenuItem("Scale",
			{"Major", "Minor", "Chromatic", "Unquantized"}, &module->scaleMode));
		menu->addChild(createIndexPtrSubmenuItem("CV output",
			{"Morphing CV", "Grain gates", "Clock pulses"}, &module->cvMode));
		menu->addChild(createBoolPtrMenuItem("Quantized clock mode", "", &module->quantized));
		menu->addChild(createBoolPtrMenuItem("Spin adds reversed grains", "", &module->whirlReverse));
		menu->addChild(createBoolPtrMenuItem("Lock buffer", "", &module->locked));
		menu->addChild(createBoolPtrMenuItem("Freeze grains", "", &module->frozen));
	}
};

Model* modelSirocco = createModel<Sirocco, SiroccoWidget>("Sirocco");
