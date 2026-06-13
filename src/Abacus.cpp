// Abacus — a dual function generator for VCV Rack, inspired by the
// architecture of classic Serge/Buchla-style rise/fall function generators
// (as popularized by Make Noise Maths). Original implementation.

#include "plugin.hpp"
#include "layout_abacus.hpp"

// Rise/fall stage time: knob 0..1 -> ~0.5 ms .. ~25 min for a full 10 V
// excursion (audio-rate cycling up to ~1 kHz at the fast end).
static inline float stageTime(float k) {
	return 0.0005f * std::pow(10.f, k * 6.5f);
}

// Slew rate in V/s. p = |level|/10, position within the 0-10 V excursion.
// shape: -1 log, 0 linear, +1 hyper-expo. The curve tracks the voltage LEVEL:
// LOG slows as the voltage rises (fast start, soft landing; full log runs
// ~1.8x longer — east coast portamento), EXPO speeds up as the voltage rises
// (soft attack, sharp top; falls drop fast and tail out; hyper is fastest).
static inline float slewRate(float p, float T, float shape) {
	float lin = 10.f / T;
	if (shape > 0.f) {
		float expo = (p * 46.f + 1.2f) / T;
		if (shape <= 0.6f)
			return crossfade(lin, expo, shape / 0.6f);
		float hyper = (p * p * 150.f + 2.5f) / T;
		return crossfade(expo, hyper, (shape - 0.6f) / 0.4f);
	}
	else if (shape < 0.f) {
		float logr = ((1.f - p) * 30.f + 0.15f) / T;
		return crossfade(lin, logr, -shape);
	}
	return lin;
}

struct TimeParamQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float T = stageTime(getValue());
		if (T < 1.f)
			return string::f("%.1f ms", T * 1000.f);
		if (T < 60.f)
			return string::f("%.2f s", T);
		return string::f("%.1f min", T / 60.f);
	}
};

struct Abacus : Module {
	enum ParamId {
		RISE1_PARAM,
		FALL1_PARAM,
		RESP1_PARAM,
		CYCLE1_PARAM,
		RISE4_PARAM,
		FALL4_PARAM,
		RESP4_PARAM,
		CYCLE4_PARAM,
		ATT1_PARAM,
		ATT2_PARAM,
		ATT3_PARAM,
		ATT4_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		IN1_INPUT,
		TRIG1_INPUT,
		RISE1_INPUT,
		FALL1_INPUT,
		BOTH1_INPUT,
		IN2_INPUT,
		IN3_INPUT,
		IN4_INPUT,
		TRIG4_INPUT,
		RISE4_INPUT,
		FALL4_INPUT,
		BOTH4_INPUT,
		CYCLE1_INPUT,
		CYCLE4_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		UNITY1_OUTPUT,
		VAR1_OUTPUT,
		EOR_OUTPUT,
		OUT2_OUTPUT,
		OUT3_OUTPUT,
		UNITY4_OUTPUT,
		VAR4_OUTPUT,
		EOC_OUTPUT,
		SUM_OUTPUT,
		INV_OUTPUT,
		OR_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		CYCLE1_LIGHT,
		CYCLE4_LIGHT,
		ENUMS(CH_LIGHT, 4 * 2), // bipolar (green/red) per channel, post-attenuverter
		EOR_LIGHT,
		EOC_LIGHT,
		OR_LIGHT,
		INV_LIGHT,
		LIGHTS_LEN
	};

	struct FuncGen {
		float out = 0.f;
		float peak = 10.f; // triggered functions travel to 10 V, self-cycling to 8 V
		bool oneShot = false;
		bool rising = false;
		bool falling = false;
		dsp::SchmittTrigger trig;

		void process(float dt, float target, float riseT, float fallT, float shape, bool cycle) {
			if (oneShot)
				target = peak;
			float delta = target - out;
			float ad = std::fabs(delta);
			if (ad < 1e-4f) {
				out = target;
				rising = falling = false;
				if (oneShot && out >= peak - 1e-3f)
					oneShot = false; // peak reached, begin fall
				else if (cycle && !oneShot) {
					oneShot = true; // settled at bottom: retrigger
					peak = 8.f;
				}
				return;
			}
			float T = (delta > 0.f) ? riseT : fallT;
			float p = clamp(std::fabs(out) / 10.f, 0.f, 1.f);
			float rate = slewRate(p, T, shape);
			float step = std::min(rate * dt, ad);
			out += (delta > 0.f) ? step : -step;
			rising = delta > 0.f;
			falling = delta < 0.f;
			if (oneShot && out >= peak - 1e-3f)
				oneShot = false;
		}
	};

	FuncGen fg1, fg4;
	dsp::ClockDivider lightDivider;

