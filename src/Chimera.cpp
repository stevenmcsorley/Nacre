// Chimera — a splice-based stereo loop sampler for VCV Rack with true
// granular time-stretching (independent speed and pitch), clock-matched
// stretching, chopping/splicing, scatter playback, sound-on-sound
// layering, a 3-band EQ, and a soft output limiter. Inspired by tape-style
// micro-sound samplers such as the Make Noise Morphagene, but an entirely
// original design and implementation.

#include "plugin.hpp"
#include "layout_chimera.hpp"
#include <osdialog.h>
#include <fstream>

static const int MAX_GRAINS = 6;

struct Chimera : Module {
	enum ParamId {
		SPEED_PARAM,
		PITCH_PARAM,
		GRAIN_PARAM,
		SLICE_PARAM,
		CHOP_PARAM,
		SCATTER_PARAM,
		SOS_PARAM,
		EQ_LOW_PARAM,
		EQ_MID_PARAM,
		EQ_HIGH_PARAM,
		REC_PARAM,
		PLAY_PARAM,
		SPLICE_PARAM,
		SYNC_PARAM,
		MODE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		SPEED_INPUT,
		VOCT_INPUT,
		GRAIN_INPUT,
		SLICE_INPUT,
		SCATTER_INPUT,
		CLOCK_INPUT,
		REC_INPUT,
		SPLICE_INPUT,
		IN_L_INPUT,
		IN_R_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		EOS_OUTPUT,
		ENV_OUTPUT,
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		REC_LIGHT,
		PLAY_LIGHT,
		SYNC_LIGHT,
		MODE_LIGHT,
		CLK_LIGHT,
		ENUMS(SLICE_LIGHT, 8),
		LIGHTS_LEN
	};

	struct Grain {
		bool active = false;
		double readPos = 0.0;
		double phase = 0.0;
		double dur = 1.0;
		double rate = 1.0;
		double loStart = 0.0;  // slice bounds captured at spawn,
		double loEnd = 1.0;    // so grains wrap seamlessly within the loop
	};

	// simple RBJ biquad
	struct Biquad {
		float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
		float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;
		float process(float x) {
			float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1; x1 = x;
			y2 = y1; y1 = y;
			return y;
		}
		void lowShelf(float sr, float f0, float dbGain) {
			float A = std::pow(10.f, dbGain / 40.f);
			float w = 2.f * M_PI * f0 / sr;
			float cw = std::cos(w), sw = std::sin(w);
			float alpha = sw / 2.f * std::sqrt(2.f);
			float sqA = 2.f * std::sqrt(A) * alpha;
			float a0 = (A + 1) + (A - 1) * cw + sqA;
			b0 = (A * ((A + 1) - (A - 1) * cw + sqA)) / a0;
			b1 = (2 * A * ((A - 1) - (A + 1) * cw)) / a0;
			b2 = (A * ((A + 1) - (A - 1) * cw - sqA)) / a0;
			a1 = (-2 * ((A - 1) + (A + 1) * cw)) / a0;
			a2 = ((A + 1) + (A - 1) * cw - sqA) / a0;
		}
		void highShelf(float sr, float f0, float dbGain) {
			float A = std::pow(10.f, dbGain / 40.f);
			float w = 2.f * M_PI * f0 / sr;
			float cw = std::cos(w), sw = std::sin(w);
			float alpha = sw / 2.f * std::sqrt(2.f);
			float sqA = 2.f * std::sqrt(A) * alpha;
			float a0 = (A + 1) - (A - 1) * cw + sqA;
			b0 = (A * ((A + 1) + (A - 1) * cw + sqA)) / a0;
			b1 = (-2 * A * ((A - 1) + (A + 1) * cw)) / a0;
			b2 = (A * ((A + 1) + (A - 1) * cw - sqA)) / a0;
			a1 = (2 * ((A - 1) - (A + 1) * cw)) / a0;
			a2 = ((A + 1) - (A - 1) * cw - sqA) / a0;
		}
		void peak(float sr, float f0, float q, float dbGain) {
			float A = std::pow(10.f, dbGain / 40.f);
			float w = 2.f * M_PI * f0 / sr;
			float cw = std::cos(w), sw = std::sin(w);
			float alpha = sw / (2.f * q);
			float a0 = 1 + alpha / A;
			b0 = (1 + alpha * A) / a0;
			b1 = (-2 * cw) / a0;
			b2 = (1 - alpha * A) / a0;
			a1 = b1;
			a2 = (1 - alpha / A) / a0;
		}
	};

