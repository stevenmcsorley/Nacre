// Foxglove — a stereo multimode virtual-analog filter for VCV Rack, inspired
// by the BlaknBlu Foxtrot Duo Mk II. Original implementation.
//
// Three models: LD (Minimoog-style 4-pole ladder with per-rung saturation),
// SK (Korg 35 Sallen-Key with a diode clipper in the resonance loop),
// SV (Oberheim SEM-style state variable, clean and gentle).
// Each can be swept continuously BP <- LP -> HP; halfway LP->HP is a notch.
// 2x oversampled, soft output clipper with CLIP LED, stereo cutoff OFFSET.

#include "plugin.hpp"
#include "layout_foxglove.hpp"

static inline float deadZone(float v, float dz) {
	float a = std::fabs(v);
	a = (a < dz) ? 0.f : (a - dz) / (1.f - dz);
	return std::copysign(a * a, v);
}

// One TPT one-pole lowpass stage.
struct OnePoleTPT {
	float s = 0.f;
	float process(float in, float G) {
		float v = (in - s) * G;
		float y = v + s;
		s = y + v;
		return y;
	}
};

// 4-pole ladder, tanh saturation per rung, pole-mixed BP/LP/HP responses.
struct LadderFilter {
	OnePoleTPT st[4];
	float fb = 0.f;
	void reset() { st[0].s = st[1].s = st[2].s = st[3].s = fb = 0.f; }
	// k: 0..~4.4 (self-oscillates past ~4), drive >= 1
	float process(float in, float g, float k, float drive, float morph) {
		float G = g / (1.f + g);
		float x = in * drive - k * fb;
		x = std::tanh(x);
		float y1 = st[0].process(x, G);
		float y2 = st[1].process(std::tanh(y1), G);
		float y3 = st[2].process(std::tanh(y2), G);
		float y4 = st[3].process(std::tanh(y3), G);
		fb = y4;
		float lp = y4 * (1.f + k * 0.3f); // partial comp: resonance authentically steals low end
		float bp = 4.f * (y2 - 2.f * y3 + y4) * (1.f + k * 0.4f);
		float hp = (x - 4.f * y1 + 6.f * y2 - 4.f * y3 + y4) * (1.f + k * 0.4f);
		float out = (morph < 0.5f) ? crossfade(bp, lp, morph * 2.f)
		                           : crossfade(lp, hp, (morph - 0.5f) * 2.f);
		return out / drive;
	}
};

// 2-pole state variable core (TPT). The SK model wraps it in a diode
// clipper feedback for the MS-20 scream; the SV model runs it clean.
struct SVFCore {
	float ic1 = 0.f, ic2 = 0.f;
	void reset() { ic1 = ic2 = 0.f; }
	// returns lp, bp, hp via refs. R = damping (1/(2Q)).
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

struct Foxglove : Module {
	enum ParamId {
		CUTOFF_PARAM,
		RES_PARAM,
		MORPH_PARAM,
		OFFSET_PARAM,
		TYPE_PARAM,
		BOOST_PARAM,
		AUXMODE_PARAM,
		MONO_PARAM,
		GAINL_PARAM,
		GAINR_PARAM,
		GAINCO_PARAM,
		GAINAUX_PARAM,
		GAINOS_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INL_INPUT,
		INR_INPUT,
		VOCT_INPUT,
		CO_INPUT,
		AUX_INPUT,
		OS_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTL_OUTPUT,
		OUTR_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		CLIP_LIGHT,
		LIGHTS_LEN
	};

	LadderFilter ladder[2];
	SVFCore svf[2];
	float clipEnv = 0.f;
	int lastType = -1;
	dsp::ClockDivider lightDivider;

