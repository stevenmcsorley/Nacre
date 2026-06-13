// Fretwork — a polyphonic performance quantizer for VCV Rack. Original
// design: 14 scale presets plus a 12-key editable note mask, CV over scale
// and root, diatonic transpose CV (shifts by scale degrees, staying in
// key), clocked sample-and-hold quantizing, hysteresis to prevent note
// flutter, octave shift, and a gate output that fires on note changes.

#include "plugin.hpp"
#include "layout_fretwork.hpp"

static const int NUM_SCALES = 14;
static const char* SCALE_NAMES[NUM_SCALES] = {
	"Chromatic", "Major", "Natural minor", "Harmonic minor", "Melodic minor",
	"Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
	"Major pentatonic", "Minor pentatonic", "Blues", "Whole tone"
};
static const uint16_t SCALE_MASKS[NUM_SCALES] = {
	0b111111111111, // chromatic
	0b101010110101, // major          (C D E F G A B)
	0b010110101101, // natural minor
	0b100110101101, // harmonic minor
	0b101010101101, // melodic minor
	0b011010101101, // dorian
	0b010110101011, // phrygian
	0b101011010101, // lydian
	0b011010110101, // mixolydian
	0b010101101011, // locrian
	0b001010010101, // major pentatonic
	0b010010101001, // minor pentatonic
	0b010011101001, // blues
	0b010101010101, // whole tone
};

struct Fretwork : Module {
	enum ParamId {
		ENUMS(KEY_PARAM, 12),
		SCALE_PARAM,
		ROOT_PARAM,
		OCT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		SCALE_INPUT,
		ROOT_INPUT,
		TRANS_INPUT,
		TRIG_INPUT,
		IN_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		GATE_OUTPUT,
		OUT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(KEY_LIGHT, 12),
		LIGHTS_LEN
	};

	uint16_t mask = SCALE_MASKS[1]; // active pitch classes (relative to root)
	int lastScaleIdx = 1;
	int roundMode = 0; // 0 nearest, 1 up, 2 down

	// per-channel state
	float lastSemis[16] = {};
	float heldOut[16] = {};
	dsp::PulseGenerator changePulse[16];
	dsp::SchmittTrigger trig[16];
	dsp::BooleanTrigger keyBtn[12];
	dsp::ClockDivider lightDivider;
	uint16_t activeNotes = 0; // for the display

