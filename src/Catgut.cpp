// Catgut — a dual-voice polyphonic Karplus-Strong string synthesizer for
// VCV Rack, inspired by the architecture of the Strymon SuperKar+: a Solo
// voice bank and a Chord voice bank (16 plucked/bowed strings each), with
// damping, decay, attack morphing, feedback-polarity timbre switching,
// scale-aware harmonies, per-note detune, pitch bend, and audio inputs for
// sympathetic resonance. Original implementation.

#include "plugin.hpp"
#include "layout_catgut.hpp"

static const int VOICES_PER_BANK = 16;
static const float MIN_FREQ = 16.35f; // C0

// scale interval tables (semitones within an octave), -1 terminated
static const int SCALES[8][9] = {
	{0, 2, 4, 5, 7, 9, 11, -1},   // major
	{0, 2, 3, 5, 7, 8, 11, -1},   // harmonic minor
	{0, 2, 3, 5, 7, 8, 10, -1},   // aeolian
	{0, 2, 3, 5, 7, 9, 10, -1},   // dorian
	{0, 2, 4, 6, 7, 9, 11, -1},   // lydian
	{0, 1, 3, 5, 7, 8, 10, -1},   // phrygian
	{0, 2, 4, 5, 7, 9, 11, -1},   // expanded-voicing major (wider spread)
	{0, 4, 7, -1},                // major triads only
};

struct Catgut : Module {
	enum ParamId {
		DAMP_S_PARAM,
		DECAY_S_PARAM,
		ATTACK_S_PARAM,
		DAMP_C_PARAM,
		DECAY_C_PARAM,
		ATTACK_C_PARAM,
		TUNE_PARAM,
		HARMONY_PARAM,
		TIMBRE_S_PARAM,
		TIMBRE_C_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TRIG_S_INPUT,
		ATTACK_S_INPUT,
		VOCT_S_INPUT,
		VOCT_C_INPUT,
		ATTACK_C_INPUT,
		TRIG_C_INPUT,
		DAMP_S_INPUT,
		DETUNE_INPUT,
		DECAY_S_INPUT,
		DECAY_C_INPUT,
		HARMONY_INPUT,
		DAMP_C_INPUT,
		PITCH_INPUT,
		IN_S_INPUT,
		IN_C_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(TIMBRE_S_LIGHT, 2), // green/red
		ENUMS(TIMBRE_C_LIGHT, 2),
		LIGHTS_LEN
	};

	struct StringVoice {
		std::vector<float> buf;
		int bufLen = 0;
		int w = 0;
		float delaySamp = 100.f;      // current (slewed for glide/bends)
		float targetDelay = 100.f;
		float dampState = 0.f;
		float excEnv = 0.f;
		float excRate = 0.001f;
		float detuneSemis = 0.f;
		float baseFreq = 220.f;
		bool active = false;
		float pan = 0.5f;
		uint32_t rng = 1;

		void alloc(float sr) {
			bufLen = (int)(sr / MIN_FREQ * 1.2f) + 8;
			buf.assign(bufLen, 0.f);
			w = 0;
			active = false;
			dampState = 0.f;
			excEnv = 0.f;
		}

		float noise() {
			rng = rng * 1664525u + 1013904223u;
			return (float)(int32_t)rng / 2147483648.f;
		}

		void pluck(float freq, float attackTime, float sr, float pan_, float detune) {
			baseFreq = freq;
			detuneSemis = detune;
			pan = pan_;
			excEnv = 1.f;
			excRate = 1.f / std::max(1.f, attackTime * sr);
			active = true;
		}