	std::vector<float> bufL, bufR;
	int bufLen = 0;     // allocated
	int reelLen = 0;    // recorded/loaded length
	std::vector<int> markers;

	bool recording = false;
	bool playing = true;
	bool sync = false;
	bool stretchMode = false;  // false = tape (clean direct playback)
	int recWrite = 0;
	bool freshRecording = false;

	int curSlice = 0;
	int numSlices = 1;
	double playPos = 0.0;   // absolute position within reel
	double sliceStart = 0.0, sliceEnd = 1.0;
	float sliceDir = 1.f;

	Grain grains[MAX_GRAINS];
	double spawnTimer = 0.0;

	float clockPeriod = 0.5f;
	float lastEdge = 1e9f;
	bool clockSeen = false;
	int syncBeat = -1;     // -1 = waiting for the next clock edge to start the loop
	float declick = 1.f;   // short output ramp after a phase snap

	Biquad eqLow[2], eqMid[2], eqHigh[2];
	float eqLowVal = 1e9f, eqMidVal = 1e9f, eqHighVal = 1e9f;
	float envFollow = 0.f;

	dsp::SchmittTrigger recTrig, spliceTrig, clockTrig;
	dsp::BooleanTrigger recBtn, playBtn, spliceBtn, syncBtn, modeBtn;
	dsp::PulseGenerator eosPulse;
	dsp::ClockDivider lightDivider;