	Fretwork() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		static const char* keyNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		for (int i = 0; i < 12; i++)
			configButton(KEY_PARAM + i, string::f("Note %s", keyNames[i]));
		configParam(SCALE_PARAM, 0.f, NUM_SCALES - 1, 1.f, "Scale");
		paramQuantities[SCALE_PARAM]->snapEnabled = true;
		configParam(ROOT_PARAM, 0.f, 11.f, 0.f, "Root", " st");
		paramQuantities[ROOT_PARAM]->snapEnabled = true;
		configParam(OCT_PARAM, -4.f, 4.f, 0.f, "Octave shift", " oct");
		paramQuantities[OCT_PARAM]->snapEnabled = true;
		configInput(SCALE_INPUT, "Scale CV (1 V per scale)");
		configInput(ROOT_INPUT, "Root CV (1 V/oct, semitones)");
		configInput(TRANS_INPUT, "Diatonic transpose CV (1 V per scale degree)");
		configInput(TRIG_INPUT, "Trigger (sample & hold quantize)");
		configInput(IN_INPUT, "1V/oct (polyphonic)");
		configOutput(GATE_OUTPUT, "Note-change trigger (polyphonic)");
		configOutput(OUT_OUTPUT, "Quantized 1V/oct (polyphonic)");
		configBypass(IN_INPUT, OUT_OUTPUT);
		lightDivider.setDivision(64);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "mask", json_integer(mask));
		json_object_set_new(root, "roundMode", json_integer(roundMode));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "mask"))) mask = (uint16_t)json_integer_value(j);
		if ((j = json_object_get(root, "roundMode"))) roundMode = json_integer_value(j);
		lastScaleIdx = -1; // don't clobber the loaded mask
	}

	// nearest enabled semitone to s (absolute semitones), honoring root + round mode
	float quantize(float s, int root) {
		if (mask == 0)
			return s; // nothing enabled: pass through
		float best = s;
		float bestD = 1e9f;
		int sFloor = (int)std::floor(s);
		for (int k = sFloor - 13; k <= sFloor + 13; k++) {
			int pc = ((k - root) % 12 + 12) % 12;
			if (!(mask >> pc & 1))
				continue;
			if (roundMode == 1 && k < s - 0.001f)
				continue;
			if (roundMode == 2 && k > s + 0.001f)
				continue;
			float d = std::fabs(k - s);
			if (d < bestD) {
				bestD = d;
				best = (float)k;
			}
		}
		return best;
	}

	// shift a quantized note by `deg` scale degrees within the mask
	float degreeShift(float semis, int root, int deg) {
		if (deg == 0 || mask == 0)
			return semis;
		int n = 0;
		for (int i = 0; i < 12; i++)
			if (mask >> i & 1)
				n++;
		if (n == 0)
			return semis;
		int k = (int)std::round(semis);
		int step = (deg > 0) ? 1 : -1;
		int remaining = std::abs(deg);
		while (remaining > 0) {
			k += step;
			int pc = ((k - root) % 12 + 12) % 12;
			if (mask >> pc & 1)
				remaining--;
		}
		return (float)k;
	}

	void process(const ProcessArgs& args) override {
		// ---- scale preset loading ----
		int scaleIdx = clamp((int)std::round(params[SCALE_PARAM].getValue() + inputs[SCALE_INPUT].getVoltage() * 1.4f), 0, NUM_SCALES - 1);
		if (scaleIdx != lastScaleIdx) {
			if (lastScaleIdx >= 0)
				mask = SCALE_MASKS[scaleIdx];
			lastScaleIdx = scaleIdx;
		}
		// key buttons edit the mask
		for (int i = 0; i < 12; i++) {
			if (keyBtn[i].process(params[KEY_PARAM + i].getValue() > 0.f))
				mask ^= (1 << i);
		}

		int root = clamp((int)std::round(params[ROOT_PARAM].getValue() + inputs[ROOT_INPUT].getVoltage() * 12.f), 0, 24) % 12;
		int deg = (int)std::round(clamp(inputs[TRANS_INPUT].getVoltage(), -8.f, 8.f));
		float octShift = params[OCT_PARAM].getValue();
		bool sah = inputs[TRIG_INPUT].isConnected();

		int channels = std::max(1, inputs[IN_INPUT].getChannels());
		outputs[OUT_OUTPUT].setChannels(channels);
		outputs[GATE_OUTPUT].setChannels(channels);
		uint16_t notesNow = 0;

		for (int c = 0; c < channels; c++) {
			bool update = true;
			if (sah) {
				int trigCh = std::min(c, inputs[TRIG_INPUT].getChannels() - 1);
				update = trig[c].process(inputs[TRIG_INPUT].getVoltage(std::max(0, trigCh)), 0.1f, 1.f);
			}
			if (update) {
				float s = inputs[IN_INPUT].getVoltage(c) * 12.f;
				float q = quantize(s, root);
				// hysteresis: don't switch for tiny drifts around the boundary
				if (!sah && std::fabs(q - lastSemis[c]) > 0.f && std::fabs(s - lastSemis[c]) < 0.58f) {
					int pcLast = (((int)std::round(lastSemis[c]) - root) % 12 + 12) % 12;
					if (mask >> pcLast & 1)
						q = lastSemis[c];
				}
				if (q != lastSemis[c]) {
					lastSemis[c] = q;
					changePulse[c].trigger(0.002f);
				}
				float out = degreeShift(q, root, deg) / 12.f + octShift;
				heldOut[c] = out;
			}
			outputs[OUT_OUTPUT].setVoltage(heldOut[c], c);
			outputs[GATE_OUTPUT].setVoltage(changePulse[c].process(args.sampleTime) ? 10.f : 0.f, c);
			int pc = (((int)std::round(lastSemis[c]) - root) % 12 + 12) % 12;
			notesNow |= (1 << pc);
		}
		activeNotes = notesNow;

		// ---- lights ----
		if (lightDivider.process()) {
			for (int i = 0; i < 12; i++) {
				bool enabled = mask >> i & 1;
				bool active = activeNotes >> i & 1;
				lights[KEY_LIGHT + i].setBrightness(active && enabled ? 1.f : (enabled ? 0.25f : 0.f));
			}
		}
	}
};

struct FretworkWidget : ModuleWidget {
	FretworkWidget(Fretwork* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Fretwork.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace flayout;

		static const bool BLACK[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
		for (int i = 0; i < 12; i++) {
			float y = KEY_Y0 - i * KEY_DY;
			float x = BLACK[i] ? KEY_BLACK_X : KEY_X;
			addParam(createParamCentered<TL1105>(mm2px(Vec(x, y)), module, Fretwork::KEY_PARAM + i));
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(KEY_BLACK_X + KEY_LIGHT_DX, y)), module, Fretwork::KEY_LIGHT + i));
		}

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SCALE_X, SCALE_Y)), module, Fretwork::SCALE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(ROOT_X, ROOT_Y)), module, Fretwork::ROOT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(OCT_X, OCT_Y)), module, Fretwork::OCT_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[0], R1)), module, Fretwork::SCALE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[1], R1)), module, Fretwork::ROOT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[2], R1)), module, Fretwork::TRANS_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[0], R2)), module, Fretwork::TRIG_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C3[2], R2)), module, Fretwork::GATE_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[0], R3)), module, Fretwork::IN_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C3[2], R3)), module, Fretwork::OUT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Fretwork* module = getModule<Fretwork>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Rounding",
			{"Nearest", "Up", "Down"}, &module->roundMode));
	}
};

Model* modelFretwork = createModel<Fretwork, FretworkWidget>("Fretwork");
