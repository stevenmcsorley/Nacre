// Lariat — a CV/gate phrase looper for VCV Rack. Sits between a controller
// (e.g. MIDI-CV) and a voice: passes V/oct, gate, and two aux CV lanes
// through while idle, records them as you play, then loops the captured
// phrase out of the matching jacks. Because it loops control voltages
// rather than audio, the SPEED control time-stretches the phrase without
// changing pitch. Original implementation.

#include "plugin.hpp"
#include "layout_lariat.hpp"

static const int NUM_LANES = 4;
static const float REC_SR = 4000.f;       // CV sampling rate
static const float MAX_SECONDS = 240.f;   // max phrase length

struct Lariat : Module {
	enum ParamId {
		REC_PARAM,
		PLAY_PARAM,
		SPEED_PARAM,
		QUANT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		REC_INPUT,
		PLAY_INPUT,
		CLOCK_INPUT,
		RESET_INPUT,
		ENUMS(LANE_INPUT, NUM_LANES),
		INPUTS_LEN
	};
	enum OutputId {
		EOL_OUTPUT,
		ENUMS(LANE_OUTPUT, NUM_LANES),
		OUTPUTS_LEN
	};
	enum LightId {
		REC_LIGHT,
		PLAY_LIGHT,
		QUANT_LIGHT,
		LIGHTS_LEN
	};

	std::vector<float> lanes[NUM_LANES];
	int maxFrames = 0;
	int recLen = 0;
	bool hasLoop = false;

	bool recording = false;
	bool playing = false;
	int recWrite = 0;
	double recPhase = 0.0;
	double playPos = 0.0;

	float clockPeriod = 0.5f;
	float lastEdge = 1e9f;
	bool clockSeen = false;
	int loopBeats = 0;   // loop length in clock periods (when quantized)
	int syncTick = -1;   // -1 = align loop start to the next clock edge

	bool quantizeVoct = false;
	bool timeQuantize = true; // snap note timing to the clock grid on record
	dsp::SchmittTrigger recTrig, playTrig, resetTrig, clockTrig;
	dsp::BooleanTrigger recBtn, playBtn, quantBtn;
	dsp::PulseGenerator eolPulse;
	dsp::ClockDivider lightDivider;