		// returns voice output; loop params supplied per block
		float process(float dampA, float g, float polarity, float bendRatio, float glideK, float extIn, float sr) {
			if (!active)
				return 0.f;
			float freq = baseFreq * std::exp2(detuneSemis / 12.f) * bendRatio;
			freq = clamp(freq, MIN_FREQ, sr * 0.4f);
			// negative-polarity loops resonate at half the loop frequency
			float effDelay = sr / freq;
			if (polarity < 0.f)
				effDelay *= 0.5f;
			// subtract the damping filter's phase delay to stay in tune
			float lpDelay = (1.f - dampA) / std::max(0.01f, dampA);
			targetDelay = clamp(effDelay - lpDelay, 2.f, (float)(bufLen - 4));
			delaySamp += (targetDelay - delaySamp) * glideK;

			// read with linear interpolation
			float rp = (float)w - delaySamp;
			while (rp < 0.f)
				rp += bufLen;
			int i0 = (int)rp;
			float fr = rp - i0;
			int i1 = (i0 + 1 >= bufLen) ? 0 : i0 + 1;
			float read = buf[i0] + (buf[i1] - buf[i0]) * fr;

			// loop damping (one-pole lowpass)
			dampState += dampA * (read - dampState);
			float fb = dampState * g * polarity;

			// exciter: enveloped noise (short = pluck, long = bow)
			float exc = 0.f;
			if (excEnv > 0.0005f) {
				exc = noise() * excEnv * 0.6f;
				excEnv -= excEnv * excRate * 4.f;
			}

			buf[w] = exc + extIn + fb;
			w++;
			if (w >= bufLen)
				w = 0;

			float out = dampState;
			// retire silent voices
			if (excEnv <= 0.0005f && std::fabs(out) < 1e-5f && std::fabs(read) < 1e-5f)
				active = false;
			return out;
		}
	};

	StringVoice solo[VOICES_PER_BANK];
	StringVoice chord[VOICES_PER_BANK];
	int soloNext = 0;
	bool timbreS = true;  // true = string (positive), false = tube (negative)
	bool timbreC = true;

	// menu settings
	int polyphony = 8;
	int outMode = 0;       // 0 wide, 1 narrow, 2 split, 3 mono
	int scaleIdx = 0;
	bool tuneQuantize = true;
	float glideS = 0.f, glideC = 0.f;
	float levelS = 1.f, levelC = 1.f;

	dsp::SchmittTrigger trigS[VOICES_PER_BANK], trigC;
	dsp::ClockDivider lightDivider;