	Foxglove() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(CUTOFF_PARAM, 0.f, 1.f, 0.7f, "Cutoff frequency");
		configParam(RES_PARAM, 0.f, 1.f, 0.f, "Resonance");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.5f, "Response (BP - LP - HP, LP at centre)");
		configParam(OFFSET_PARAM, 0.f, 1.f, 0.5f, "Stereo cutoff offset (centre = off)");
		configSwitch(TYPE_PARAM, 0.f, 2.f, 0.f, "Filter type", {"LD (ladder)", "SK (Sallen-Key)", "SV (state variable)"});
		configSwitch(BOOST_PARAM, 0.f, 1.f, 0.f, "Boost", {"Off", "On"});
		configSwitch(AUXMODE_PARAM, 0.f, 2.f, 0.f, "Aux CV mode", {"Mix (response)", "Q (resonance)", "CO (cutoff)"});
		configSwitch(MONO_PARAM, 0.f, 1.f, 0.f, "Input mode", {"Stereo", "Mono (summed)"});
		configParam(GAINL_PARAM, 0.f, 1.f, 1.f, "Input L gain");
		configParam(GAINR_PARAM, 0.f, 1.f, 1.f, "Input R gain");
		configParam(GAINCO_PARAM, 0.f, 1.f, 1.f, "Cutoff CV gain");
		configParam(GAINAUX_PARAM, 0.f, 1.f, 1.f, "Aux CV gain");
		configParam(GAINOS_PARAM, 0.f, 1.f, 1.f, "Offset CV gain");

		configInput(INL_INPUT, "Left audio");
		configInput(INR_INPUT, "Right audio");
		configInput(VOCT_INPUT, "V/oct cutoff tracking");
		configInput(CO_INPUT, "Cutoff CV");
		configInput(AUX_INPUT, "Aux CV (mode switch: mix / Q / cutoff)");
		configInput(OS_INPUT, "Stereo offset CV");
		configOutput(OUTL_OUTPUT, "Left");
		configOutput(OUTR_OUTPUT, "Right");

		lightDivider.setDivision(64);
	}

	void process(const ProcessArgs& args) override {
		int type = (int)params[TYPE_PARAM].getValue();
		if (type != lastType) {
			for (int c = 0; c < 2; c++) {
				ladder[c].reset();
				svf[c].reset();
			}
			lastType = type;
		}
		bool boost = params[BOOST_PARAM].getValue() > 0.5f;
		bool mono = params[MONO_PARAM].getValue() > 0.5f;
		int auxMode = (int)params[AUXMODE_PARAM].getValue();
		float aux = inputs[AUX_INPUT].getVoltage() * params[GAINAUX_PARAM].getValue();

		// response morph: 0 BP, 0.5 LP (small detent), 1 HP
		float morph = params[MORPH_PARAM].getValue();
		morph = 0.5f + deadZone((morph - 0.5f) * 2.f, 0.04f) * 0.5f;
		if (auxMode == 0)
			morph = clamp(morph + aux / 10.f, 0.f, 1.f);

		float res = params[RES_PARAM].getValue();
		if (auxMode == 1)
			res = clamp(res + aux / 10.f, 0.f, 1.f);

		// cutoff: knob 20 Hz .. 20 kHz expo + V/oct + CO CV + aux (CO mode)
		float coOct = params[CUTOFF_PARAM].getValue() * 10.f;
		coOct += inputs[VOCT_INPUT].getVoltage();
		coOct += inputs[CO_INPUT].getVoltage() * params[GAINCO_PARAM].getValue();
		if (auxMode == 2)
			coOct += aux;

		// stereo offset: raises one channel, lowers the other (±2 octaves)
		float off = deadZone((params[OFFSET_PARAM].getValue() - 0.5f) * 2.f, 0.06f) * 2.f;
		off += inputs[OS_INPUT].getVoltage() * params[GAINOS_PARAM].getValue() * 0.4f;

		float inL = inputs[INL_INPUT].getVoltage() * params[GAINL_PARAM].getValue();
		float inR = inputs[INR_INPUT].getVoltage() * params[GAINR_PARAM].getValue();
		if (mono)
			inL = inR = inL + inR; // both inputs summed into both filters

		float drive = boost ? 3.2f : 1.f;
		float srOS = args.sampleRate * 2.f; // 2x oversampling
		bool clipped = false;

		float out[2];
		for (int c = 0; c < 2; c++) {
			float fc = 20.f * dsp::exp2_taylor5(coOct + (c == 0 ? -off : off));
			fc = clamp(fc, 10.f, std::min(20000.f, args.sampleRate * 0.45f));
			float g = std::tan(M_PI * fc / srOS);
			float x = (c == 0 ? inL : inR) / 5.f;

			float y = 0.f;
			for (int os = 0; os < 2; os++) {
				if (type == 0) {
					// LD: self-oscillates past k~4, boost raises the max
					float k = res * (boost ? 4.6f : 4.3f);
					y = ladder[c].process(x, g, k, drive, morph);
				}
				else if (type == 1) {
					// SK: 2-pole, diode-clipped resonance loop, keen to scream
					float k = res * (boost ? 2.6f : 2.2f);
					float R = std::max(1.f - k * 0.5f, 0.005f);
					float lp, bp, hp;
					float xd = std::tanh(x * drive * 1.5f);
					svf[c].process(xd, g, R, lp, bp, hp);
					// diode clipper on the resonant peak, fuzz-pedal style
					float q = 1.f / (2.f * R);
					lp = std::tanh(lp * (1.f + q * 0.35f));
					bp = std::tanh(bp * (1.f + q * 0.35f));
					hp = std::tanh(hp * (1.f + q * 0.35f));
					y = (morph < 0.5f) ? crossfade(bp * 1.6f, lp, morph * 2.f)
					                   : crossfade(lp, hp, (morph - 0.5f) * 2.f);
					y /= (drive * 0.8f + 0.2f);
				}
				else {
					// SV: clean SEM — gentle resonance, no self-oscillation
					float R = 1.f - res * (boost ? 0.97f : 0.94f);
					float lp, bp, hp;
					float xd = boost ? std::tanh(x * drive) : x;
					svf[c].process(xd, g, R, lp, bp, hp);
					y = (morph < 0.5f) ? crossfade(bp * 1.4f, lp, morph * 2.f)
					                   : crossfade(lp, hp, (morph - 0.5f) * 2.f);
					if (boost)
						y /= drive * 0.7f;
				}
			}

			y *= 5.f;
			// output soft clipper + LED
			if (std::fabs(y) > 5.f) {
				clipped = true;
				y = std::copysign(5.f + std::tanh((std::fabs(y) - 5.f) / 3.f) * 3.f, y);
			}
			out[c] = y;
		}

		outputs[OUTL_OUTPUT].setVoltage(out[0]);
		outputs[OUTR_OUTPUT].setVoltage(out[1]);

		clipEnv = clipped ? 1.f : clipEnv * (1.f - 8.f * args.sampleTime);
		if (lightDivider.process())
			lights[CLIP_LIGHT].setBrightness(clipEnv);
	}
};