	Lariat() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configButton(REC_PARAM, "Record");
		configButton(PLAY_PARAM, "Play/stop loop");
		configButton(QUANT_PARAM, "Quantize V/oct lane to semitones");
		configParam(SPEED_PARAM, -2.f, 2.f, 0.f, "Speed", "x", 2.f);
		configInput(REC_INPUT, "Record gate");
		configInput(PLAY_INPUT, "Play gate (toggle)");
		configInput(CLOCK_INPUT, "Clock (quantizes loop length)");
		configInput(RESET_INPUT, "Reset to loop start");
		configInput(LANE_INPUT + 0, "V/oct");
		configInput(LANE_INPUT + 1, "Gate");
		configInput(LANE_INPUT + 2, "CV 1");
		configInput(LANE_INPUT + 3, "CV 2");
		configOutput(EOL_OUTPUT, "End of loop trigger");
		configOutput(LANE_OUTPUT + 0, "V/oct");
		configOutput(LANE_OUTPUT + 1, "Gate");
		configOutput(LANE_OUTPUT + 2, "CV 1");
		configOutput(LANE_OUTPUT + 3, "CV 2");
		for (int k = 0; k < NUM_LANES; k++)
			configBypass(LANE_INPUT + k, LANE_OUTPUT + k);
		lightDivider.setDivision(64);
		maxFrames = (int)(REC_SR * MAX_SECONDS);
		for (int k = 0; k < NUM_LANES; k++)
			lanes[k].assign(maxFrames, 0.f);
	}

	void onReset() override {
		recording = false;
		playing = false;
		hasLoop = false;
		recLen = 0;
		recWrite = 0;
		playPos = 0.0;
	}

	void startRecording() {
		recording = true;
		playing = false;
		recWrite = 0;
		recPhase = 0.0;
	}

	void stopRecording() {
		recording = false;
		if (recWrite < (int)(REC_SR * 0.05f)) {
			// too short to be a phrase; ignore
			return;
		}
		recLen = recWrite;
		// quantize length to whole clock periods if a clock has been seen
		loopBeats = 0;
		if (inputs[CLOCK_INPUT].isConnected() && clockSeen) {
			float loopSec = recLen / REC_SR;
			int n = std::max(1, (int)std::round(loopSec / clockPeriod));
			int target = (int)(n * clockPeriod * REC_SR);
			target = std::min(target, maxFrames);
			if (target > recLen) {
				// pad: hold CVs, close the gate
				for (int i = recLen; i < target; i++) {
					lanes[0][i] = lanes[0][recLen - 1];
					lanes[1][i] = 0.f;
					lanes[2][i] = lanes[2][recLen - 1];
					lanes[3][i] = lanes[3][recLen - 1];
				}
			}
			recLen = target;
			loopBeats = n;
			// snap the played notes onto the clock grid: rebuild the lanes
			// from a note list with quantized onsets
			if (timeQuantize)
				quantizeNoteTiming();
		}
		hasLoop = true;
		playing = true;
		playPos = 0.0;
		syncTick = -1; // re-anchor to the next clock edge
	}

	// Rebuild all four lanes from a note list whose onsets are snapped to
	// the nearest clock tick. CV values are sampled at each note's onset.
	void quantizeNoteTiming() {
		if (recLen <= 0 || loopBeats <= 0)
			return;
		float grid = clockPeriod * REC_SR; // one tick
		struct Note {
			int start, len;
			float voct, cv1, cv2;
		};
		std::vector<Note> notes;
		bool high = false;
		int onset = 0;
		for (int i = 0; i < recLen; i++) {
			bool g = lanes[1][i] > 1.f;
			if (g && !high) {
				high = true;
				onset = i;
			}
			else if (!g && high) {
				high = false;
				int probe = std::min(onset + (int)(REC_SR * 0.002f), recLen - 1);
				notes.push_back({onset, std::max(i - onset, (int)(REC_SR * 0.01f)),
				                 lanes[0][probe], lanes[2][probe], lanes[3][probe]});
			}
		}
		if (high) { // note held across the loop end
			int probe = std::min(onset + (int)(REC_SR * 0.002f), recLen - 1);
			notes.push_back({onset, recLen - onset, lanes[0][probe], lanes[2][probe], lanes[3][probe]});
		}
		if (notes.empty())
			return;
		// re-render: gates closed, CVs hold their previous values
		float holdV = notes[0].voct, hold1 = notes[0].cv1, hold2 = notes[0].cv2;
		for (int i = 0; i < recLen; i++)
			lanes[1][i] = 0.f;
		for (Note& n : notes) {
			int q = (int)(std::round(n.start / grid) * grid);
			q = ((q % recLen) + recLen) % recLen;
			int len = std::min(n.len, recLen - 1);
			for (int k = 0; k < len; k++) {
				int i = (q + k) % recLen;
				lanes[1][i] = 5.f;
			}
			// hold the note's CVs from its onset to the next note's onset
			n.start = q;
		}
		std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
		int ni = 0;
		// values before the first note wrap from the last note
		holdV = notes.back().voct;
		hold1 = notes.back().cv1;
		hold2 = notes.back().cv2;
		for (int i = 0; i < recLen; i++) {
			while (ni < (int)notes.size() && notes[ni].start == i) {
				holdV = notes[ni].voct;
				hold1 = notes[ni].cv1;
				hold2 = notes[ni].cv2;
				ni++;
			}
			lanes[0][i] = holdV;
			lanes[2][i] = hold1;
			lanes[3][i] = hold2;
		}
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;

		// ---- clock ----
		lastEdge += dt;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (lastEdge < 8.f && lastEdge > 0.001f) {
				clockPeriod = clamp(lastEdge, 0.001f, 8.f);
				clockSeen = true;
			}
			lastEdge = 0.f;
			// phase-lock: restart the loop on its downbeat tick, so the
			// phrase never drifts against the clock. Only when the speed
			// works out to a whole number of ticks per loop.
			if (playing && hasLoop && loopBeats > 0) {
				float speed = std::exp2(params[SPEED_PARAM].getValue());
				float ticksF = loopBeats / std::max(0.01f, speed);
				int cycleTicks = std::max(1, (int)std::round(ticksF));
				if (std::fabs(ticksF - cycleTicks) < 0.02f) {
					syncTick++;
					if (syncTick < 0)
						syncTick = 0;
					if (syncTick % cycleTicks == 0) {
						if (playPos > 1.0)
							eolPulse.trigger(0.002f); // loop boundary
						playPos = 0.0;
					}
				}
			}
		}

		// ---- transport ----
		if (recBtn.process(params[REC_PARAM].getValue() > 0.f) ||
		    recTrig.process(inputs[REC_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (recording)
				stopRecording();
			else
				startRecording();
		}
		if (playBtn.process(params[PLAY_PARAM].getValue() > 0.f) ||
		    playTrig.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (recording) {
				// finish the take and start looping (otherwise playback
				// would read lanes that recording is still overwriting)
				stopRecording();
			}
			else if (hasLoop) {
				playing = !playing;
				if (playing) {
					playPos = 0.0;
					syncTick = -1; // align to the next clock edge
				}
			}
		}
		if (quantBtn.process(params[QUANT_PARAM].getValue() > 0.f))
			quantizeVoct = !quantizeVoct;
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			playPos = 0.0;
			syncTick = -1; // re-anchor the clock sync
		}

		// ---- record lanes at REC_SR ----
		if (recording) {
			recPhase += REC_SR * dt;
			while (recPhase >= 1.0) {
				recPhase -= 1.0;
				if (recWrite < maxFrames) {
					for (int k = 0; k < NUM_LANES; k++)
						lanes[k][recWrite] = inputs[LANE_INPUT + k].getVoltage();
					recWrite++;
				}
				else {
					stopRecording();
					break;
				}
			}
		}

		// ---- outputs ----
		if (playing && hasLoop && recLen > 0) {
			float speed = std::exp2(params[SPEED_PARAM].getValue());
			playPos += REC_SR * dt * speed;
			if (playPos >= recLen) {
				playPos = std::fmod(playPos, (double)recLen);
				eolPulse.trigger(0.002f);
			}
			int idx = clamp((int)playPos, 0, recLen - 1);
			for (int k = 0; k < NUM_LANES; k++) {
				float v = lanes[k][idx];
				if (k == 0 && quantizeVoct)
					v = std::round(v * 12.f) / 12.f; // semitone quantize
				if (k == 1)
					v = (v > 1.f) ? 10.f : 0.f; // clean gate reconstruction
				outputs[LANE_OUTPUT + k].setVoltage(v);
			}
		}
		else {
			// pass through while idle or recording (monitor what you play)
			for (int k = 0; k < NUM_LANES; k++) {
				float v = inputs[LANE_INPUT + k].getVoltage();
				if (k == 0 && quantizeVoct)
					v = std::round(v * 12.f) / 12.f;
				outputs[LANE_OUTPUT + k].setVoltage(v);
			}
		}
		outputs[EOL_OUTPUT].setVoltage(eolPulse.process(dt) ? 10.f : 0.f);

		// ---- lights ----
		if (lightDivider.process()) {
			lights[REC_LIGHT].setBrightness(recording ? 1.f : 0.f);
			float pb = playing ? ((playPos / std::max(1, recLen)) < 0.06 ? 1.f : 0.6f) : (hasLoop ? 0.12f : 0.f);
			lights[PLAY_LIGHT].setBrightness(pb);
			lights[QUANT_LIGHT].setBrightness(quantizeVoct ? 1.f : 0.f);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "playing", json_boolean(playing));
		json_object_set_new(root, "hasLoop", json_boolean(hasLoop));
		json_object_set_new(root, "recLen", json_integer(recLen));
		json_object_set_new(root, "loopBeats", json_integer(loopBeats));
		json_object_set_new(root, "clockPeriod", json_real(clockPeriod));
		json_object_set_new(root, "quantizeVoct", json_boolean(quantizeVoct));
		json_object_set_new(root, "timeQuantize", json_boolean(timeQuantize));
		// store the recorded lanes (compactly, as arrays of reals)
		if (hasLoop && recLen > 0 && recLen <= (int)(REC_SR * 60.f)) {
			json_t* lanesJ = json_array();
			for (int k = 0; k < NUM_LANES; k++) {
				json_t* arr = json_array();
				for (int i = 0; i < recLen; i++)
					json_array_append_new(arr, json_real(lanes[k][i]));
				json_array_append_new(lanesJ, arr);
			}
			json_object_set_new(root, "lanes", lanesJ);
		}
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "playing"))) playing = json_boolean_value(j);
		if ((j = json_object_get(root, "hasLoop"))) hasLoop = json_boolean_value(j);
		if ((j = json_object_get(root, "recLen"))) recLen = clamp((int)json_integer_value(j), 0, maxFrames);
		if ((j = json_object_get(root, "loopBeats"))) loopBeats = json_integer_value(j);
		if ((j = json_object_get(root, "clockPeriod"))) clockPeriod = json_real_value(j);
		if ((j = json_object_get(root, "quantizeVoct"))) quantizeVoct = json_boolean_value(j);
		if ((j = json_object_get(root, "timeQuantize"))) timeQuantize = json_boolean_value(j);
		json_t* lanesJ = json_object_get(root, "lanes");
		if (lanesJ) {
			for (int k = 0; k < NUM_LANES && k < (int)json_array_size(lanesJ); k++) {
				json_t* arr = json_array_get(lanesJ, k);
				int n = std::min(recLen, (int)json_array_size(arr));
				for (int i = 0; i < n; i++)
					lanes[k][i] = json_real_value(json_array_get(arr, i));
			}
		}
		else {
			hasLoop = false;
			playing = false;
		}
		playPos = 0.0;
	}
};

