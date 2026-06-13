// Capo — a four-voice polyphonic chord oscillator for VCV Rack, inspired
// by the architecture of the Qu-Bit Chord v2: root/third/fifth/seventh
// oscillators with individual and mix outputs, morphing wavetable banks,
// chord quality and voicing controls, melody/poly modes, and diatonic
// auto-harmonization. Original implementation; all wavetable banks are
// procedurally synthesized at startup.

#include "plugin.hpp"
#include "layout_capo.hpp"

static const int TABLE_LEN = 2048;
static const int NUM_BANKS = 8;
static const int NUM_QUALITIES = 8;
static const int NUM_VOICINGS = 16;

// chord qualities: semitone offsets {third, fifth, seventh}
static const int QUALITY_TABLE[NUM_QUALITIES][3] = {
	{4, 7, 11},  // major 7
	{3, 7, 10},  // minor 7
	{4, 7, 10},  // dominant 7
	{3, 6, 10},  // half-diminished
	{3, 6, 9},   // diminished 7
	{2, 7, 11},  // sus2 major 7
	{5, 7, 10},  // sus4 minor 7
	{4, 8, 10},  // augmented 7
};

// voicings: octave multipliers {root, third, fifth, seventh}
static const float VOICING_TABLE[NUM_VOICINGS][4] = {
	{1, 1, 1, 1},      // closed
	{1, 0.5f, 1, 1},   // drop the third
	{1, 1, 0.5f, 1},   // drop the fifth
	{2, 1, 1, 1},      // first inversion
	{2, 1, 0.5f, 1},   // first inv, dropped fifth
	{2, 1, 1, 0.5f},   // first inv, dropped seventh
	{2, 2, 1, 0.5f},   // second inv, dropped seventh
	{2, 2, 1, 1},      // second inversion
	{1, 2, 1, 1},      // raised third
	{1, 2, 2, 1},      // raised third & fifth
	{2, 1, 2, 1},      // first inv, raised fifth
	{2, 2, 2, 1},      // third inversion
	{0.5f, 1, 1, 2},   // spread
	{2, 4, 0.5f, 1},   // spread second inv
	{2, 2, 4, 0.5f},   // spread third inv
	{0.5f, 1, 2, 4},   // big spread
};

// diatonic seventh-chord quality for each scale degree
static const int MAJOR_SCALE[7] = {0, 2, 4, 5, 7, 9, 11};
static const int MAJOR_DEGREE_QUALITY[7] = {0, 1, 1, 0, 2, 1, 3};
static const int MINOR_SCALE[7] = {0, 2, 3, 5, 7, 8, 10};
static const int MINOR_DEGREE_QUALITY[7] = {1, 3, 0, 1, 2, 0, 2};

struct Capo : Module {
	enum ParamId {
		BANK_PARAM,
		WAVE_PARAM,
		FREQ_PARAM,
		FINE_PARAM,
		QUALITY_PARAM,
		VOICING_PARAM,
		FM_ATT_PARAM,
		MODE_PARAM,
		HARM_PARAM,
		TRIAD_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		FM_INPUT,
		BANK_INPUT,
		WAVE_INPUT,
		VOICING_INPUT,
		QUALITY_INPUT,
		LEAD_INPUT,
		VOCT_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ROOT_OUTPUT,
		THIRD_OUTPUT,
		FIFTH_OUTPUT,
		SEVENTH_OUTPUT,
		MIX_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(QLED_LIGHT, 7),
		ENUMS(MODE_LIGHT, 3),
		ENUMS(HARM_LIGHT, 3),
		TRIAD_LIGHT,
		LIGHTS_LEN
	};

	// wavetables: [bank][table][mip][TABLE_LEN]; banks 0/1/5 are rendered live
	std::vector<float> tables[NUM_BANKS][5][2];
	int tablesPerBank[NUM_BANKS] = {4, 1, 5, 5, 5, 1, 4, 4};

	struct Osc {
		double phase = 0.0;
		float freq = 220.f;
		float freqSmooth = 220.f;
		float lpState = 0.f; // for the filtered-saw bank
	};
	Osc osc[4];