	Chimera() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SPEED_PARAM, -2.f, 2.f, 1.f, "Speed (time only; through zero)", "x");
		configParam(PITCH_PARAM, -12.f, 12.f, 0.f, "Pitch (independent of speed)", " st");
		configParam(GRAIN_PARAM, 0.f, 1.f, 0.4f, "Grain size (20 ms – 500 ms)");
		configParam(SLICE_PARAM, 0.f, 1.f, 0.f, "Slice select");
		configParam(CHOP_PARAM, 0.f, 1.f, 0.f, "Chop (auto-slice count 1–32)");
		configParam(SCATTER_PARAM, 0.f, 1.f, 0.f, "Scatter (random slice order / reverse)", "%", 0.f, 100.f);
		configParam(SOS_PARAM, 0.f, 1.f, 0.7f, "Sound-on-sound (overdub keep level)", "%", 0.f, 100.f);
		configParam(EQ_LOW_PARAM, -12.f, 12.f, 0.f, "EQ low shelf (120 Hz)", " dB");
		configParam(EQ_MID_PARAM, -12.f, 12.f, 0.f, "EQ mid bell (1 kHz)", " dB");
		configParam(EQ_HIGH_PARAM, -12.f, 12.f, 0.f, "EQ high shelf (6 kHz)", " dB");
		configButton(REC_PARAM, "Record / overdub");
		configButton(PLAY_PARAM, "Play/stop");
		configButton(SPLICE_PARAM, "Add splice marker at playhead");
		configButton(SYNC_PARAM, "Clock sync (stretch slice to whole beats)");
		configButton(MODE_PARAM, "Mode (tape: clean varispeed / stretch: granular time-stretch)");
		configInput(SPEED_INPUT, "Speed CV (±5 V)");
		configInput(VOCT_INPUT, "Pitch (1V/oct)");
		configInput(GRAIN_INPUT, "Grain size CV");
		configInput(SLICE_INPUT, "Slice select CV");
		configInput(SCATTER_INPUT, "Scatter CV");
		configInput(CLOCK_INPUT, "Clock");
		configInput(REC_INPUT, "Record gate (toggle)");
		configInput(SPLICE_INPUT, "Splice trigger");
		configInput(IN_L_INPUT, "Left audio (normalled to right)");
		configInput(IN_R_INPUT, "Right audio");
		configOutput(EOS_OUTPUT, "End of slice trigger");
		configOutput(ENV_OUTPUT, "Envelope follower");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		lightDivider.setDivision(64);
		allocate(44100.f);
		synthesizeDemoLoop(44100.f);
	}

	float maxSeconds(float sr) {
		return sr > 100000.f ? 30.f : 60.f;
	}

	void allocate(float sr) {
		bufLen = (int)(sr * maxSeconds(sr));
		bufL.assign(bufLen, 0.f);
		bufR.assign(bufLen, 0.f);
		reelLen = 0;
		markers.clear();
		playPos = 0.0;
		for (Grain& g : grains)
			g.active = false;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
		synthesizeDemoLoop(e.sampleRate);
		eqLowVal = eqMidVal = eqHighVal = 1e9f;
	}

	// small built-in arpeggio loop so the controls are audible out of the box
	void synthesizeDemoLoop(float sr) {
		float seconds = 2.f;
		reelLen = std::min((int)(sr * seconds), bufLen);
		static const float semis[8] = {0, 7, 12, 16, 19, 16, 12, 7};
		int noteLen = reelLen / 8;
		for (int i = 0; i < reelLen; i++) {
			int n = std::min(7, i / noteLen);
			float t = (float)(i % noteLen) / sr;
			float f = 110.f * std::exp2(semis[n] / 12.f);
			float env = std::exp(-t * 6.f);
			// two detuned saws via wrapped phase
			float p1 = std::fmod(f * (i / sr), 1.f);
			float p2 = std::fmod(f * 1.004f * (i / sr), 1.f);
			float v = (p1 * 2.f - 1.f) * 0.4f + (p2 * 2.f - 1.f) * 0.3f;
			bufL[i] = v * env * 0.7f;
			bufR[i] = (p2 * 2.f - 1.f) * 0.4f * env * 0.7f + (p1 * 2.f - 1.f) * 0.3f * env * 0.7f;
		}
		markers.clear();
		for (int k = 1; k < 8; k++)
			markers.push_back(k * noteLen);
	}

	bool loadWav(const std::string& path) {
		std::ifstream f(path, std::ios::binary);
		if (!f)
			return false;
		auto rd32 = [&]() { uint32_t v = 0; f.read((char*)&v, 4); return v; };
		auto rd16 = [&]() { uint16_t v = 0; f.read((char*)&v, 2); return v; };
		char tag[5] = {};
		f.read(tag, 4);
		if (std::string(tag, 4) != "RIFF")
			return false;
		rd32();
		f.read(tag, 4);
		if (std::string(tag, 4) != "WAVE")
			return false;
		uint16_t fmt = 0, channels = 0, bits = 0;
		uint32_t srate = 0;
		std::vector<char> data;
		while (f && !f.eof()) {
			f.read(tag, 4);
			uint32_t size = rd32();
			if (!f)
				break;
			std::string t(tag, 4);
			if (t == "fmt ") {
				fmt = rd16();
				channels = rd16();
				srate = rd32();
				rd32();
				rd16();
				bits = rd16();
				if (size > 16)
					f.seekg(size - 16, std::ios::cur);
			}
			else if (t == "data") {
				data.resize(size);
				f.read(data.data(), size);
				break;
			}
			else {
				f.seekg(size + (size & 1), std::ios::cur);
			}
		}
		if (data.empty() || channels == 0 || srate == 0)
			return false;
		if (!(fmt == 1 || fmt == 3 || fmt == 0xFFFE))
			return false;
		float engineSr = APP->engine->getSampleRate();
		int bytesPer = bits / 8;
		int frames = (int)(data.size() / (bytesPer * channels));
		if (frames < 2)
			return false;
		// resample to engine rate on load (linear)
		double ratio = (double)srate / engineSr;
		int outFrames = std::min((int)(frames / ratio), bufLen);
		auto sampleAt = [&](int i, int c) -> float {
			const char* p = data.data() + (size_t)(i * channels + std::min<int>(c, channels - 1)) * bytesPer;
			if (bits == 16)
				return *(const int16_t*)p / 32768.f;
			if (bits == 24) {
				int32_t x = (uint8_t)p[0] | ((uint8_t)p[1] << 8) | ((int8_t)p[2] << 16);
				return x / 8388608.f;
			}
			if (bits == 32 && (fmt == 3 || fmt == 0xFFFE))
				return *(const float*)p;
			if (bits == 32)
				return *(const int32_t*)p / 2147483648.f;
			if (bits == 8)
				return ((const uint8_t*)p)[0] / 128.f - 1.f;
			return 0.f;
		};
		for (int i = 0; i < outFrames; i++) {
			double sp = i * ratio;
			int i0 = std::min((int)sp, frames - 2);
			float fr = (float)(sp - i0);
			bufL[i] = sampleAt(i0, 0) + (sampleAt(i0 + 1, 0) - sampleAt(i0, 0)) * fr;
			bufR[i] = sampleAt(i0, 1) + (sampleAt(i0 + 1, 1) - sampleAt(i0, 1)) * fr;
		}
		reelLen = outFrames;
		markers.clear();
		playPos = 0.0;
		return true;
	}

	void updateSlices() {
		float chop = params[CHOP_PARAM].getValue();
		if (!markers.empty()) {
			numSlices = (int)markers.size() + 1;
		}
		else {
			numSlices = 1 << (int)(chop * 5.f + 0.5f);
		}
		numSlices = std::max(1, numSlices);
	}

	void sliceBounds(int idx, double& start, double& end) {
		if (!markers.empty()) {
			int n = (int)markers.size() + 1;
			idx = clamp(idx, 0, n - 1);
			start = (idx == 0) ? 0.0 : (double)markers[idx - 1];
			end = (idx == n - 1) ? (double)reelLen : (double)markers[idx];
		}
		else {
			double len = (double)reelLen / numSlices;
			start = idx * len;
			end = start + len;
		}
	}

	void enterSlice(int idx, bool fromStart) {
		curSlice = clamp(idx, 0, numSlices - 1);
		sliceBounds(curSlice, sliceStart, sliceEnd);
		if (fromStart)
			playPos = (sliceDir >= 0.f) ? sliceStart : sliceEnd - 1.0;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;
		if (bufLen == 0)
			return;

		// ---- buttons / gates ----
		if (recBtn.process(params[REC_PARAM].getValue() > 0.f) ||
		    recTrig.process(inputs[REC_INPUT].getVoltage(), 0.1f, 1.f)) {
			recording = !recording;
			if (recording && reelLen == 0) {
				freshRecording = true;
				recWrite = 0;
			}
			else if (!recording && freshRecording) {
				freshRecording = false;
				reelLen = std::max(recWrite, (int)(0.05f * sr));
				markers.clear();
				playPos = 0.0;
			}
		}
		if (playBtn.process(params[PLAY_PARAM].getValue() > 0.f))
			playing = !playing;
		if (syncBtn.process(params[SYNC_PARAM].getValue() > 0.f)) {
			sync = !sync;
			syncBeat = -1; // align loop start to the next clock edge
		}
		if (modeBtn.process(params[MODE_PARAM].getValue() > 0.f))
			stretchMode = !stretchMode;
		if ((spliceBtn.process(params[SPLICE_PARAM].getValue() > 0.f) ||
		     spliceTrig.process(inputs[SPLICE_INPUT].getVoltage(), 0.1f, 1.f)) &&
		    reelLen > 0) {
			int m = clamp((int)playPos, 1, reelLen - 2);
			auto it = std::lower_bound(markers.begin(), markers.end(), m);
			if (it == markers.end() || std::abs(*it - m) > (int)(0.01f * sr))
				markers.insert(it, m);
		}

		// ---- clock ----
		lastEdge += dt;
		bool clockTick = false;
		if (clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (lastEdge < 8.f && lastEdge > 0.001f) {
				clockPeriod = clamp(lastEdge, 0.001f, 8.f);
				clockSeen = true;
			}
			lastEdge = 0.f;
			clockTick = true;
		}

		// ---- fresh recording (defines the reel) ----
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f : inL;
		if (recording && freshRecording) {
			if (recWrite < bufLen) {
				bufL[recWrite] = inL;
				bufR[recWrite] = inR;
				recWrite++;
			}
			else {
				recording = false;
				freshRecording = false;
				reelLen = bufLen;
				markers.clear();
			}
			// monitor input while building the reel
			outputs[OUT_L_OUTPUT].setVoltage(inL * 5.f);
			outputs[OUT_R_OUTPUT].setVoltage(inR * 5.f);
			outputs[ENV_OUTPUT].setVoltage(0.f);
			lights[REC_LIGHT].setBrightness(1.f);
			return;
		}

		if (reelLen <= 0) {
			outputs[OUT_L_OUTPUT].setVoltage(0.f);
			outputs[OUT_R_OUTPUT].setVoltage(0.f);
			return;
		}

		// ---- slices ----
		updateSlices();
		float sliceCtl = clamp(params[SLICE_PARAM].getValue() + inputs[SLICE_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		int selSlice = std::min(numSlices - 1, (int)(sliceCtl * numSlices));
		sliceBounds(curSlice, sliceStart, sliceEnd);
		double sliceLen = std::max(64.0, sliceEnd - sliceStart);

		// ---- speed (time stretch) ----
		float speed = clamp(params[SPEED_PARAM].getValue() + inputs[SPEED_INPUT].getVoltage() / 2.5f, -4.f, 4.f);
		bool syncActive = sync && clockSeen && inputs[CLOCK_INPUT].isConnected();
		int beats = 1;
		float speedEff;
		if (syncActive) {
			// stretch so the slice occupies a *musical* number of beats
			static const int MUSICAL[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
			float sliceSec = (float)sliceLen / sr;
			float bestErr = 1e9f;
			for (int n : MUSICAL) {
				float err = std::fabs(std::log(sliceSec / (n * clockPeriod)));
				if (err < bestErr) {
					bestErr = err;
					beats = n;
				}
			}
			float base = sliceSec / (beats * clockPeriod);
			// knob quantized to musical multipliers
			float a = std::fabs(speed);
			float m = (a < 0.375f) ? 0.25f : (a < 0.75f) ? 0.5f : (a < 1.5f) ? 1.f : (a < 3.f) ? 2.f : 4.f;
			speedEff = base * m * ((speed < 0.f) ? -1.f : 1.f);
			// loop occupies beats/m clock ticks; keep it a whole number
			beats = std::max(1, (int)std::round(beats / m));
		}
		else {
			speedEff = speed;
		}
		if (!playing)
			speedEff = 0.f;

		// ---- phase lock: snap the loop to the clock grid ----
		if (syncActive && clockTick && playing) {
			syncBeat++;
			if (syncBeat <= 0 || (syncBeat % beats) == 0) {
				if (syncBeat <= 0)
					syncBeat = 0;
				double target = (speedEff * sliceDir >= 0.f) ? sliceStart : sliceEnd - 1.0;
				double err = std::fabs(playPos - target);
				if (err > 64.0 && err < sliceLen - 64.0)
					declick = 0.f; // brief fade to mask the jump
				playPos = target;
				spawnTimer = 0.0; // re-align grains to the downbeat
			}
		}
		if (!syncActive)
			syncBeat = -1;

		// ---- pitch / grain ----
		float semis = params[PITCH_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage() * 12.f;
		double pitchRatio = std::exp2(clamp(semis, -36.f, 36.f) / 12.f);
		float grainCtl = clamp(params[GRAIN_PARAM].getValue() + inputs[GRAIN_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		double grainSamp = (0.02 + 0.48 * grainCtl * grainCtl) * sr;
		// tape mode: pitch merges into speed (varispeed) for pristine playback
		if (!stretchMode && playing)
			speedEff *= (float)pitchRatio;

		// ---- overdub (sound on sound) at the playhead ----
		if (recording && !freshRecording) {
			float sos = params[SOS_PARAM].getValue();
			int wi = clamp((int)playPos, 0, reelLen - 1);
			bufL[wi] = clamp(bufL[wi] * sos + inL, -2.f, 2.f);
			bufR[wi] = clamp(bufR[wi] * sos + inR, -2.f, 2.f);
		}

		// ---- advance playhead within slice ----
		playPos += speedEff * sliceDir;
		bool eos = false;
		if (playPos >= sliceEnd || playPos < sliceStart) {
			eos = true;
			eosPulse.trigger(0.002f);
			float scatter = clamp(params[SCATTER_PARAM].getValue() + inputs[SCATTER_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			int next = selSlice;
			sliceDir = 1.f;
			if (scatter > 0.003f && random::uniform() < scatter) {
				next = (int)(random::u32() % numSlices);
				if (random::uniform() < scatter * 0.4f)
					sliceDir = -1.f;
			}
			enterSlice(next, true);
		}
		else if (curSlice != selSlice && params[SCATTER_PARAM].getValue() < 0.003f && !inputs[SCATTER_INPUT].isConnected()) {
			// direct slice selection takes effect immediately when not scattering
			enterSlice(selSlice, true);
		}
		static_cast<void>(eos);

		// ---- render ----
		auto readReel = [&](double rp, float& l, float& r) {
			while (rp < 0.0)
				rp += reelLen;
			while (rp >= reelLen)
				rp -= reelLen;
			int i0 = (int)rp;
			float fr = (float)(rp - i0);
			int i1 = (i0 + 1 >= reelLen) ? 0 : i0 + 1;
			l = bufL[i0] + (bufL[i1] - bufL[i0]) * fr;
			r = bufR[i0] + (bufR[i1] - bufR[i0]) * fr;
		};

		float wetL = 0.f, wetR = 0.f;
		if (!stretchMode) {
			// tape mode: pristine direct read with an equal-power crossfade
			// at the loop boundary for seamless cycling
			readReel(playPos, wetL, wetR);
			double fadeSamp = std::min(0.012 * sr, sliceLen * 0.25);
			double dist = (speedEff * sliceDir >= 0.f) ? (sliceEnd - playPos) : (playPos - sliceStart);
			if (dist < fadeSamp && fadeSamp > 1.0) {
				float wNow = std::sin(0.5f * M_PI * (float)(dist / fadeSamp));
				float wWrap = std::cos(0.5f * M_PI * (float)(dist / fadeSamp));
				float l2, r2;
				double wrapPos = (speedEff * sliceDir >= 0.f) ? playPos - sliceLen : playPos + sliceLen;
				readReel(wrapPos, l2, r2);
				wetL = wetL * wNow + l2 * wWrap;
				wetR = wetR * wNow + r2 * wWrap;
			}
		}
		else {
			// stretch mode: granular, grains wrapping inside the slice so
			// clock-synced loops cycle without seams.
			// grains keep spawning even at speed 0: a frozen, sustained texture
			spawnTimer -= 1.0;
			if (spawnTimer <= 0.0) {
				spawnTimer = grainSamp * 0.5;
				for (Grain& g : grains) {
					if (!g.active) {
						g.active = true;
						g.readPos = playPos;
						g.phase = 0.0;
						g.dur = grainSamp;
						g.rate = pitchRatio * ((sliceDir < 0.f) ? -1.0 : 1.0);
						g.loStart = sliceStart;
						g.loEnd = sliceEnd;
						break;
					}
				}
			}
			for (Grain& g : grains) {
				if (!g.active)
					continue;
				float p = (float)(g.phase / g.dur);
				if (p >= 1.f) {
					g.active = false;
					continue;
				}
				float win = 0.5f - 0.5f * std::cos(2.f * M_PI * p);
				float l, r;
				readReel(g.readPos, l, r);
				wetL += l * win;
				wetR += r * win;
				g.readPos += g.rate;
				double gLen = g.loEnd - g.loStart;
				if (gLen > 1.0) {
					if (g.readPos >= g.loEnd)
						g.readPos -= gLen;
					else if (g.readPos < g.loStart)
						g.readPos += gLen;
				}
				g.phase += 1.0;
			}
			wetL *= 0.75f;
			wetR *= 0.75f;
		}

		// declick ramp after phase snaps
		declick = std::min(1.f, declick + dt / 0.004f);
		wetL *= declick;
		wetR *= declick;

		// ---- EQ (recompute coefficients only when knobs move) ----
		float lo = params[EQ_LOW_PARAM].getValue();
		float mi = params[EQ_MID_PARAM].getValue();
		float hi = params[EQ_HIGH_PARAM].getValue();
		if (lo != eqLowVal) {
			eqLowVal = lo;
			for (int c = 0; c < 2; c++)
				eqLow[c].lowShelf(sr, 120.f, lo);
		}
		if (mi != eqMidVal) {
			eqMidVal = mi;
			for (int c = 0; c < 2; c++)
				eqMid[c].peak(sr, 1000.f, 0.8f, mi);
		}
		if (hi != eqHighVal) {
			eqHighVal = hi;
			for (int c = 0; c < 2; c++)
				eqHigh[c].highShelf(sr, 6000.f, hi);
		}
		wetL = eqHigh[0].process(eqMid[0].process(eqLow[0].process(wetL)));
		wetR = eqHigh[1].process(eqMid[1].process(eqLow[1].process(wetR)));

		// ---- soft limiter ----
		wetL = std::tanh(wetL * 1.1f) * 0.95f;
		wetR = std::tanh(wetR * 1.1f) * 0.95f;

		outputs[OUT_L_OUTPUT].setVoltage(wetL * 5.f);
		outputs[OUT_R_OUTPUT].setVoltage(wetR * 5.f);
		outputs[EOS_OUTPUT].setVoltage(eosPulse.process(dt) ? 10.f : 0.f);

		float level = 0.5f * (std::fabs(wetL) + std::fabs(wetR));
		envFollow = std::max(level, envFollow * std::exp(-dt / 0.12f));
		outputs[ENV_OUTPUT].setVoltage(clamp(envFollow * 8.f, 0.f, 10.f));

		// ---- lights ----
		if (lightDivider.process()) {
			lights[REC_LIGHT].setBrightness(recording ? 1.f : 0.f);
			lights[PLAY_LIGHT].setBrightness(playing ? 1.f : 0.f);
			lights[SYNC_LIGHT].setBrightness(sync ? 1.f : 0.f);
			lights[MODE_LIGHT].setBrightness(stretchMode ? 1.f : 0.f);
			lights[CLK_LIGHT].setBrightness((clockSeen && lastEdge < 0.06f) ? 1.f : 0.f);
			for (int i = 0; i < 8; i++) {
				int mapped = (numSlices <= 8) ? i : i * numSlices / 8;
				bool exists = (numSlices <= 8) ? (i < numSlices) : true;
				bool isCur = (numSlices <= 8) ? (i == curSlice) : (curSlice * 8 / numSlices == i);
				lights[SLICE_LIGHT + i].setBrightness(!exists ? 0.f : isCur ? 1.f : 0.12f);
				static_cast<void>(mapped);
			}
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "playing", json_boolean(playing));
		json_object_set_new(root, "sync", json_boolean(sync));
		json_object_set_new(root, "stretchMode", json_boolean(stretchMode));
		json_t* mk = json_array();
		for (int m : markers)
			json_array_append_new(mk, json_integer(m));
		json_object_set_new(root, "markers", mk);
		json_object_set_new(root, "reelLen", json_integer(reelLen));
		json_object_set_new(root, "sampleRate", json_real(APP->engine->getSampleRate()));
		// save the reel audio into patch storage
		if (reelLen > 0) {
			try {
				std::string dir = createPatchStorageDirectory();
				std::ofstream f(dir + "/reel.bin", std::ios::binary);
				if (f) {
					int32_t n = reelLen;
					f.write((const char*)&n, 4);
					f.write((const char*)bufL.data(), (size_t)reelLen * 4);
					f.write((const char*)bufR.data(), (size_t)reelLen * 4);
				}
			}
			catch (Exception& e) {
			}
		}
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "playing"))) playing = json_boolean_value(j);
		if ((j = json_object_get(root, "sync"))) sync = json_boolean_value(j);
		if ((j = json_object_get(root, "stretchMode"))) stretchMode = json_boolean_value(j);
		markers.clear();
		json_t* mk = json_object_get(root, "markers");
		if (mk)
			for (size_t i = 0; i < json_array_size(mk); i++)
				markers.push_back((int)json_integer_value(json_array_get(mk, i)));
		try {
			std::string dir = getPatchStorageDirectory();
			std::ifstream f(dir + "/reel.bin", std::ios::binary);
			if (f) {
				int32_t n = 0;
				f.read((char*)&n, 4);
				if (n > 0 && n <= bufLen) {
					f.read((char*)bufL.data(), (size_t)n * 4);
					f.read((char*)bufR.data(), (size_t)n * 4);
					reelLen = n;
				}
			}
		}
		catch (Exception& e) {
		}
		// drop markers that fall outside the reel
		markers.erase(std::remove_if(markers.begin(), markers.end(),
			[&](int m) { return m <= 0 || m >= reelLen; }), markers.end());
		std::sort(markers.begin(), markers.end());
		playPos = 0.0;
	}
};

struct ChimeraWidget : ModuleWidget {
	ChimeraWidget(Chimera* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Chimera.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace clayout;

		static const int btns[5] = {Chimera::REC_PARAM, Chimera::PLAY_PARAM, Chimera::SPLICE_PARAM, Chimera::SYNC_PARAM, Chimera::MODE_PARAM};
		for (int i = 0; i < 5; i++)
			addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X[i], YA)), module, btns[i]));
		addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(BTN_X[0], YA + YA_LED_DY)), module, Chimera::REC_LIGHT));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(BTN_X[1], YA + YA_LED_DY)), module, Chimera::PLAY_LIGHT));
		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(BTN_X[3], YA + YA_LED_DY)), module, Chimera::SYNC_LIGHT));
		addChild(createLightCentered<MediumLight<BlueLight>>(mm2px(Vec(BTN_X[4], YA + YA_LED_DY)), module, Chimera::MODE_LIGHT));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SOS_X, YB)), module, Chimera::SOS_PARAM));
		static const int eq[3] = {Chimera::EQ_LOW_PARAM, Chimera::EQ_MID_PARAM, Chimera::EQ_HIGH_PARAM};
		for (int i = 0; i < 3; i++)
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(EQ_X[i], YB)), module, eq[i]));

		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(SPEED_X, YC)), module, Chimera::SPEED_PARAM));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(PITCH_X, YC)), module, Chimera::PITCH_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(GRAIN_X, YC)), module, Chimera::GRAIN_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SLICE_X, YC)), module, Chimera::SLICE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CHOP_X, YC)), module, Chimera::CHOP_PARAM));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SCATTER_X, YD)), module, Chimera::SCATTER_PARAM));
		for (int i = 0; i < 8; i++)
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(SLICE_LIGHT_X0 + i * SLICE_LIGHT_DX, SLICE_LIGHT_Y)), module, Chimera::SLICE_LIGHT + i));
		addChild(createLightCentered<SmallLight<WhiteLight>>(mm2px(Vec(CLK_LIGHT_X, CLK_LIGHT_Y)), module, Chimera::CLK_LIGHT));

		static const int r1[6] = {Chimera::SPEED_INPUT, Chimera::VOCT_INPUT, Chimera::GRAIN_INPUT,
		                          Chimera::SLICE_INPUT, Chimera::SCATTER_INPUT, Chimera::CLOCK_INPUT};
		for (int i = 0; i < 6; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[i], R1)), module, r1[i]));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[0], R2)), module, Chimera::REC_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[1], R2)), module, Chimera::SPLICE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[2], R2)), module, Chimera::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[3], R2)), module, Chimera::IN_R_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[4], R2)), module, Chimera::EOS_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[5], R2)), module, Chimera::ENV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[4], R3)), module, Chimera::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[5], R3)), module, Chimera::OUT_R_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Chimera* module = getModule<Chimera>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Stretch mode (granular; off = clean tape)", "", &module->stretchMode));
		menu->addChild(createMenuItem("Load sample…", "", [=]() {
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, NULL);
			if (path) {
				module->loadWav(path);
				free(path);
			}
		}));
		menu->addChild(createMenuItem("Restore demo loop", "", [=]() {
			module->synthesizeDemoLoop(APP->engine->getSampleRate());
		}));
		menu->addChild(createMenuItem("Clear splice markers", "", [=]() {
			module->markers.clear();
		}));
		menu->addChild(createMenuItem("Erase reel", "", [=]() {
			module->reelLen = 0;
			module->markers.clear();
		}));
	}
};

Model* modelChimera = createModel<Chimera, ChimeraWidget>("Chimera");
