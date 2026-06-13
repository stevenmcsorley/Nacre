// Fieldfare — a pocket-studio performance workstation for VCV Rack,
// inspired by the workflow of the Teenage Engineering OP-1 field: four
// color-coded context knobs editing the selected page (sound/envelope/
// effects/mix), a set of synth engines, a four-track varispeed tape deck
// with loop brace and motor inertia, an endless note sequencer, and
// master drive + reverb. Original implementation; wavetable data decoded
// from Émilie Gillet's MIT-licensed Plaits (see eurorack/), reverb is the
// MIT-licensed Clouds reverb.

#include "plugin.hpp"
#include "layout_fieldfare.hpp"
#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/reverb.h"
#include "plaits/resources.h"
#include <fstream>

static const int NUM_VOICES = 4;
static const int NUM_TRACKS = 4;
static const float TAPE_SECONDS = 60.f;
static const int NUM_ENGINES = 7;
static const int MAX_SEQ = 128;

struct Fieldfare : Module {
	enum ParamId {
		ENUMS(MACRO_PARAM, 4),  // blue, green, white, orange
		SPEED_PARAM,
		ENGINE_PARAM,
		PAGE_PARAM,
		SEQ_PARAM,
		TRACK_PARAM,
		REC_PARAM,
		PLAY_PARAM,
		REV_PARAM,
		LOOP_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		GATE_INPUT,
		CLOCK_INPUT,
		SPEED_INPUT,
		REC_INPUT,
		PLAY_INPUT,
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
		ENUMS(ENGINE_LIGHT, 3),
		ENUMS(PAGE_LIGHT, 3),
		ENUMS(SEQ_LIGHT, 3),
		ENUMS(TRACK_LIGHT, 3),
		REC_LIGHT,
		PLAY_LIGHT,
		REV_LIGHT,
		ENUMS(LOOP_LIGHT, 3),
		LIGHTS_LEN
	};

	// ---- synth voice ----
	struct Voice {
		float phase = 0.f, phase2 = 0.f, lfoPh = 0.f;
		float voct = 0.f;
		bool gate = false;
		// ADSR
		int stage = 4; // 0 a, 1 d, 2 s, 3 r, 4 idle
		float env = 0.f;
		// filters / state
		float lp = 0.f, lp2 = 0.f;
		uint32_t rng = 12345;
		double samplePos = 0.0;
		// karplus-strong
		std::vector<float> ks;
		int ksLen = 0, ksPos = 0;
		float ksLp = 0.f;
		// grit
		float gritEnv = 0.f, gritPh = 0.f, gritHeld = 0.f;
		int gritHold = 0;

		float noise() {
			rng = rng * 1664525u + 1013904223u;
			return (float)(int32_t)rng / 2147483648.f;
		}
	};

	Voice voices[NUM_VOICES];
	float wavetable[64][257];   // decoded plaits waves

	// per-page stored values (knobs write through)
	float engineMacro[NUM_ENGINES][4];
	float envVals[4] = {0.05f, 0.3f, 0.7f, 0.3f};        // A D S R
	float fxVals[4] = {0.1f, 0.25f, 0.4f, 0.1f};         // drive, verb amt, verb time, wow
	float trackVol[4] = {0.8f, 0.8f, 0.8f, 0.8f};

	int engine = 0;
	int page = 0;     // 0 sound, 1 env, 2 fx, 3 mix
	int track = 0;

	// knob write-through
	bool inited = false;
	int knobContext = -1;
	float knobPrev[4] = {};

	// ---- tape ----
	std::vector<float> tape[NUM_TRACKS];
	int tapeLen = 0;
	int trackUsed[NUM_TRACKS] = {};
	double tapePos = 0.0;
	float tapeSpeed = 0.f;   // slewed (motor inertia)
	bool tapePlaying = false;
	bool tapeRec = false;
	bool tapeRev = false;
	int loopIn = -1, loopOut = -1;
	int loopState = 0; // 0 none, 1 in set, 2 active
	double wowPh = 0.0;
	float wowNoise = 0.f;

	// ---- sampler (engine 7): grab audio from the inputs, play with V/oct ----
	std::vector<float> sampleBuf;
	int sampleMax = 0;
	int sampleLen = 0;
	bool sampling = false;
	float recHold = -1.f;
	bool recConsumed = false;

	// ---- endless sequencer ----
	int seqMode = 0; // 0 off, 1 record-arm, 2 play
	float seqNotes[MAX_SEQ];
	int seqLen = 0;
	int seqPos = 0;
	float seqGateTimer = 0.f;
	float lastClockPeriod = 0.5f;
	float clockEdgeTimer = 1e9f;

	clouds::Reverb reverb;
	uint16_t reverbBuffer[16384] = {};

	dsp::SchmittTrigger gateTrig, clockTrig, recTrig, playTrig;
	dsp::BooleanTrigger engineBtn, pageBtn, seqBtn, trackBtn, recBtn, playBtn, revBtn, loopBtn;
	dsp::ClockDivider lightDivider;
	float seqHold = -1.f;