struct FoxgloveWidget : ModuleWidget {
	FoxgloveWidget(Foxglove* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Foxglove.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace fglayout;
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(SW_X[0], SW_Y)), module, Foxglove::TYPE_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(SW_X[1], SW_Y)), module, Foxglove::BOOST_PARAM));
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(SW_X[2], SW_Y)), module, Foxglove::AUXMODE_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(SW_X[3], SW_Y)), module, Foxglove::MONO_PARAM));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX1, KY1)), module, Foxglove::CUTOFF_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX2, KY1)), module, Foxglove::RES_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX1, KY2)), module, Foxglove::MORPH_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KX2, KY2)), module, Foxglove::OFFSET_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(OS_TRIM_X, OS_TRIM_Y)), module, Foxglove::GAINOS_PARAM));

		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(CLIP_LED_X, CLIP_LED_Y)), module, Foxglove::CLIP_LIGHT));

		static const int trims[4] = {Foxglove::GAINL_PARAM, Foxglove::GAINR_PARAM, Foxglove::GAINCO_PARAM, Foxglove::GAINAUX_PARAM};
		for (int i = 0; i < 4; i++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(TRIM_X[i], TRIM_Y)), module, trims[i]));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[0], R1)), module, Foxglove::INL_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[1], R1)), module, Foxglove::INR_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[2], R1)), module, Foxglove::CO_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[3], R1)), module, Foxglove::AUX_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[0], R2)), module, Foxglove::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J_X[1], R2)), module, Foxglove::OS_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J_X[2], R2)), module, Foxglove::OUTL_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J_X[3], R2)), module, Foxglove::OUTR_OUTPUT));
	}
};

Model* modelFoxglove = createModel<Foxglove, FoxgloveWidget>("Foxglove");
