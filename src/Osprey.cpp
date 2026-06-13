// Osprey — a triple-personality stereo oscillator for VCV Rack, inspired by
// the BlaknBlu Oscar Tria Mk II. Original implementation.
//
// Green mode:  stereo 'traditional' pair + sub + sub-sub, TZFM, hard sync,
//              wave folder.
// Yellow mode: 24-oscillator swarm (12 per channel) + square sub.
// Orange mode: 20-chord engine, every note a stereo oscillator pair,
//              spread over 2-5 octaves.

#include "plugin.hpp"
#include "layout_osprey.hpp"

// Pitch and Detune knobs: centre detent with a dead zone, then a square-law
// curve so tuning moves slowly near centre but reaches the full range.
static inline float detentCurve(float v) {
	float a = std::fabs(v);
	a = (a < 0.07f) ? 0.f : (a - 0.07f) / 0.93f;
	return std::copysign(a * a, v);
}

static inline float polyblep(float t, float dt) {
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.f;
	}
	if (t > 1.f - dt) {
		t = (t - 1.f) / dt;
		return t * t + t + t + 1.f;
	}
	return 0.f;
}

// Morphing wave: 0 = saw, 0.5 = triangle, 1 = square (with pulse width).
static inline float morphWave(float ph, float dt, float wave, float pw) {
	float saw = 0.f, tri = 0.f, sq = 0.f;
	if (wave < 0.5f) {
		saw = 2.f * ph - 1.f - polyblep(ph, dt);
		tri = 4.f * std::fabs(ph - 0.5f) - 1.f;
		return crossfade(saw, tri, wave * 2.f);
	}
	tri = 4.f * std::fabs(ph - 0.5f) - 1.f;
	sq = (ph < pw ? 1.f : -1.f) + polyblep(ph, dt);
	float t2 = ph - pw;
	if (t2 < 0.f)
		t2 += 1.f;
	sq -= polyblep(t2, dt);
	return crossfade(tri, sq, (wave - 0.5f) * 2.f);
}

// Triangle-reflection wave folder; identity for |x| <= 1 at zero fold.
static inline float foldWave(float x, float amount) {
	x *= 1.f + amount * 6.f;
	x = std::fmod(x + 3.f, 4.f);
	if (x < 0.f)
		x += 4.f;
	return std::fabs(x - 2.f) - 1.f;
}

// 20 chords (semitone intervals), straight from the Oscar Tria2 appendix.
static const int CHORDS[20][5] = {
	{0, 4, 7, -1, -1},   // 1  Major triad
	{0, 3, 7, -1, -1},   // 2  Minor triad
	{0, 3, 6, -1, -1},   // 3  Diminished
	{0, 4, 8, -1, -1},   // 4  Augmented
	{0, 4, 7, 11, -1},   // 5  Major 7
	{0, 3, 7, 10, -1},   // 6  Minor 7
	{0, 4, 7, 10, -1},   // 7  Dominant 7
	{0, 3, 6, 10, -1},   // 8  Half diminished 7
	{0, 3, 6, 9, -1},    // 9  Diminished 7
	{0, 4, 7, 9, -1},    // 10 Major 6
	{0, 3, 7, 9, -1},    // 11 Minor 6
	{0, 2, 7, -1, -1},   // 12 Suspended 2
	{0, 5, 7, -1, -1},   // 13 Suspended 4
	{0, 4, 7, 11, 14},   // 14 Major 9
	{0, 3, 7, 10, 14},   // 15 Minor 9
	{0, 4, 7, 10, 14},   // 16 Dominant 9
	{0, 4, 7, 14, -1},   // 17 Add 9
	{0, 3, 7, 14, -1},   // 18 Minor add 9
	{0, 4, 8, 10, -1},   // 19 Augmented 7
	{0, 2, 7, 9, -1},    // 20 Suspended 2 add 6
};
static const char* CHORD_NAMES[20] = {
	"Major", "Minor", "Diminished", "Augmented", "Major 7", "Minor 7",
	"Dominant 7", "Half dim 7", "Diminished 7", "Major 6", "Minor 6",
	"Sus 2", "Sus 4", "Major 9", "Minor 9", "Dominant 9", "Add 9",
	"Minor add 9", "Augmented 7", "Sus 2 add 6"};

