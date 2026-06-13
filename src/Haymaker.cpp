// Haymaker — a 12-pad sampler/composer groovebox for VCV Rack, inspired by
// the workflow of compact pad samplers (in the spirit of Teenage
// Engineering's EP-133 K.O. II): sample pads, a step sequencer with live
// recording and swing, punch-in performance effects, and a macro fader.
// Entirely original implementation, including a procedurally synthesized
// default drum kit and a minimal WAV file loader.

#include "plugin.hpp"
#include "layout_haymaker.hpp"
#include <osdialog.h>
#include <fstream>

static const int NUM_PADS = 12;
static const int NUM_STEPS = 16;

struct Haymaker : Module {
	enum ParamId {
		ENUMS(PAD_PARAM, NUM_PADS),
		PITCH_PARAM,
		LEVEL_PARAM,
		PAN_PARAM,
		LEN_PARAM,
		ENUMS(FX_PARAM, 6),
		BPM_PARAM,
		SWING_PARAM,
		STEPS_PARAM,
		FADER_PARAM,
		RUN_PARAM,
		REC_PARAM,
		ENUMS(STEP_PARAM, NUM_STEPS),
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(PAD_LIGHT, NUM_PADS),
		ENUMS(FX_LIGHT, 6),
		RUN_LIGHT,
		REC_LIGHT,
		ENUMS(STEP_LIGHT, NUM_STEPS * 2), // green: pattern, red: playhead
		LIGHTS_LEN
	};

	struct Slot {
		std::vector<float> dataL, dataR;
		float fileSr = 44100.f;
		std::string path;
		// per-slot edit values (0..1 normalized where noted)
		float pitch = 0.f;   // semitones -12..12
		float level = 0.8f;
		float pan = 0.5f;
		float len = 1.f;
	};

	struct Voice {
		bool active = false;
		double pos = 0.0;
		double ratio = 1.0;
		float gainL = 0.5f, gainR = 0.5f;
		float lenFrac = 1.f;
	};

	Slot slots[NUM_PADS];
	Voice voices[NUM_PADS]; // one voice per pad (natural choke on retrigger)
	bool pattern[NUM_PADS][NUM_STEPS] = {};

	int selectedPad = 0;
	bool running = true;
	bool recArmed = false;

	// knob pickup for slot editing
	bool knobAttached[4] = {};
	float knobPrev[4] = {};
	int knobContext = -1;

	// sequencer
	int step = -1;
	double stepPhase = 1e9;
	float clockPeriodExt = 0.125f;
	float lastEdge = 1e9f;
	int clockMult = 1;       // external clock interpreted as 16ths (1), 8ths (2), quarters (4)
	int subRemain = 0;       // multiplied sub-steps pending between external ticks
	float subTimer = 0.f;
	float lastStepDur = 0.125f;
	float sinceStep = 0.f;   // time since the last step, for live-rec quantizing

	// fx state
	std::vector<float> fxBufL, fxBufR; // rolling buffer for stutter/reverse
	int fxBufLen = 0;
	int fxW = 0;
	double stutterPh = 0.0;
	bool stutterHeld = false;
	int stutterStart = 0;
	float tapeRate = 1.f;
	float lpState[2][2] = {};
	float hpState[2][2] = {};
	float crushHeldL = 0.f, crushHeldR = 0.f;
	int crushCount = 0;

	dsp::SchmittTrigger clockTrig, resetTrig;
	dsp::BooleanTrigger padBtn[NUM_PADS], stepBtn[NUM_STEPS], runBtn, recBtn;
	dsp::ClockDivider lightDivider;