	int mode = 0;   // 0 chord, 1 melody, 2 free poly, 3 unison poly
	int harm = 0;   // 0 off, 1 major, 2 minor, 3 chromatic (poly modes)
	bool triad = false;

	dsp::BooleanTrigger modeBtn, harmBtn, triadBtn;
	dsp::ClockDivider lightDivider;
	int curQuality = 0;

	Capo() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(BANK_PARAM, 0.f, 7.f, 0.f, "Bank");
		paramQuantities[BANK_PARAM]->snapEnabled = true;
		configParam(WAVE_PARAM, 0.f, 1.f, 0.f, "Waveform (morph within bank)");
		configParam(FREQ_PARAM, -3.5f, 3.5f, -1.f, "Frequency (coarse)", " oct");
		configParam(FINE_PARAM, -5.f, 5.f, 0.f, "Fine tune", " st");
		configParam(QUALITY_PARAM, 0.f, 7.f, 0.f, "Chord quality");
		paramQuantities[QUALITY_PARAM]->snapEnabled = true;
		configParam(VOICING_PARAM, 0.f, 15.f, 0.f, "Voicing");
		paramQuantities[VOICING_PARAM]->snapEnabled = true;
		configParam(FM_ATT_PARAM, 0.f, 1.f, 0.f, "Linear FM amount");
		configButton(MODE_PARAM, "Mode (chord/melody/free poly/unison poly)");
		configButton(HARM_PARAM, "Auto-harmonize (off/major/minor/chromatic)");
		configButton(TRIAD_PARAM, "Triad (omit seventh from mix)");
		configInput(FM_INPUT, "Linear FM");
		configInput(BANK_INPUT, "Bank CV");
		configInput(WAVE_INPUT, "Waveform CV");
		configInput(VOICING_INPUT, "Voicing CV (free poly: fifth pitch)");
		configInput(QUALITY_INPUT, "Quality CV (free poly: seventh pitch)");
		configInput(LEAD_INPUT, "Lead (melody/poly modes)");
		configInput(VOCT_INPUT, "Root 1V/oct");
		configOutput(ROOT_OUTPUT, "Root voice");
		configOutput(THIRD_OUTPUT, "Third voice");
		configOutput(FIFTH_OUTPUT, "Fifth voice");
		configOutput(SEVENTH_OUTPUT, "Seventh voice");
		configOutput(MIX_OUTPUT, "Mix");
		lightDivider.setDivision(64);
		buildTables();
	}

	// ---- procedural wavetable construction (all original synthesis) ----
	void addHarmonic(std::vector<float>& t, int h, float amp, float phase = 0.f) {
		for (int i = 0; i < TABLE_LEN; i++)
			t[i] += amp * std::sin(2.0 * M_PI * (h * (double)i / TABLE_LEN) + phase);
	}

	void normalize(std::vector<float>& t) {
		float peak = 1e-9f;
		for (float v : t)
			peak = std::max(peak, std::fabs(v));
		for (float& v : t)
			v /= peak;
	}

	void buildBankTable(int bank, int tbl, int mip, std::function<void(std::vector<float>&, int)> gen) {
		std::vector<float>& t = tables[bank][tbl][mip];
		t.assign(TABLE_LEN, 0.f);
		gen(t, mip == 0 ? 64 : 10);
		normalize(t);
	}

	void buildTables() {
		// bank 0: classic shapes (sine, triangle, saw, square)
		buildBankTable(0, 0, 0, [&](std::vector<float>& t, int) { addHarmonic(t, 1, 1.f); });
		buildBankTable(0, 0, 1, [&](std::vector<float>& t, int) { addHarmonic(t, 1, 1.f); });
		for (int mip = 0; mip < 2; mip++) {
			buildBankTable(0, 1, mip, [&](std::vector<float>& t, int maxH) {
				for (int h = 1; h <= maxH; h += 2)
					addHarmonic(t, h, ((h / 2) % 2 ? -1.f : 1.f) / (h * h));
			});
			buildBankTable(0, 2, mip, [&](std::vector<float>& t, int maxH) {
				for (int h = 1; h <= maxH; h++)
					addHarmonic(t, h, 1.f / h);
			});
			buildBankTable(0, 3, mip, [&](std::vector<float>& t, int maxH) {
				for (int h = 1; h <= maxH; h += 2)
					addHarmonic(t, h, 1.f / h);
			});
			// bank 1: saw (filtered live at render time)
			buildBankTable(1, 0, mip, [&](std::vector<float>& t, int maxH) {
				for (int h = 1; h <= maxH; h++)
					addHarmonic(t, h, 1.f / h);
			});
			// bank 2: two-operator FM family (index grows per table)
			for (int k = 0; k < 5; k++) {
				buildBankTable(2, k, mip, [&](std::vector<float>& t, int maxH) {
					float idx = (mip == 0 ? 0.4f + k * 1.1f : 0.2f + k * 0.35f);
					int ratio = (k < 3) ? 2 : 3;
					for (int i = 0; i < TABLE_LEN; i++) {
						double p = (double)i / TABLE_LEN;
						t[i] = std::sin(2.0 * M_PI * p + idx * std::sin(2.0 * M_PI * ratio * p));
					}
				});
			}
			// bank 3: waveshaped/distorted sine family
			for (int k = 0; k < 5; k++) {
				buildBankTable(3, k, mip, [&](std::vector<float>& t, int maxH) {
					float drive = 1.f + k * (mip == 0 ? 2.6f : 0.9f);
					for (int i = 0; i < TABLE_LEN; i++) {
						double p = (double)i / TABLE_LEN;
						float s = std::sin(2.0 * M_PI * p);
						t[i] = std::tanh(s * drive) + 0.15f * std::sin(2.0 * M_PI * 3 * p) * (k / 4.f);
					}
				});
			}
			// bank 4: vocal formant family (vowel-ish spectra)
			static const float FORMANTS[5][2] = {{700, 1100}, {500, 1900}, {320, 2300}, {450, 850}, {350, 750}};
			for (int k = 0; k < 5; k++) {
				buildBankTable(4, k, mip, [&](std::vector<float>& t, int maxH) {
					float f0 = 110.f;
					for (int h = 1; h <= maxH; h++) {
						float fh = h * f0;
						float a = 0.f;
						for (int fm = 0; fm < 2; fm++) {
							float d = (fh - FORMANTS[k][fm]) / (FORMANTS[k][fm] * 0.25f);
							a += std::exp(-d * d);
						}
						addHarmonic(t, h, (a + 0.02f) / std::sqrt((float)h));
					}
				});
			}
			// bank 6: chip family (narrow pulses, stepped triangle)
			static const float DUTIES[4] = {0.5f, 0.25f, 0.125f, 0.0625f};
			for (int k = 0; k < 4; k++) {
				buildBankTable(6, k, mip, [&](std::vector<float>& t, int maxH) {
					for (int h = 1; h <= maxH; h++)
						addHarmonic(t, h, (2.f / (h * M_PI)) * std::sin(h * M_PI * DUTIES[k]));
				});
			}
			// bank 7: organ drawbar registrations
			static const int PARTIALS[8] = {1, 2, 3, 4, 6, 8, 10, 12};
			static const float REGS[4][8] = {
				{1, 0.5f, 0, 0.2f, 0, 0, 0, 0},
				{1, 0.8f, 0.5f, 0.4f, 0.2f, 0.1f, 0, 0},
				{0.8f, 1, 0.3f, 0.7f, 0.4f, 0.5f, 0.2f, 0.1f},
				{1, 0.3f, 0.8f, 0.2f, 0.7f, 0.2f, 0.5f, 0.4f},
			};
			for (int k = 0; k < 4; k++) {
				buildBankTable(7, k, mip, [&](std::vector<float>& t, int maxH) {
					for (int p = 0; p < 8; p++)
						if (REGS[k][p] > 0.f && PARTIALS[p] <= maxH)
							addHarmonic(t, PARTIALS[p], REGS[k][p]);
				});
			}
		}
		// bank 5 (PWM pulse) is rendered live from the bank-1 saw table
	}

	float readTable(const std::vector<float>& t, double phase) {
		double p = phase * TABLE_LEN;
		int i0 = (int)p % TABLE_LEN;
		float fr = (float)(p - std::floor(p));
		int i1 = (i0 + 1) % TABLE_LEN;
		return t[i0] + (t[i1] - t[i0]) * fr;
	}

	float renderOsc(int v, int bank, float wave, float freq, float sr) {
		Osc& o = osc[v];
		int mip = (freq * 64.f > sr * 0.4f) ? 1 : 0;
		float out;
		if (bank == 1) {
			// saw through a one-pole lowpass; wave sets the cutoff
			float s = readTable(tables[1][0][mip], o.phase);
			float fc = 80.f * std::pow(150.f, wave);
			float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
			o.lpState += a * (s - o.lpState);
			out = o.lpState * (1.f + (1.f - wave) * 1.5f);
		}
		else if (bank == 5) {
			// pulse with PWM from two saws
			float w = 0.5f - wave * 0.45f;
			float s1 = readTable(tables[1][0][mip], o.phase);
			float s2 = readTable(tables[1][0][mip], std::fmod(o.phase + w, 1.0));
			out = (s1 - s2) * 0.8f;
		}
		else {
			int n = tablesPerBank[bank];
			float pos = wave * (n - 1);
			int t0 = clamp((int)pos, 0, n - 1);
			int t1 = std::min(t0 + 1, n - 1);
			float fr = pos - t0;
			float a = readTable(tables[bank][t0][mip], o.phase);
			float b = readTable(tables[bank][t1][mip], o.phase);
			out = a + (b - a) * fr;
		}
		return out;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;

		// ---- buttons ----
		if (modeBtn.process(params[MODE_PARAM].getValue() > 0.f))
			mode = (mode + 1) % 4;
		if (harmBtn.process(params[HARM_PARAM].getValue() > 0.f))
			harm = (harm + 1) % ((mode >= 2) ? 4 : 3);
		if (triadBtn.process(params[TRIAD_PARAM].getValue() > 0.f))
			triad = !triad;

		// ---- shared controls ----
		int bank = clamp((int)std::round(params[BANK_PARAM].getValue() + inputs[BANK_INPUT].getVoltage() * 1.4f), 0, 7);
		float wave = clamp(params[WAVE_PARAM].getValue() + inputs[WAVE_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float baseOct = params[FREQ_PARAM].getValue() + params[FINE_PARAM].getValue() / 12.f;
		float fm = inputs[FM_INPUT].getVoltage() * params[FM_ATT_PARAM].getValue() * 30.f;

		// ---- pitch logic per mode ----
		float semis[4]; // root, third, fifth, seventh (semitones rel. C4)
		float vfac[4] = {1, 1, 1, 1};
		int qIdx = clamp((int)std::round(params[QUALITY_PARAM].getValue() + inputs[QUALITY_INPUT].getVoltage() * 0.8f), 0, 7);

		auto quantizeScale = [&](float st, const int* scale) {
			int oct = (int)std::floor(st / 12.f);
			float in = st - oct * 12.f;
			int best = scale[0], bestD = 127, deg = 0;
			for (int i = 0; i < 7; i++) {
				int d = (int)std::round(std::fabs(in - scale[i]));
				if (d < bestD) {
					bestD = d;
					best = scale[i];
					deg = i;
				}
			}
			curDegree = deg;
			return (float)(oct * 12 + best);
		};

		if (mode == 2) {
			// free poly: four independent oscillators on four CVs
			semis[0] = inputs[VOCT_INPUT].getVoltage() * 12.f;
			semis[1] = inputs[LEAD_INPUT].getVoltage() * 12.f;
			semis[2] = inputs[VOICING_INPUT].getVoltage() * 12.f;
			semis[3] = inputs[QUALITY_INPUT].getVoltage() * 12.f;
		}
		else if (mode == 3) {
			// unison poly: all track the root, offset per CV jack
			float r = inputs[VOCT_INPUT].getVoltage() * 12.f;
			semis[0] = r;
			semis[1] = r + inputs[LEAD_INPUT].getVoltage() * 12.f;
			semis[2] = r + inputs[VOICING_INPUT].getVoltage() * 12.f;
			semis[3] = r + inputs[QUALITY_INPUT].getVoltage() * 12.f;
		}
		else {
			float rootSt = inputs[VOCT_INPUT].getVoltage() * 12.f;
			if (harm == 1) {
				rootSt = quantizeScale(rootSt, MAJOR_SCALE);
				qIdx = MAJOR_DEGREE_QUALITY[curDegree];
			}
			else if (harm == 2) {
				rootSt = quantizeScale(rootSt, MINOR_SCALE);
				qIdx = MINOR_DEGREE_QUALITY[curDegree];
			}
			int vIdx = clamp((int)std::round(params[VOICING_PARAM].getValue() + inputs[VOICING_INPUT].getVoltage() * 1.6f), 0, NUM_VOICINGS - 1);
			for (int k = 0; k < 4; k++)
				vfac[k] = VOICING_TABLE[vIdx][k];
			semis[0] = rootSt;
			semis[1] = rootSt + QUALITY_TABLE[qIdx][0];
			semis[2] = rootSt + QUALITY_TABLE[qIdx][1];
			semis[3] = rootSt + QUALITY_TABLE[qIdx][2];
			if (mode == 1) {
				// melody: the seventh becomes an independent lead voice
				float leadSt = inputs[LEAD_INPUT].getVoltage() * 12.f;
				if (harm == 1)
					leadSt = quantizeScale(leadSt, MAJOR_SCALE);
				else if (harm == 2)
					leadSt = quantizeScale(leadSt, MINOR_SCALE);
				semis[3] = leadSt;
			}
		}
		// poly-mode quantization
		if (mode >= 2 && harm > 0 && harm < 3) {
			const int* sc = (harm == 1) ? MAJOR_SCALE : MINOR_SCALE;
			for (int k = 0; k < 4; k++)
				semis[k] = quantizeScale(semis[k], sc);
		}
		else if (mode >= 2 && harm == 3) {
			for (int k = 0; k < 4; k++)
				semis[k] = std::round(semis[k]);
		}
		curQuality = qIdx;

		// ---- render the four voices ----
		float base = dsp::FREQ_C4 * std::exp2(baseOct);
		float outs[4];
		float k = 1.f - std::exp(-args.sampleTime / 0.004f);
		for (int v = 0; v < 4; v++) {
			float f = base * std::exp2(semis[v] / 12.f) * vfac[v];
			osc[v].freq = clamp(f, 8.f, sr * 0.45f);
			osc[v].freqSmooth += (osc[v].freq - osc[v].freqSmooth) * k;
			float fEff = osc[v].freqSmooth + fm;
			osc[v].phase += clamp(fEff, 0.f, sr * 0.49f) * args.sampleTime;
			if (osc[v].phase >= 1.0)
				osc[v].phase -= std::floor(osc[v].phase);
			outs[v] = renderOsc(v, bank, wave, osc[v].freqSmooth, sr);
		}

		outputs[ROOT_OUTPUT].setVoltage(outs[0] * 5.f);
		outputs[THIRD_OUTPUT].setVoltage(outs[1] * 5.f);
		outputs[FIFTH_OUTPUT].setVoltage(outs[2] * 5.f);
		outputs[SEVENTH_OUTPUT].setVoltage(outs[3] * 5.f);
		float mix;
		if (triad)
			mix = (outs[0] + outs[1] + outs[2]) * 0.42f;
		else
			mix = (outs[0] + outs[1] + outs[2] + outs[3]) * 0.32f;
		outputs[MIX_OUTPUT].setVoltage(std::tanh(mix * 1.2f) * 5.f);

		// ---- lights ----
		if (lightDivider.process()) {
			// quality LED family: maj min dom dim sus aug extra
			static const int qLed[8] = {0, 1, 2, 3, 3, 4, 4, 5};
			for (int i = 0; i < 7; i++)
				lights[QLED_LIGHT + i].setBrightness(0.f);
			lights[QLED_LIGHT + qLed[curQuality]].setBrightness(1.f);
			if (curQuality == 3 || curQuality == 6)
				lights[QLED_LIGHT + 6].setBrightness(0.5f); // variant marker
			static const float modeColors[4][3] = {
				{0.f, 0.f, 0.f}, {0.f, 0.2f, 1.f}, {0.f, 1.f, 0.2f}, {0.f, 0.8f, 0.8f}};
			static const float harmColors[4][3] = {
				{0.f, 0.f, 0.f}, {0.f, 0.2f, 1.f}, {0.f, 1.f, 0.2f}, {0.f, 0.8f, 0.8f}};
			for (int c = 0; c < 3; c++) {
				lights[MODE_LIGHT + c].setBrightness(modeColors[mode][c]);
				lights[HARM_LIGHT + c].setBrightness(harmColors[harm][c]);
			}
			lights[TRIAD_LIGHT].setBrightness(triad ? 1.f : 0.f);
		}
	}

	int curDegree = 0;

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "mode", json_integer(mode));
		json_object_set_new(root, "harm", json_integer(harm));
		json_object_set_new(root, "triad", json_boolean(triad));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "mode"))) mode = json_integer_value(j);
		if ((j = json_object_get(root, "harm"))) harm = json_integer_value(j);
		if ((j = json_object_get(root, "triad"))) triad = json_boolean_value(j);
	}
};

