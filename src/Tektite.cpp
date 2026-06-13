// Tektite — a stereo performance looper for VCV Rack, inspired by the
// architecture of the Qu-Bit Stardust (varispeed tape-style loop with
// start/size windowing, slices with probabilistic transformations,
// layered recording modes, always-on effect pairs, and a multi-mode
// CV/gate output). Original DSP implementation; the reverb stage uses
// Émilie Gillet's MIT-licensed Clouds reverb (see eurorack/).

#include "plugin.hpp"
#include "layout_tektite.hpp"
#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/reverb.h"

static const float BUFFER_SECONDS = 60.f;

struct Tektite : Module {
	enum ParamId {
		FLUTTER_PARAM,
		HISS_PARAM,
		VARISPEED_PARAM,
		INERTIA_PARAM,
		START_PARAM,
		SIZE_PARAM,
		MIX_PARAM,
		LEVEL_PARAM,
		SKIP_PARAM,
		SLICE_PARAM,
		RECORD_PARAM,
		PLAY_PARAM,
		RESET_PARAM,
		REVERSE_PARAM,
		FREEZE_PARAM,
		LOOPMODE_PARAM,
		FXMODE_PARAM,
		ERASE_PARAM,
		UNDO_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		LEVEL_INPUT,
		FLUTTER_INPUT,
		HISS_INPUT,
		MIX_INPUT,
		START_INPUT,
		SIZE_INPUT,
		SLICE_INPUT,
		SKIP_INPUT,
		RECORD_INPUT,
		PLAY_INPUT,
		RESET_INPUT,
		REVERSE_INPUT,
		FREEZE_INPUT,
		CLOCK_INPUT,
		VOCT_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		NOVA_OUTPUT,
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		FREEZE_LIGHT,
		ENUMS(LOOPMODE_LIGHT, 3),
		ENUMS(FXMODE_LIGHT, 3),
		RECORD_LIGHT,
		PLAY_LIGHT,
		RESET_LIGHT,
		REVERSE_LIGHT,
		LIGHTS_LEN
	};

	std::vector<float> bufL, bufR;
	std::vector<float> undoL, undoR;
	int bufLen = 0;
	int recLen = 0;
	bool hasLoop = false;
	bool hasUndo = false;

	bool recording = false;
	bool playing = true;
	bool reversed = false;
	bool frozen = false;
	int loopMode = 0;  // 0 sound-on-sound, 1 replace, 2 fripper, 3 resample
	int fxMode = 0;    // 0 tape, 1 digital, 2 reverb, 3 filter
	int novaMode = 0;  // 0 loop+slice gates, 1 loop gates, 2 slice gates, 3 position CV
	int speedQuant = 0;
	// hardware secondary functions
	bool clockMode = true;    // sync transport + slices to clock pulses
	int punchMode = 0;        // 0 none, 1 immediate full, 2 queued full
	float inertiaSlope = 0.f; // -1 lag on decel only .. +1 lag on accel only
	bool fxOnDry = false;     // pre/post effect chain (shift+FX on hardware)
	int mixCurve = 0;         // 0 constant power, 1 linear, 2 transition
	int reverbType = 0;       // 0 normal, 1 bright, 2 dark
	bool recQueued = false;
	bool pendPlay = false, pendReset = false, pendReverse = false, pendFreeze = false;
	float eraseHold = -1.f;

	float fxAmt[8] = {};   // [mode*2 + (0 flutter, 1 hiss)]
	float fxKnobPrev[2] = {};
	int fxKnobContext = -1;

	// playback
	double pos = 0.0;       // position within loop window, samples
	float speed = 1.f;      // current (slewed) speed
	int recWrite = 0;       // first-recording write index
	int resampleLen = 0;
	float resetBlink = 0.f;

	// slices
	int sliceIdx = -1;
	float sliceOffset = 0.f;
	bool sliceRev = false;
	float sliceGainL = 1.f, sliceGainR = 1.f;
	float sliceRatio = 1.f;
	float sliceRatioTarget = 1.f;
	bool sliceLag = false;

	// freeze
	double freezeStart = 0.0;
	float freezeLen = 3000.f;
	double freezePh = 0.0;

	// nova gate lengths (50% duty of the event period, like hardware)
	float novaHalfSlice = 0.005f;
	float novaHalfLoop = 0.005f;
	float blinkPh = 0.f;

	// fx state
	double wowPh = 0.f;
	float wowNoise = 0.f;
	float heldL = 0.f, heldR = 0.f;
	int holdCount = 0;
	float compEnv = 0.f;
	float hpL[2] = {}, hpR[2] = {};
	float lpL[2] = {}, lpR[2] = {};
	uint32_t noiseState = 22222;

	float clockPeriod = 0.5f;
	float lastEdge = 1e9f;

	clouds::Reverb reverb;
	uint16_t reverbBuffer[16384] = {};

	dsp::SchmittTrigger recTrig, playTrig, resetTrig, revTrig, frzTrig, clockTrig;
	dsp::BooleanTrigger recBtn, playBtn, resetBtn, revBtn, frzBtn, loopBtn, fxBtn, eraseBtn, undoBtn;
	dsp::PulseGenerator novaPulse;
	dsp::ClockDivider lightDivider;