	Haymaker() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < NUM_PADS; i++)
			configButton(PAD_PARAM + i, string::f("Pad %d", i + 1));
		configParam(PITCH_PARAM, -12.f, 12.f, 0.f, "Pad pitch", " st");
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.8f, "Pad level", "%", 0.f, 100.f);
		configParam(PAN_PARAM, 0.f, 1.f, 0.5f, "Pad pan");
		configParam(LEN_PARAM, 0.f, 1.f, 1.f, "Pad length", "%", 0.f, 100.f);
		const char* fxNames[6] = {"Stutter", "Tape stop", "Lowpass", "Highpass", "Crush", "Reverse"};
		for (int i = 0; i < 6; i++)
			configButton(FX_PARAM + i, string::f("Punch FX: %s (hold)", fxNames[i]));
		configParam(BPM_PARAM, 40.f, 240.f, 120.f, "Tempo", " BPM");
		paramQuantities[BPM_PARAM]->snapEnabled = true;
		configParam(SWING_PARAM, 0.f, 1.f, 0.f, "Swing", "%", 0.f, 100.f);
		configParam(STEPS_PARAM, 1.f, 16.f, 16.f, "Pattern length");
		paramQuantities[STEPS_PARAM]->snapEnabled = true;
		configParam(FADER_PARAM, 0.f, 1.f, 1.f, "FX fader");
		configButton(RUN_PARAM, "Run/stop");
		configButton(REC_PARAM, "Record (live pad capture)");
		for (int i = 0; i < NUM_STEPS; i++)
			configButton(STEP_PARAM + i, string::f("Step %d (selected pad)", i + 1));
		configInput(CLOCK_INPUT, "Clock (16th notes, overrides BPM)");
		configInput(RESET_INPUT, "Reset");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		lightDivider.setDivision(64);
		allocate(44100.f);
		synthesizeKit(44100.f);
	}

	void allocate(float sr) {
		fxBufLen = (int)(sr * 2.f);
		fxBufL.assign(fxBufLen, 0.f);
		fxBufR.assign(fxBufLen, 0.f);
		fxW = 0;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	// ---- procedurally synthesized default kit ----
	void synthSlot(int idx, float seconds, float sr, std::function<float(float, float)> fn) {
		Slot& s = slots[idx];
		int n = (int)(seconds * sr);
		s.dataL.resize(n);
		s.dataR.resize(n);
		s.fileSr = sr;
		s.path = "";
		uint32_t rng = 1234 + idx * 999;
		for (int i = 0; i < n; i++) {
			rng = rng * 1664525u + 1013904223u;
			float noise = (float)(int32_t)rng / 2147483648.f;
			float t = i / sr;
			float v = fn(t, noise);
			// global fade-out
			float rel = 1.f - (float)i / n;
			v *= std::min(1.f, rel * 8.f);
			s.dataL[i] = s.dataR[i] = clamp(v, -1.f, 1.f);
		}
	}

	void synthesizeKit(float sr) {
		// 1 kick: falling sine sweep + click
		synthSlot(0, 0.45f, sr, [](float t, float n) {
			// integrated falling-frequency sine sweep + click
			return std::sin(2.f * M_PI * (42.f * t + (180.f / 28.f) * (1.f - std::exp(-t * 28.f)))) *
			       std::exp(-t * 7.f) + n * 0.3f * std::exp(-t * 300.f);
		});
		// 2 snare: tone + bright noise
		synthSlot(1, 0.3f, sr, [](float t, float n) {
			return 0.5f * std::sin(2.f * M_PI * 196.f * t) * std::exp(-t * 25.f) +
			       0.7f * n * std::exp(-t * 16.f);
		});
		// 3 clap: noise bursts
		synthSlot(2, 0.3f, sr, [](float t, float n) {
			float env = 0.f;
			for (int b = 0; b < 3; b++)
				env += std::exp(-std::fabs(t - b * 0.012f) * 320.f);
			env += std::exp(-t * 12.f) * 0.5f;
			return n * env * 0.8f;
		});
		// 4 rim: short click + ring
		synthSlot(3, 0.12f, sr, [](float t, float n) {
			return 0.8f * std::sin(2.f * M_PI * 1750.f * t) * std::exp(-t * 90.f) +
			       n * 0.4f * std::exp(-t * 500.f);
		});
		// 5 closed hat
		synthSlot(4, 0.09f, sr, [](float t, float n) {
			float metal = std::sin(2.f * M_PI * 6300.f * t) + std::sin(2.f * M_PI * 8900.f * t);
			return (n * 0.7f + metal * 0.2f) * std::exp(-t * 65.f);
		});
		// 6 open hat
		synthSlot(5, 0.45f, sr, [](float t, float n) {
			float metal = std::sin(2.f * M_PI * 6300.f * t) + std::sin(2.f * M_PI * 8900.f * t);
			return (n * 0.6f + metal * 0.2f) * std::exp(-t * 9.f);
		});
		// 7-9 toms low/mid/high
		for (int k = 0; k < 3; k++) {
			float base = 90.f * std::pow(1.5f, k);
			synthSlot(6 + k, 0.4f, sr, [base](float t, float n) {
				float f = base * (1.f + 0.8f * std::exp(-t * 18.f));
				return std::sin(2.f * M_PI * f * t) * std::exp(-t * 9.f) + n * 0.1f * std::exp(-t * 60.f);
			});
		}
		// 10 cowbell-ish: two detuned squares
		synthSlot(9, 0.25f, sr, [](float t, float n) {
			float a = std::sin(2.f * M_PI * 555.f * t) > 0.f ? 1.f : -1.f;
			float b = std::sin(2.f * M_PI * 832.f * t) > 0.f ? 1.f : -1.f;
			return (a + b) * 0.25f * std::exp(-t * 16.f);
		});
		// 11 shaker
		synthSlot(10, 0.15f, sr, [](float t, float n) {
			float env = std::exp(-std::fabs(t - 0.03f) * 60.f);
			return n * env * 0.6f;
		});
		// 12 zap: pitch-dive laser
		synthSlot(11, 0.3f, sr, [](float t, float n) {
			float f = 1400.f * std::exp(-t * 14.f) + 60.f;
			return std::sin(2.f * M_PI * f * t * 1.5f) * std::exp(-t * 10.f);
		});
	}

	// ---- minimal WAV loader (PCM 16/24-bit and float32, mono/stereo) ----
	bool loadWav(int idx, const std::string& path) {
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return false;
		auto rd32 = [&]() { uint32_t v = 0; f.read((char*)&v, 4); return v; };
		auto rd16 = [&]() { uint16_t v = 0; f.read((char*)&v, 2); return v; };
		char tag[5] = {};
		f.read(tag, 4);
		if (std::string(tag, 4) != "RIFF")
			return false;
		rd32();
		f.read(tag, 4);
		if (std::string(tag, 4) != "WAVE")
			return false;
		uint16_t fmt = 0, channels = 0, bits = 0;
		uint32_t srate = 0;
		std::vector<char> data;
		while (f && !f.eof()) {
			f.read(tag, 4);
			uint32_t size = rd32();
			if (!f)
				break;
			std::string t(tag, 4);
			if (t == "fmt ") {
				fmt = rd16();
				channels = rd16();
				srate = rd32();
				rd32();
				rd16();
				bits = rd16();
				if (size > 16)
					f.seekg(size - 16, std::ios::cur);
			}
			else if (t == "data") {
				data.resize(size);
				f.read(data.data(), size);
				break;
			}
			else {
				f.seekg(size + (size & 1), std::ios::cur);
			}
		}
		if (data.empty() || channels == 0 || srate == 0)
			return false;
		if (!(fmt == 1 || fmt == 3 || fmt == 0xFFFE))
			return false;
		int bytesPer = bits / 8;
		int frames = (int)(data.size() / (bytesPer * channels));
		if (frames < 2)
			return false;
		frames = std::min(frames, (int)(srate * 20)); // cap 20 s
		Slot& s = slots[idx];
		s.dataL.resize(frames);
		s.dataR.resize(frames);
		for (int i = 0; i < frames; i++) {
			for (int c = 0; c < std::min<int>(channels, 2); c++) {
				const char* p = data.data() + (size_t)(i * channels + c) * bytesPer;
				float v = 0.f;
				if (bits == 16)
					v = *(const int16_t*)p / 32768.f;
				else if (bits == 24) {
					int32_t x = (uint8_t)p[0] | ((uint8_t)p[1] << 8) | ((int8_t)p[2] << 16);
					v = x / 8388608.f;
				}
				else if (bits == 32 && (fmt == 3 || fmt == 0xFFFE))
					v = *(const float*)p;
				else if (bits == 32)
					v = *(const int32_t*)p / 2147483648.f;
				else if (bits == 8)
					v = ((const uint8_t*)p)[0] / 128.f - 1.f;
				if (c == 0)
					s.dataL[i] = v;
				else
					s.dataR[i] = v;
			}
			if (channels == 1)
				s.dataR[i] = s.dataL[i];
		}
		s.fileSr = (float)srate;
		s.path = path;
		return true;
	}

	void triggerPad(int p) {
		Slot& s = slots[p];
		if (s.dataL.empty())
			return;
		Voice& v = voices[p];
		v.active = true;
		v.pos = 0.0;
		v.ratio = std::exp2(s.pitch / 12.f) * (double)s.fileSr / APP->engine->getSampleRate();
		float g = s.level * 1.2f;
		v.gainL = std::cos(s.pan * 0.5f * M_PI) * g * 1.3f;
		v.gainR = std::sin(s.pan * 0.5f * M_PI) * g * 1.3f;
		v.lenFrac = s.len;
	}

	float dataNorm(int k) {
		Slot& s = slots[selectedPad];
		switch (k) {
			case 0: return (s.pitch + 12.f) / 24.f;
			case 1: return s.level;
			case 2: return s.pan;
			default: return s.len;
		}
	}

	void writeData(int k, float v) {
		Slot& s = slots[selectedPad];
		switch (k) {
			case 0: s.pitch = v * 24.f - 12.f; break;
			case 1: s.level = v; break;
			case 2: s.pan = v; break;
			default: s.len = v; break;
		}
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;

		// ---- transport ----
		if (runBtn.process(params[RUN_PARAM].getValue() > 0.f))
			running = !running;
		if (recBtn.process(params[REC_PARAM].getValue() > 0.f))
			recArmed = !recArmed;
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			step = -1;
			stepPhase = 1e9;
		}

		// ---- clock ----
		bool extClock = inputs[CLOCK_INPUT].isConnected();
		lastEdge += dt;
		bool tick = false;
		if (extClock) {
			if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				if (lastEdge < 4.f && lastEdge > 0.0005f)
					clockPeriodExt = clamp(lastEdge, 0.0005f, 4.f);
				lastEdge = 0.f;
				tick = true;
			}
		}
		float stepDur = extClock ? clockPeriodExt / clockMult : 60.f / params[BPM_PARAM].getValue() / 4.f;
		float swing = params[SWING_PARAM].getValue() * 0.5f;
		int numSteps = (int)params[STEPS_PARAM].getValue();

		if (running) {
			if (extClock) {
				if (tick) {
					advanceStep(numSteps);
					lastStepDur = clockPeriodExt / clockMult;
					sinceStep = 0.f;
					// schedule multiplied sub-steps between ticks
					subRemain = clockMult - 1;
					subTimer = lastStepDur;
				}
				if (subRemain > 0) {
					subTimer -= dt;
					if (subTimer <= 0.f) {
						advanceStep(numSteps);
						subRemain--;
						subTimer += lastStepDur;
						sinceStep = 0.f;
					}
				}
			}
			else {
				float thisDur = stepDur * (((step & 1) == 1) ? (1.f - swing) : (1.f + swing));
				stepPhase += dt;
				if (stepPhase >= thisDur) {
					stepPhase -= thisDur; // keep the remainder: no drift
					if (stepPhase >= thisDur)
						stepPhase = 0.0; // startup/reset sentinel: don't machine-gun
					advanceStep(numSteps);
					lastStepDur = thisDur;
					sinceStep = 0.f;
				}
			}
			sinceStep += dt;
		}

		// ---- pads ----
		for (int p = 0; p < NUM_PADS; p++) {
			if (padBtn[p].process(params[PAD_PARAM + p].getValue() > 0.f)) {
				selectedPad = p;
				triggerPad(p);
				if (recArmed && running && step >= 0) {
					// quantize to the nearest step (works for both clocks)
					int target = step;
					if (sinceStep > lastStepDur * 0.5f)
						target = (step + 1) % numSteps;
					pattern[p][target] = true;
				}
			}
		}

		// ---- step buttons (edit selected pad) ----
		for (int i = 0; i < NUM_STEPS; i++) {
			if (stepBtn[i].process(params[STEP_PARAM + i].getValue() > 0.f))
				pattern[selectedPad][i] = !pattern[selectedPad][i];
		}

		// ---- slot knobs with pickup ----
		if (knobContext != selectedPad) {
			knobContext = selectedPad;
			static const int kn[4] = {PITCH_PARAM, LEVEL_PARAM, PAN_PARAM, LEN_PARAM};
			for (int k = 0; k < 4; k++) {
				knobAttached[k] = false;
				float raw = params[kn[k]].getValue();
				knobPrev[k] = (k == 0) ? (raw + 12.f) / 24.f : raw;
			}
		}
		static const int knIds[4] = {PITCH_PARAM, LEVEL_PARAM, PAN_PARAM, LEN_PARAM};
		for (int k = 0; k < 4; k++) {
			float raw = params[knIds[k]].getValue();
			float v = (k == 0) ? (raw + 12.f) / 24.f : raw;
			float data = dataNorm(k);
			if (!knobAttached[k]) {
				bool crossed = (knobPrev[k] - data) * (v - data) <= 0.f && std::fabs(v - knobPrev[k]) > 1e-6f;
				if (std::fabs(v - data) < 0.03f || crossed)
					knobAttached[k] = true;
			}
			if (knobAttached[k] && std::fabs(v - knobPrev[k]) > 1e-6f)
				writeData(k, v);
			knobPrev[k] = v;
		}

		// ---- voices ----
		float mixL = 0.f, mixR = 0.f;
		for (int p = 0; p < NUM_PADS; p++) {
			Voice& v = voices[p];
			if (!v.active)
				continue;
			Slot& s = slots[p];
			int n = (int)s.dataL.size();
			float endPos = n * v.lenFrac;
			if (v.pos >= endPos - 1 || v.pos >= n - 1) {
				v.active = false;
				continue;
			}
			int i0 = (int)v.pos;
			float fr = (float)(v.pos - i0);
			float l = s.dataL[i0] + (s.dataL[i0 + 1] - s.dataL[i0]) * fr;
			float r = s.dataR[i0] + (s.dataR[i0 + 1] - s.dataR[i0]) * fr;
			// short fades at start and at trimmed end
			float fadeIn = std::min(1.f, (float)v.pos / (0.002f * sr));
			float fadeOut = std::min(1.f, (float)(endPos - v.pos) / (0.005f * sr));
			float g = fadeIn * fadeOut;
			mixL += l * v.gainL * g;
			mixR += r * v.gainR * g;
			v.pos += v.ratio * tapeRate;
		}

		// ---- punch-in FX ----
		float fader = params[FADER_PARAM].getValue();
		bool fxStut = params[FX_PARAM + 0].getValue() > 0.f;
		bool fxTape = params[FX_PARAM + 1].getValue() > 0.f;
		bool fxLp = params[FX_PARAM + 2].getValue() > 0.f;
		bool fxHp = params[FX_PARAM + 3].getValue() > 0.f;
		bool fxCrush = params[FX_PARAM + 4].getValue() > 0.f;
		bool fxRev = params[FX_PARAM + 5].getValue() > 0.f;

		// tape stop: slew global rate toward 0 while held
		float tapeTarget = fxTape ? 0.f : 1.f;
		float tapeK = 1.f - std::exp(-dt / (0.05f + 0.6f * fader));
		tapeRate += (tapeTarget - tapeRate) * tapeK;

		// rolling FX buffer (pre-stutter output)
		fxBufL[fxW] = mixL;
		fxBufR[fxW] = mixR;

		float outL = mixL, outR = mixR;
		float stutLen = stepDur * sr * (0.25f + 0.75f * (1.f - fader)); // fader shortens the loop
		if ((fxStut || fxRev) && fxBufLen > 0) {
			if (!stutterHeld) {
				stutterHeld = true;
				stutterStart = fxW;
				stutterPh = 0.0;
			}
			stutterPh += fxRev ? -1.0 : 1.0;
			double ph = std::fmod(stutterPh, (double)stutLen);
			if (ph < 0)
				ph += stutLen;
			int ri = (int)(stutterStart - stutLen + ph);
			while (ri < 0)
				ri += fxBufLen;
			ri %= fxBufLen;
			outL = fxBufL[ri];
			outR = fxBufR[ri];
		}
		else {
			stutterHeld = false;
		}
		fxW = (fxW + 1) % fxBufLen;

		if (fxLp) {
			float fc = 12000.f * std::pow(80.f / 12000.f, fader);
			float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
			float xl = outL, xr = outR;
			for (int s2 = 0; s2 < 2; s2++) {
				lpState[0][s2] += a * (xl - lpState[0][s2]);
				lpState[1][s2] += a * (xr - lpState[1][s2]);
				xl = lpState[0][s2];
				xr = lpState[1][s2];
			}
			outL = xl;
			outR = xr;
		}
		if (fxHp) {
			float fc = 30.f * std::pow(6000.f / 30.f, fader);
			float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
			for (int s2 = 0; s2 < 2; s2++) {
				hpState[0][s2] += a * (outL - hpState[0][s2]);
				hpState[1][s2] += a * (outR - hpState[1][s2]);
			}
			outL -= hpState[0][1];
			outR -= hpState[1][1];
		}
		if (fxCrush) {
			int holdN = 1 + (int)(fader * fader * 30.f);
			if (++crushCount >= holdN) {
				crushCount = 0;
				float bits = 12.f - fader * 8.f;
				float q = std::pow(2.f, bits);
				crushHeldL = std::round(clamp(outL, -1.2f, 1.2f) * q) / q;
				crushHeldR = std::round(clamp(outR, -1.2f, 1.2f) * q) / q;
			}
			outL = crushHeldL;
			outR = crushHeldR;
		}

		outputs[OUT_L_OUTPUT].setVoltage(clamp(std::tanh(outL) * 5.f, -10.f, 10.f));
		outputs[OUT_R_OUTPUT].setVoltage(clamp(std::tanh(outR) * 5.f, -10.f, 10.f));

		// ---- lights ----
		if (lightDivider.process()) {
			for (int p = 0; p < NUM_PADS; p++) {
				float b = voices[p].active ? 1.f : (p == selectedPad ? 0.35f : 0.05f);
				lights[PAD_LIGHT + p].setBrightness(b);
			}
			static const int fxp[6] = {FX_PARAM, FX_PARAM + 1, FX_PARAM + 2, FX_PARAM + 3, FX_PARAM + 4, FX_PARAM + 5};
			for (int i = 0; i < 6; i++)
				lights[FX_LIGHT + i].setBrightness(params[fxp[i]].getValue() > 0.f);
			lights[RUN_LIGHT].setBrightness(running);
			lights[REC_LIGHT].setBrightness(recArmed);
			for (int i = 0; i < NUM_STEPS; i++) {
				lights[STEP_LIGHT + i * 2 + 0].setBrightness(pattern[selectedPad][i] ? 0.7f : 0.f);
				lights[STEP_LIGHT + i * 2 + 1].setBrightness(i == step ? 1.f : 0.f);
			}
		}
	}

	void advanceStep(int numSteps) {
		step = (step + 1) % numSteps;
		for (int p = 0; p < NUM_PADS; p++) {
			if (pattern[p][step])
				triggerPad(p);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "running", json_boolean(running));
		json_object_set_new(root, "selectedPad", json_integer(selectedPad));
		json_object_set_new(root, "clockMult", json_integer(clockMult));
		json_t* slotsJ = json_array();
		for (int p = 0; p < NUM_PADS; p++) {
			json_t* js = json_object();
			json_object_set_new(js, "path", json_string(slots[p].path.c_str()));
			json_object_set_new(js, "pitch", json_real(slots[p].pitch));
			json_object_set_new(js, "level", json_real(slots[p].level));
			json_object_set_new(js, "pan", json_real(slots[p].pan));
			json_object_set_new(js, "len", json_real(slots[p].len));
			json_t* steps = json_array();
			for (int i = 0; i < NUM_STEPS; i++)
				json_array_append_new(steps, json_boolean(pattern[p][i]));
			json_object_set_new(js, "steps", steps);
			json_array_append_new(slotsJ, js);
		}
		json_object_set_new(root, "slots", slotsJ);
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "running"))) running = json_boolean_value(j);
		if ((j = json_object_get(root, "selectedPad"))) selectedPad = json_integer_value(j);
		if ((j = json_object_get(root, "clockMult"))) clockMult = clamp((int)json_integer_value(j), 1, 4);
		json_t* slotsJ = json_object_get(root, "slots");
		if (slotsJ) {
			for (int p = 0; p < NUM_PADS && p < (int)json_array_size(slotsJ); p++) {
				json_t* js = json_array_get(slotsJ, p);
				json_t* f;
				if ((f = json_object_get(js, "path"))) {
					std::string path = json_string_value(f);
					if (!path.empty())
						loadWav(p, path);
				}
				if ((f = json_object_get(js, "pitch"))) slots[p].pitch = json_real_value(f);
				if ((f = json_object_get(js, "level"))) slots[p].level = json_real_value(f);
				if ((f = json_object_get(js, "pan"))) slots[p].pan = json_real_value(f);
				if ((f = json_object_get(js, "len"))) slots[p].len = json_real_value(f);
				json_t* steps = json_object_get(js, "steps");
				if (steps)
					for (int i = 0; i < NUM_STEPS && i < (int)json_array_size(steps); i++)
						pattern[p][i] = json_boolean_value(json_array_get(steps, i));
			}
		}
		knobContext = -1;
	}
};

