// Maelstrom — a voltage-controlled pitch-shifting echo for VCV Rack,
// inspired by the architecture of the Make Noise/soundhack Echophon:
// a delay line feeding a pitch shifter, with two crossfaded feedback
// paths — one around the shifter (each repeat shifts again, spiraling)
// and one around the plain delay (conventional repeats) — plus clock
// sync, freeze, and an external feedback loop. Original implementation.

#include "plugin.hpp"
#include "layout_maelstrom.hpp"

static const float MAX_DELAY_SECONDS = 2.f;
static const float MIN_ECHO = 0.007f;
static const float MAX_ECHO = 1.7f;

// clock div/mult steps for the echo knob when TEMPO is patched
// (the hardware's exact ratio ladder)
static const float TEMPO_MULTS[16] = {
	1.f / 16.f, 3.f / 32.f, 1.f / 8.f, 3.f / 16.f, 1.f / 4.f, 3.f / 8.f, 1.f / 2.f, 3.f / 4.f,
	1.f, 1.5f, 2.f, 3.f, 4.f, 6.f, 8.f, 12.f
};

struct Maelstrom : Module {
	enum ParamId {
		LEVEL_PARAM,
		MIX_PARAM,
		PITCH_PARAM,
		ECHO_PARAM,
		DEPTH_PARAM,
		FEEDBACK_PARAM,
		FREEZE_PARAM,
		DEPTH_ATT_PARAM,
		P1_ATT_PARAM,
		FB_ATT_PARAM,
		ECHO_ATT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		MIX_INPUT,
		DEPTH_INPUT,
		PITCH1_INPUT,
		PITCH2_INPUT,
		ECHO_INPUT,
		FB_INPUT,
		TEMPO_INPUT,
		FREEZE_INPUT,
		IN_INPUT,
		FBLOOP_IN_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		CLK_OUTPUT,
		FBLOOP_OUT_OUTPUT,
		OUT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		TEMPH1_LIGHT,
		TEMPH2_LIGHT,
		CLK_LIGHT,
		FREEZE_LIGHT,
		LIGHTS_LEN
	};

	std::vector<float> buf;
	int bufLen = 0;
	int w = 0;

	float curDelay = 4800.f;
	bool frozen = false;
	double freezePh = 0.0;
	float frozenLen = 0.f;

	// granular pitch shifter state
	std::vector<float> shiftBuf;
	int shiftLen = 0;
	int shiftW = 0;
	float shiftPh = 0.f;

	float fbSendDc = 0.f;
	float loopDc = 0.f;
	float lastShifted = 0.f;
	float lastEchoTap = 0.f;

	float clockPeriod = 0.5f;
	float lastEdge = 1e9f;
	float clkPhase = 0.f;
	float clkPulse = 0.f;

	dsp::SchmittTrigger tempoTrig, freezeTrig;
	dsp::BooleanTrigger freezeBtn;
	dsp::ClockDivider lightDivider;