struct CapoWidget : ModuleWidget {
	CapoWidget(Capo* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Capo.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace pl;

		for (int i = 0; i < 7; i++)
			addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(QLED_X0 + i * QLED_DX, QLED_Y)), module, Capo::QLED_LIGHT + i));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(BANK_X, BANK_Y)), module, Capo::BANK_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(WAVE_X, WAVE_Y)), module, Capo::WAVE_PARAM));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(FREQ_X, FREQ_Y)), module, Capo::FREQ_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(FINE_X, FINE_Y)), module, Capo::FINE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(QUAL_X, QUAL_Y)), module, Capo::QUALITY_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X, MODE_Y)), module, Capo::MODE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(BTN_X + BTN_LED_DX + 2.f, MODE_Y)), module, Capo::MODE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X, HARM_Y)), module, Capo::HARM_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(BTN_X + BTN_LED_DX + 2.f, HARM_Y)), module, Capo::HARM_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X, TRIAD_Y)), module, Capo::TRIAD_PARAM));
		addChild(createLightCentered<MediumLight<WhiteLight>>(mm2px(Vec(BTN_X + BTN_LED_DX + 2.f, TRIAD_Y)), module, Capo::TRIAD_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(FM_JACK_X, FM_ROW_Y)), module, Capo::FM_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(FM_ATT_X, FM_ROW_Y)), module, Capo::FM_ATT_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(VOICING_X, VOICING_Y)), module, Capo::VOICING_PARAM));

		static const int cvIns[6] = {Capo::BANK_INPUT, Capo::WAVE_INPUT, Capo::VOICING_INPUT,
		                             Capo::QUALITY_INPUT, Capo::LEAD_INPUT, Capo::VOCT_INPUT};
		for (int i = 0; i < 6; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CV_X[i], CV_Y)), module, cvIns[i]));

		static const int outs[5] = {Capo::ROOT_OUTPUT, Capo::THIRD_OUTPUT, Capo::FIFTH_OUTPUT,
		                            Capo::SEVENTH_OUTPUT, Capo::MIX_OUTPUT};
		for (int i = 0; i < 5; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_X[i], OUT_Y)), module, outs[i]));
	}

	void appendContextMenu(Menu* menu) override {
		Capo* module = getModule<Capo>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Mode",
			{"Chord", "Melody (lead = seventh)", "Free poly", "Unison poly"}, &module->mode));
		menu->addChild(createIndexPtrSubmenuItem("Auto-harmonize",
			{"Off", "Major", "Minor", "Chromatic (poly)"}, &module->harm));
		menu->addChild(createBoolPtrMenuItem("Triad mix (omit seventh)", "", &module->triad));
	}
};

Model* modelCapo = createModel<Capo, CapoWidget>("Capo");