struct HaymakerWidget : ModuleWidget {
	HaymakerWidget(Haymaker* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Haymaker.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace hlayout;

		for (int p = 0; p < NUM_PADS; p++) {
			float x = PAD_X[p % 3];
			float y = PAD_Y[p / 3];
			addParam(createParamCentered<VCVButton>(mm2px(Vec(x, y)), module, Haymaker::PAD_PARAM + p));
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(x + PAD_LIGHT_DX, y + PAD_LIGHT_DY)), module, Haymaker::PAD_LIGHT + p));
		}

		static const int knIds[4] = {Haymaker::PITCH_PARAM, Haymaker::LEVEL_PARAM, Haymaker::PAN_PARAM, Haymaker::LEN_PARAM};
		for (int k = 0; k < 4; k++)
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KN_X[k], KN_Y)), module, knIds[k]));

		for (int i = 0; i < 6; i++) {
			addParam(createParamCentered<TL1105>(mm2px(Vec(FX_X[i], FX_Y)), module, Haymaker::FX_PARAM + i));
			addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(FX_X[i], FX_LED_Y)), module, Haymaker::FX_LIGHT + i));
		}

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(BPM_X, TK_Y)), module, Haymaker::BPM_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(SWING_X, TK_Y)), module, Haymaker::SWING_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(STEPS_X, TK_Y)), module, Haymaker::STEPS_PARAM));
		addParam(createParamCentered<VCVSlider>(mm2px(Vec(FADER_X, FADER_Y)), module, Haymaker::FADER_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(RUN_X, TR_Y)), module, Haymaker::RUN_PARAM));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(RUN_X - 6.f, TR_Y - 5.f)), module, Haymaker::RUN_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(REC_X, TR_Y)), module, Haymaker::REC_PARAM));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(REC_X + 6.f, TR_Y - 5.f)), module, Haymaker::REC_LIGHT));

		for (int i = 0; i < NUM_STEPS; i++) {
			float x = ST_X0 + (i % 8) * ST_DX;
			float y = (i < 8) ? ST_Y1 : ST_Y2;
			addParam(createParamCentered<TL1105>(mm2px(Vec(x, y)), module, Haymaker::STEP_PARAM + i));
			addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(x, y + ST_LIGHT_DY)), module, Haymaker::STEP_LIGHT + i * 2));
		}

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CLK_X, JK_Y1)), module, Haymaker::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(RST_X, JK_Y1)), module, Haymaker::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUTL_X, JK_Y2)), module, Haymaker::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUTR_X, JK_Y2)), module, Haymaker::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Haymaker* module = getModule<Haymaker>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("External clock rate",
			{"16th notes (x1)", "8th notes (x2)", "Quarter notes (x4)"},
			[=]() { return module->clockMult == 4 ? 2 : module->clockMult == 2 ? 1 : 0; },
			[=](int v) { module->clockMult = (v == 2) ? 4 : (v == 1) ? 2 : 1; }));
		menu->addChild(createMenuItem(string::f("Load sample into pad %d…", module->selectedPad + 1), "", [=]() {
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, NULL);
			if (path) {
				module->loadWav(module->selectedPad, path);
				free(path);
			}
		}));
		menu->addChild(createMenuItem("Restore built-in kit", "", [=]() {
			module->synthesizeKit(APP->engine->getSampleRate());
		}));
		menu->addChild(createMenuItem("Clear pattern (selected pad)", "", [=]() {
			for (int i = 0; i < NUM_STEPS; i++)
				module->pattern[module->selectedPad][i] = false;
		}));
		menu->addChild(createMenuItem("Clear all patterns", "", [=]() {
			std::memset(module->pattern, 0, sizeof(module->pattern));
		}));
	}
};

Model* modelHaymaker = createModel<Haymaker, HaymakerWidget>("Haymaker");