	Maelstrom() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.7f, "Input level (overdrives near max)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
		configParam(PITCH_PARAM, -1.f, 1.f, 0.f, "Pitch (scaled by depth)");
		configParam(ECHO_PARAM, 0.f, 1.f, 0.4f, "Echo time / clock div-mult");
		configParam(DEPTH_PARAM, 0.f, 1.f, 0.f, "Depth (pitch shift index)");
		configParam(FEEDBACK_PARAM, -1.f, 1.f, 0.f, "Feedback (CCW: spiral loop, CW: echo loop)");
		configButton(FREEZE_PARAM, "Freeze echo chamber");
		configParam(DEPTH_ATT_PARAM, 0.f, 1.f, 0.f, "Depth CV attenuator");
		configParam(P1_ATT_PARAM, -1.f, 1.f, 0.f, "Pitch 1 CV attenuverter");
		configParam(FB_ATT_PARAM, -1.f, 1.f, 0.f, "Feedback CV attenuverter");
		configParam(ECHO_ATT_PARAM, 0.f, 1.f, 0.f, "Echo CV attenuator");
		configInput(MIX_INPUT, "Mix CV (0–5 V, scales the mix knob)");
		configInput(DEPTH_INPUT, "Depth CV (0–5 V)");
		configInput(PITCH1_INPUT, "Pitch 1 CV (±4 V, via attenuverter)");
		configInput(PITCH2_INPUT, "Pitch 2 CV (±2 V = full range)");
		configInput(ECHO_INPUT, "Echo time CV (0–8 V)");
		configInput(FB_INPUT, "Feedback CV (±8 V, via attenuverter)");
		configInput(TEMPO_INPUT, "Tempo clock (echo knob becomes div/mult)");
		configInput(FREEZE_INPUT, "Freeze gate");
		configInput(IN_INPUT, "Audio");
		configInput(FBLOOP_IN_INPUT, "External feedback return");
		configOutput(CLK_OUTPUT, "Echo clock");
		configOutput(FBLOOP_OUT_OUTPUT, "External feedback send");
		configOutput(OUT_OUTPUT, "Mix");
		configBypass(IN_INPUT, OUT_OUTPUT);
		lightDivider.setDivision(64);
		allocate(44100.f);
	}

	void allocate(float sr) {
		bufLen = (int)(sr * (MAX_DELAY_SECONDS + 0.1f));
		buf.assign(bufLen, 0.f);
		w = 0;
		shiftLen = (int)(sr * 0.12f);
		shiftBuf.assign(shiftLen, 0.f);
		shiftW = 0;
		shiftPh = 0.f;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	void onReset() override {
		frozen = false;
		std::fill(buf.begin(), buf.end(), 0.f);
		std::fill(shiftBuf.begin(), shiftBuf.end(), 0.f);
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "frozen", json_boolean(frozen));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j = json_object_get(root, "frozen");
		if (j)
			frozen = json_boolean_value(j);
	}

	float readDelay(float delaySamp) {
		float rp = (float)w - delaySamp;
		while (rp < 0.f)
			rp += bufLen;
		int i0 = (int)rp;
		float fr = rp - i0;
		int i1 = (i0 + 1 >= bufLen) ? 0 : i0 + 1;
		return buf[i0] + (buf[i1] - buf[i0]) * fr;
	}

	float readShift(float delay) {
		float rp = (float)shiftW - delay;
		while (rp < 0.f)
			rp += shiftLen;
		int i0 = (int)rp;
		float fr = rp - i0;
		int i1 = (i0 + 1 >= shiftLen) ? 0 : i0 + 1;
		return shiftBuf[i0] + (shiftBuf[i1] - shiftBuf[i0]) * fr;
	}

	// dual-head granular pitch shifter
	float pitchShift(float x, float ratio, float W) {
		shiftBuf[shiftW] = x;
		shiftPh += (1.f - ratio);
		if (shiftPh < 0.f)
			shiftPh += W;
		if (shiftPh >= W)
			shiftPh -= W;
		float d2 = shiftPh + W * 0.5f;
		if (d2 >= W)
			d2 -= W;
		float g1 = std::sin(M_PI * shiftPh / W);
		float g2 = std::sin(M_PI * d2 / W);
		float out = readShift(shiftPh) * g1 + readShift(d2) * g2;
		shiftW++;
		if (shiftW >= shiftLen)
			shiftW = 0;
		return out * 0.82f;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;
		if (bufLen == 0)
			return;

		// ---- freeze ----
		if (freezeBtn.process(params[FREEZE_PARAM].getValue() > 0.f))
			frozen = !frozen;
		// freeze button has top priority: the gate is ignored while
		// button-frozen (like hardware)
		bool gateFrozen = !frozen && inputs[FREEZE_INPUT].isConnected() && inputs[FREEZE_INPUT].getVoltage() > 1.5f;
		bool isFrozen = frozen || gateFrozen;

		// ---- tempo clock ----
		lastEdge += dt;
		if (tempoTrig.process(inputs[TEMPO_INPUT].getVoltage(), 0.3f, 1.5f)) {
			if (lastEdge < 4.f && lastEdge > 0.001f)
				clockPeriod = clamp(lastEdge, 0.001f, 4.f);
			lastEdge = 0.f;
		}

		// ---- echo time ----
		float echoCtl = clamp(params[ECHO_PARAM].getValue() +
			clamp(inputs[ECHO_INPUT].getVoltage() / 8.f, 0.f, 1.f) * params[ECHO_ATT_PARAM].getValue(), 0.f, 1.f);
		float echoSec;
		if (inputs[TEMPO_INPUT].isConnected()) {
			int idx = clamp((int)(echoCtl * 15.999f), 0, 15);
			echoSec = clamp(clockPeriod * TEMPO_MULTS[idx], MIN_ECHO, MAX_ECHO);
		}
		else {
			echoSec = MIN_ECHO * std::pow(MAX_ECHO / MIN_ECHO, echoCtl);
		}
		// tape-style slew of the delay time (re-pitching as it moves)
		float targetSamp = echoSec * sr;
		curDelay += (targetSamp - curDelay) * (1.f - std::exp(-dt / 0.06f));

		// echo clock output / LED
		clkPhase += dt / echoSec;
		if (clkPhase >= 1.f) {
			clkPhase -= 1.f;
			clkPulse = 0.004f;
		}
		clkPulse = std::max(0.f, clkPulse - dt);
		outputs[CLK_OUTPUT].setVoltage(clkPulse > 0.f ? 5.f : 0.f);

		// ---- pitch amount ----
		float depth = clamp(params[DEPTH_PARAM].getValue() +
			clamp(inputs[DEPTH_INPUT].getVoltage() / 5.f, 0.f, 1.f) * params[DEPTH_ATT_PARAM].getValue(), 0.f, 1.f);
		float pitchCtl = params[PITCH_PARAM].getValue();
		pitchCtl += inputs[PITCH1_INPUT].getVoltage() / 4.f * params[P1_ATT_PARAM].getValue();
		pitchCtl += inputs[PITCH2_INPUT].getVoltage() / 2.f;
		pitchCtl = clamp(pitchCtl, -1.f, 1.f);
		float semis = pitchCtl * depth * 24.f; // ±2 octaves at full depth
		float ratio = std::exp2(semis / 12.f);

		// ---- feedback amounts ----
		float fbCtl = clamp(params[FEEDBACK_PARAM].getValue() +
			inputs[FB_INPUT].getVoltage() / 8.f * params[FB_ATT_PARAM].getValue(), -1.f, 1.f);
		float fb1 = std::max(0.f, -fbCtl) * 1.05f; // spiral loop (around shifter)
		float fb2 = std::max(0.f, fbCtl) * 1.05f;  // plain echo loop

		// ---- input with level + overdrive ----
		float in = inputs[IN_INPUT].getVoltage() / 5.f;
		float lvl = params[LEVEL_PARAM].getValue();
		float gain = lvl * 1.6f;
		in *= gain;
		if (lvl > 0.7f)
			in = std::tanh(in * (1.f + (lvl - 0.7f) * 6.f));

		// ---- read echo tap, run the shifter ----
		float echoTap;
		if (isFrozen) {
			// the frozen slice follows the Echo Time control: changing it
			// re-slices the chamber destructively, like hardware
			frozenLen = clamp(curDelay, 64.f, (float)bufLen - 4.f);
			freezePh += 1.0;
			if (freezePh >= frozenLen)
				freezePh = std::fmod(freezePh, (double)frozenLen);
			echoTap = readDelay(frozenLen + 1.f - (float)freezePh);
		}
		else {
			frozenLen = 0.f;
			echoTap = readDelay(curDelay);
		}
		float windowSamp = clamp(curDelay * 0.5f, 0.002f * sr, 0.1f * sr);
		float shifted = pitchShift(echoTap, ratio, windowSamp);

		// ---- external feedback loop send/return. The send taps BEFORE the
		// pitch shifter so the external loop can skip the shifting machine
		// (hardware tip) ----
		outputs[FBLOOP_OUT_OUTPUT].setVoltage(clamp(echoTap * 5.f, -10.f, 10.f));
		float extRet = 0.f;
		if (inputs[FBLOOP_IN_INPUT].isConnected()) {
			float r = inputs[FBLOOP_IN_INPUT].getVoltage() / 5.f;
			fbSendDc += (2.f * M_PI * 10.f / sr) * (r - fbSendDc);
			extRet = r - fbSendDc; // AC coupled return
		}

		// ---- write into the echo chamber ----
		if (!isFrozen) {
			float loopIn = in + shifted * fb1 + echoTap * fb2 + extRet;
			loopDc += (2.f * M_PI * 8.f / sr) * (loopIn - loopDc);
			loopIn -= loopDc;
			loopIn = std::tanh(loopIn * 0.85f) * 1.176f; // keep regeneration civil
			buf[w] = loopIn;
			w++;
			if (w >= bufLen)
				w = 0;
		}

		// ---- mix output (combo-pot style mix CV) ----
		float mix = params[MIX_PARAM].getValue();
		if (inputs[MIX_INPUT].isConnected())
			mix *= clamp(inputs[MIX_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		float dryGain = std::cos(mix * 0.5f * M_PI);
		float wetGain = std::sin(mix * 0.5f * M_PI);
		float dry = inputs[IN_INPUT].getVoltage() / 5.f;
		float out = dry * dryGain + shifted * wetGain;
		outputs[OUT_OUTPUT].setVoltage(clamp(out * 5.f, -10.f, 10.f));

		lastShifted = shifted;
		lastEchoTap = echoTap;

		// ---- lights ----
		if (lightDivider.process()) {
			// temphonic: show the two shifter heads' window positions
			float p = shiftPh / std::max(1.f, windowSamp);
			lights[TEMPH1_LIGHT].setBrightness(std::sin(M_PI * p));
			lights[TEMPH2_LIGHT].setBrightness(std::sin(M_PI * std::fmod(p + 0.5f, 1.f)));
			lights[CLK_LIGHT].setBrightness(clkPhase < 0.15f ? 1.f : 0.f);
			lights[FREEZE_LIGHT].setBrightness(isFrozen);
		}
	}
};

struct MaelstromWidget : ModuleWidget {
	MaelstromWidget(Maelstrom* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Maelstrom.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace mslayout;

		addParam(createParamCentered<Trimpot>(mm2px(Vec(LEVEL_X, YA)), module, Maelstrom::LEVEL_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(MIXK_X, YA)), module, Maelstrom::MIX_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(MIXCV_X, YA)), module, Maelstrom::MIX_INPUT));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(LED_X, LED_Y)), module, Maelstrom::TEMPH1_LIGHT));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(LED_X + 4.5f, LED_Y)), module, Maelstrom::TEMPH2_LIGHT));
		addChild(createLightCentered<SmallLight<WhiteLight>>(mm2px(Vec(LED_X + 2.2f, LED_Y + 5.f)), module, Maelstrom::CLK_LIGHT));

		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(PITCH_X, YB)), module, Maelstrom::PITCH_PARAM));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(ECHO_X, YB)), module, Maelstrom::ECHO_PARAM));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(DEPTH_X, YC)), module, Maelstrom::DEPTH_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(FB_X, YC)), module, Maelstrom::FEEDBACK_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(FRZ_X, YC)), module, Maelstrom::FREEZE_PARAM));
		addChild(createLightCentered<MediumLight<WhiteLight>>(mm2px(Vec(FRZ_X, YC - 6.6f)), module, Maelstrom::FREEZE_LIGHT));

		static const int trims[4] = {Maelstrom::DEPTH_ATT_PARAM, Maelstrom::P1_ATT_PARAM, Maelstrom::FB_ATT_PARAM, Maelstrom::ECHO_ATT_PARAM};
		for (int i = 0; i < 4; i++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(TRIM_X[i], YD)), module, trims[i]));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[0], R1)), module, Maelstrom::DEPTH_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[1], R1)), module, Maelstrom::PITCH1_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[2], R1)), module, Maelstrom::PITCH2_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[3], R1)), module, Maelstrom::ECHO_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[0], R2)), module, Maelstrom::FB_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[1], R2)), module, Maelstrom::TEMPO_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(JC[2], R2)), module, Maelstrom::CLK_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[3], R2)), module, Maelstrom::FREEZE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[0], R3)), module, Maelstrom::IN_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(JC[1], R3)), module, Maelstrom::FBLOOP_IN_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(JC[2], R3)), module, Maelstrom::FBLOOP_OUT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(JC[3], R3)), module, Maelstrom::OUT_OUTPUT));
	}
};

Model* modelMaelstrom = createModel<Maelstrom, MaelstromWidget>("Maelstrom");