struct LariatWidget : ModuleWidget {
	LariatWidget(Lariat* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Lariat.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace llayout;

		// lights live inside the buttons (keeps the header art clear)
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedLight>>>(mm2px(Vec(REC_BTN_X, REC_BTN_Y)), module, Lariat::REC_PARAM, Lariat::REC_LIGHT));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<GreenLight>>>(mm2px(Vec(PLAY_BTN_X, PLAY_BTN_Y)), module, Lariat::PLAY_PARAM, Lariat::PLAY_LIGHT));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SPEED_X, SPEED_Y)), module, Lariat::SPEED_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[0], R1)), module, Lariat::REC_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[1], R1)), module, Lariat::PLAY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[2], R1)), module, Lariat::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(C3[0], R2)), module, Lariat::RESET_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(C3[1], R2)), module, Lariat::QUANT_PARAM));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(C3[1] + 5.6f, R2 - 4.6f)), module, Lariat::QUANT_LIGHT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(C3[2], R2)), module, Lariat::EOL_OUTPUT));

		for (int k = 0; k < NUM_LANES; k++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IN_X, IO_Y[k])), module, Lariat::LANE_INPUT + k));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(OUT_X, IO_Y[k])), module, Lariat::LANE_OUTPUT + k));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Lariat* module = getModule<Lariat>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Quantize note timing to clock (on record)", "", &module->timeQuantize));
		menu->addChild(createBoolPtrMenuItem("Quantize V/oct to semitones", "", &module->quantizeVoct));
	}
};

Model* modelLariat = createModel<Lariat, LariatWidget>("Lariat");
