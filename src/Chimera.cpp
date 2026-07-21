// Chimera — a splice-based stereo loop sampler for VCV Rack with true
// granular time-stretching (independent speed and pitch), clock-matched
// stretching, chopping/splicing, scatter playback, sound-on-sound
// layering, a 3-band EQ, and a soft output limiter. Inspired by tape-style
// micro-sound samplers such as the Make Noise Morphagene, but an entirely
// original design and implementation.

#include "plugin.hpp"
#include "layout_chimera.hpp"
#include <osdialog.h>
#include <cstring>
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
		SPLICE_LIGHT,
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
	int reelRev = 0;    // bumped whenever the reel audio changes (display cache key)
	float lastSr = 44100.f;

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
	dsp::PulseGenerator eosPulse, splicePulse;
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

	void resetReelTransport() {
		recording = false;
		freshRecording = false;
		recWrite = 0;
		curSlice = 0;
		numSlices = 1;
		playPos = 0.0;
		sliceStart = 0.0;
		sliceEnd = std::max(1, reelLen);
		sliceDir = 1.f;
		spawnTimer = 0.0;
		for (Grain& g : grains)
			g.active = false;
		syncBeat = -1;
		declick = 1.f;
		envFollow = 0.f;
	}

	void eraseReel() {
		reelLen = 0;
		markers.clear();
		resetReelTransport();
		reelRev++;
	}

	void updateButtonLights(bool spliceLit) {
		if (!lightDivider.process())
			return;
		lights[SPLICE_LIGHT].setBrightness(spliceLit ? 1.f : 0.f);
		lights[REC_LIGHT].setBrightness(recording ? 1.f : 0.f);
		lights[PLAY_LIGHT].setBrightness(playing ? 1.f : 0.f);
		lights[SYNC_LIGHT].setBrightness(sync ? 1.f : 0.f);
		lights[MODE_LIGHT].setBrightness(stretchMode ? 1.f : 0.f);
	}

	void allocate(float sr) {
		bufLen = (int)(sr * maxSeconds(sr));
		bufL.assign(bufLen, 0.f);
		bufR.assign(bufLen, 0.f);
		eraseReel();
	}

	// linear-resample the current reel (and markers) to a new sample rate
	void resampleReel(float fromSr, float toSr) {
		if (reelLen < 2 || fromSr <= 0.f || toSr <= 0.f)
			return;
		std::vector<float> oL(bufL.begin(), bufL.begin() + reelLen);
		std::vector<float> oR(bufR.begin(), bufR.begin() + reelLen);
		double ratio = (double)fromSr / toSr;
		int nLen = std::min((int)(reelLen / ratio), bufLen);
		if (nLen < 2) {
			eraseReel();
			return;
		}
		for (int i = 0; i < nLen; i++) {
			double sp = i * ratio;
			int i0 = std::min((int)sp, reelLen - 2);
			float fr = (float)(sp - i0);
			bufL[i] = oL[i0] + (oL[i0 + 1] - oL[i0]) * fr;
			bufR[i] = oR[i0] + (oR[i0 + 1] - oR[i0]) * fr;
		}
		reelLen = nLen;
		for (int& m : markers)
			m = clamp((int)(m / ratio), 1, std::max(1, reelLen - 1));
		markers.erase(std::unique(markers.begin(), markers.end()), markers.end());
		playPos = 0.0;
		reelRev++;
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		// keep the user's reel across sample-rate changes instead of wiping it
		float oldSr = lastSr;
		std::vector<float> oL, oR;
		std::vector<int> oM = markers;
		int oLen = reelLen;
		if (oLen > 1) {
			oL.assign(bufL.begin(), bufL.begin() + oLen);
			oR.assign(bufR.begin(), bufR.begin() + oLen);
		}
		allocate(e.sampleRate);
		if (oLen > 1 && oldSr > 0.f) {
			double ratio = (double)oldSr / e.sampleRate;
			int nLen = std::min((int)(oLen / ratio), bufLen);
			if (nLen >= 2) {
				for (int i = 0; i < nLen; i++) {
					double sp = i * ratio;
					int i0 = std::min((int)sp, oLen - 2);
					float fr = (float)(sp - i0);
					bufL[i] = oL[i0] + (oL[i0 + 1] - oL[i0]) * fr;
					bufR[i] = oR[i0] + (oR[i0 + 1] - oR[i0]) * fr;
				}
				reelLen = nLen;
				markers.clear();
				for (int m : oM) {
					int nm = clamp((int)(m / ratio), 1, reelLen - 1);
					if (markers.empty() || markers.back() != nm)
						markers.push_back(nm);
				}
			}
		}
		// An intentionally empty reel stays empty. The constructor installs
		// the demo explicitly; sample-rate changes should not resurrect it.
		lastSr = e.sampleRate;
		eqLowVal = eqMidVal = eqHighVal = 1e9f;
		reelRev++;
	}

	// small built-in arpeggio loop so the controls are audible out of the box
	void synthesizeDemoLoop(float sr) {
		resetReelTransport();
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
		playPos = 0.0;
		reelRev++;
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
		uint16_t fmt = 0, sampleFormat = 0, channels = 0, bits = 0;
		uint32_t srate = 0;
		std::streampos dataPos = std::streampos(-1);
		uint32_t dataSize = 0;
		while (f && !f.eof()) {
			f.read(tag, 4);
			uint32_t size = rd32();
			if (!f)
				break;
			std::string t(tag, 4);
			if (t == "fmt ") {
				if (size < 16)
					return false;
				fmt = rd16();
				sampleFormat = fmt;
				channels = rd16();
				srate = rd32();
				rd32();
				rd16();
				bits = rd16();
				if (size > 16) {
					uint32_t extraSize = size - 16;
					char extra[24] = {};
					uint32_t take = std::min<uint32_t>(extraSize, sizeof(extra));
					f.read(extra, take);
					// WAVE_FORMAT_EXTENSIBLE stores the real PCM/float format as
					// the first DWORD of its SubFormat GUID.
					if (fmt == 0xFFFE && take >= 24) {
						uint32_t subFormat = 0;
						std::memcpy(&subFormat, extra + 8, sizeof(subFormat));
						sampleFormat = (uint16_t)subFormat;
					}
					if (extraSize > take)
						f.seekg((std::streamoff)(extraSize - take), std::ios::cur);
				}
				if (size & 1)
					f.seekg(1, std::ios::cur);
			}
			else if (t == "data") {
				// Remember the chunk and keep scanning. Although fmt normally
				// precedes data, legal RIFF files do not require that ordering.
				dataPos = f.tellg();
				dataSize = size;
				f.seekg((std::streamoff)size + (size & 1u), std::ios::cur);
			}
			else {
				f.seekg((std::streamoff)size + (size & 1u), std::ios::cur);
			}
		}
		if (dataPos == std::streampos(-1) || dataSize == 0 || channels == 0 || channels > 8 ||
		    srate < 1000 || srate > 768000)
			return false;
		if (!(sampleFormat == 1 || sampleFormat == 3))
			return false;
		if ((sampleFormat == 1 && !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
		    (sampleFormat == 3 && !(bits == 32 || bits == 64)))
			return false;
		float engineSr = APP->engine->getSampleRate();
		int bytesPer = bits / 8;
		uint64_t frameBytes = (uint64_t)bytesPer * channels;
		int frames = (int)std::min<uint64_t>(dataSize / frameBytes, 0x7fffffff);
		if (frames < 2)
			return false;
		// resample to engine rate on load (linear)
		double ratio = (double)srate / engineSr;
		int outFrames = (int)std::min((double)frames / ratio, (double)bufLen);
		if (outFrames < 2)
			return false;
		int framesNeeded = std::min(frames,
			(int)std::ceil((outFrames - 1) * ratio) + 2);
		size_t bytesNeeded = (size_t)framesNeeded * (size_t)frameBytes;
		// A corrupt header must not make Rack reserve gigabytes. Eight-channel
		// 60-second files at normal production rates remain comfortably below it.
		static const size_t MAX_WAV_BYTES = (size_t)512 * 1024 * 1024;
		if (bytesNeeded > MAX_WAV_BYTES)
			return false;
		std::vector<char> data(bytesNeeded);
		f.clear();
		f.seekg(dataPos);
		f.read(data.data(), (std::streamsize)bytesNeeded);
		if (!f)
			return false;
		auto sampleAt = [&](int i, int c) -> float {
			const char* p = data.data() + (size_t)(i * channels + std::min<int>(c, channels - 1)) * bytesPer;
			float value = 0.f;
			if (bits == 16) {
				int16_t x = 0;
				std::memcpy(&x, p, sizeof(x));
				value = x / 32768.f;
			}
			else if (bits == 24) {
				int32_t x = (uint8_t)p[0] | ((uint8_t)p[1] << 8) | ((uint8_t)p[2] << 16);
				if (x & 0x800000)
					x |= ~0xFFFFFF;
				value = x / 8388608.f;
			}
			else if (bits == 32 && sampleFormat == 3) {
				float x = 0.f;
				std::memcpy(&x, p, sizeof(x));
				value = x;
			}
			else if (bits == 64 && sampleFormat == 3) {
				double x = 0.0;
				std::memcpy(&x, p, sizeof(x));
				value = (float)x;
			}
			else if (bits == 32) {
				int32_t x = 0;
				std::memcpy(&x, p, sizeof(x));
				value = x / 2147483648.f;
			}
			else if (bits == 8)
				value = ((const uint8_t*)p)[0] / 128.f - 1.f;
			return std::isfinite(value) ? clamp(value, -8.f, 8.f) : 0.f;
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
		resetReelTransport();
		reelRev++;
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
		curSlice = clamp(curSlice, 0, numSlices - 1);
	}

	void sliceBounds(int idx, double& start, double& end) {
		if (!markers.empty()) {
			int n = (int)markers.size() + 1;
			idx = clamp(idx, 0, n - 1);
			start = (idx == 0) ? 0.0 : (double)markers[idx - 1];
			end = (idx == n - 1) ? (double)reelLen : (double)markers[idx];
		}
		else {
			idx = clamp(idx, 0, numSlices - 1);
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
				// Keep only samples that were actually written. Padding a very
				// short take used to expose stale audio from the previous reel.
				reelLen = recWrite >= 2 ? recWrite : 0;
				markers.clear();
				playPos = 0.0;
				reelRev++;
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
		    reelLen > 2) {
			int m = clamp((int)playPos, 1, reelLen - 2);
			// with sync running, splices snap to the clock grid so every slice
			// is an exact number of beats — this is what keeps the loop tight
			if (sync && clockSeen && inputs[CLOCK_INPUT].isConnected()) {
				double beat = (double)clockPeriod * sr;
				if (beat > 32.0)
					m = clamp((int)(std::round(m / beat) * beat), 1, reelLen - 2);
			}
			auto it = std::lower_bound(markers.begin(), markers.end(), m);
			if (it == markers.end() || std::abs(*it - m) > (int)(0.01f * sr))
				markers.insert(it, m);
			splicePulse.trigger(0.2f);
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
		bool spliceLit = splicePulse.process(dt);

		// ---- fresh recording (defines the reel) ----
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float inR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f : inL;
		if (!std::isfinite(inL)) inL = 0.f;
		if (!std::isfinite(inR)) inR = 0.f;
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
				reelRev++;
			}
			// monitor input while building the reel
			outputs[OUT_L_OUTPUT].setVoltage(inL * 5.f);
			outputs[OUT_R_OUTPUT].setVoltage(inR * 5.f);
			outputs[ENV_OUTPUT].setVoltage(0.f);
			outputs[EOS_OUTPUT].setVoltage(0.f);
			eosPulse.process(dt);
			updateButtonLights(spliceLit);
			return;
		}

		if (reelLen <= 0) {
			outputs[OUT_L_OUTPUT].setVoltage(0.f);
			outputs[OUT_R_OUTPUT].setVoltage(0.f);
			outputs[ENV_OUTPUT].setVoltage(0.f);
			outputs[EOS_OUTPUT].setVoltage(0.f);
			envFollow = 0.f;
			eosPulse.process(dt);
			updateButtonLights(spliceLit);
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
				// scatter decisions land ON the downbeat when synced, so
				// random slice jumps stay locked to the grid
				float scatter = clamp(params[SCATTER_PARAM].getValue() + inputs[SCATTER_INPUT].getVoltage() / 10.f, 0.f, 1.f);
				if (scatter > 0.003f && random::uniform() < scatter) {
					sliceDir = (random::uniform() < scatter * 0.4f) ? -1.f : 1.f;
					enterSlice((int)(random::u32() % numSlices), false);
					sliceLen = std::max(64.0, sliceEnd - sliceStart);
				}
				else if (scatter <= 0.003f && curSlice != selSlice) {
					sliceDir = 1.f;
					enterSlice(selSlice, false);
					sliceLen = std::max(64.0, sliceEnd - sliceStart);
				}
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
		// clock-synced grains: the knob picks a musical division of the beat,
		// so granular stutters are rhythmic instead of free-running
		if (syncActive && stretchMode) {
			static const float GDIV[9] = {1/16.f, 1/12.f, 1/8.f, 1/6.f, 1/4.f, 1/3.f, 1/2.f, 3/4.f, 1.f};
			int gi = clamp((int)(grainCtl * 8.99f), 0, 8);
			grainSamp = std::min(std::max((double)clockPeriod * GDIV[gi] * (double)sr, 0.005 * (double)sr), 2.0 * (double)sr);
		}
		// tape mode: pitch merges into speed (varispeed) for pristine playback
		if (!stretchMode && playing)
			speedEff *= (float)pitchRatio;

		// ---- advance playhead within slice ----
		double prevPos = playPos;
		playPos += speedEff * sliceDir;
		bool jumped = false;
		if (playPos >= sliceEnd || playPos < sliceStart) {
			eosPulse.trigger(0.002f);
			float scatter = clamp(params[SCATTER_PARAM].getValue() + inputs[SCATTER_INPUT].getVoltage() / 10.f, 0.f, 1.f);
			int next = selSlice;
			// when clock-synced, scatter jumps happen on the downbeat snap
			// instead (above) so the pattern stays on the grid
			if (syncActive) {
				next = curSlice;
			}
			else {
				sliceDir = 1.f;
				if (scatter > 0.003f && random::uniform() < scatter) {
					next = (int)(random::u32() % numSlices);
					if (random::uniform() < scatter * 0.4f)
						sliceDir = -1.f;
					declick = 0.f; // scattered jumps land anywhere: fade the seam
				}
			}
			enterSlice(next, true);
			jumped = true;
		}
		else if (!syncActive && curSlice != selSlice && params[SCATTER_PARAM].getValue() < 0.003f && !inputs[SCATTER_INPUT].isConnected()) {
			// direct slice selection is immediate when free-running; when
			// clock-synced it waits for the downbeat (handled in the snap)
			enterSlice(selSlice, true);
			declick = 0.f;
			jumped = true;
		}

		// ---- overdub (sound on sound) along the playhead's path ----
		// writing a single rounded sample leaves gaps at any speed != 1x
		if (recording && !freshRecording) {
			float sos = params[SOS_PARAM].getValue();
			int i0 = (int)std::floor(std::min(prevPos, playPos));
			int i1 = (int)std::floor(std::max(prevPos, playPos));
			if (jumped || i1 - i0 > 16) {
				i0 = i1 = clamp((int)prevPos, 0, reelLen - 1);
			}
			for (int wi = std::max(0, i0); wi <= std::min(reelLen - 1, i1); wi++) {
				bufL[wi] = clamp(bufL[wi] * sos + inL, -2.f, 2.f);
				bufR[wi] = clamp(bufR[wi] * sos + inR, -2.f, 2.f);
			}
		}

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
		// a stopped tape is silent, not a held DC sample
		if (!stretchMode && !playing) {
			wetL = 0.f;
			wetR = 0.f;
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

		// ---- illuminated transport buttons ----
		updateButtonLights(spliceLit);
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
		int expectedLen = 1; // old patches always have storage when a reel exists
		if ((j = json_object_get(root, "reelLen")))
			expectedLen = (int)json_integer_value(j);
		reelLen = 0;
		resetReelTransport();
		markers.clear();
		json_t* mk = json_object_get(root, "markers");
		if (mk)
			for (size_t i = 0; i < json_array_size(mk); i++)
				markers.push_back((int)json_integer_value(json_array_get(mk, i)));
		if (expectedLen > 0) {
			try {
				std::string dir = getPatchStorageDirectory();
				std::ifstream f(dir + "/reel.bin", std::ios::binary);
				if (f) {
					int32_t n = 0;
					f.read((char*)&n, 4);
					if (n >= 2 && n <= bufLen) {
						f.read((char*)bufL.data(), (size_t)n * 4);
						f.read((char*)bufR.data(), (size_t)n * 4);
						if (f) {
							reelLen = n;
							for (int i = 0; i < reelLen; i++) {
								bufL[i] = std::isfinite(bufL[i]) ? clamp(bufL[i], -8.f, 8.f) : 0.f;
								bufR[i] = std::isfinite(bufR[i]) ? clamp(bufR[i], -8.f, 8.f) : 0.f;
							}
						}
					}
				}
			}
			catch (Exception& e) {
			}
		}
		// drop markers that fall outside the reel
		markers.erase(std::remove_if(markers.begin(), markers.end(),
			[&](int m) { return m <= 0 || m >= reelLen; }), markers.end());
		std::sort(markers.begin(), markers.end());
		markers.erase(std::unique(markers.begin(), markers.end()), markers.end());
		playPos = 0.0;
		// a reel saved at a different sample rate would play at the wrong pitch
		float savedSr = 0.f;
		if ((j = json_object_get(root, "sampleRate")))
			savedSr = (float)json_number_value(j);
		float esr = APP->engine->getSampleRate();
		if (reelLen > 1 && savedSr > 0.f && std::fabs(savedSr - esr) > 0.5f)
			resampleReel(savedSr, esr);
		updateSlices();
		sliceBounds(curSlice, sliceStart, sliceEnd);
		reelRev++;
	}
};

// Live reel display: waveform, splice markers, active slice, playhead, grain
// positions and record state. Click a region to select that slice.
struct ChimeraDisplay : LedDisplay {
	Chimera* module = nullptr;
	static const int NCOL = 168;
	std::vector<float> peaks;
	int cachedRev = -1;
	int refreshCtr = 0;

	void rebuildPeaks(int len) {
		peaks.assign(NCOL, 0.f);
		if (!module || len < 2)
			return;
		for (int c = 0; c < NCOL; c++) {
			int a = (int)((double)c * len / NCOL);
			int b = std::max(a + 1, (int)((double)(c + 1) * len / NCOL));
			int stride = std::max(1, (b - a) / 48);
			float pk = 0.f;
			for (int i = a; i < b && i < module->bufLen; i += stride)
				pk = std::max(pk, std::max(std::fabs(module->bufL[i]), std::fabs(module->bufR[i])));
			peaks[c] = std::min(1.f, pk);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) {
			LedDisplay::drawLayer(args, layer);
			return;
		}
		float w = box.size.x, h = box.size.y;
		nvgSave(args.vg);
		nvgScissor(args.vg, 0, 0, w, h);

		std::shared_ptr<Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		NVGcolor dim = nvgRGB(0x8b, 0x8f, 0x99);
		NVGcolor wave = nvgRGB(0xbf, 0xd2, 0xe8);
		NVGcolor red = nvgRGB(0xe8, 0x4b, 0x4b);

		if (!module) {
			// module browser preview: a decorative waveform
			nvgBeginPath(args.vg);
			for (int i = 0; i < 60; i++) {
				float x = w * i / 59.f;
				float y = h * 0.5f + std::sin(i * 0.7f) * h * 0.3f * std::sin(i * 0.13f);
				if (i == 0) nvgMoveTo(args.vg, x, y); else nvgLineTo(args.vg, x, y);
			}
			nvgStrokeColor(args.vg, wave);
			nvgStrokeWidth(args.vg, 1.f);
			nvgStroke(args.vg);
			nvgResetScissor(args.vg);
			nvgRestore(args.vg);
			return;
		}

		bool fresh = module->recording && module->freshRecording;
		int len = fresh ? std::max(module->recWrite, 1) : module->reelLen;

		// refresh the cached waveform when the reel changes; keep refreshing
		// at a low rate while any recording is in progress
		refreshCtr++;
		if (module->reelRev != cachedRev || (module->recording && refreshCtr >= 15)) {
			cachedRev = module->reelRev;
			refreshCtr = 0;
			rebuildPeaks(len);
		}

		if (len < 2) {
			if (font) {
				nvgFontFaceId(args.vg, font->handle);
				nvgFontSize(args.vg, 10.f);
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(args.vg, dim);
				nvgText(args.vg, w * 0.5f, h * 0.5f, "EMPTY - REC OR LOAD SAMPLE", NULL);
			}
			nvgResetScissor(args.vg);
			nvgRestore(args.vg);
			return;
		}

		// active slice highlight
		if (!fresh && module->numSlices > 1) {
			float x0 = (float)(module->sliceStart / len) * w;
			float x1 = (float)(module->sliceEnd / len) * w;
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x0, 0, std::max(1.f, x1 - x0), h);
			nvgFillColor(args.vg, nvgRGBA(0xd9, 0xa4, 0x41, 0x30));
			nvgFill(args.vg);
		}

		// waveform
		float mid = h * 0.54f;
		float amp = h * 0.31f;
		nvgBeginPath(args.vg);
		for (int c = 0; c < NCOL; c++) {
			float x = (c + 0.5f) * w / NCOL;
			float e = std::max(0.02f, peaks[c]) * amp;
			nvgMoveTo(args.vg, x, mid - e);
			nvgLineTo(args.vg, x, mid + e);
		}
		nvgStrokeColor(args.vg, fresh ? nvgRGBA(0xe8, 0x4b, 0x4b, 0xb0) : nvgRGBA(0xbf, 0xd2, 0xe8, 0x9a));
		nvgStrokeWidth(args.vg, std::max(1.f, w / NCOL * 0.7f));
		nvgStroke(args.vg);

		// splice markers
		for (int m : module->markers) {
			float x = (float)m / len * w;
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x, 0);
			nvgLineTo(args.vg, x, h);
			nvgStrokeColor(args.vg, nvgRGBA(0xd9, 0xa4, 0x41, 0x90));
			nvgStrokeWidth(args.vg, 1.f);
			nvgStroke(args.vg);
		}

		// grain positions (stretch mode)
		if (!fresh && module->stretchMode) {
			for (const Chimera::Grain& g : module->grains) {
				if (!g.active)
					continue;
				float x = (float)(g.readPos / len) * w;
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, x, h * 0.16f, 1.6f);
				nvgFillColor(args.vg, nvgRGBA(0x7f, 0xd4, 0xe8, 0xc0));
				nvgFill(args.vg);
			}
		}

		// playhead
		float px = fresh ? ((float)module->recWrite / len * w)
		                 : (float)(module->playPos / len) * w;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, px, 0);
		nvgLineTo(args.vg, px, h);
		nvgStrokeColor(args.vg, fresh ? red : nvgRGB(0xff, 0xff, 0xff));
		nvgStrokeWidth(args.vg, 1.4f);
		nvgStroke(args.vg);

		// status line
		if (font) {
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 9.f);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			float sec = len / APP->engine->getSampleRate();
			char txt[96];
			if (fresh)
				snprintf(txt, sizeof(txt), "REC %.1fs", sec);
			else
				snprintf(txt, sizeof(txt), "%s %.1fs  S%d/%d%s%s",
					module->stretchMode ? "STRETCH" : "TAPE", sec,
					module->curSlice + 1, module->numSlices,
					module->sync ? "  SYNC" : "",
					(module->recording && !module->freshRecording) ? "  DUB" : "");
			nvgFillColor(args.vg, fresh ? red : dim);
			nvgText(args.vg, 3.f, 2.f, txt, NULL);
		}

		// clock pulse dot, top-right
		if (module->clockSeen) {
			float a = clamp(1.f - module->lastEdge / 0.1f, 0.f, 1.f);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, w - 6.f, 6.f, 2.2f);
			nvgFillColor(args.vg, nvgRGBA(0xd9, 0xa4, 0x41, (unsigned char)(0x28 + a * 0xd0)));
			nvgFill(args.vg);
		}

		// Eight-position slice navigator. For reels with more than eight
		// slices each dot represents a bank, so the current region remains
		// legible without crowding the waveform.
		float navY = h - 3.2f;
		float navStep = std::min(10.f, (w - 18.f) / 8.f);
		float navX0 = w * 0.5f - navStep * 3.5f;
		for (int i = 0; i < 8; i++) {
			bool exists = module->numSlices <= 8 ? i < module->numSlices : true;
			bool active = module->numSlices <= 8
				? i == module->curSlice
				: i == clamp(module->curSlice * 8 / module->numSlices, 0, 7);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, navX0 + i * navStep, navY, active ? 1.75f : 1.35f);
			nvgFillColor(args.vg, !exists ? nvgRGBA(0x8b, 0x8f, 0x99, 0x28)
				: active ? nvgRGB(0x7f, 0xd4, 0xe8)
				: nvgRGBA(0x8b, 0x8f, 0x99, 0x88));
			nvgFill(args.vg);
		}

		nvgResetScissor(args.vg);
		nvgRestore(args.vg);
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module && module->reelLen > 1) {
			double pos = clamp(e.pos.x / box.size.x, 0.f, 0.9999f) * module->reelLen;
			int n = module->numSlices;
			for (int i = 0; i < n; i++) {
				double s, en;
				module->sliceBounds(i, s, en);
				if (pos >= s && pos < en) {
					module->params[Chimera::SLICE_PARAM].setValue((i + 0.5f) / n);
					break;
				}
			}
			e.consume(this);
			return;
		}
		LedDisplay::onButton(e);
	}
};