	Catgut() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DAMP_S_PARAM, 0.f, 1.f, 0.5f, "Solo damp");
		configParam(DECAY_S_PARAM, 0.f, 1.f, 0.5f, "Solo decay");
		configParam(ATTACK_S_PARAM, 0.f, 1.f, 0.1f, "Solo attack (strike to bow)");
		configParam(DAMP_C_PARAM, 0.f, 1.f, 0.5f, "Chord damp");
		configParam(DECAY_C_PARAM, 0.f, 1.f, 0.55f, "Chord decay");
		configParam(ATTACK_C_PARAM, 0.f, 1.f, 0.15f, "Chord attack (strike to bow)");
		configParam(TUNE_PARAM, -0.5f, 0.5f, 0.f, "Tune", " oct");
		configParam(HARMONY_PARAM, 0.f, 14.f, 7.f, "Harmony");
		paramQuantities[HARMONY_PARAM]->snapEnabled = true;
		configButton(TIMBRE_S_PARAM, "Solo timbre (string/tube)");
		configButton(TIMBRE_C_PARAM, "Chord timbre (string/tube)");
		configInput(TRIG_S_INPUT, "Solo trigger (polyphonic)");
		configInput(ATTACK_S_INPUT, "Solo attack CV");
		configInput(VOCT_S_INPUT, "Solo 1V/oct (polyphonic)");
		configInput(VOCT_C_INPUT, "Chord root 1V/oct (semitone quantized)");
		configInput(ATTACK_C_INPUT, "Chord attack CV");
		configInput(TRIG_C_INPUT, "Chord trigger");
		configInput(DAMP_S_INPUT, "Solo damp CV");
		configInput(DETUNE_INPUT, "Detune (random per-note tuning error, 0–5 V)");
		configInput(DECAY_S_INPUT, "Solo decay CV");
		configInput(DECAY_C_INPUT, "Chord decay CV");
		configInput(HARMONY_INPUT, "Harmony CV");
		configInput(DAMP_C_INPUT, "Chord damp CV");
		configInput(PITCH_INPUT, "Solo pitch bend (±1 octave)");
		configInput(IN_S_INPUT, "Solo audio (sympathetic excitation)");
		configInput(IN_C_INPUT, "Chord audio (sympathetic excitation)");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		lightDivider.setDivision(64);
		allocate(44100.f);
	}

	void allocate(float sr) {
		for (int i = 0; i < VOICES_PER_BANK; i++) {
			solo[i].alloc(sr);
			chord[i].alloc(sr);
			solo[i].rng = 100 + i;
			chord[i].rng = 900 + i;
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "timbreS", json_boolean(timbreS));
		json_object_set_new(root, "timbreC", json_boolean(timbreC));
		json_object_set_new(root, "polyphony", json_integer(polyphony));
		json_object_set_new(root, "outMode", json_integer(outMode));
		json_object_set_new(root, "scaleIdx", json_integer(scaleIdx));
		json_object_set_new(root, "tuneQuantize", json_boolean(tuneQuantize));
		json_object_set_new(root, "glideS", json_real(glideS));
		json_object_set_new(root, "glideC", json_real(glideC));
		json_object_set_new(root, "levelS", json_real(levelS));
		json_object_set_new(root, "levelC", json_real(levelC));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "timbreS"))) timbreS = json_boolean_value(j);
		if ((j = json_object_get(root, "timbreC"))) timbreC = json_boolean_value(j);
		if ((j = json_object_get(root, "polyphony"))) polyphony = json_integer_value(j);
		if ((j = json_object_get(root, "outMode"))) outMode = json_integer_value(j);
		if ((j = json_object_get(root, "scaleIdx"))) scaleIdx = json_integer_value(j);
		if ((j = json_object_get(root, "tuneQuantize"))) tuneQuantize = json_boolean_value(j);
		if ((j = json_object_get(root, "glideS"))) glideS = json_real_value(j);
		if ((j = json_object_get(root, "glideC"))) glideC = json_real_value(j);
		if ((j = json_object_get(root, "levelS"))) levelS = json_real_value(j);
		if ((j = json_object_get(root, "levelC"))) levelC = json_real_value(j);
	}

	float tuneOffset() {
		float t = params[TUNE_PARAM].getValue();
		if (tuneQuantize)
			return std::round(t * 12.f) / 12.f;
		return t;
	}

	// quantize a semitone offset (relative to root) into the active scale
	int scaleDegreeUp(int rootSemi, int degreesUp) {
		const int* sc = SCALES[scaleIdx];
		int n = 0;
		while (sc[n] >= 0)
			n++;
		int oct = degreesUp / n;
		int idx = degreesUp % n;
		return rootSemi + oct * 12 + sc[idx];
	}

	// chord intervals (in semitones, relative to root) for a harmony index
	int harmonyNotes(int harmIdx, int rootSemi, int* notes) {
		int k = 0;
		notes[k++] = rootSemi;
		switch (harmIdx) {
			case 0: notes[k++] = rootSemi - 24; break;                       // root −2 oct
			case 1: notes[k++] = rootSemi - 12; break;                       // root −oct
			case 2: notes[k++] = rootSemi - 5; break;                        // root −4th
			case 3: notes[k++] = rootSemi + 7; break;                        // root +5th
			case 4: notes[k++] = rootSemi + 12; break;                       // root +oct
			case 5: notes[k++] = rootSemi + 12; notes[k++] = rootSemi + 19; break; // oct + 5th
			case 6: notes[k++] = rootSemi + 24; break;                       // root +2 oct
			default: {
				// smart harmonies: stack scale thirds above the root
				int stack = harmIdx - 6; // 1..8 added voices
				// find the root's degree within the scale (nearest)
				const int* sc = SCALES[scaleIdx];
				int n = 0;
				while (sc[n] >= 0)
					n++;
				int rootPc = ((rootSemi % 12) + 12) % 12;
				int deg = 0, best = 127;
				for (int i = 0; i < n; i++) {
					int d = std::abs(sc[i] - rootPc);
					if (d < best) {
						best = d;
						deg = i;
					}
				}
				int base = rootSemi - sc[deg]; // octave-aligned scale base
				bool expanded = (scaleIdx == 6);
				for (int v = 1; v <= stack && k < 8; v++) {
					int step = expanded ? v * 3 : v * 2; // expanded voicing spreads wider
					notes[k++] = scaleDegreeUp(base, deg + step) - base + rootSemi - sc[deg];
				}
			} break;
		}
		return k;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;

		// ---- timbre buttons ----
		if (timbreBtnS.process(params[TIMBRE_S_PARAM].getValue() > 0.f))
			timbreS = !timbreS;
		if (timbreBtnC.process(params[TIMBRE_C_PARAM].getValue() > 0.f))
			timbreC = !timbreC;

		float tune = tuneOffset();

		// ---- solo voice triggering ----
		int polyCh = std::max(inputs[VOCT_S_INPUT].getChannels(), inputs[TRIG_S_INPUT].getChannels());
		float attackS = clamp(params[ATTACK_S_PARAM].getValue() + inputs[ATTACK_S_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float attackTimeS = 0.001f * std::pow(400.f, attackS); // 1 ms .. 400 ms
		float detuneAmt = clamp(inputs[DETUNE_INPUT].getVoltage() / 5.f, 0.f, 1.f);

		if (polyCh > 1) {
			// polyphonic: each channel is a note (V/oct + gate)
			for (int c = 0; c < std::min(polyCh, VOICES_PER_BANK); c++) {
				if (trigS[c].process(inputs[TRIG_S_INPUT].getVoltage(c), 0.1f, 1.f)) {
					float v = inputs[VOCT_S_INPUT].getVoltage(c);
					float freq = MIN_FREQ * 2.f * std::exp2(v + tune); // C1 base
					float det = (solo[c].noise()) * detuneAmt * 1.f;
					float pan = (outMode == 0) ? ((c & 1) ? 0.9f : 0.1f) : (outMode == 1) ? ((c & 1) ? 0.65f : 0.35f) : 0.5f;
					solo[c].pluck(freq, attackTimeS, sr, pan, det);
				}
			}
		}
		else {
			// mono: round-robin so previous notes ring out
			if (trigS[0].process(inputs[TRIG_S_INPUT].getVoltage(), 0.1f, 1.f)) {
				int n = clamp(polyphony, 1, VOICES_PER_BANK);
				StringVoice& v = solo[soloNext % n];
				soloNext++;
				float volts = inputs[VOCT_S_INPUT].getVoltage();
				float freq = MIN_FREQ * 2.f * std::exp2(volts + tune);
				float det = v.noise() * detuneAmt * 1.f;
				float pan = (outMode == 0) ? ((soloNext & 1) ? 0.9f : 0.1f) : (outMode == 1) ? ((soloNext & 1) ? 0.65f : 0.35f) : 0.5f;
				v.pluck(freq, attackTimeS, sr, pan, det);
			}
		}

		// ---- chord voice triggering ----
		if (trigC.process(inputs[TRIG_C_INPUT].getVoltage(), 0.1f, 1.f)) {
			float attackC = clamp(params[ATTACK_C_PARAM].getValue() + inputs[ATTACK_C_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			float attackTimeC = 0.001f * std::pow(400.f, attackC);
			float harm = clamp(params[HARMONY_PARAM].getValue() + inputs[HARMONY_INPUT].getVoltage() * 1.4f, 0.f, 14.f);
			int harmIdx = (int)std::round(harm);
			// root quantized to semitones for stability
			float rootV = inputs[VOCT_C_INPUT].getVoltage();
			int rootSemi = (int)std::round(rootV * 12.f);
			int notes[8];
			int nNotes = harmonyNotes(harmIdx, rootSemi, notes);
			for (int i = 0; i < nNotes && i < VOICES_PER_BANK; i++) {
				float freq = MIN_FREQ * 2.f * std::exp2(notes[i] / 12.f + tune);
				float pan;
				if (outMode == 0)
					pan = 0.15f + 0.7f * (float)i / std::max(1, nNotes - 1);
				else if (outMode == 1)
					pan = 0.35f + 0.3f * (float)i / std::max(1, nNotes - 1);
				else
					pan = 0.5f;
				chord[i].pluck(freq, attackTimeC, sr, pan, 0.f);
			}
		}

		// ---- per-sample loop parameters ----
		float dampS = clamp(params[DAMP_S_PARAM].getValue() + inputs[DAMP_S_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float dampC = clamp(params[DAMP_C_PARAM].getValue() + inputs[DAMP_C_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float decayS = clamp(params[DECAY_S_PARAM].getValue() + inputs[DECAY_S_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float decayC = clamp(params[DECAY_C_PARAM].getValue() + inputs[DECAY_C_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		// damp: loop lowpass coefficient (bright .. muted)
		float dampAS = 1.f - std::exp(-2.f * M_PI * (400.f * std::pow(30.f, 1.f - dampS)) / sr);
		float dampAC = 1.f - std::exp(-2.f * M_PI * (400.f * std::pow(30.f, 1.f - dampC)) / sr);
		float bend = std::exp2(clamp(inputs[PITCH_INPUT].getVoltage() / 5.f, -1.f, 1.f));
		float glideKS = (glideS > 0.001f) ? (1.f - std::exp(-dt / (glideS * 0.5f))) : 1.f;
		float glideKC = (glideC > 0.001f) ? (1.f - std::exp(-dt / (glideC * 0.5f))) : 1.f;
		float polS = timbreS ? 1.f : -1.f;
		float polC = timbreC ? 1.f : -1.f;
		float extS = inputs[IN_S_INPUT].getVoltage() / 5.f * 0.25f;
		float extC = inputs[IN_C_INPUT].getVoltage() / 5.f * 0.25f;

		auto decayGain = [&](float decay, float delaySamp) {
			if (decay >= 0.995f)
				return 1.f;
			float t60 = 0.08f * std::pow(250.f, decay); // 0.08 s .. 20 s
			return std::pow(10.f, -3.f * (delaySamp / sr) / t60);
		};

		// ---- render ----
		float sumLS = 0.f, sumRS = 0.f, sumLC = 0.f, sumRC = 0.f;
		for (int i = 0; i < VOICES_PER_BANK; i++) {
			if (solo[i].active) {
				float g = decayGain(decayS, solo[i].delaySamp);
				float o = solo[i].process(dampAS, g, polS, bend, glideKS, extS, sr);
				sumLS += o * std::cos(solo[i].pan * 0.5f * M_PI);
				sumRS += o * std::sin(solo[i].pan * 0.5f * M_PI);
			}
			if (chord[i].active) {
				float g = decayGain(decayC, chord[i].delaySamp);
				float o = chord[i].process(dampAC, g, polC, 1.f, glideKC, extC, sr);
				sumLC += o * std::cos(chord[i].pan * 0.5f * M_PI);
				sumRC += o * std::sin(chord[i].pan * 0.5f * M_PI);
			}
		}
		sumLS *= levelS;
		sumRS *= levelS;
		sumLC *= levelC;
		sumRC *= levelC;

		float outL, outR;
		if (outMode == 2) { // split: solo L, chord R
			outL = sumLS + sumRS;
			outR = sumLC + sumRC;
		}
		else if (outMode == 3) { // mono sum
			outL = outR = (sumLS + sumRS + sumLC + sumRC) * 0.7f;
		}
		else {
			outL = sumLS + sumLC;
			outR = sumRS + sumRC;
		}
		outputs[OUT_L_OUTPUT].setVoltage(std::tanh(outL * 1.5f) * 5.f);
		outputs[OUT_R_OUTPUT].setVoltage(std::tanh(outR * 1.5f) * 5.f);

		// ---- lights ----
		if (lightDivider.process()) {
			lights[TIMBRE_S_LIGHT + 0].setBrightness(timbreS ? 1.f : 0.f);
			lights[TIMBRE_S_LIGHT + 1].setBrightness(timbreS ? 0.f : 1.f);
			lights[TIMBRE_C_LIGHT + 0].setBrightness(timbreC ? 1.f : 0.f);
			lights[TIMBRE_C_LIGHT + 1].setBrightness(timbreC ? 0.f : 1.f);
		}
	}

	dsp::BooleanTrigger timbreBtnS, timbreBtnC;
};

struct CatgutWidget : ModuleWidget {
	CatgutWidget(Catgut* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Catgut.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace glayout;

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SOLO_X, ROW1)), module, Catgut::DAMP_S_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(MID_X, ROW1)), module, Catgut::TUNE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(CHORD_X, ROW1)), module, Catgut::DAMP_C_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SOLO_X, ROW2)), module, Catgut::DECAY_S_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(MID_X, ROW2)), module, Catgut::HARMONY_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(CHORD_X, ROW2)), module, Catgut::DECAY_C_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SOLO_X, ROW3)), module, Catgut::ATTACK_S_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(CHORD_X, ROW3)), module, Catgut::ATTACK_C_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(TIMBRE_SX, TIMBRE_Y)), module, Catgut::TIMBRE_S_PARAM));
		addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(TIMBRE_SX, TIMBRE_Y + TIMBRE_LED_DY)), module, Catgut::TIMBRE_S_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(TIMBRE_CX, TIMBRE_Y)), module, Catgut::TIMBRE_C_PARAM));
		addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(TIMBRE_CX, TIMBRE_Y + TIMBRE_LED_DY)), module, Catgut::TIMBRE_C_LIGHT));

		static const int r1[6] = {Catgut::TRIG_S_INPUT, Catgut::ATTACK_S_INPUT, Catgut::VOCT_S_INPUT,
		                          Catgut::VOCT_C_INPUT, Catgut::ATTACK_C_INPUT, Catgut::TRIG_C_INPUT};
		static const int r2[6] = {Catgut::DAMP_S_INPUT, Catgut::DETUNE_INPUT, Catgut::DECAY_S_INPUT,
		                          Catgut::DECAY_C_INPUT, Catgut::HARMONY_INPUT, Catgut::DAMP_C_INPUT};
		for (int i = 0; i < 6; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[i], R1)), module, r1[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[i], R2)), module, r2[i]));
		}
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[0], R3)), module, Catgut::PITCH_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[1], R3)), module, Catgut::IN_S_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[3], R3)), module, Catgut::IN_C_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[4], R3)), module, Catgut::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[5], R3)), module, Catgut::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Catgut* module = getModule<Catgut>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Output mode",
			{"Wide stereo", "Narrow stereo", "Split (solo L / chord R)", "Mono sum"}, &module->outMode));
		menu->addChild(createIndexPtrSubmenuItem("Scale (smart harmonies)",
			{"Major", "Harmonic minor", "Aeolian", "Dorian", "Lydian", "Phrygian", "Expanded major", "Major triads"}, &module->scaleIdx));
		menu->addChild(createBoolPtrMenuItem("Quantize tune to semitones", "", &module->tuneQuantize));
		struct PolyQuantity : Quantity {
			Catgut* m;
			void setValue(float v) override { m->polyphony = clamp((int)std::round(v), 1, 16); }
			float getValue() override { return m->polyphony; }
			float getMaxValue() override { return 16.f; }
			float getMinValue() override { return 1.f; }
			float getDefaultValue() override { return 8.f; }
			std::string getLabel() override { return "Solo polyphony (mono trig)"; }
		};
		struct PolySlider : ui::Slider {
			PolySlider(Catgut* m) {
				PolyQuantity* q = new PolyQuantity;
				q->m = m;
				quantity = q;
				box.size.x = 200.f;
			}
			~PolySlider() { delete quantity; }
		};
		menu->addChild(new PolySlider(module));
		struct FloatPtrQuantity : Quantity {
			float* p;
			std::string label;
			void setValue(float v) override { *p = clamp(v, 0.f, 1.f); }
			float getValue() override { return *p; }
			std::string getLabel() override { return label; }
		};
		struct FloatSlider : ui::Slider {
			FloatSlider(float* p, const std::string& label) {
				FloatPtrQuantity* q = new FloatPtrQuantity;
				q->p = p;
				q->label = label;
				quantity = q;
				box.size.x = 200.f;
			}
			~FloatSlider() { delete quantity; }
		};
		menu->addChild(new FloatSlider(&module->glideS, "Solo glide"));
		menu->addChild(new FloatSlider(&module->glideC, "Chord glide"));
		menu->addChild(new FloatSlider(&module->levelS, "Solo level"));
		menu->addChild(new FloatSlider(&module->levelC, "Chord level"));
	}
};

Model* modelCatgut = createModel<Catgut, CatgutWidget>("Catgut");