	Fieldfare() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(MACRO_PARAM + 0, 0.f, 1.f, 0.5f, "Blue (per page)");
		configParam(MACRO_PARAM + 1, 0.f, 1.f, 0.5f, "Green (per page)");
		configParam(MACRO_PARAM + 2, 0.f, 1.f, 0.5f, "White (per page)");
		configParam(MACRO_PARAM + 3, 0.f, 1.f, 0.5f, "Orange (per page)");
		configParam(SPEED_PARAM, -2.f, 2.f, 0.f, "Tape speed", "x", 2.f);
		configButton(ENGINE_PARAM, "Engine (duo/saws/pulse/waves/string/grit/sample)");
		configButton(PAGE_PARAM, "Page (sound/envelope/effects/mix)");
		configButton(SEQ_PARAM, "Endless sequencer (off/record/play; hold: clear)");
		configButton(TRACK_PARAM, "Tape track select");
		configButton(REC_PARAM, "Tape record (tap) / hold 1 s: sample the audio inputs");
		configButton(PLAY_PARAM, "Tape play/stop");
		configButton(REV_PARAM, "Tape reverse");
		configButton(LOOP_PARAM, "Loop brace (press: set in, set out, clear)");
		configInput(VOCT_INPUT, "V/oct (poly, up to 4 voices)");
		configInput(GATE_INPUT, "Gate (poly)");
		configInput(CLOCK_INPUT, "Clock (advances the endless sequencer)");
		configInput(SPEED_INPUT, "Tape speed CV (1V/oct)");
		configInput(REC_INPUT, "Tape record gate");
		configInput(PLAY_INPUT, "Tape play gate (toggle)");
		configInput(IN_L_INPUT, "External audio L");
		configInput(IN_R_INPUT, "External audio R");
		configOutput(OUT_L_OUTPUT, "Left audio");
		configOutput(OUT_R_OUTPUT, "Right audio");
		lightDivider.setDivision(64);
		reverb.Init(reverbBuffer);
		reverb.set_diffusion(0.7f);
		// engine macro defaults
		for (int e = 0; e < NUM_ENGINES; e++)
			for (int k = 0; k < 4; k++)
				engineMacro[e][k] = 0.5f;
		decodeWaves();
		allocate(44100.f);
	}

	void decodeWaves() {
		for (int wv = 0; wv < 64; wv++) {
			const int16_t* w = plaits::wav_integrated_waves + wv * 3 * 260;
			float peak = 1.f;
			for (int i = 0; i < 256; i++) {
				wavetable[wv][i] = (float)(w[i + 1] - w[i]);
				peak = std::max(peak, std::fabs(wavetable[wv][i]));
			}
			float norm = 0.9f / peak;
			for (int i = 0; i < 256; i++)
				wavetable[wv][i] *= norm;
			wavetable[wv][256] = wavetable[wv][0];
		}
	}

	void allocate(float sr) {
		tapeLen = (int)(sr * TAPE_SECONDS);
		for (int t = 0; t < NUM_TRACKS; t++) {
			tape[t].assign(tapeLen, 0.f);
			trackUsed[t] = 0;
		}
		tapePos = 0.0;
		loopIn = loopOut = -1;
		loopState = 0;
		sampleMax = (int)(sr * 8.f);
		sampleBuf.assign(sampleMax, 0.f);
		sampleLen = 0;
		sampling = false;
		for (Voice& v : voices) {
			v.ks.assign((int)(sr / 25.f) + 8, 0.f);
		}
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		allocate(e.sampleRate);
	}

	void onReset() override {
		engine = 0;
		page = 0;
		track = 0;
		seqMode = 0;
		seqLen = 0;
		tapePlaying = false;
		tapeRec = false;
		tapeRev = false;
		knobContext = -1;
		inited = false;
		allocate(APP->engine->getSampleRate());
	}

	// the knobs edit the current page's stored values, write-through style
	float* pageVals() {
		switch (page) {
			case 0: return engineMacro[engine];
			case 1: return envVals;
			case 2: return fxVals;
			default: return trackVol;
		}
	}

	// ---- engines: one sample for one voice. freq in Hz, returns ~[-1,1] ----
	float renderVoice(Voice& v, float sr) {
		float freq = dsp::FREQ_C4 * std::exp2(v.voct);
		freq = clamp(freq, 8.f, 12000.f);
		float* m = engineMacro[engine];
		float out = 0.f;
		float d = freq / sr;
		switch (engine) {
			case 0: { // DUO: 2-op FM pair with fold
				float ratio = 0.5f + std::round(m[0] * 7.f) * 0.5f;
				v.phase2 += d * ratio;
				if (v.phase2 >= 1.f) v.phase2 -= 1.f;
				float mod = std::sin(2.f * M_PI * v.phase2) * m[1] * 6.f * v.env;
				v.phase += d;
				if (v.phase >= 1.f) v.phase -= 1.f;
				float a = std::sin(2.f * M_PI * v.phase + mod);
				float b = std::sin(2.f * M_PI * v.phase * (1.f + m[2] * 0.01f) + mod);
				out = (a + b) * 0.5f;
				float g = 1.f + m[3] * 5.f;
				out = std::sin(out * g * 0.5f * M_PI) * 0.9f; // sine fold
			} break;
			case 1: { // SAWS: detuned stack + sub
				static const float dets[5] = {0.f, -1.f, 1.f, -2.3f, 2.3f};
				float spread = m[0] * 0.012f;
				int nv = 1 + (int)(m[1] * 4.f + 0.5f);
				float s = 0.f;
				float ph = v.phase;
				for (int i = 0; i < nv; i++) {
					float pp = std::fmod(ph * (1.f + dets[i] * spread) + i * 0.17f, 1.f);
					s += pp * 2.f - 1.f;
				}
				v.phase += d;
				if (v.phase >= 1.f) v.phase -= 1.f;
				s /= nv;
				v.phase2 += d * 0.5f;
				if (v.phase2 >= 1.f) v.phase2 -= 1.f;
				s += (v.phase2 < 0.5f ? 1.f : -1.f) * m[2] * 0.5f; // sub octave
				float fc = 200.f * std::pow(60.f, m[3]);
				float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
				v.lp += a * (s - v.lp);
				out = v.lp;
			} break;
			case 2: { // PULSE: pwm square
				v.lfoPh += (0.1f + m[1] * 8.f) / sr;
				if (v.lfoPh >= 1.f) v.lfoPh -= 1.f;
				float width = 0.5f + (m[0] - 0.5f) * 0.8f + std::sin(2.f * M_PI * v.lfoPh) * m[2] * 0.4f;
				width = clamp(width, 0.05f, 0.95f);
				v.phase += d;
				if (v.phase >= 1.f) v.phase -= 1.f;
				out = (v.phase < width) ? 1.f : -1.f;
				out = std::tanh(out * (1.f + m[3] * 3.f)) * 0.8f;
			} break;
			case 3: { // WAVES: plaits wavetable scan
				float wf = m[0] * 62.99f;
				int w0 = (int)wf;
				float wfr = wf - w0;
				v.phase += d;
				if (v.phase >= 1.f) v.phase -= 1.f;
				float pp = v.phase * 256.f;
				int i0 = (int)pp;
				float fr = pp - i0;
				float s0 = wavetable[w0][i0] + (wavetable[w0][i0 + 1] - wavetable[w0][i0]) * fr;
				float s1 = wavetable[w0 + 1][i0] + (wavetable[w0 + 1][i0 + 1] - wavetable[w0 + 1][i0]) * fr;
				out = s0 + (s1 - s0) * wfr;
				if (m[2] > 0.02f) { // fold
					float g = 1.f + m[2] * 4.f;
					out = std::sin(out * g * 0.5f * M_PI);
				}
				float fc = 300.f * std::pow(40.f, m[3]);
				float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
				v.lp += a * (out - v.lp);
				out = v.lp;
			} break;
			case 4: { // STRING: karplus-strong pluck
				float damp = 0.2f + m[0] * 0.75f;
				int len = clamp((int)(sr / freq - (1.f - damp) / damp), 8, (int)v.ks.size() - 2);
				v.ksPos++;
				if (v.ksPos >= len) v.ksPos = 0;
				float y = v.ks[v.ksPos];
				int nx = (v.ksPos + 1) % len;
				float fb = 0.9f + m[2] * 0.099f;
				v.ksLp += damp * (0.5f * (y + v.ks[nx]) - v.ksLp);
				v.ks[v.ksPos] = v.ksLp * fb;
				float fc = 400.f * std::pow(20.f, m[3]);
				float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
				v.lp += a * (y - v.lp);
				out = v.lp * 1.4f;
			} break;
			case 6: { // SAMPLE: chromatic playback of the captured audio
				if (sampleLen < 64)
					break;
				float startF = m[0] * (sampleLen - 64);
				float lenF = std::max(64.f, m[1] * (sampleLen - startF));
				float rate = std::exp2(v.voct);
				v.samplePos += rate;
				if (v.samplePos >= lenF) {
					if (m[2] > 0.5f)
						v.samplePos = std::fmod(v.samplePos, (double)lenF); // loop
					else
						break; // one-shot done
				}
				double rp = startF + v.samplePos;
				int i0 = (int)rp;
				float fr = (float)(rp - i0);
				int i1 = std::min(i0 + 1, sampleLen - 1);
				out = sampleBuf[i0] + (sampleBuf[i1] - sampleBuf[i0]) * fr;
				float fc = 300.f * std::pow(60.f, m[3]);
				float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
				v.lp += a * (out - v.lp);
				out = v.lp * 1.5f;
			} break;
			default: { // GRIT: percussive fm + noise
				v.gritEnv *= std::exp(-1.f / (sr * (0.02f + m[1] * 0.5f)));
				float pe = 1.f + v.gritEnv * m[3] * 24.f;
				v.gritPh += d * pe;
				if (v.gritPh >= 1.f) v.gritPh -= 1.f;
				float body = std::sin(2.f * M_PI * v.gritPh);
				float fc = 100.f * std::pow(100.f, m[0]);
				float a = 1.f - std::exp(-2.f * M_PI * fc / sr);
				v.lp += a * (v.noise() - v.lp);
				out = body * 0.7f + v.lp * v.gritEnv * 0.8f;
				int holdN = 1 + (int)(m[2] * m[2] * 30.f);
				if (++v.gritHold >= holdN) {
					v.gritHold = 0;
					v.gritHeld = out;
				}
				out = (m[2] > 0.02f) ? v.gritHeld : out;
			} break;
		}
		return out;
	}

	void triggerVoice(Voice& v) {
		v.stage = 0;
		if (engine == 4) { // pluck the string
			float bright = 0.3f + engineMacro[4][1] * 0.7f;
			float s = 0.f;
			for (int i = 0; i < (int)v.ks.size(); i++) {
				s += bright * (v.noise() - s);
				v.ks[i] = s * 2.f;
			}
			v.ksLp = 0.f;
		}
		if (engine == 5)
			v.gritEnv = 1.f;
		v.samplePos = 0.0;
	}

	float adsr(Voice& v, float sr) {
		float a = 0.001f + envVals[0] * envVals[0] * 2.f;
		float dd = 0.005f + envVals[1] * envVals[1] * 3.f;
		float s = envVals[2];
		float r = 0.005f + envVals[3] * envVals[3] * 4.f;
		switch (v.stage) {
			case 0:
				v.env += 1.f / (a * sr);
				if (v.env >= 1.f) {
					v.env = 1.f;
					v.stage = 1;
				}
				break;
			case 1:
				v.env += (s - v.env) * (1.f - std::exp(-1.f / (dd * sr * 0.3f)));
				if (std::fabs(v.env - s) < 0.001f)
					v.stage = 2;
				break;
			case 2:
				v.env = s;
				break;
			case 3:
				v.env *= std::exp(-1.f / (r * sr * 0.3f));
				if (v.env < 0.0005f) {
					v.env = 0.f;
					v.stage = 4;
				}
				break;
			default:
				v.env = 0.f;
				break;
		}
		return v.env;
	}

	void process(const ProcessArgs& args) override {
		float sr = args.sampleRate;
		float dt = args.sampleTime;
		if (tapeLen == 0)
			return;

		// ---- buttons ----
		if (engineBtn.process(params[ENGINE_PARAM].getValue() > 0.f))
			engine = (engine + 1) % NUM_ENGINES;
		if (pageBtn.process(params[PAGE_PARAM].getValue() > 0.f))
			page = (page + 1) % 4;
		if (trackBtn.process(params[TRACK_PARAM].getValue() > 0.f))
			track = (track + 1) % NUM_TRACKS;
		if (revBtn.process(params[REV_PARAM].getValue() > 0.f))
			tapeRev = !tapeRev;
		if (loopBtn.process(params[LOOP_PARAM].getValue() > 0.f)) {
			if (loopState == 0) {
				loopIn = (int)tapePos;
				loopState = 1;
			}
			else if (loopState == 1) {
				loopOut = (int)tapePos;
				if (loopOut < loopIn)
					std::swap(loopIn, loopOut);
				if (loopOut - loopIn > 256)
					loopState = 2;
				else
					loopState = 0;
			}
			else {
				loopState = 0;
				loopIn = loopOut = -1;
			}
		}
		// endless seq button: tap cycles off->record->play->off; hold clears
		bool seqPressed = params[SEQ_PARAM].getValue() > 0.f;
		if (seqPressed) {
			if (seqHold < 0.f)
				seqHold = 0.f;
			else {
				seqHold += dt;
				if (seqHold > 1.2f && seqLen > 0) {
					seqLen = 0;
					seqMode = 0;
					seqHold = 1e9f; // consumed
				}
			}
		}
		else {
			if (seqHold >= 0.f && seqHold < 1.2f) {
				seqMode = (seqMode + 1) % 3;
				if (seqMode == 2 && seqLen == 0)
					seqMode = 0;
				if (seqMode == 2)
					seqPos = -1;
			}
			seqHold = -1.f;
		}
		// REC: tap toggles tape recording; hold ~1 s to sample the audio
		// inputs into the sampler engine (release or 8 s ends the sample)
		bool recPressed = params[REC_PARAM].getValue() > 0.f;
		if (recPressed) {
			if (recHold < 0.f) {
				recHold = 0.f;
				recConsumed = false;
			}
			else {
				recHold += dt;
				if (recHold > 0.8f && !recConsumed) {
					recConsumed = true;
					sampling = true;
					sampleLen = 0;
					engine = NUM_ENGINES - 1; // jump to the sampler engine
				}
			}
		}
		else {
			if (recHold >= 0.f) {
				if (sampling)
					sampling = false;
				else if (!recConsumed) {
					tapeRec = !tapeRec;
					if (tapeRec && !tapePlaying)
						tapePlaying = true;
				}
			}
			recHold = -1.f;
		}
		if (recTrig.process(inputs[REC_INPUT].getVoltage(), 0.1f, 1.f)) {
			tapeRec = !tapeRec;
			if (tapeRec && !tapePlaying)
				tapePlaying = true;
		}
		if (playBtn.process(params[PLAY_PARAM].getValue() > 0.f) ||
		    playTrig.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 1.f)) {
			tapePlaying = !tapePlaying;
			if (!tapePlaying)
				tapeRec = false;
		}

		// ---- knob write-through (page/engine context) ----
		if (!inited) {
			inited = true;
			for (int k = 0; k < 4; k++)
				knobPrev[k] = params[MACRO_PARAM + k].getValue();
		}
		int ctx = page * 100 + (page == 0 ? engine : 0);
		float* vals = pageVals();
		if (ctx != knobContext) {
			knobContext = ctx;
			for (int k = 0; k < 4; k++) {
				params[MACRO_PARAM + k].setValue(vals[k]);
				knobPrev[k] = vals[k];
			}
		}
		for (int k = 0; k < 4; k++) {
			float v = params[MACRO_PARAM + k].getValue();
			if (v != knobPrev[k]) {
				vals[k] = v;
				knobPrev[k] = v;
			}
		}

		// ---- sampler (engine 7): grab audio from the inputs, play with V/oct ----
	std::vector<float> sampleBuf;
	int sampleMax = 0;
	int sampleLen = 0;
	bool sampling = false;
	float recHold = -1.f;
	bool recConsumed = false;

	// ---- endless sequencer ----
		clockEdgeTimer += dt;
		bool clockTick = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		if (clockTick) {
			if (clockEdgeTimer < 4.f && clockEdgeTimer > 0.001f)
				lastClockPeriod = clockEdgeTimer;
			clockEdgeTimer = 0.f;
			// recording + clock arrives = you clearly want it to play
			if (seqMode == 1 && seqLen > 0) {
				seqMode = 2;
				seqPos = -1;
			}
		}

		// poly gate handling
		int chans = std::max(1, inputs[VOCT_INPUT].getChannels());
		chans = std::min(chans, NUM_VOICES);
		for (int c = 0; c < chans; c++) {
			bool g = inputs[GATE_INPUT].getVoltage(c) > 1.f;
			Voice& v = voices[c];
			if (g && !v.gate) {
				v.voct = inputs[VOCT_INPUT].getVoltage(c);
				if (seqMode == 1 && seqLen < MAX_SEQ)
					seqNotes[seqLen++] = v.voct; // capture
				triggerVoice(v);
			}
			else if (!g && v.gate) {
				v.stage = 3;
			}
			v.gate = g;
		}
		// seq playback: clock advances notes on voice 0
		if (seqMode == 2) {
			if (clockTick && seqLen > 0) {
				seqPos = (seqPos + 1) % seqLen;
				Voice& v = voices[0];
				v.voct = seqNotes[seqPos];
				triggerVoice(v);
				v.gate = true;
				seqGateTimer = lastClockPeriod * 0.5f;
			}
			if (seqGateTimer > 0.f) {
				seqGateTimer -= dt;
				if (seqGateTimer <= 0.f && voices[0].gate) {
					voices[0].stage = 3;
					voices[0].gate = false;
				}
			}
		}

		// ---- render synth ----
		float synth = 0.f;
		for (int c = 0; c < NUM_VOICES; c++) {
			Voice& v = voices[c];
			float e = adsr(v, sr);
			if (e > 0.0004f || v.stage < 3)
				synth += renderVoice(v, sr) * e;
		}
		synth *= 0.5f;

		float extL = inputs[IN_L_INPUT].getVoltage() / 5.f;
		float extR = inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() / 5.f : extL;
		float recordIn = synth + (extL + extR) * 0.5f;
		if (sampling) {
			if (sampleLen < sampleMax)
				sampleBuf[sampleLen++] = (extL + extR) * 0.5f;
			else
				sampling = false;
		}

		// ---- tape transport (motor inertia) ----
		float speedKnob = params[SPEED_PARAM].getValue() + inputs[SPEED_INPUT].getVoltage();
		float targetSpeed = tapePlaying ? std::exp2(clamp(speedKnob, -2.f, 2.f)) * (tapeRev ? -1.f : 1.f) : 0.f;
		tapeSpeed += (targetSpeed - tapeSpeed) * (1.f - std::exp(-dt / 0.12f));

		// wow & flutter from the FX page
		float wow = 0.f;
		if (fxVals[3] > 0.01f) {
			wowPh += 2.0 * M_PI * 1.3 * dt;
			if (wowPh > 2.0 * M_PI)
				wowPh -= 2.0 * M_PI;
			wowNoise += 0.001f * (voices[0].noise() - wowNoise);
			wow = (std::sin((float)wowPh) * 0.004f + wowNoise * 0.5f) * fxVals[3];
		}

		float tapeMix = 0.f;
		if (std::fabs(tapeSpeed) > 0.0005f) {
			double oldPos = tapePos;
			tapePos += tapeSpeed * (1.f + wow);
			// loop brace
			if (loopState == 2) {
				if (tapeSpeed > 0.f && tapePos >= loopOut)
					tapePos = loopIn + (tapePos - loopOut);
				else if (tapeSpeed < 0.f && tapePos < loopIn)
					tapePos = loopOut - (loopIn - tapePos);
			}
			if (tapePos >= tapeLen)
				tapePos -= tapeLen;
			if (tapePos < 0.0)
				tapePos += tapeLen;
			// record: fill every integer slot crossed
			if (tapeRec) {
				int a = (int)oldPos;
				int b = (int)tapePos;
				int steps = std::abs(b - a);
				if (steps <= 8) {
					int dir = (tapeSpeed >= 0.f) ? 1 : -1;
					int i = a;
					for (int s = 0; s <= steps; s++) {
						int idx = ((i % tapeLen) + tapeLen) % tapeLen;
						tape[track][idx] = recordIn;
						trackUsed[track] = std::max(trackUsed[track], idx + 1);
						i += dir;
					}
				}
			}
			// play all tracks at the head
			int i0 = (int)tapePos;
			float fr = (float)(tapePos - i0);
			int i1 = (i0 + 1) % tapeLen;
			for (int t = 0; t < NUM_TRACKS; t++) {
				float s = tape[t][i0] + (tape[t][i1] - tape[t][i0]) * fr;
				tapeMix += s * trackVol[t];
			}
		}

		// ---- master out: synth monitor + tape -> drive -> reverb ----
		float mix = synth + (extL + extR) * 0.5f * 0.5f + tapeMix;
		float drive = 1.f + fxVals[0] * 6.f;
		mix = std::tanh(mix * drive) / std::tanh(drive * 0.4f + 0.6f);
		reverb.set_amount(fxVals[1] * 0.54f);
		reverb.set_time(0.35f + 0.6f * fxVals[2]);
		reverb.set_input_gain(0.2f);
		reverb.set_lp(0.75f);
		clouds::FloatFrame frame;
		frame.l = mix;
		frame.r = mix;
		reverb.Process(&frame, 1);
		outputs[OUT_L_OUTPUT].setVoltage(clamp(frame.l * 5.f, -12.f, 12.f));
		outputs[OUT_R_OUTPUT].setVoltage(clamp(frame.r * 5.f, -12.f, 12.f));

		// ---- lights ----
		if (lightDivider.process()) {
			static const float engCol[NUM_ENGINES][3] = {
				{0.1f, 0.4f, 1.f}, {0.1f, 1.f, 0.2f}, {1.f, 0.7f, 0.f},
				{0.7f, 0.f, 1.f}, {0.f, 1.f, 1.f}, {1.f, 0.1f, 0.1f},
				{1.f, 1.f, 1.f}};
			static const float pgCol[4][3] = {
				{0.1f, 0.4f, 1.f}, {0.1f, 1.f, 0.2f}, {1.f, 0.7f, 0.f}, {1.f, 1.f, 1.f}};
			static const float trCol[4][3] = {
				{1.f, 0.2f, 0.2f}, {1.f, 0.7f, 0.f}, {0.1f, 1.f, 0.2f}, {0.1f, 0.4f, 1.f}};
			for (int k = 0; k < 3; k++) {
				lights[ENGINE_LIGHT + k].setBrightness(engCol[engine][k]);
				lights[PAGE_LIGHT + k].setBrightness(pgCol[page][k]);
				lights[TRACK_LIGHT + k].setBrightness(trCol[track][k]);
			}
			// seq: off, blinking red while recording, green playing
			float blink = (std::fmod(args.frame * dt, 0.5f) < 0.25f) ? 1.f : 0.2f;
			lights[SEQ_LIGHT + 0].setBrightness(seqMode == 1 ? blink : 0.f);
			lights[SEQ_LIGHT + 1].setBrightness(seqMode == 2 ? 1.f : 0.f);
			lights[SEQ_LIGHT + 2].setBrightness(0.f);
			float sblink = (std::fmod(args.frame * dt, 0.3f) < 0.15f) ? 1.f : 0.1f;
			lights[REC_LIGHT].setBrightness(sampling ? sblink : (tapeRec ? 1.f : 0.f));
			lights[PLAY_LIGHT].setBrightness(tapePlaying ? 1.f : 0.f);
			lights[REV_LIGHT].setBrightness(tapeRev);
			lights[LOOP_LIGHT + 0].setBrightness(loopState == 1 ? 1.f : 0.f);
			lights[LOOP_LIGHT + 1].setBrightness(loopState == 2 ? 1.f : 0.f);
			lights[LOOP_LIGHT + 2].setBrightness(0.f);
		}
	}

	// ---- persistence: settings in JSON, tape as int16 in patch storage ----
	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "engine", json_integer(engine));
		json_object_set_new(root, "page", json_integer(page));
		json_object_set_new(root, "track", json_integer(track));
		json_object_set_new(root, "tapeRev", json_boolean(tapeRev));
		json_object_set_new(root, "loopIn", json_integer(loopIn));
		json_object_set_new(root, "loopOut", json_integer(loopOut));
		json_object_set_new(root, "loopState", json_integer(loopState));
		json_object_set_new(root, "seqMode", json_integer(seqMode));
		json_t* sn = json_array();
		for (int i = 0; i < seqLen; i++)
			json_array_append_new(sn, json_real(seqNotes[i]));
		json_object_set_new(root, "seqNotes", sn);
		auto arr4 = [&](const char* name, float* v, int n) {
			json_t* a = json_array();
			for (int i = 0; i < n; i++)
				json_array_append_new(a, json_real(v[i]));
			json_object_set_new(root, name, a);
		};
		arr4("env", envVals, 4);
		arr4("fx", fxVals, 4);
		arr4("vol", trackVol, 4);
		json_t* em = json_array();
		for (int e = 0; e < NUM_ENGINES; e++)
			for (int k = 0; k < 4; k++)
				json_array_append_new(em, json_real(engineMacro[e][k]));
		json_object_set_new(root, "macros", em);
		// tape to patch storage (int16, used lengths only)
		std::string dir = createPatchStorageDirectory();
		if (!dir.empty()) {
			std::ofstream f(dir + "/tape.bin", std::ios::binary);
			if (f) {
				int32_t sr = (int32_t)APP->engine->getSampleRate();
				f.write((char*)&sr, 4);
				for (int t = 0; t < NUM_TRACKS; t++) {
					int32_t n = trackUsed[t];
					f.write((char*)&n, 4);
					for (int i = 0; i < n; i++) {
						int16_t s = (int16_t)clamp(tape[t][i] * 32000.f, -32767.f, 32767.f);
						f.write((char*)&s, 2);
					}
				}
				int32_t sn = sampleLen;
				f.write((char*)&sn, 4);
				for (int i = 0; i < sn; i++) {
					int16_t s = (int16_t)clamp(sampleBuf[i] * 32000.f, -32767.f, 32767.f);
					f.write((char*)&s, 2);
				}
			}
		}
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "engine"))) engine = json_integer_value(j);
		if ((j = json_object_get(root, "page"))) page = json_integer_value(j);
		if ((j = json_object_get(root, "track"))) track = json_integer_value(j);
		if ((j = json_object_get(root, "tapeRev"))) tapeRev = json_boolean_value(j);
		if ((j = json_object_get(root, "loopIn"))) loopIn = json_integer_value(j);
		if ((j = json_object_get(root, "loopOut"))) loopOut = json_integer_value(j);
		if ((j = json_object_get(root, "loopState"))) loopState = json_integer_value(j);
		if ((j = json_object_get(root, "seqMode"))) seqMode = json_integer_value(j);
		json_t* sn = json_object_get(root, "seqNotes");
		if (sn) {
			seqLen = std::min((int)json_array_size(sn), MAX_SEQ);
			for (int i = 0; i < seqLen; i++)
				seqNotes[i] = json_real_value(json_array_get(sn, i));
		}
		auto rd4 = [&](const char* name, float* v, int n) {
			json_t* a = json_object_get(root, name);
			if (a)
				for (int i = 0; i < n && i < (int)json_array_size(a); i++)
					v[i] = json_real_value(json_array_get(a, i));
		};
		rd4("env", envVals, 4);
		rd4("fx", fxVals, 4);
		rd4("vol", trackVol, 4);
		json_t* em = json_object_get(root, "macros");
		if (em)
			for (int e = 0; e < NUM_ENGINES; e++)
				for (int k = 0; k < 4; k++)
					if (e * 4 + k < (int)json_array_size(em))
						engineMacro[e][k] = json_real_value(json_array_get(em, e * 4 + k));
		// tape from patch storage
		std::string dir = getPatchStorageDirectory();
		if (!dir.empty()) {
			std::ifstream f(dir + "/tape.bin", std::ios::binary);
			if (f) {
				int32_t fileSr = 0;
				f.read((char*)&fileSr, 4);
				for (int t = 0; t < NUM_TRACKS; t++) {
					int32_t n = 0;
					f.read((char*)&n, 4);
					n = clamp(n, 0, tapeLen);
					trackUsed[t] = n;
					for (int i = 0; i < n; i++) {
						int16_t s = 0;
						f.read((char*)&s, 2);
						tape[t][i] = s / 32000.f;
					}
				}
				int32_t sn = 0;
				f.read((char*)&sn, 4);
				sn = clamp(sn, 0, sampleMax);
				sampleLen = sn;
				for (int i = 0; i < sn; i++) {
					int16_t s = 0;
					f.read((char*)&s, 2);
					sampleBuf[i] = s / 32000.f;
				}
			}
		}
		knobContext = -1;
		inited = false;
	}
};

