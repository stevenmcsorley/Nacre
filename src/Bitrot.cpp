// Bitrot — a circuit-bent stereo audio buffer for VCV Rack, inspired by the
// architecture of the Qu-Bit Data Bender (clocked buffer windows, stutter
// subdivisions, tape-style bend mangling, digital break glitches, and an
// end-of-chain corrupt section). Original DSP implementation.

#include "plugin.hpp"
#include "layout_bitrot.hpp"

static const float BUFFER_SECONDS = 32.f;
static const float MIN_WINDOW = 0.0125f; // 80 Hz
static const float MAX_WINDOW = 16.f;

// External-clock div/mult of the window length (time knob steps).
static const float CLOCK_MULTS[9] = {16.f, 8.f, 4.f, 2.f, 1.f, 0.5f, 1.f / 3.f, 0.25f, 0.125f};

struct Bitrot : Module {
	enum ParamId {
		TIME_PARAM,
		REPEATS_PARAM,
		MIX_PARAM,
		BEND_PARAM,
		BREAK_PARAM,
		CORRUPT_PARAM,
		CLOCK_BTN_PARAM,
		MODE_BTN_PARAM,
		FREEZE_BTN_PARAM,
		BEND_BTN_PARAM,
		BREAK_BTN_PARAM,
		CORRUPT_BTN_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		TIME_INPUT,
		REPEATS_INPUT,
		MIX_INPUT,
		BEND_INPUT,
		BREAK_INPUT,
		CORRUPT_INPUT,
		BEND_GATE_INPUT,
		BREAK_GATE_INPUT,
		CORRUPT_GATE_INPUT,
		CLOCK_INPUT,
		FREEZE_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(CLOCK_LIGHT, 3),
		ENUMS(MODE_LIGHT, 3),
		FREEZE_LIGHT,
		ENUMS(BEND_LIGHT, 3),
		ENUMS(BREAK_LIGHT, 3),
		ENUMS(CORRUPT_LIGHT, 3),
		LIGHTS_LEN
	};

	std::vector<float> bufL, bufR;
	int bufLen = 0;
	int w = 0;

	// modes / states
	bool extClock = false;
	bool microMode = false;
	bool bendOn = false;
	bool breakOn = false;
	int corruptMode = 0;       // 0 decimate, 1 dropout, 2 destroy
	bool frozen = false;
	bool freezePending = false;
	bool microReverse = false;
	bool microBreakSilence = false;
	bool stereoUnique = false;
	bool freezeInstant = false;
	float windowing = 0.02f;   // glitch window crossfade fraction (menu)

	// chunk playback state (per channel)
	double playPos[2] = {};
	double rate[2] = {1.0, 1.0};
	double targetRate[2] = {1.0, 1.0};
	float slewTime[2] = {0.002f, 0.002f};
	bool silentChunk[2] = {};
	float silenceDuty = 0.f;
	double chunkPhase = 0.0;
	double chunkLen = 4800.0;

	// clock
	float clockPeriod = 0.5f;
	float lastEdge = 1e9f;
	float intClockPhase = 0.f;
	bool clockBlip = false;

	// corrupt state
	float heldL = 0.f, heldR = 0.f;
	int holdCount = 0;
	float dropTimer = 0.2f;
	float dropRemain = 0.f;

	dsp::SchmittTrigger clockTrig, freezeTrig, bendGateTrig, breakGateTrig, corruptGateTrig;
	dsp::BooleanTrigger clockBtn, modeBtn, freezeBtn, bendBtn, breakBtn, corruptBtn;
	dsp::ClockDivider lightDivider;

