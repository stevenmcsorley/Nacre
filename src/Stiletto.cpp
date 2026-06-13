// Stiletto — a stereo dual filter for VCV Rack, inspired by the architecture
// of the Shakmat Dual Dagger (per channel: 24 dB/oct HP into 24 dB/oct LP,
// shared cutoffs, assignable resonance, link/bandpass mode, pan CVs).
// Original DSP implementation (ZDF ladder topology).

#include "plugin.hpp"
#include "layout_stiletto.hpp"

// Zero-delay-feedback 4-pole lowpass ladder (Zavalishin TPT topology).
struct LadderLP {
	float s1 = 0.f, s2 = 0.f, s3 = 0.f, s4 = 0.f;

	float process(float x, float g, float k) {
		float G = g / (1.f + g);
		float den = 1.f / (1.f + g);
		float S = (G * G * G * s1 + G * G * s2 + G * s3 + s4) * den;
		float G4 = G * G * G * G;
		float u = (x - k * S) / (1.f + k * G4);
		u = std::tanh(u); // gentle saturation bounds self-oscillation
		float v, y;
		v = (u - s1) * G; y = v + s1; s1 = y + v; u = y;
		v = (u - s2) * G; y = v + s2; s2 = y + v; u = y;
		v = (u - s3) * G; y = v + s3; s3 = y + v; u = y;
		v = (u - s4) * G; y = v + s4; s4 = y + v;
		return y;
	}

	void reset() { s1 = s2 = s3 = s4 = 0.f; }
};

// Zero-delay-feedback 4-pole highpass ladder.
struct LadderHP {
	float s1 = 0.f, s2 = 0.f, s3 = 0.f, s4 = 0.f;

	float process(float x, float g, float k) {
		float G = g / (1.f + g);
		float Hh = 1.f / (1.f + g);
		float H2 = Hh * Hh;
		float H4 = H2 * H2;
		float C = H4 * s1 + H2 * Hh * s2 + H2 * s3 + Hh * s4;
		float u = (x + k * C) / (1.f + k * H4);
		u = std::tanh(u);
		float v, ylp;
		v = (u - s1) * G; ylp = v + s1; float y1 = u - ylp; s1 = ylp + v;
		v = (y1 - s2) * G; ylp = v + s2; float y2 = y1 - ylp; s2 = ylp + v;
		v = (y2 - s3) * G; ylp = v + s3; float y3 = y2 - ylp; s3 = ylp + v;
		v = (y3 - s4) * G; ylp = v + s4; float y4 = y3 - ylp; s4 = ylp + v;
		return y4;
	}

	void reset() { s1 = s2 = s3 = s4 = 0.f; }
};

struct FreqParamQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float f = 20.f * std::exp2(getValue() * 10.f);
		if (f < 1000.f)
			return string::f("%.0f Hz", f);
		return string::f("%.2f kHz", f / 1000.f);
	}
};

struct Stiletto : Module {
	enum ParamId {
		LPF_PARAM,
		RES_PARAM,
		HPF_PARAM,
		LINK_PARAM,
		HPRES_PARAM,
		LPRES_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		LPF_INPUT,
		RES_INPUT,
		HPF_INPUT,
		PANLP_INPUT,
		PANHP_INPUT,
		IN1_INPUT,
		IN2_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT1_OUTPUT,
		OUT2_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	LadderHP hp[2];
	LadderLP lp[2];
	bool hiResRange = false; // back-panel jumper: Hi allows self-oscillation

	Stiletto() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam<FreqParamQuantity>(LPF_PARAM, 0.f, 1.f, 1.f, "Low-pass cutoff");
		configParam(RES_PARAM, 0.f, 1.f, 0.f, "Resonance", "%", 0.f, 100.f);
		configParam<FreqParamQuantity>(HPF_PARAM, 0.f, 1.f, 0.f, "High-pass cutoff");
		configSwitch(LINK_PARAM, 0.f, 1.f, 0.f, "Link (bandpass mode)", {"Off", "On"});
		configSwitch(HPRES_PARAM, 0.f, 1.f, 0.f, "Resonance on high-pass", {"Off", "On"});
		configSwitch(LPRES_PARAM, 0.f, 1.f, 1.f, "Resonance on low-pass", {"Off", "On"});
		configInput(LPF_INPUT, "Low-pass cutoff (1V/oct)");
		configInput(RES_INPUT, "Resonance CV");
		configInput(HPF_INPUT, "High-pass cutoff (1V/oct)");
		configInput(PANLP_INPUT, "Low-pass pan CV (offsets ch 1 up, ch 2 down)");
		configInput(PANHP_INPUT, "High-pass pan CV (offsets ch 1 up, ch 2 down)");
		configInput(IN1_INPUT, "Channel 1 audio");
		configInput(IN2_INPUT, "Channel 2 audio (normalled to ch 1)");
		configOutput(OUT1_OUTPUT, "Channel 1 audio");
		configOutput(OUT2_OUTPUT, "Channel 2 audio");
		configBypass(IN1_INPUT, OUT1_OUTPUT);
		configBypass(IN2_INPUT, OUT2_OUTPUT);
	}