struct FieldfareWidget : ModuleWidget {
	FieldfareWidget(Fieldfare* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Fieldfare.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace fflayout;

		// mode buttons with status LEDs
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X[0], YA)), module, Fieldfare::ENGINE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(BTN_X[0] + 5.6f, YA)), module, Fieldfare::ENGINE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X[1], YA)), module, Fieldfare::PAGE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(BTN_X[1] + 5.6f, YA)), module, Fieldfare::PAGE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X[2], YA)), module, Fieldfare::SEQ_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(BTN_X[2] + 5.6f, YA)), module, Fieldfare::SEQ_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(BTN_X[3], YA)), module, Fieldfare::TRACK_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(BTN_X[3] + 5.6f, YA)), module, Fieldfare::TRACK_LIGHT));

		// the four color knobs
		addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(KNOB_X[0], YB)), module, Fieldfare::MACRO_PARAM + 0));
		addParam(createParamCentered<Rogan1PSGreen>(mm2px(Vec(KNOB_X[1], YB)), module, Fieldfare::MACRO_PARAM + 1));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(KNOB_X[2], YB)), module, Fieldfare::MACRO_PARAM + 2));
		addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(KNOB_X[3], YB)), module, Fieldfare::MACRO_PARAM + 3));

		// tape transport
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(SPEED_X, YC)), module, Fieldfare::SPEED_PARAM));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<RedLight>>>(mm2px(Vec(TR_X[0], YC)), module, Fieldfare::REC_PARAM, Fieldfare::REC_LIGHT));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<GreenLight>>>(mm2px(Vec(TR_X[1], YC)), module, Fieldfare::PLAY_PARAM, Fieldfare::PLAY_LIGHT));
		addParam(createLightParamCentered<VCVLightButton<MediumSimpleLight<BlueLight>>>(mm2px(Vec(TR_X[2], YC)), module, Fieldfare::REV_PARAM, Fieldfare::REV_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(TR_X[3], YC)), module, Fieldfare::LOOP_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(TR_X[3] + 5.6f, YC)), module, Fieldfare::LOOP_LIGHT));

		// jacks
		static const int r1[6] = {Fieldfare::VOCT_INPUT, Fieldfare::GATE_INPUT, Fieldfare::CLOCK_INPUT,
		                          Fieldfare::SPEED_INPUT, Fieldfare::REC_INPUT, Fieldfare::PLAY_INPUT};
		for (int i = 0; i < 6; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[i], R1)), module, r1[i]));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[0], R2)), module, Fieldfare::IN_L_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[1], R2)), module, Fieldfare::IN_R_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[4], R2)), module, Fieldfare::OUT_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[5], R2)), module, Fieldfare::OUT_R_OUTPUT));
	}

	struct Preset {
		const char* name;
		int engine;
		float m[4];
		float env[4];
		float fx[4];
	};

	void appendContextMenu(Menu* menu) override {
		Fieldfare* module = getModule<Fieldfare>();
		menu->addChild(new MenuSeparator);
		// crafted sounds: engine + macros + envelope + effects in one click
		static const Preset presets[] = {
			{"Lush Pad",     3, {0.42f, 0.20f, 0.15f, 0.55f}, {0.55f, 0.50f, 0.85f, 0.65f}, {0.05f, 0.50f, 0.70f, 0.25f}},
			{"Haze Pad",     1, {0.80f, 0.90f, 0.15f, 0.30f}, {0.80f, 0.60f, 0.85f, 0.85f}, {0.05f, 0.65f, 0.90f, 0.40f}},
			{"Drift Choir",  3, {0.18f, 0.10f, 0.45f, 0.35f}, {0.70f, 0.60f, 0.90f, 0.80f}, {0.05f, 0.60f, 0.85f, 0.35f}},
			{"Super Pad",    1, {0.50f, 1.00f, 0.30f, 0.45f}, {0.60f, 0.50f, 0.80f, 0.70f}, {0.10f, 0.55f, 0.75f, 0.20f}},
			{"Glass Bells",  0, {0.86f, 0.35f, 0.10f, 0.20f}, {0.00f, 0.45f, 0.00f, 0.50f}, {0.00f, 0.45f, 0.60f, 0.05f}},
			{"Solar Lead",   0, {0.29f, 0.50f, 0.35f, 0.45f}, {0.02f, 0.40f, 0.60f, 0.30f}, {0.30f, 0.25f, 0.40f, 0.10f}},
			{"Warm Bass",    2, {0.35f, 0.05f, 0.10f, 0.50f}, {0.01f, 0.30f, 0.70f, 0.15f}, {0.25f, 0.10f, 0.30f, 0.00f}},
			{"Acid Nibble",  2, {0.60f, 0.70f, 0.60f, 0.80f}, {0.00f, 0.25f, 0.30f, 0.10f}, {0.45f, 0.15f, 0.30f, 0.00f}},
			{"Nylon",        4, {0.55f, 0.60f, 0.50f, 0.50f}, {0.00f, 0.40f, 0.60f, 0.40f}, {0.05f, 0.30f, 0.50f, 0.00f}},
			{"Tin Drum",     5, {0.50f, 0.30f, 0.40f, 0.60f}, {0.00f, 0.20f, 0.00f, 0.15f}, {0.20f, 0.20f, 0.30f, 0.00f}},
		};
		menu->addChild(createSubmenuItem("Sound presets", "", [=](Menu* sub) {
			for (const Preset& p : presets) {
				sub->addChild(createMenuItem(p.name, "", [=]() {
					module->engine = p.engine;
					for (int k = 0; k < 4; k++) {
						module->engineMacro[p.engine][k] = p.m[k];
						module->envVals[k] = p.env[k];
						module->fxVals[k] = p.fx[k];
					}
					module->page = 0;
					module->knobContext = -1; // snap the knobs
				}));
			}
		}));
		menu->addChild(createIndexPtrSubmenuItem("Engine",
			{"Duo (FM pair)", "Saws (super stack)", "Pulse (PWM)", "Waves (wavetable)",
			 "String (pluck)", "Grit (percussive)"}, &module->engine));
		menu->addChild(createIndexPtrSubmenuItem("Knob page",
			{"Sound (engine macros)", "Envelope (ADSR)", "Effects (drive/verb/wow)", "Mix (track levels)"},
			&module->page));
		menu->addChild(createIndexPtrSubmenuItem("Tape track", {"1", "2", "3", "4"}, &module->track));
		menu->addChild(createIndexPtrSubmenuItem("Endless sequencer",
			{"Off", "Recording", "Playing"}, &module->seqMode));
		menu->addChild(createMenuItem("Erase selected track", "", [=]() {
			std::fill(module->tape[module->track].begin(), module->tape[module->track].end(), 0.f);
			module->trackUsed[module->track] = 0;
		}));
		menu->addChild(createMenuItem("Erase whole tape", "", [=]() {
			for (int t = 0; t < NUM_TRACKS; t++) {
				std::fill(module->tape[t].begin(), module->tape[t].end(), 0.f);
				module->trackUsed[t] = 0;
			}
		}));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Quick guide:"));
		menu->addChild(createMenuLabel("Tape: tap REC + PLAY records the synth/inputs onto the track"));
		menu->addChild(createMenuLabel("Sample: HOLD REC ~1s while audio plays, release to finish,"));
		menu->addChild(createMenuLabel("  then play it from V/OCT (Sample engine, white LED)"));
		menu->addChild(createMenuLabel("Seq: press SEQ (blinks red), play notes, then send a clock —"));
		menu->addChild(createMenuLabel("  it starts playing them automatically. Hold SEQ to clear."));
	}
};

Model* modelFieldfare = createModel<Fieldfare, FieldfareWidget>("Fieldfare");