	Bitrot() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(TIME_PARAM, 0.f, 1.f, 0.55f, "Time (window length / clock div-mult)");
		configParam(REPEATS_PARAM, 0.f, 1.f, 0.f, "Repeats (buffer subdivisions)");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
		configParam(BEND_PARAM, 0.f, 1.f, 0.3f, "Bend (macro: tape mangling / micro: speed)");
		configParam(BREAK_PARAM, 0.f, 1.f, 0.3f, "Break (macro: glitches / micro: traverse-silence)");
		configParam(CORRUPT_PARAM, 0.f, 1.f, 0.f, "Corrupt amount");
		configButton(CLOCK_BTN_PARAM, "Clock source (internal/external)");
		configButton(MODE_BTN_PARAM, "Mode (macro/micro)");
		configButton(FREEZE_BTN_PARAM, "Freeze");
		configButton(BEND_BTN_PARAM, "Bend on/off (micro: reverse)");
		configButton(BREAK_BTN_PARAM, "Break on/off (micro: traverse/silence)");
		configButton(CORRUPT_BTN_PARAM, "Corrupt effect (decimate/dropout/destroy)");
		configInput(TIME_INPUT, "Time CV");
		configInput(REPEATS_INPUT, "Repeats CV");
		configInput(MIX_INPUT, "Mix CV");
		configInput(BEND_INPUT, "Bend CV");
		configInput(BREAK_INPUT, "Break CV");
		configInput(CORRUPT_INPUT, "Corrupt CV");
		configInput(BEND_GATE_INPUT, "Bend gate (toggle)");
		configInput(BREAK_GATE_INPUT, "Break gate (toggle)");
		configInput(CORRUPT_GATE_INPUT, "Corrupt gate (cycle effect)");
		configInput(CLOCK_INPUT, "Clock");
		configInput(FREEZE_INPUT, "Freeze gate");
		configInput(IN_L_INPUT, "Left audio");
		configInput(IN_R_INPUT, "Right audio (normalled to left)");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		lightDivider.setDivision(64);
		allocate(44100.f);
	}

	void allocate(float sr) {
		bufLen = (int)(sr * BUFFER_SECONDS);
		bufL.assign(bufLen, 0.f);
		bufR.assign(bufLen, 0.f);
		w = 0;
		playPos[0] = playPos[1] = 0.0;
		chunkPhase = 0.0;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	void onReset() override {
		extClock = false;
		microMode = false;
		bendOn = false;
		breakOn = false;
		corruptMode = 0;
		frozen = false;
		microReverse = false;
		microBreakSilence = false;
		stereoUnique = false;
		freezeInstant = false;
		windowing = 0.02f;
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "extClock", json_boolean(extClock));
		json_object_set_new(root, "microMode", json_boolean(microMode));
		json_object_set_new(root, "bendOn", json_boolean(bendOn));
		json_object_set_new(root, "breakOn", json_boolean(breakOn));
		json_object_set_new(root, "corruptMode", json_integer(corruptMode));
		json_object_set_new(root, "frozen", json_boolean(frozen));
		json_object_set_new(root, "microReverse", json_boolean(microReverse));
		json_object_set_new(root, "microBreakSilence", json_boolean(microBreakSilence));
		json_object_set_new(root, "stereoUnique", json_boolean(stereoUnique));
		json_object_set_new(root, "freezeInstant", json_boolean(freezeInstant));
		json_object_set_new(root, "windowing", json_real(windowing));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "extClock"))) extClock = json_boolean_value(j);
		if ((j = json_object_get(root, "microMode"))) microMode = json_boolean_value(j);
		if ((j = json_object_get(root, "bendOn"))) bendOn = json_boolean_value(j);
		if ((j = json_object_get(root, "breakOn"))) breakOn = json_boolean_value(j);
		if ((j = json_object_get(root, "corruptMode"))) corruptMode = json_integer_value(j);
		if ((j = json_object_get(root, "frozen"))) frozen = json_boolean_value(j);
		if ((j = json_object_get(root, "microReverse"))) microReverse = json_boolean_value(j);
		if ((j = json_object_get(root, "microBreakSilence"))) microBreakSilence = json_boolean_value(j);
		if ((j = json_object_get(root, "stereoUnique"))) stereoUnique = json_boolean_value(j);
		if ((j = json_object_get(root, "freezeInstant"))) freezeInstant = json_boolean_value(j);
		if ((j = json_object_get(root, "windowing"))) windowing = json_real_value(j);
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

	// Decide per-channel chunk behavior at a boundary.
	void decideChunk(int c, double windowSamp, double subLen, int n, float bendVal, float breakVal) {
		double anchor = (double)w - subLen;
		silentChunk[c] = false;

		if (microMode) {
			// micro: bend = direct playback speed, break = traverse or silence duty
			float oct = bendVal * 6.f - 3.f;
			float r = std::exp2(oct);
			targetRate[c] = microReverse ? -r : r;
			slewTime[c] = 0.002f;
			if (!microBreakSilence && n > 1) {
				int sel = (int)(breakVal * (n - 1) + 0.5f);
				anchor = (double)w - windowSamp + sel * subLen;
			}
			silenceDuty = microBreakSilence ? breakVal * 0.9f : 0.f;
		}
		else {
			silenceDuty = 0.f;
			// bend: tape-style speed/direction events
			if (bendOn && random::uniform() < bendVal * 0.7f) {
				float u = random::uniform();
				if (u < 0.3f)
					targetRate[c] = -1.0;
				else if (u < 0.5f)
					targetRate[c] = 2.0;
				else if (u < 0.7f)
					targetRate[c] = 0.5f;
				else if (u < 0.85f)
					targetRate[c] = -0.5f;
				else
					targetRate[c] = 0.02; // tape stop
				slewTime[c] = 0.002f + bendVal * 0.25f * random::uniform();
			}
			else {
				targetRate[c] = 1.0;
				slewTime[c] = 0.002f + (bendOn ? bendVal * 0.1f : 0.f);
			}
			// break: playhead jumps, ratchets, silence
			if (breakOn) {
				if (random::uniform() < breakVal * 0.8f) {
					int sel = random::u32() % std::max(1, n);
					anchor = (double)w - windowSamp + sel * subLen;
				}
				float silenceProb = std::max(0.f, breakVal - 0.4f) * 1.5f;
				silentChunk[c] = random::uniform() < silenceProb * 0.9f;
			}
		}

		playPos[c] = (rate[c] >= 0.0) ? anchor : anchor + subLen;
		while (playPos[c] < 0.0)
			playPos[c] += bufLen;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;
		if (bufLen == 0)
			return;

		// ---- buttons & gates ----
		if (clockBtn.process(params[CLOCK_BTN_PARAM].getValue() > 0.f))
			extClock = !extClock;
		if (modeBtn.process(params[MODE_BTN_PARAM].getValue() > 0.f))
			microMode = !microMode;
		bool bendPress = bendBtn.process(params[BEND_BTN_PARAM].getValue() > 0.f) ||
		                 bendGateTrig.process(inputs[BEND_GATE_INPUT].getVoltage(), 0.1f, 1.f);
		if (bendPress) {
			if (microMode)
				microReverse = !microReverse;
			else
				bendOn = !bendOn;
		}
		bool breakPress = breakBtn.process(params[BREAK_BTN_PARAM].getValue() > 0.f) ||
		                  breakGateTrig.process(inputs[BREAK_GATE_INPUT].getVoltage(), 0.1f, 1.f);
		if (breakPress) {
			if (microMode)
				microBreakSilence = !microBreakSilence;
			else
				breakOn = !breakOn;
		}
		if (corruptBtn.process(params[CORRUPT_BTN_PARAM].getValue() > 0.f) ||
		    corruptGateTrig.process(inputs[CORRUPT_GATE_INPUT].getVoltage(), 0.1f, 1.f))
			corruptMode = (corruptMode + 1) % 3;
		if (freezeBtn.process(params[FREEZE_BTN_PARAM].getValue() > 0.f)) {
			if (freezeInstant)
				frozen = !frozen;
			else
				freezePending = true; // takes effect at next chunk boundary
		}
		bool gateFrozen = inputs[FREEZE_INPUT].isConnected() && inputs[FREEZE_INPUT].getVoltage() > 0.5f;
		bool isFrozen = frozen || gateFrozen;

		// ---- clock ----
		lastEdge += dt;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (lastEdge < 8.f && lastEdge > 0.0005f)
				clockPeriod = clamp(lastEdge, 0.0005f, 8.f);
			lastEdge = 0.f;
		}

		// ---- window length ----
		float timeVal = cvParam(TIME_PARAM, TIME_INPUT);
		float windowSec;
		if (extClock) {
			int idx = clamp((int)(timeVal * 8.999f), 0, 8);
			windowSec = clamp(clockPeriod * CLOCK_MULTS[idx], MIN_WINDOW, MAX_WINDOW);
		}
		else {
			windowSec = MAX_WINDOW * std::pow(MIN_WINDOW / MAX_WINDOW, timeVal);
		}
		intClockPhase += dt / windowSec;
		if (intClockPhase >= 1.f) {
			intClockPhase -= 1.f;
			clockBlip = true;
		}

		// ---- record ----
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f : inL;
		if (!isFrozen) {
			bufL[w] = inL;
			bufR[w] = inR;
			w++;
			if (w >= bufLen)
				w = 0;
		}

		// ---- chunk boundaries ----
		float repeats = cvParam(REPEATS_PARAM, REPEATS_INPUT);
		int n = (int)std::round(std::exp2(repeats * 5.f)); // 1..32 subdivisions
		float bendVal = cvParam(BEND_PARAM, BEND_INPUT);
		float breakVal = cvParam(BREAK_PARAM, BREAK_INPUT);
		double windowSamp = (double)windowSec * sr;
		chunkPhase += 1.0;
		if (chunkPhase >= chunkLen) {
			chunkPhase = 0.0;
			if (freezePending) {
				frozen = !frozen;
				freezePending = false;
			}
			double subLen = std::max(64.0, windowSamp / n);
			chunkLen = subLen;
			decideChunk(0, windowSamp, subLen, n, bendVal, breakVal);
			if (stereoUnique) {
				decideChunk(1, windowSamp, subLen, n, bendVal, breakVal);
			}
			else {
				targetRate[1] = targetRate[0];
				slewTime[1] = slewTime[0];
				silentChunk[1] = silentChunk[0];
				playPos[1] = playPos[0];
			}
		}

		// ---- playback ----
		float wetL = 0.f, wetR = 0.f;
		float fadeSamp = std::max(4.f, (float)chunkLen * clamp(windowing, 0.f, 0.5f));
		float win = std::min(1.f, std::min((float)chunkPhase, (float)(chunkLen - chunkPhase)) / fadeSamp);
		win = win * win * (3.f - 2.f * win); // smoothstep
		if (silenceDuty > 0.f && chunkPhase > chunkLen * (1.f - silenceDuty))
			win = 0.f;
		for (int c = 0; c < 2; c++) {
			float k = 1.f - std::exp(-dt / std::max(0.0005f, slewTime[c]));
			rate[c] += (targetRate[c] - rate[c]) * k;
			float y = 0.f;
			if (!silentChunk[c])
				y = readBuf(c == 0 ? bufL : bufR, playPos[c]) * win;
			playPos[c] += rate[c];
			if (playPos[c] >= bufLen)
				playPos[c] -= bufLen;
			else if (playPos[c] < 0.0)
				playPos[c] += bufLen;
			if (c == 0)
				wetL = y;
			else
				wetR = y;
		}

		// ---- corrupt (end of chain) ----
		float crpt = cvParam(CORRUPT_PARAM, CORRUPT_INPUT);
		if (crpt > 0.001f) {
			switch (corruptMode) {
				case 0: { // decimate
					int holdN = 1 + (int)(crpt * crpt * 30.f);
					if (++holdCount >= holdN) {
						holdCount = 0;
						float bits = 16.f - crpt * 13.f;
						float q = std::pow(2.f, bits);
						heldL = std::round(clamp(wetL, -1.2f, 1.2f) * q) / q;
						heldR = std::round(clamp(wetR, -1.2f, 1.2f) * q) / q;
					}
					wetL = heldL;
					wetR = heldR;
				} break;
				case 1: { // dropout
					dropTimer -= dt;
					if (dropTimer <= 0.f) {
						float rate_ = 0.3f + crpt * 14.f;
						dropTimer = -std::log(1.f - random::uniform() * 0.999f) / rate_;
						dropRemain = 0.3f * std::pow(0.05f, crpt); // long->short
					}
					if (dropRemain > 0.f) {
						dropRemain -= dt;
						wetL = 0.f;
						wetR = 0.f;
					}
				} break;
				default: { // destroy: saturate then hard clip
					float drive = 1.f + crpt * 7.f;
					wetL = std::tanh(wetL * drive);
					wetR = std::tanh(wetR * drive);
					if (crpt > 0.5f) {
						float th = 1.f - (crpt - 0.5f) * 1.7f;
						wetL = clamp(wetL, -th, th) / th;
						wetR = clamp(wetR, -th, th) / th;
					}
				} break;
			}
		}

		// ---- mix ----
		float mix = cvParam(MIX_PARAM, MIX_INPUT);
		float outL = inL * (1.f - mix) + wetL * mix;
		float outR = inR * (1.f - mix) + wetR * mix;
		outputs[OUT_L_OUTPUT].setVoltage(clamp(outL * 5.f, -12.f, 12.f));
		outputs[OUT_R_OUTPUT].setVoltage(clamp(outR * 5.f, -12.f, 12.f));

		// ---- lights ----
		if (lightDivider.process()) {
			bool blink = extClock ? (clockTrig.isHigh() || lastEdge < 0.05f) : (intClockPhase < 0.12f);
			float r = extClock ? 1.f : 0.f, g = extClock ? 1.f : 0.2f, b = 1.f;
			lights[CLOCK_LIGHT + 0].setBrightness(blink ? r : 0.f);
			lights[CLOCK_LIGHT + 1].setBrightness(blink ? g : 0.f);
			lights[CLOCK_LIGHT + 2].setBrightness(blink ? b : 0.f);
			clockBlip = false;
			// mode: blue macro, green micro
			lights[MODE_LIGHT + 0].setBrightness(0.f);
			lights[MODE_LIGHT + 1].setBrightness(microMode ? 1.f : 0.f);
			lights[MODE_LIGHT + 2].setBrightness(microMode ? 0.f : 1.f);
			lights[FREEZE_LIGHT].setBrightness(isFrozen ? 1.f : (freezePending ? 0.4f : 0.f));
			// bend: macro on/off blue; micro: blue fwd, green reverse
			bool bendLit = microMode || bendOn;
			lights[BEND_LIGHT + 0].setBrightness(0.f);
			lights[BEND_LIGHT + 1].setBrightness(bendLit && microMode && microReverse ? 1.f : 0.f);
			lights[BEND_LIGHT + 2].setBrightness(bendLit && !(microMode && microReverse) ? 1.f : 0.f);
			// break: macro on/off blue; micro: blue traverse, green silence
			bool breakLit = microMode || breakOn;
			lights[BREAK_LIGHT + 0].setBrightness(0.f);
			lights[BREAK_LIGHT + 1].setBrightness(breakLit && microMode && microBreakSilence ? 1.f : 0.f);
			lights[BREAK_LIGHT + 2].setBrightness(breakLit && !(microMode && microBreakSilence) ? 1.f : 0.f);
			// corrupt: blue decimate, green dropout, red destroy
			lights[CORRUPT_LIGHT + 0].setBrightness(corruptMode == 2);
			lights[CORRUPT_LIGHT + 1].setBrightness(corruptMode == 1);
			lights[CORRUPT_LIGHT + 2].setBrightness(corruptMode == 0);
		}
	}
};