struct Osprey : Module {
	enum ParamId {
		WAVE_PARAM,
		PITCH_PARAM,
		PW_PARAM,
		FOLD_PARAM,
		MODE_PARAM,
		PAGE_PARAM,
		OCT_PARAM,
		MONO_PARAM,
		GFM_PARAM,
		GWAV_PARAM,
		GA1_PARAM,
		GA2_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		FM_INPUT,
		WAVE_INPUT,
		PWM_INPUT,
		AUX1_INPUT,
		AUX2_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTL_OUTPUT,
		OUTR_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		PAGE_LIGHT,
		LIGHTS_LEN
	};

	// Per-mode, per-page stored values for the three paged knobs (the
	// hardware remembers settings from mode to mode), plus the fourth
	// FOLD/DETN/SPRD knob which is single-page but per-mode.
	float kv[3][2][3];
	float kv4[3];
	int knobContext = -1;

	float ph[26] = {};  // 0-23 mains/swarm, 24 sub, 25 sub-sub
	dsp::SchmittTrigger syncTrig;
	dsp::ClockDivider lightDivider;

	// Static swarm detune offsets: cubed for density near the centre,
	// interleaved L/R like the hardware's 12 + 12 split.
	float swarmOff[24];

	Osprey() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(WAVE_PARAM, 0.f, 1.f, 0.f, "Waveform (saw-tri-square)");
		configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch (±1 octave, centre detent)");
		configParam(PW_PARAM, 0.f, 1.f, 0.5f, "Pulse width");
		configParam(FOLD_PARAM, 0.f, 1.f, 0.f, "Fold / Detune spread / Octave span");
		configSwitch(MODE_PARAM, 0.f, 2.f, 0.f, "Mode", {"Green (stereo VCO)", "Yellow (swarm)", "Orange (chords)"});
		configSwitch(PAGE_PARAM, 0.f, 1.f, 0.f, "Page", {"1", "2"});
		configSwitch(OCT_PARAM, 0.f, 2.f, 1.f, "Octave", {"-1", "0", "+1"});
		configSwitch(MONO_PARAM, 0.f, 1.f, 0.f, "Output", {"Stereo", "Mono"});
		configParam(GFM_PARAM, 0.f, 1.f, 0.f, "FM gain");
		configParam(GWAV_PARAM, 0.f, 1.f, 0.f, "Wave CV gain");
		configParam(GA1_PARAM, 0.f, 1.f, 0.f, "Aux 1 gain");
		configParam(GA2_PARAM, 0.f, 1.f, 0.f, "Aux 2 gain");

		configInput(VOCT_INPUT, "V/oct pitch");
		configInput(FM_INPUT, "Linear through-zero FM");
		configInput(WAVE_INPUT, "Wave CV");
		configInput(PWM_INPUT, "Pulse width CV");
		configInput(AUX1_INPUT, "Aux 1 (green: hard sync, yellow: spread CV, orange: chord CV)");
		configInput(AUX2_INPUT, "Aux 2 (green: fold CV, yellow: pan CV, orange: notes CV)");
		configOutput(OUTL_OUTPUT, "Left");
		configOutput(OUTR_OUTPUT, "Right");

		for (int m = 0; m < 3; m++) {
			kv[m][0][0] = 0.f;   // wave
			kv[m][0][1] = 0.5f;  // pitch
			kv[m][0][2] = 0.5f;  // pw
		}
		// page 2 defaults: green sub/detune/sub2, yellow oscs/pan/sub, orange chord/detune/notes
		kv[0][1][0] = 0.f;  kv[0][1][1] = 0.5f; kv[0][1][2] = 0.f;
		kv[1][1][0] = 0.3f; kv[1][1][1] = 0.5f; kv[1][1][2] = 0.f;
		kv[2][1][0] = 0.f;  kv[2][1][1] = 0.5f; kv[2][1][2] = 1.f;
		kv4[0] = 0.f;
		kv4[1] = 0.25f;
		kv4[2] = 0.f;