	Abacus() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam<TimeParamQuantity>(RISE1_PARAM, 0.f, 1.f, 0.35f, "Ch 1 rise time");
		configParam<TimeParamQuantity>(FALL1_PARAM, 0.f, 1.f, 0.45f, "Ch 1 fall time");
		configParam(RESP1_PARAM, -1.f, 1.f, 0.f, "Ch 1 response (log/linear/expo/hyper)");
		configSwitch(CYCLE1_PARAM, 0.f, 1.f, 0.f, "Ch 1 cycle", {"Off", "On"});
		configParam<TimeParamQuantity>(RISE4_PARAM, 0.f, 1.f, 0.35f, "Ch 4 rise time");
		configParam<TimeParamQuantity>(FALL4_PARAM, 0.f, 1.f, 0.45f, "Ch 4 fall time");
		configParam(RESP4_PARAM, -1.f, 1.f, 0.f, "Ch 4 response (log/linear/expo/hyper)");
		configSwitch(CYCLE4_PARAM, 0.f, 1.f, 0.f, "Ch 4 cycle", {"Off", "On"});
		configParam(ATT1_PARAM, -1.f, 1.f, 1.f, "Ch 1 attenuverter", "%", 0.f, 100.f);
		configParam(ATT2_PARAM, -1.f, 1.f, 0.f, "Ch 2 attenuverter", "%", 0.f, 100.f);
		configParam(ATT3_PARAM, -1.f, 1.f, 0.f, "Ch 3 attenuverter", "%", 0.f, 100.f);
		configParam(ATT4_PARAM, -1.f, 1.f, 1.f, "Ch 4 attenuverter", "%", 0.f, 100.f);

		configInput(IN1_INPUT, "Ch 1 signal");
		configInput(TRIG1_INPUT, "Ch 1 trigger");
		configInput(RISE1_INPUT, "Ch 1 rise CV");
		configInput(FALL1_INPUT, "Ch 1 fall CV");
		configInput(BOTH1_INPUT, "Ch 1 both CV (positive shortens the function)");
		configInput(IN2_INPUT, "Ch 2 (normalled +10 V, ±10 V offset)");
		configInput(IN3_INPUT, "Ch 3 (normalled +5 V, ±5 V offset)");
		configInput(IN4_INPUT, "Ch 4 signal");
		configInput(TRIG4_INPUT, "Ch 4 trigger");
		configInput(RISE4_INPUT, "Ch 4 rise CV");
		configInput(FALL4_INPUT, "Ch 4 fall CV");
		configInput(BOTH4_INPUT, "Ch 4 both CV (positive shortens the function)");
		configInput(CYCLE1_INPUT, "Ch 1 cycle gate (cycles while high)");
		configInput(CYCLE4_INPUT, "Ch 4 cycle gate (cycles while high)");

		configOutput(UNITY1_OUTPUT, "Ch 1 unity");
		configOutput(VAR1_OUTPUT, "Ch 1 variable (removes ch 1 from bus)");
		configOutput(EOR_OUTPUT, "End of rise gate");
		configOutput(OUT2_OUTPUT, "Ch 2 (removes ch 2 from bus)");
		configOutput(OUT3_OUTPUT, "Ch 3 (removes ch 3 from bus)");
		configOutput(UNITY4_OUTPUT, "Ch 4 unity");
		configOutput(VAR4_OUTPUT, "Ch 4 variable (removes ch 4 from bus)");
		configOutput(EOC_OUTPUT, "End of cycle gate");
		configOutput(SUM_OUTPUT, "Sum");
		configOutput(INV_OUTPUT, "Inverted sum");
		configOutput(OR_OUTPUT, "Analog OR (max)");