struct BitrotWidget : ModuleWidget {
	BitrotWidget(Bitrot* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Bitrot.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace blayout;

		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(TIME_X, TIME_Y)), module, Bitrot::TIME_PARAM));

		static const int topBtns[3] = {Bitrot::CLOCK_BTN_PARAM, Bitrot::MODE_BTN_PARAM, Bitrot::FREEZE_BTN_PARAM};
		addParam(createParamCentered<VCVButton>(mm2px(Vec(TOP_BTN_X[0], TOP_BTN_Y)), module, topBtns[0]));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(TOP_BTN_X[0], TOP_LED_Y)), module, Bitrot::CLOCK_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(TOP_BTN_X[1], TOP_BTN_Y)), module, topBtns[1]));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(TOP_BTN_X[1], TOP_LED_Y)), module, Bitrot::MODE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(TOP_BTN_X[2], TOP_BTN_Y)), module, topBtns[2]));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(TOP_BTN_X[2], TOP_LED_Y)), module, Bitrot::FREEZE_LIGHT));

		static const int k2[4] = {Bitrot::REPEATS_PARAM, Bitrot::BEND_PARAM, Bitrot::BREAK_PARAM, Bitrot::CORRUPT_PARAM};
		for (int i = 0; i < 4; i++)
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(K2_X[i], K2_Y)), module, k2[i]));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(MIX_X, MIX_Y)), module, Bitrot::MIX_PARAM));

		static const int enBtns[3] = {Bitrot::BEND_BTN_PARAM, Bitrot::BREAK_BTN_PARAM, Bitrot::CORRUPT_BTN_PARAM};
		static const int enLights[3] = {Bitrot::BEND_LIGHT, Bitrot::BREAK_LIGHT, Bitrot::CORRUPT_LIGHT};
		for (int i = 0; i < 3; i++) {
			addParam(createParamCentered<VCVButton>(mm2px(Vec(EN_BTN_X[i], EN_BTN_Y)), module, enBtns[i]));
			addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(EN_BTN_X[i] + EN_LED_DX, EN_BTN_Y)), module, enLights[i]));
		}

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[0], R1)), module, Bitrot::TIME_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[1], R1)), module, Bitrot::BEND_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[2], R1)), module, Bitrot::BREAK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[3], R1)), module, Bitrot::CORRUPT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[4], R1)), module, Bitrot::CLOCK_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[0], R2)), module, Bitrot::REPEATS_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[1], R2)), module, Bitrot::BEND_GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[2], R2)), module, Bitrot::BREAK_GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[3], R2)), module, Bitrot::CORRUPT_GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[4], R2)), module, Bitrot::FREEZE_INPUT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[0], R3)), module, Bitrot::MIX_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[1], R3)), module, Bitrot::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J[2], R3)), module, Bitrot::IN_R_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J[3], R3)), module, Bitrot::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J[4], R3)), module, Bitrot::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Bitrot* module = getModule<Bitrot>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Corrupt effect",
			{"Decimate", "Dropout", "Destroy"}, &module->corruptMode));
		menu->addChild(createBoolPtrMenuItem("Micro mode", "", &module->microMode));
		menu->addChild(createBoolPtrMenuItem("External clock", "", &module->extClock));
		menu->addChild(createBoolPtrMenuItem("Stereo unique randomness", "", &module->stereoUnique));
		menu->addChild(createBoolPtrMenuItem("Instant freeze (else clock-synced)", "", &module->freezeInstant));
		// glitch windowing amount
		struct WindowingQuantity : Quantity {
			Bitrot* m;
			void setValue(float v) override { m->windowing = clamp(v, 0.f, 0.5f); }
			float getValue() override { return m->windowing; }
			float getMaxValue() override { return 0.5f; }
			float getDefaultValue() override { return 0.02f; }
			std::string getLabel() override { return "Glitch windowing"; }
			std::string getUnit() override { return "%"; }
			float getDisplayValue() override { return getValue() * 200.f; }
			void setDisplayValue(float v) override { setValue(v / 200.f); }
		};
		struct WindowingSlider : ui::Slider {
			WindowingSlider(Bitrot* m) {
				WindowingQuantity* q = new WindowingQuantity;
				q->m = m;
				quantity = q;
				box.size.x = 200.f;
			}
			~WindowingSlider() { delete quantity; }
		};
		menu->addChild(new WindowingSlider(module));
	}
};

Model* modelBitrot = createModel<Bitrot, BitrotWidget>("Bitrot");