struct ChimeraWidget : ModuleWidget {
	ChimeraWidget(Chimera* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Chimera.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace clayout;

		// transport: light-in-button bezels, no floating LEDs
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(BTN_X[0], YA)), module, Chimera::REC_PARAM, Chimera::REC_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(mm2px(Vec(BTN_X[1], YA)), module, Chimera::PLAY_PARAM, Chimera::PLAY_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(BTN_X[2], YA)), module, Chimera::SPLICE_PARAM, Chimera::SPLICE_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(mm2px(Vec(BTN_X[3], YA)), module, Chimera::SYNC_PARAM, Chimera::SYNC_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<BlueLight>>(mm2px(Vec(BTN_X[4], YA)), module, Chimera::MODE_PARAM, Chimera::MODE_LIGHT));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(SOS_X, YB)), module, Chimera::SOS_PARAM));
		static const int eq[3] = {Chimera::EQ_LOW_PARAM, Chimera::EQ_MID_PARAM, Chimera::EQ_HIGH_PARAM};
		for (int i = 0; i < 3; i++)
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(EQ_X[i], YB)), module, eq[i]));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(CHOP_X, YB)), module, Chimera::CHOP_PARAM));

		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(SPEED_X, YC)), module, Chimera::SPEED_PARAM));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(PITCH_X, YC)), module, Chimera::PITCH_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(GRAIN_X, YC)), module, Chimera::GRAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(SLICE_X, YC)), module, Chimera::SLICE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(SCATTER_X, YC)), module, Chimera::SCATTER_PARAM));

		ChimeraDisplay* disp = createWidget<ChimeraDisplay>(mm2px(Vec(DISP_X, DISP_Y)));
		disp->box.size = mm2px(Vec(DISP_W, DISP_H));
		disp->module = module;
		addChild(disp);

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
			module->updateSlices();
			module->enterSlice(module->curSlice, false);
		}));
		menu->addChild(createMenuItem("Erase reel", "", [=]() {
			module->eraseReel();
		}));
	}
};

Model* modelChimera = createModel<Chimera, ChimeraWidget>("Chimera");