		lightDivider.setDivision(32);
	}

	float effectiveTime(int knobId, int cvId, int bothId) {
		float k = params[knobId].getValue();
		k += inputs[cvId].getVoltage() / 10.f;
		// BOTH responds inversely: positive CV shortens the whole function
		k -= inputs[bothId].getVoltage() / 8.f;
		return stageTime(clamp(k, 0.f, 1.f));
	}

	void processChannel(FuncGen& fg, float dt, int inId, int trigId, int riseKnob, int riseCv,
	                    int fallKnob, int fallCv, int bothCv, int respId, int cycleId, int cycleIn) {
		if (fg.trig.process(inputs[trigId].getVoltage(), 0.1f, 1.f) && !fg.rising) {
			fg.oneShot = true;
			fg.peak = 10.f;
		}
		float target = inputs[inId].isConnected() ? clamp(inputs[inId].getVoltage(), -10.f, 10.f) : 0.f;
		float riseT = effectiveTime(riseKnob, riseCv, bothCv);
		float fallT = effectiveTime(fallKnob, fallCv, bothCv);
		// gate high cycles; the button cycles regardless of the gate
		bool cycle = (params[cycleId].getValue() > 0.5f) || (inputs[cycleIn].getVoltage() >= 2.5f);
		fg.process(dt, target, riseT, fallT, params[respId].getValue(), cycle);
	}

	void process(const ProcessArgs& args) override {
		processChannel(fg1, args.sampleTime, IN1_INPUT, TRIG1_INPUT, RISE1_PARAM, RISE1_INPUT,
		               FALL1_PARAM, FALL1_INPUT, BOTH1_INPUT, RESP1_PARAM, CYCLE1_PARAM, CYCLE1_INPUT);
		processChannel(fg4, args.sampleTime, IN4_INPUT, TRIG4_INPUT, RISE4_PARAM, RISE4_INPUT,
		               FALL4_PARAM, FALL4_INPUT, BOTH4_INPUT, RESP4_PARAM, CYCLE4_PARAM, CYCLE4_INPUT);

		float ch1 = fg1.out * params[ATT1_PARAM].getValue();
		float in2 = inputs[IN2_INPUT].isConnected() ? inputs[IN2_INPUT].getVoltage() : 10.f;
		float in3 = inputs[IN3_INPUT].isConnected() ? inputs[IN3_INPUT].getVoltage() : 5.f;
		float ch2 = in2 * params[ATT2_PARAM].getValue();
		float ch3 = in3 * params[ATT3_PARAM].getValue();
		float ch4 = fg4.out * params[ATT4_PARAM].getValue();

		outputs[UNITY1_OUTPUT].setVoltage(fg1.out);
		outputs[UNITY4_OUTPUT].setVoltage(fg4.out);
		outputs[VAR1_OUTPUT].setVoltage(ch1);
		outputs[VAR4_OUTPUT].setVoltage(ch4);
		outputs[OUT2_OUTPUT].setVoltage(ch2);
		outputs[OUT3_OUTPUT].setVoltage(ch3);

		// EOR: high once ch 1 finishes rising, through the fall; low at rest at 0 V.
		// EOC: high once ch 4 finishes falling, through the rise; low at rest at top.
		bool eor = fg1.falling || (!fg1.rising && fg1.out > 0.05f);
		bool eoc = fg4.rising || (!fg4.falling && fg4.out < 0.05f);
		outputs[EOR_OUTPUT].setVoltage(eor ? 10.f : 0.f);
		outputs[EOC_OUTPUT].setVoltage(eoc ? 10.f : 0.f);

		// Patching a channel's variable output removes it from the bus.
		float sum = 0.f;
		float orv = 0.f;
		if (!outputs[VAR1_OUTPUT].isConnected()) {
			sum += ch1;
			orv = std::max(orv, ch1);
		}
		if (!outputs[OUT2_OUTPUT].isConnected()) {
			sum += ch2;
			orv = std::max(orv, ch2);
		}
		if (!outputs[OUT3_OUTPUT].isConnected()) {
			sum += ch3;
			orv = std::max(orv, ch3);
		}
		if (!outputs[VAR4_OUTPUT].isConnected()) {
			sum += ch4;
			orv = std::max(orv, ch4);
		}
		// analog summing amp: linear until pinned at the rails
		sum = clamp(sum, -10.5f, 10.5f);

		outputs[SUM_OUTPUT].setVoltage(sum);
		outputs[INV_OUTPUT].setVoltage(0.f - sum + 0.f); // +0 avoids a "-0.00" readout
		outputs[OR_OUTPUT].setVoltage(orv);

		if (lightDivider.process()) {
			lights[CYCLE1_LIGHT].setBrightness(params[CYCLE1_PARAM].getValue());
			lights[CYCLE4_LIGHT].setBrightness(params[CYCLE4_PARAM].getValue());
			// bipolar channel indicators, post-attenuverter (like the hardware)
			float chVals[4] = {ch1, ch2, ch3, ch4};
			for (int i = 0; i < 4; i++) {
				lights[CH_LIGHT + i * 2 + 0].setBrightness(clamp(chVals[i] / 10.f, 0.f, 1.f));
				lights[CH_LIGHT + i * 2 + 1].setBrightness(clamp(-chVals[i] / 10.f, 0.f, 1.f));
			}
			lights[EOR_LIGHT].setBrightness(eor ? 1.f : 0.f);
			lights[EOC_LIGHT].setBrightness(eoc ? 1.f : 0.f);
			lights[OR_LIGHT].setBrightness(clamp(orv / 10.f, 0.f, 1.f));
			lights[INV_LIGHT].setBrightness(clamp(-sum / 10.f, 0.f, 1.f));
		}
	}
};