		for (int i = 0; i < 24; i++) {
			// pair index 0..11 spread symmetrically, cubed for density near
			// the centre (supersaw-style), L/R interleaved and the R side
			// mirrored slightly off so the channels never beat identically
			float c = (i / 2) / 11.f * 2.f - 1.f;
			swarmOff[i] = c * c * c;
			if (i & 1)
				swarmOff[i] = -swarmOff[i] * 0.93f;
			ph[i] = random::uniform();
		}
		lightDivider.setDivision(64);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_t* kvJ = json_array();
		for (int m = 0; m < 3; m++)
			for (int p = 0; p < 2; p++)
				for (int k = 0; k < 3; k++)
					json_array_append_new(kvJ, json_real(kv[m][p][k]));
		json_object_set_new(rootJ, "kv", kvJ);
		json_t* kv4J = json_array();
		for (int m = 0; m < 3; m++)
			json_array_append_new(kv4J, json_real(kv4[m]));
		json_object_set_new(rootJ, "kv4", kv4J);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* kvJ = json_object_get(rootJ, "kv");
		if (kvJ)
			for (int m = 0; m < 3; m++)
				for (int p = 0; p < 2; p++)
					for (int k = 0; k < 3; k++)
						kv[m][p][k] = json_real_value(json_array_get(kvJ, (m * 2 + p) * 3 + k));
		json_t* kv4J = json_object_get(rootJ, "kv4");
		if (kv4J)
			for (int m = 0; m < 3; m++)
				kv4[m] = json_real_value(json_array_get(kv4J, m));
		knobContext = -1;
	}

	void process(const ProcessArgs& args) override {
		int mode = (int)params[MODE_PARAM].getValue();
		int page = (int)params[PAGE_PARAM].getValue();

		// Write-through paged knobs: snap to the stored layer on context
		// change, then track the knob (no pickup dead feel).
		static const int knobIds[3] = {WAVE_PARAM, PITCH_PARAM, PW_PARAM};
		int ctx = mode * 2 + page;
		if (ctx != knobContext) {
			for (int k = 0; k < 3; k++)
				params[knobIds[k]].setValue(kv[mode][page][k]);
			params[FOLD_PARAM].setValue(kv4[mode]);
			knobContext = ctx;
		}
		else {
			for (int k = 0; k < 3; k++)
				kv[mode][page][k] = params[knobIds[k]].getValue();
			kv4[mode] = params[FOLD_PARAM].getValue();
		}

		// page-1 values always drive the sound, whatever page is showing
		float wave = clamp(kv[mode][0][0] + inputs[WAVE_INPUT].getVoltage() / 10.f * params[GWAV_PARAM].getValue(), 0.f, 1.f);
		float pitchK = detentCurve((kv[mode][0][1] - 0.5f) * 2.f) * 12.f;
		float pw = clamp(kv[mode][0][2] + inputs[PWM_INPUT].getVoltage() / 10.f, 0.05f, 0.95f);

		float oct = params[OCT_PARAM].getValue() - 1.f;
		float pitch = inputs[VOCT_INPUT].getVoltage() + oct + pitchK / 12.f;
		float f0 = dsp::FREQ_C4 * dsp::exp2_taylor5(pitch);
		// linear through-zero FM
		float fm = inputs[FM_INPUT].getVoltage() * params[GFM_PARAM].getValue();
		float f = f0 * (1.f + fm * 0.4f);

		bool mono = params[MONO_PARAM].getValue() > 0.5f;
		float outL = 0.f, outR = 0.f;
		float sr = args.sampleRate;

		auto step = [&](int i, float freq) -> float {
			float dt = freq / sr;
			ph[i] += dt;
			ph[i] -= std::floor(ph[i]);
			return std::fabs(dt);
		};

		if (mode == 0) {
			// ---- GREEN: stereo pair + sub + sub-sub, sync, TZFM, folder ----
			float subLvl = kv[0][1][0];
			float det = detentCurve((kv[0][1][1] - 0.5f) * 2.f) * 12.f; // ±1 octave, like Pitch
			float sub2Lvl = kv[0][1][2];
			float foldAmt = clamp(kv4[0] + inputs[AUX2_INPUT].getVoltage() / 5.f * params[GA2_PARAM].getValue(), 0.f, 1.f);

			// hard sync on AUX1 rising edges
			if (syncTrig.process(inputs[AUX1_INPUT].getVoltage() * params[GA1_PARAM].getValue(), 0.1f, 1.f))
				for (int i = 0; i < 26; i++)
					ph[i] = 0.f;

			float fR = f * dsp::exp2_taylor5(det / 12.f);
			float dtL = step(0, f);
			float dtR = step(1, fR);
			float mainL = morphWave(ph[0], dtL, wave, pw);
			float mainR = morphWave(ph[1], dtR, wave, pw);
			if (foldAmt > 0.003f) {
				mainL = foldWave(mainL, foldAmt);
				mainR = foldWave(mainR, foldAmt);
			}
			// subs are octave divisions of the core, so they follow FM too
			float dtS = step(24, f * 0.5f);
			float dtS2 = step(25, f * 0.25f);
			float sub = morphWave(ph[24], dtS, 1.f, 0.5f) * subLvl;
			float sub2 = morphWave(ph[25], dtS2, 1.f, 0.5f) * sub2Lvl;
			float norm = 1.f / (1.f + 0.7f * (subLvl + sub2Lvl));
			outL = (mainL + sub + sub2) * norm;
			outR = (mainR + sub + sub2) * norm;
		}
		else if (mode == 1) {
			// ---- YELLOW: the swarm ----
			int n = 1 + (int)(kv[1][1][0] * 23.999f); // 1..24 oscillators
			float subLvl = kv[1][1][2];
			float pan = clamp((kv[1][1][1] - 0.5f) * 2.f + inputs[AUX2_INPUT].getVoltage() / 5.f * params[GA2_PARAM].getValue(), -1.f, 1.f);
			float spread = clamp(kv4[1] + inputs[AUX1_INPUT].getVoltage() / 5.f * params[GA1_PARAM].getValue(), 0.f, 1.f);
			float cents = 130.f * spread * std::sqrt(spread);

			float sumL = 0.f, sumR = 0.f;
			int nL = 0, nR = 0;
			for (int i = 0; i < n; i++) {
				float fi = f * dsp::exp2_taylor5(swarmOff[i] * cents / 1200.f);
				float dt = step(i, fi);
				float v = morphWave(ph[i], dt, wave, pw);
				if (i & 1) { sumR += v; nR++; }
				else { sumL += v; nL++; }
			}
			if (nR == 0) { sumR = sumL; nR = nL; } // single oscillator: both sides
			sumL /= std::sqrt((float)std::max(nL, 1));
			sumR /= std::sqrt((float)std::max(nR, 1));
			float dtS = step(24, f * 0.5f);
			float sub = morphWave(ph[24], dtS, 1.f, 0.5f) * subLvl;
			float norm = 0.75f / (1.f + 0.7f * subLvl);
			outL = (sumL + sub) * norm;
			outR = (sumR + sub) * norm;
			// equal-power pan, unity at centre; mono sums everything to both
			// outputs so panning must not attenuate it
			if (!mono) {
				float a = (pan + 1.f) * 0.25f * M_PI;
				outL *= std::cos(a) * 1.414f;
				outR *= std::sin(a) * 1.414f;
			}
		}
		else {
			// ---- ORANGE: chord engine ----
			float chordK = clamp(kv[2][1][0] + inputs[AUX1_INPUT].getVoltage() / 5.f * params[GA1_PARAM].getValue(), 0.f, 1.f);
			int chord = (int)(chordK * 19.999f);
			float det = detentCurve((kv[2][1][1] - 0.5f) * 2.f) * 12.f;
			int span = 2 + (int)(kv4[2] * 3.999f); // 2..5 octaves
			// build the note list: chord intervals stacked into octaves, capped by span
			int notes[12];
			int nInt = 0;
			while (nInt < 5 && CHORDS[chord][nInt] >= 0)
				nInt++;
			int total = 0;
			for (int j = 0; total < 12; j++) {
				int semis = CHORDS[chord][j % nInt] + 12 * (j / nInt);
				if (semis > span * 12)
					break;
				notes[total++] = semis;
			}
			float countK = clamp(kv[2][1][2] + inputs[AUX2_INPUT].getVoltage() / 5.f * params[GA2_PARAM].getValue(), 0.f, 1.f);
			int count = 1 + (int)(countK * (total - 0.001f));
			count = clamp(count, 1, total);

			float sumL = 0.f, sumR = 0.f;
			for (int k = 0; k < count; k++) {
				float fn = f * dsp::exp2_taylor5(notes[k] / 12.f);
				float fnR = fn * dsp::exp2_taylor5(det / 12.f);
				float dtL = step(k * 2, fn);
				float dtR = step(k * 2 + 1, fnR);
				sumL += morphWave(ph[k * 2], dtL, wave, pw);
				sumR += morphWave(ph[k * 2 + 1], dtR, wave, pw);
			}
			float norm = 1.f / std::sqrt((float)count);
			outL = sumL * norm;
			outR = sumR * norm;
		}

		if (mono) {
			float m = (outL + outR) * 0.5f;
			outL = outR = m;
		}
		outputs[OUTL_OUTPUT].setVoltage(clamp(outL * 5.f, -12.f, 12.f));
		outputs[OUTR_OUTPUT].setVoltage(clamp(outR * 5.f, -12.f, 12.f));

		if (lightDivider.process())
			lights[PAGE_LIGHT].setBrightness(page ? 1.f : 0.f);
	}
};