	void onReset() override {
		hiResRange = false;
		for (int c = 0; c < 2; c++) {
			hp[c].reset();
			lp[c].reset();
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "hiResRange", json_boolean(hiResRange));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "hiResRange");
		if (j)
			hiResRange = json_boolean_value(j);
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;

		bool link = params[LINK_PARAM].getValue() > 0.5f;
		float lpKnob = params[LPF_PARAM].getValue();
		float hpKnob = params[HPF_PARAM].getValue();
		float lpCv = inputs[LPF_INPUT].getVoltage();
		float hpCv = inputs[HPF_INPUT].getVoltage();
		float panLp = inputs[PANLP_INPUT].getVoltage() * 0.5f; // 1 V = 1/2 octave split
		float panHp = inputs[PANHP_INPUT].getVoltage() * 0.5f;

		float res = clamp(params[RES_PARAM].getValue() + inputs[RES_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		float kMax = hiResRange ? 4.3f : 3.4f;
		float kLp = params[LPRES_PARAM].getValue() > 0.5f ? res * kMax : 0.f;
		float kHp = params[HPRES_PARAM].getValue() > 0.5f ? res * kMax : 0.f;

		float in1 = inputs[IN1_INPUT].getVoltage();
		float in2 = inputs[IN2_INPUT].isConnected() ? inputs[IN2_INPUT].getVoltage() : in1;
		float ins[2] = {in1 * 0.2f, in2 * 0.2f};

		for (int c = 0; c < 2; c++) {
			float dir = (c == 0) ? 1.f : -1.f;
			// cutoffs in octaves above 20 Hz
			float hpOct = hpKnob * 10.f + hpCv + panHp * dir;
			float lpOct;
			if (link) {
				// HPF control moves all four filters; LPF knob sets bandwidth.
				lpOct = hpOct + lpKnob * 8.f + lpCv + panLp * dir;
			}
			else {
				lpOct = lpKnob * 10.f + lpCv + panLp * dir;
			}
			float hpF = clamp(20.f * std::exp2(hpOct), 1.f, 0.45f * sr);
			float lpF = clamp(20.f * std::exp2(lpOct), 1.f, 0.45f * sr);
			float gHp = std::tan(M_PI * hpF / sr);
			float gLp = std::tan(M_PI * lpF / sr);

			float y = hp[c].process(ins[c], gHp, kHp);
			y = lp[c].process(y, gLp, kLp);
			outputs[c == 0 ? OUT1_OUTPUT : OUT2_OUTPUT].setVoltage(clamp(y * 5.f, -10.f, 10.f));
		}
	}
};

struct StilettoWidget : ModuleWidget {
	StilettoWidget(Stiletto* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Stiletto.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace slayout;

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(LPF_X, LPF_Y)), module, Stiletto::LPF_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(RES_X, RES_Y)), module, Stiletto::RES_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(HPF_X, HPF_Y)), module, Stiletto::HPF_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(LINK_X, LINK_Y)), module, Stiletto::LINK_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(HPRES_X, HPRES_Y)), module, Stiletto::HPRES_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(LPRES_X, LPRES_Y)), module, Stiletto::LPRES_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XL, R1)), module, Stiletto::LPF_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XC, R1)), module, Stiletto::RES_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XR, R1)), module, Stiletto::HPF_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XL, R2)), module, Stiletto::PANLP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XR, R2)), module, Stiletto::PANHP_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XL, R3)), module, Stiletto::IN1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(XR, R3)), module, Stiletto::IN2_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(XL, R4)), module, Stiletto::OUT1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(XR, R4)), module, Stiletto::OUT2_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Stiletto* module = getModule<Stiletto>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Hi resonance range (self-oscillation)", "", &module->hiResRange));
	}
};

Model* modelStiletto = createModel<Stiletto, StilettoWidget>("Stiletto");