	Tektite() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(FLUTTER_PARAM, 0.f, 1.f, 0.f, "Effect A (per FX mode)");
		configParam(HISS_PARAM, 0.f, 1.f, 0.f, "Effect B (per FX mode)");
		configParam(VARISPEED_PARAM, -2.f, 3.f, 0.f, "Varispeed", " oct");
		configParam(INERTIA_PARAM, 0.f, 1.f, 0.f, "Inertia (tape lag)");
		configParam(START_PARAM, 0.f, 1.f, 0.f, "Loop start");
		configParam(SIZE_PARAM, 0.f, 1.f, 1.f, "Loop size");
		configParam(MIX_PARAM, 0.f, 1.f, 0.5f, "Mix", "%", 0.f, 100.f);
		configParam(LEVEL_PARAM, 0.f, 1.f, 0.5f, "Input level (saturates past center)");
		configParam(SKIP_PARAM, 0.f, 1.f, 0.f, "Skip (slice transformations)");
		configParam(SLICE_PARAM, 0.f, 1.f, 0.f, "Slices");
		configButton(RECORD_PARAM, "Record");
		configButton(PLAY_PARAM, "Play/pause");
		configButton(RESET_PARAM, "Reset to loop start");
		configButton(REVERSE_PARAM, "Reverse");
		configButton(FREEZE_PARAM, "Freeze");
		configButton(LOOPMODE_PARAM, "Loop mode (sound-on-sound/replace/fripper/resample)");
		configButton(FXMODE_PARAM, "FX mode (tape/digital/reverb/filter)");
		configButton(ERASE_PARAM, "Erase loop");
		configButton(UNDO_PARAM, "Undo last recording");
		configInput(LEVEL_INPUT, "Input level CV");
		configInput(FLUTTER_INPUT, "Effect A CV");
		configInput(HISS_INPUT, "Effect B CV");
		configInput(MIX_INPUT, "Mix CV");
		configInput(START_INPUT, "Loop start CV");
		configInput(SIZE_INPUT, "Loop size CV");
		configInput(SLICE_INPUT, "Slices CV");
		configInput(SKIP_INPUT, "Skip CV");
		configInput(RECORD_INPUT, "Record gate");
		configInput(PLAY_INPUT, "Play/pause gate");
		configInput(RESET_INPUT, "Reset gate");
		configInput(REVERSE_INPUT, "Reverse gate");
		configInput(FREEZE_INPUT, "Freeze gate");
		configInput(CLOCK_INPUT, "Clock");
		configInput(VOCT_INPUT, "Varispeed (1V/oct)");
		configInput(IN_L_INPUT, "Left audio (normalled to right)");
		configInput(IN_R_INPUT, "Right audio");
		configOutput(NOVA_OUTPUT, "Nova (loop/slice gates or position CV)");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		lightDivider.setDivision(64);
		reverb.Init(reverbBuffer);
		reverb.set_diffusion(0.7f);
		allocate(44100.f);
	}

	void allocate(float sr) {
		bufLen = (int)(sr * BUFFER_SECONDS);
		bufL.assign(bufLen, 0.f);
		bufR.assign(bufLen, 0.f);
		undoL.assign(bufLen, 0.f);
		undoR.assign(bufLen, 0.f);
		erase();
	}

	void erase() {
		std::fill(bufL.begin(), bufL.end(), 0.f);
		std::fill(bufR.begin(), bufR.end(), 0.f);
		hasLoop = false;
		hasUndo = false;
		recording = false;
		recLen = 0;
		recWrite = 0;
		pos = 0.0;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	void onReset() override {
		loopMode = 0;
		fxMode = 0;
		novaMode = 0;
		speedQuant = 0;
		for (int i = 0; i < 8; i++)
			fxAmt[i] = 0.f;
		playing = true;
		reversed = false;
		frozen = false;
		erase();
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "loopMode", json_integer(loopMode));
		json_object_set_new(root, "fxMode", json_integer(fxMode));
		json_object_set_new(root, "novaMode", json_integer(novaMode));
		json_object_set_new(root, "speedQuant", json_integer(speedQuant));
		json_object_set_new(root, "playing", json_boolean(playing));
		json_object_set_new(root, "reversed", json_boolean(reversed));
		json_object_set_new(root, "clockMode", json_boolean(clockMode));
		json_object_set_new(root, "punchMode", json_integer(punchMode));
		json_object_set_new(root, "inertiaSlope", json_real(inertiaSlope));
		json_object_set_new(root, "fxOnDry", json_boolean(fxOnDry));
		json_object_set_new(root, "mixCurve", json_integer(mixCurve));
		json_object_set_new(root, "reverbType", json_integer(reverbType));
		json_t* fx = json_array();
		for (int i = 0; i < 8; i++)
			json_array_append_new(fx, json_real(fxAmt[i]));
		json_object_set_new(root, "fxAmt", fx);
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "loopMode"))) loopMode = json_integer_value(j);
		if ((j = json_object_get(root, "fxMode"))) fxMode = json_integer_value(j);
		if ((j = json_object_get(root, "novaMode"))) novaMode = json_integer_value(j);
		if ((j = json_object_get(root, "speedQuant"))) speedQuant = json_integer_value(j);
		if ((j = json_object_get(root, "playing"))) playing = json_boolean_value(j);
		if ((j = json_object_get(root, "reversed"))) reversed = json_boolean_value(j);
		if ((j = json_object_get(root, "clockMode"))) clockMode = json_boolean_value(j);
		if ((j = json_object_get(root, "punchMode"))) punchMode = json_integer_value(j);
		if ((j = json_object_get(root, "inertiaSlope"))) inertiaSlope = json_real_value(j);
		if ((j = json_object_get(root, "fxOnDry"))) fxOnDry = json_boolean_value(j);
		if ((j = json_object_get(root, "mixCurve"))) mixCurve = json_integer_value(j);
		if ((j = json_object_get(root, "reverbType"))) reverbType = json_integer_value(j);
		json_t* fx = json_object_get(root, "fxAmt");
		if (fx)
			for (int i = 0; i < 8 && i < (int)json_array_size(fx); i++)
				fxAmt[i] = json_real_value(json_array_get(fx, i));
		fxKnobContext = -1;
	}

	float readBuf(const std::vector<float>& b, double p) {
		double fl = std::floor(p);
		float frac = (float)(p - fl);
		int i = (int)fl % recLen;
		if (i < 0)
			i += recLen;
		int i1 = (i + 1 >= recLen) ? 0 : i + 1;
		return b[i] + (b[i1] - b[i]) * frac;
	}

	float cvParam(int paramId, int inputId) {
		return clamp(params[paramId].getValue() + inputs[inputId].getVoltage() / 10.f, 0.f, 1.f);
	}

	float whiteNoise() {
		noiseState = noiseState * 1664525u + 1013904223u;
		return (float)(int32_t)noiseState / 2147483648.f;
	}

	void saveUndo() {
		if (recLen > 0) {
			std::copy(bufL.begin(), bufL.begin() + recLen, undoL.begin());
			std::copy(bufR.begin(), bufR.begin() + recLen, undoR.begin());
			hasUndo = true;
		}
	}

	// The always-on effect chain: tape (wow/flutter + hiss/comp), digital
	// (downsample + crush), filters (HP/LP) and the reverb pair.
	void processChain(float& l, float& r, const float* fxA, const float* fxB, float sr, float dt) {
		if (fxA[0] > 0.003f) {
			wowPh += dt * (1.1 + 5.7 * random::uniform() * 0.001);
			if (wowPh > 1.0)
				wowPh -= 1.0;
			wowNoise += 0.002f * (whiteNoise() - wowNoise);
			float am = 1.f - fxA[0] * 0.25f * (0.5f + 0.5f * std::sin(2.f * M_PI * (float)wowPh * 6.3f) + wowNoise);
			l *= am;
			r *= am;
		}
		if (fxB[0] > 0.003f) {
			float n = whiteNoise() * 0.015f * fxB[0];
			float level = std::fabs(l) + std::fabs(r);
			compEnv = std::max(level, compEnv * std::exp(-dt / 0.08f));
			float comp = 1.f / (1.f + compEnv * fxB[0] * 1.5f);
			l = l * comp + n;
			r = r * comp + n;
		}
		if (fxA[1] > 0.003f || fxB[1] > 0.003f) {
			int holdN = 1 + (int)(fxA[1] * fxA[1] * 40.f);
			if (++holdCount >= holdN) {
				holdCount = 0;
				heldL = l;
				heldR = r;
				if (fxB[1] > 0.003f) {
					float bits = 14.f - fxB[1] * 11.f;
					float q = std::pow(2.f, bits);
					heldL = std::round(clamp(heldL, -1.2f, 1.2f) * q) / q;
					heldR = std::round(clamp(heldR, -1.2f, 1.2f) * q) / q;
				}
			}
			l = heldL;
			r = heldR;
		}
		if (fxA[3] > 0.003f) {
			float fc = 20.f * std::pow(600.f, fxA[3]);
			float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
			for (int s = 0; s < 2; s++) {
				hpL[s] += a * (l - hpL[s]);
				hpR[s] += a * (r - hpR[s]);
			}
			l -= hpL[1];
			r -= hpR[1];
		}
		if (fxB[3] > 0.003f) {
			float fc = 18000.f * std::pow(25.f / 18000.f, fxB[3]);
			float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
			float xl = l, xr = r;
			for (int s = 0; s < 2; s++) {
				lpL[s] += a * (xl - lpL[s]);
				lpR[s] += a * (xr - lpR[s]);
				xl = lpL[s];
				xr = lpR[s];
			}
			l = xl;
			r = xr;
		}
		reverb.set_amount(fxA[2] * 0.54f);
		reverb.set_time(0.3f + 0.68f * fxB[2]);
		reverb.set_input_gain(0.2f);
		reverb.set_lp(reverbType == 1 ? 0.95f : reverbType == 2 ? 0.4f : 0.7f);
		clouds::FloatFrame frame;
		frame.l = l;
		frame.r = r;
		reverb.Process(&frame, 1);
		l = frame.l;
		r = frame.r;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;
		if (bufLen == 0)
			return;

		// ---- clock ----
		lastEdge += dt;
		bool clockTick = false;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (lastEdge < 8.f && lastEdge > 0.0005f)
				clockPeriod = clamp(lastEdge, 0.0005f, 8.f);
			lastEdge = 0.f;
			clockTick = true;
		}
		// clock mode queues transport actions to the next clock pulse
		bool sync = clockMode && inputs[CLOCK_INPUT].isConnected();

		// ---- buttons & gates ----
		if (loopBtn.process(params[LOOPMODE_PARAM].getValue() > 0.f))
			loopMode = (loopMode + 1) % 4;
		if (fxBtn.process(params[FXMODE_PARAM].getValue() > 0.f))
			fxMode = (fxMode + 1) % 4;
		// erase on release; holding for 2s cancels (like hardware)
		if (params[ERASE_PARAM].getValue() > 0.f) {
			if (eraseHold < 0.f)
				eraseHold = 0.f;
			else
				eraseHold += dt;
		}
		else if (eraseHold >= 0.f) {
			if (eraseHold < 2.f) {
				saveUndo();
				int savedLen = recLen;
				erase();
				recLen = savedLen; // undo can restore
			}
			eraseHold = -1.f;
		}
		// undo does not operate in frippertronics mode (like hardware)
		if (undoBtn.process(params[UNDO_PARAM].getValue() > 0.f) && hasUndo && recLen > 0 && loopMode != 2) {
			for (int i = 0; i < recLen; i++) {
				std::swap(bufL[i], undoL[i]);
				std::swap(bufR[i], undoR[i]);
			}
			hasLoop = true;
		}
		auto startRecording = [&]() {
			if (hasLoop)
				saveUndo();
			if (loopMode == 3)
				resampleLen = 0;
			recording = true;
		};
		auto stopRecording = [&]() {
			recording = false;
			if (!hasLoop && recWrite > 0) {
				recLen = recWrite;
				hasLoop = true;
				pos = 0.0;
			}
			else if (loopMode == 3 && resampleLen > 0) {
				// resample: undo buffer holds the new recording
				for (int i = 0; i < resampleLen; i++) {
					std::swap(bufL[i], undoL[i]);
					std::swap(bufR[i], undoR[i]);
				}
				recLen = resampleLen;
				hasLoop = true;
				hasUndo = true;
				params[VARISPEED_PARAM].setValue(0.f);
				pos = 0.0;
			}
		};
		if (recBtn.process(params[RECORD_PARAM].getValue() > 0.f) ||
		    recTrig.process(inputs[RECORD_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (recQueued) {
				recQueued = false; // cancel a queued recording
			}
			else if (!recording) {
				if (punchMode == 2 && hasLoop)
					recQueued = true; // queued full: start at next loop point
				else
					startRecording();
			}
			else {
				stopRecording();
			}
		}
		if (playBtn.process(params[PLAY_PARAM].getValue() > 0.f) ||
		    playTrig.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (sync)
				pendPlay = !pendPlay;
			else
				playing = !playing;
		}
		if (resetBtn.process(params[RESET_PARAM].getValue() > 0.f) ||
		    resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (sync) {
				pendReset = true;
			}
			else {
				pos = 0.0;
				resetBlink = 0.1f;
				if (recording && punchMode >= 1)
					stopRecording(); // full-loop punch modes end on reset
			}
		}
		if (revBtn.process(params[REVERSE_PARAM].getValue() > 0.f) ||
		    revTrig.process(inputs[REVERSE_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (sync)
				pendReverse = !pendReverse;
			else
				reversed = !reversed;
		}
		if (frzBtn.process(params[FREEZE_PARAM].getValue() > 0.f)) {
			if (sync)
				pendFreeze = !pendFreeze;
			else
				frozen = !frozen;
		}
		// apply queued transport actions on the clock pulse
		if (sync && clockTick) {
			if (pendPlay) {
				playing = !playing;
				pendPlay = false;
			}
			if (pendReset) {
				pos = 0.0;
				resetBlink = 0.1f;
				pendReset = false;
				if (recording && punchMode >= 1)
					stopRecording();
			}
			if (pendReverse) {
				reversed = !reversed;
				pendReverse = false;
			}
			if (pendFreeze) {
				frozen = !frozen;
				pendFreeze = false;
			}
		}
		bool gateFrozen = inputs[FREEZE_INPUT].isConnected() && inputs[FREEZE_INPUT].getVoltage() > 0.5f;
		bool isFrozen = frozen || gateFrozen;

		// ---- input with level + saturation ----
		float lvl = cvParam(LEVEL_PARAM, LEVEL_INPUT);
		float gain = lvl * 2.f;
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f : inL;
		inL *= gain;
		inR *= gain;
		if (gain > 1.f) {
			float drive = 1.f + (gain - 1.f) * 2.f;
			inL = std::tanh(inL * drive) / std::tanh(drive * 0.5f + 0.5f);
			inR = std::tanh(inR * drive) / std::tanh(drive * 0.5f + 0.5f);
		}

		// ---- FX knobs: write-through. Switching FX mode snaps the knobs to
		// that pair's stored values, so the knob position always shows the
		// live amount and every movement responds. ----
		if (fxKnobContext != fxMode) {
			fxKnobContext = fxMode;
			for (int k = 0; k < 2; k++) {
				float v = fxAmt[fxMode * 2 + k];
				params[k == 0 ? FLUTTER_PARAM : HISS_PARAM].setValue(v);
				fxKnobPrev[k] = v;
			}
		}
		for (int k = 0; k < 2; k++) {
			float v = params[k == 0 ? FLUTTER_PARAM : HISS_PARAM].getValue();
			if (v != fxKnobPrev[k]) {
				fxAmt[fxMode * 2 + k] = v;
				fxKnobPrev[k] = v;
			}
		}
		float fxA[4], fxB[4];
		for (int m = 0; m < 4; m++) {
			fxA[m] = fxAmt[m * 2];
			fxB[m] = fxAmt[m * 2 + 1];
		}
		// CV adds to the currently selected pair
		fxA[fxMode] = clamp(fxA[fxMode] + inputs[FLUTTER_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		fxB[fxMode] = clamp(fxB[fxMode] + inputs[HISS_INPUT].getVoltage() / 10.f, 0.f, 1.f);

		// ---- first recording ----
		if (recording && !hasLoop) {
			if (recWrite < bufLen) {
				bufL[recWrite] = inL;
				bufR[recWrite] = inR;
				recWrite++;
			}
			else {
				recLen = bufLen;
				hasLoop = true;
				recording = false;
				pos = 0.0;
			}
		}

		// ---- speed (varispeed + inertia) ----
		float oct = params[VARISPEED_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();
		switch (speedQuant) {
			case 1: oct = std::round(oct * 12.f) / 12.f; break;
			case 2: { // octaves & fifths
				float o = std::floor(oct);
				float fr = oct - o;
				oct = o + ((fr < 0.29f) ? 0.f : (fr < 0.79f) ? 7.f / 12.f : 1.f);
			} break;
			case 3: oct = std::round(oct); break;
		}
		float targetSpeed = std::exp2(clamp(oct, -2.f, 3.f)) * (playing ? 1.f : 0.f);
		// inertial slope: lag can be biased toward decelerations or
		// accelerations only (hardware shift+inertia)
		float slopeScale = (targetSpeed > speed) ? (1.f - std::max(0.f, -inertiaSlope))
		                                         : (1.f - std::max(0.f, inertiaSlope));
		float inertia = params[INERTIA_PARAM].getValue() * slopeScale;
		if (inertia > 0.003f) {
			float k = 1.f - std::exp(-dt / (inertia * inertia * 2.f));
			speed += (targetSpeed - speed) * k;
		}
		else {
			speed = targetSpeed;
		}

		// ---- playback ----
		float wetL = 0.f, wetR = 0.f;
		bool loopWrapped = false;
		bool sliceCrossed = false;
		if (hasLoop && recLen > 0) {
			float startV = cvParam(START_PARAM, START_INPUT);
			float sizeV = cvParam(SIZE_PARAM, SIZE_INPUT);
			double loopStart = startV * recLen;
			double loopSize = std::fmin(std::fmax((double)sizeV * recLen, 0.005 * sr), (double)recLen);

			// slices: count rises in powers of 2; the top of the knob is the
			// "purple zone" where slices stay referenced but random slice
			// repeats are disabled (like hardware). In clock mode the splice
			// grid locks to musical clock divisions instead.
			float sliceV = cvParam(SLICE_PARAM, SLICE_INPUT);
			bool sliceRepeats = sliceV < 0.85f;
			float szn = std::min(sliceV, 0.84f) / 0.84f;
			int nSlice = (int)(szn * 5.f + 0.5f);
			int slices;
			if (sync && nSlice >= 1) {
				// knob picks 2 beats .. 1/8 beat per splice
				double want = clockPeriod * sr * std::pow(2.0, 2 - nSlice);
				slices = clamp((int)std::round(loopSize / std::max(1.0, want)), 1, 64);
			}
			else {
				slices = 1 << nSlice;
			}
			while (slices > 1 && loopSize / slices < 0.062 * sr)
				slices--;
			double sliceLen = loopSize / slices;

			novaHalfSlice = (float)(sliceLen / sr) * 0.5f / std::max(0.25f, std::fabs(speed));
			novaHalfLoop = (float)(loopSize / sr) * 0.5f / std::max(0.25f, std::fabs(speed));

			if (isFrozen) {
				if (freezePh == 0.0 && freezeStart == 0.0) {
					freezeStart = pos;
					freezeLen = sync ? clockPeriod * sr / 32.f
					                 : std::max(0.062f * sr, (float)loopSize * 0.01f);
				}
				freezePh += std::fabs(speed);
				if (freezePh >= freezeLen)
					freezePh -= freezeLen;
				double rp = loopStart + std::fmod(freezeStart + freezePh, loopSize);
				wetL = readBuf(bufL, rp);
				wetR = readBuf(bufR, rp);
			}
			else {
				freezeStart = 0.0;
				freezePh = 0.0;

				// slice boundary detection
				int curSlice = (int)(pos / sliceLen);
				if (curSlice != sliceIdx) {
					sliceIdx = curSlice;
					sliceCrossed = true;
					// defaults
					sliceOffset = 0.f;
					sliceRev = false;
					sliceGainL = sliceGainR = 1.f;
					sliceRatioTarget = 1.f;
					sliceLag = false;
					// random slice repeat (disabled in the purple zone)
					if (sliceRepeats && slices > 1 && random::uniform() < 0.18f)
						pos = (double)(random::u32() % slices) * sliceLen;
					// skip transformations
					float skipV = cvParam(SKIP_PARAM, SKIP_INPUT);
					if (skipV > 0.003f) {
						float zpos = skipV * 9.f;
						int zone = std::min(8, (int)zpos);
						float amt = clamp(zpos - zone, 0.f, 1.f);
						if (zone >= 7)
							amt = clamp((skipV - (zone == 7 ? 7.f / 9.f : 8.f / 9.f)) * 9.f, 0.f, 1.f);
						bool z1 = zone == 0 || zone >= 7;
						bool z2 = zone == 1 || zone >= 7;
						bool z3 = zone == 2 || zone >= 7;
						bool z4 = zone == 3 || zone >= 7;
						bool z5 = zone == 4 || zone == 7;
						bool z6 = zone == 5;
						bool z7 = zone == 6 || zone == 8;
						if (z1 && random::uniform() < amt)
							sliceOffset = random::uniform() * amt * (float)sliceLen;
						if (z2 && random::uniform() < amt * 0.6f)
							sliceRev = true;
						if (z3 && random::uniform() < amt) {
							float pan = 0.5f + (random::uniform() - 0.5f) * amt;
							float ag = 1.f - random::uniform() * amt * 0.6f;
							sliceGainL = std::cos(pan * 0.5f * M_PI) * 1.4f * ag;
							sliceGainR = std::sin(pan * 0.5f * M_PI) * 1.4f * ag;
						}
						if (z4 && random::uniform() < amt)
							sliceRatioTarget = std::exp2((random::uniform() - 0.5f) * 0.06f * amt);
						if (z5 && random::uniform() < amt * 0.5f)
							sliceRatioTarget = (random::u32() & 1) ? 2.f : 0.5f;
						if (z6 && random::uniform() < amt)
							sliceRatioTarget = std::exp2((float)((int)(random::u32() % 25) - 12) / 12.f);
						if (z7 && random::uniform() < amt) {
							sliceRatioTarget = std::exp2((float)((int)(random::u32() % 25) - 12) / 12.f);
							sliceLag = true;
						}
						if (!sliceLag)
							sliceRatio = sliceRatioTarget;
					}
					else {
						sliceRatio = 1.f;
					}
				}
				if (sliceLag) {
					float k = 1.f - std::exp(-dt / 0.08f);
					sliceRatio += (sliceRatioTarget - sliceRatio) * k;
				}
				else {
					sliceRatio = sliceRatioTarget;
				}

				// read position within slice (reversed slices read backwards)
				double sliceStart = (double)sliceIdx * sliceLen;
				double q = pos - sliceStart;
				double rq = sliceRev ? (sliceLen - q) : q;
				double rp = loopStart + std::fmod(sliceStart + rq + sliceOffset, loopSize);
				wetL = readBuf(bufL, rp) * sliceGainL;
				wetR = readBuf(bufR, rp) * sliceGainR;

				// short fade at slice edges to avoid clicks when mangling
				if (slices > 1 || sliceRev || sliceOffset > 0.f) {
					float edge = std::min((float)q, (float)(sliceLen - q)) / (0.003f * sr);
					float g = clamp(edge, 0.f, 1.f);
					wetL *= g;
					wetR *= g;
				}

				// ---- overdub recording ----
				if (recording) {
					if (loopMode == 3) {
						// resample: capture playback into the undo buffer
						if (resampleLen < bufLen) {
							undoL[resampleLen] = wetL;
							undoR[resampleLen] = wetR;
							resampleLen++;
						}
					}
					else {
						int wi = (int)(loopStart + std::fmod(pos, loopSize)) % recLen;
						switch (loopMode) {
							case 1: // replace
								bufL[wi] = inL;
								bufR[wi] = inR;
								break;
							case 2: // fripper: old layers decay each pass
								bufL[wi] = bufL[wi] * 0.82f + inL;
								bufR[wi] = bufR[wi] * 0.82f + inR;
								break;
							default: // sound on sound
								bufL[wi] = clamp(bufL[wi] + inL, -2.f, 2.f);
								bufR[wi] = clamp(bufR[wi] + inR, -2.f, 2.f);
								break;
						}
					}
				}

				// advance
				double adv = (double)speed * sliceRatio * (reversed ? -1.0 : 1.0);
				pos += adv;
				if (pos >= loopSize) {
					pos -= loopSize;
					loopWrapped = true;
					sliceIdx = -1;
				}
				else if (pos < 0.0) {
					pos += loopSize;
					loopWrapped = true;
					sliceIdx = -1;
				}
			}

			// punch-in: queued recordings start at the loop point, and the
			// full-loop modes stop once the loop wraps
			if (loopWrapped) {
				if (recQueued) {
					recQueued = false;
					startRecording();
				}
				else if (recording && punchMode >= 1) {
					stopRecording();
				}
			}

		}

		// ---- effect chain + mix. The chain (incl. reverb) is applied to
		// the wet path by default, or to the full mix when pre/post is
		// toggled (hardware shift+FX). ----
		float mix = cvParam(MIX_PARAM, MIX_INPUT);
		float dryGain, wetGain;
		switch (mixCurve) {
			case 1: // linear
				dryGain = 1.f - mix;
				wetGain = mix;
				break;
			case 2: // transition: both signals at full around the center
				dryGain = clamp(2.f * (1.f - mix), 0.f, 1.f);
				wetGain = clamp(2.f * mix, 0.f, 1.f);
				break;
			default: // constant power
				dryGain = std::cos(mix * 0.5f * M_PI);
				wetGain = std::sin(mix * 0.5f * M_PI);
				break;
		}
		float rawInL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float rawInR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f : rawInL;
		float outL, outR;
		if (fxOnDry) {
			outL = rawInL * dryGain + wetL * wetGain;
			outR = rawInR * dryGain + wetR * wetGain;
			processChain(outL, outR, fxA, fxB, sr, dt);
		}
		else {
			processChain(wetL, wetR, fxA, fxB, sr, dt);
			outL = rawInL * dryGain + wetL * wetGain;
			outR = rawInR * dryGain + wetR * wetGain;
		}
		outputs[OUT_L_OUTPUT].setVoltage(clamp(outL * 5.f, -12.f, 12.f));
		outputs[OUT_R_OUTPUT].setVoltage(clamp(outR * 5.f, -12.f, 12.f));

		// ---- nova output ----
		bool fire = false;
		if (novaMode == 0)
			fire = loopWrapped || sliceCrossed;
		else if (novaMode == 1)
			fire = loopWrapped;
		else if (novaMode == 2)
			fire = sliceCrossed;
		if (fire)
			novaPulse.trigger(clamp(sliceCrossed ? novaHalfSlice : novaHalfLoop, 0.002f, 2.f));
		if (novaMode == 3) {
			float sizeV = cvParam(SIZE_PARAM, SIZE_INPUT);
			double loopSize = std::max(1.0, (double)(sizeV * recLen));
			outputs[NOVA_OUTPUT].setVoltage(hasLoop ? (float)(pos / loopSize) * 5.f : 0.f);
		}
		else {
			outputs[NOVA_OUTPUT].setVoltage(novaPulse.process(dt) ? 5.f : 0.f);
		}

		// ---- lights ----
		resetBlink = std::max(0.f, resetBlink - dt);
		blinkPh += dt;
		if (blinkPh >= 0.5f)
			blinkPh -= 0.5f;
		if (lightDivider.process()) {
			static const float modeColors[4][3] = {
				{0.f, 0.2f, 1.f}, {0.f, 1.f, 0.1f}, {1.f, 0.6f, 0.f}, {0.6f, 0.f, 1.f}};
			for (int k = 0; k < 3; k++) {
				lights[LOOPMODE_LIGHT + k].setBrightness(modeColors[loopMode][k]);
				lights[FXMODE_LIGHT + k].setBrightness(modeColors[fxMode][k]);
			}
			lights[FREEZE_LIGHT].setBrightness(isFrozen);
			// queued punch-in recording blinks the record LED
			lights[RECORD_LIGHT].setBrightness(recording ? 1.f : (recQueued && blinkPh < 0.25f) ? 0.7f : 0.f);
			lights[PLAY_LIGHT].setBrightness(playing ? 1.f : 0.f);
			lights[RESET_LIGHT].setBrightness(resetBlink > 0.f ? 1.f : 0.f);
			lights[REVERSE_LIGHT].setBrightness(reversed);
		}
	}
};

struct TektiteWidget : ModuleWidget {
	TektiteWidget(Tektite* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Tektite.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace tlayout;

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(FLUT_X, YA)), module, Tektite::FLUTTER_PARAM));
		// mode lights live inside the buttons (keeps the title clear)
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(FRZ_X, YA)), module, Tektite::FREEZE_PARAM, Tektite::FREEZE_LIGHT));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedGreenBlueLight>>>(mm2px(Vec(LOOP_X, YA)), module, Tektite::LOOPMODE_PARAM, Tektite::LOOPMODE_LIGHT));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(VARI_X, VARI_Y)), module, Tektite::VARISPEED_PARAM));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedGreenBlueLight>>>(mm2px(Vec(FX_X, YA)), module, Tektite::FXMODE_PARAM, Tektite::FXMODE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(UNDO_X, YA)), module, Tektite::UNDO_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(HISS_X, YA)), module, Tektite::HISS_PARAM));

		addParam(createParamCentered<VCVButton>(mm2px(Vec(ERASE_X, YB)), module, Tektite::ERASE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(START_X, YB)), module, Tektite::START_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(INERTIA_X, INERTIA_Y)), module, Tektite::INERTIA_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SIZE_X, YB)), module, Tektite::SIZE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(MIX_X, YB)), module, Tektite::MIX_PARAM));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(LVL_X, YC)), module, Tektite::LEVEL_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SKIP_X, YC)), module, Tektite::SKIP_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SLICE_X, YC)), module, Tektite::SLICE_PARAM));

		static const int trParams[4] = {Tektite::RECORD_PARAM, Tektite::PLAY_PARAM, Tektite::RESET_PARAM, Tektite::REVERSE_PARAM};
		static const int trLights[4] = {Tektite::RECORD_LIGHT, Tektite::PLAY_LIGHT, Tektite::RESET_LIGHT, Tektite::REVERSE_LIGHT};
		for (int i = 0; i < 4; i++) {
			addParam(createParamCentered<VCVButton>(mm2px(Vec(TR_X[i], YD)), module, trParams[i]));
			if (i == 0)
				addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(TR_X[i] - 6.5f, YD)), module, trLights[i]));
			else if (i == 1)
				addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(TR_X[i] - 6.5f, YD)), module, trLights[i]));
			else if (i == 2)
				addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(TR_X[i] - 6.5f, YD)), module, trLights[i]));
			else
				addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(TR_X[i] - 6.5f, YD)), module, trLights[i]));
		}

		static const int r1Ins[8] = {Tektite::LEVEL_INPUT, Tektite::FLUTTER_INPUT, Tektite::HISS_INPUT, Tektite::MIX_INPUT,
		                             Tektite::START_INPUT, Tektite::SIZE_INPUT, Tektite::SLICE_INPUT, Tektite::SKIP_INPUT};
		static const int r2Ins[7] = {Tektite::RECORD_INPUT, Tektite::PLAY_INPUT, Tektite::RESET_INPUT, Tektite::REVERSE_INPUT,
		                             Tektite::FREEZE_INPUT, Tektite::CLOCK_INPUT, Tektite::VOCT_INPUT};
		for (int i = 0; i < 8; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[i], R1)), module, r1Ins[i]));
		for (int i = 0; i < 7; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[i], R2)), module, r2Ins[i]));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J8[7], R2)), module, Tektite::NOVA_OUTPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[0], R3)), module, Tektite::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[1], R3)), module, Tektite::IN_R_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J8[6], R3)), module, Tektite::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J8[7], R3)), module, Tektite::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Tektite* module = getModule<Tektite>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Loop mode",
			{"Sound on sound", "Replace", "Fripper (decaying layers)", "Resample"}, &module->loopMode));
		menu->addChild(createIndexPtrSubmenuItem("FX mode (knobs edit)",
			{"Tape (flutter/hiss)", "Digital (downsample/crush)", "Reverb (amount/time)", "Filter (HP/LP)"}, &module->fxMode));
		menu->addChild(createIndexPtrSubmenuItem("Nova output",
			{"Loop + slice gates", "Loop gates", "Slice gates", "Position CV"}, &module->novaMode));
		menu->addChild(createIndexPtrSubmenuItem("Varispeed quantize",
			{"Unquantized", "Semitones", "Octaves & fifths", "Octaves"}, &module->speedQuant));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Clock mode (sync transport to clock pulses)", "", &module->clockMode));
		menu->addChild(createIndexPtrSubmenuItem("Punch-in record mode",
			{"None (manual)", "Immediate full", "Queued full"}, &module->punchMode));
		menu->addChild(createBoolPtrMenuItem("Effects on dry signal (pre/post)", "", &module->fxOnDry));
		menu->addChild(createIndexPtrSubmenuItem("Mix curve",
			{"Constant power", "Linear", "Transition"}, &module->mixCurve));
		menu->addChild(createIndexPtrSubmenuItem("Reverb type",
			{"Normal", "Bright", "Dark"}, &module->reverbType));
		{
			struct SlopeQuantity : Quantity {
				Tektite* m;
				void setValue(float v) override { m->inertiaSlope = clamp(v, -1.f, 1.f); }
				float getValue() override { return m->inertiaSlope; }
				float getMinValue() override { return -1.f; }
				float getMaxValue() override { return 1.f; }
				float getDefaultValue() override { return 0.f; }
				std::string getLabel() override { return "Inertia slope (decel only < both > accel only)"; }
				int getDisplayPrecision() override { return 2; }
			};
			ui::Slider* s = new ui::Slider;
			SlopeQuantity* q = new SlopeQuantity;
			q->m = module;
			s->quantity = q;
			s->box.size.x = 240.f;
			menu->addChild(s);
		}
		menu->addChild(createMenuItem("Reset modes & effects to defaults", "", [=]() {
			module->loopMode = 0;
			module->fxMode = 0;
			module->fxOnDry = false;
			for (int i = 0; i < 8; i++)
				module->fxAmt[i] = 0.f;
			module->fxKnobContext = -1;
		}));
	}
};

Model* modelTektite = createModel<Tektite, TektiteWidget>("Tektite");