struct OspreyWidget : ModuleWidget {
	OspreyWidget(Osprey* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Osprey.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace oslayout;
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(MODE_SW_X, SW_Y)), module, Osprey::MODE_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(OCT_SW_X, SW_Y)), module, Osprey::OCT_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(MONO_SW_X, SW_Y)), module, Osprey::MONO_PARAM));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX1, KY1)), module, Osprey::WAVE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX2, KY1)), module, Osprey::PITCH_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX1, KY2)), module, Osprey::PW_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX2, KY2)), module, Osprey::FOLD_PARAM));

		addParam(createParamCentered<CKSS>(mm2px(Vec(PG_SW_X, PG_SW_Y)), module, Osprey::PAGE_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(PG_LED_X, PG_LED_Y)), module, Osprey::PAGE_LIGHT));

		static const int trims[4] = {Osprey::GFM_PARAM, Osprey::GWAV_PARAM, Osprey::GA1_PARAM, Osprey::GA2_PARAM};
		for (int i = 0; i < 4; i++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(TRIM_X[i], TRIM_Y)), module, trims[i]));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[0], R1)), module, Osprey::FM_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[1], R1)), module, Osprey::WAVE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[2], R1)), module, Osprey::AUX1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[3], R1)), module, Osprey::AUX2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[0], R2)), module, Osprey::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[1], R2)), module, Osprey::PWM_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J_X[2], R2)), module, Osprey::OUTL_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J_X[3], R2)), module, Osprey::OUTR_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Osprey* module = getModule<Osprey>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Page 2 knob functions"));
		menu->addChild(createMenuLabel("Green: sub level / detune / sub-sub level"));
		menu->addChild(createMenuLabel("Yellow: swarm size / pan / sub level"));
		menu->addChild(createMenuLabel("Orange: chord / detune / note count"));
		menu->addChild(createMenuLabel(string::f("Current chord: %s",
			CHORD_NAMES[(int)(clamp(module->kv[2][1][0], 0.f, 1.f) * 19.999f)])));
	}
};

Model* modelOsprey = createModel<Osprey, OspreyWidget>("Osprey");