struct AbacusWidget : ModuleWidget {
	AbacusWidget(Abacus* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Abacus.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace alayout;

		// top row: in1, trig1, in2, in3, trig4, in4 (like the hardware)
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN1_X, TOP_Y)), module, Abacus::IN1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TRIG1_X, TOP_Y)), module, Abacus::TRIG1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN2_X, TOP_Y)), module, Abacus::IN2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN3_X, TOP_Y)), module, Abacus::IN3_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(TRIG4_X, TOP_Y)), module, Abacus::TRIG4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN4_X, TOP_Y)), module, Abacus::IN4_INPUT));

		// cycle buttons + red LEDs on the wings
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(CYC_BTN_X, CYC_BTN_Y)), module, Abacus::CYCLE1_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(CYC_LED_X, CYC_LED_Y)), module, Abacus::CYCLE1_LIGHT));
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(W_MM - CYC_BTN_X, CYC_BTN_Y)), module, Abacus::CYCLE4_PARAM));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(W_MM - CYC_LED_X, CYC_LED_Y)), module, Abacus::CYCLE4_LIGHT));

		// big rise / fall / response knobs, mirrored
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(RISE_X, RISE_Y)), module, Abacus::RISE1_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(FALL_X, FALL_Y)), module, Abacus::FALL1_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(RESP_X, RESP_Y)), module, Abacus::RESP1_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(W_MM - RISE_X, RISE_Y)), module, Abacus::RISE4_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(W_MM - FALL_X, FALL_Y)), module, Abacus::FALL4_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(W_MM - RESP_X, RESP_Y)), module, Abacus::RESP4_PARAM));

		// center attenuverter column
		static const int attParams[4] = {Abacus::ATT1_PARAM, Abacus::ATT2_PARAM, Abacus::ATT3_PARAM, Abacus::ATT4_PARAM};
		for (int i = 0; i < 4; i++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(ATT_X, ATT_Y[i])), module, attParams[i]));
		addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(CH1LED_X, CH1LED_Y)), module, Abacus::CH_LIGHT + 0));
		addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(CH2LED_X, CH2LED_Y)), module, Abacus::CH_LIGHT + 2));
		addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(CH3LED_X, CH3LED_Y)), module, Abacus::CH_LIGHT + 4));
		addChild(createLightCentered<MediumLight<GreenRedLight>>(mm2px(Vec(CH4LED_X, CH4LED_Y)), module, Abacus::CH_LIGHT + 6));

		// edge CV columns: rise, both, fall, cycle gate
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(EDGE_X, EDGE_Y[0])), module, Abacus::RISE1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(EDGE_X, EDGE_Y[1])), module, Abacus::BOTH1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(EDGE_X, EDGE_Y[2])), module, Abacus::FALL1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(EDGE_X, EDGE_Y[3])), module, Abacus::CYCLE1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(W_MM - EDGE_X, EDGE_Y[0])), module, Abacus::RISE4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(W_MM - EDGE_X, EDGE_Y[1])), module, Abacus::BOTH4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(W_MM - EDGE_X, EDGE_Y[2])), module, Abacus::FALL4_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(W_MM - EDGE_X, EDGE_Y[3])), module, Abacus::CYCLE4_INPUT));

		// numbered channel outputs (var 1, out 2, out 3, var 4)
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(VAR_X[0], VAR_Y)), module, Abacus::VAR1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(VAR_X[1], VAR_Y)), module, Abacus::OUT2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(VAR_X[2], VAR_Y)), module, Abacus::OUT3_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(VAR_X[3], VAR_Y)), module, Abacus::VAR4_OUTPUT));

		// bus row with indicator LEDs
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BUS_X[0], BUS_Y)), module, Abacus::OR_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BUS_X[1], BUS_Y)), module, Abacus::SUM_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(BUS_X[2], BUS_Y)), module, Abacus::INV_OUTPUT));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(ORLED_X, ORLED_Y)), module, Abacus::OR_LIGHT));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(INVLED_X, INVLED_Y)), module, Abacus::INV_LIGHT));

		// corners: EOR + unity 1 (left), unity 4 + EOC (right)
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(EOR_X, CORNER_Y)), module, Abacus::EOR_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(U1_X, CORNER_Y)), module, Abacus::UNITY1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(U4_X, CORNER_Y)), module, Abacus::UNITY4_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(EOC_X, CORNER_Y)), module, Abacus::EOC_OUTPUT));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(EORLED_X, EORLED_Y)), module, Abacus::EOR_LIGHT));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(EOCLED_X, EOCLED_Y)), module, Abacus::EOC_LIGHT));
	}
};

Model* modelAbacus = createModel<Abacus, AbacusWidget>("Abacus");
