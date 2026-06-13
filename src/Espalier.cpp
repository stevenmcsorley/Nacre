// Espalier — a 3-channel generative "fractal" sequencer for VCV Rack,
// inspired by the architecture of the Qu-Bit Bloom v2 (trunk sequences with
// generative branches, path navigation, mutation, per-step tune modes,
// ornaments, pattern bank, and note/gate/mod outputs per channel).
// Original implementation.

#include "plugin.hpp"
#include "layout_espalier.hpp"

static const int MAX_STEPS = 64;
static const int PAGE_STEPS = 8;
static const int MAX_BRANCHES = 7;
static const int NUM_PATTERNS = 16;

static const int SCALE_TABLE[8][13] = {
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -1}, // chromatic
	{0, 2, 4, 5, 7, 9, 11, -1},                 // major
	{0, 2, 3, 5, 7, 8, 10, -1},                 // minor
	{0, 2, 4, 7, 9, -1},                        // major pentatonic
	{0, 3, 5, 7, 10, -1},                       // minor pentatonic
	{0, 2, 3, 5, 7, 8, 11, -1},                 // harmonic minor
	{0, 2, 4, 6, 8, 10, -1},                    // whole tone
	{-1},                                       // unquantized
};

static const float RATIO_TABLE[9] = {0.125f, 0.25f, 1.f / 3.f, 0.5f, 1.f, 2.f, 3.f, 4.f, 8.f};
static const float RESIZE_TABLE[7] = {0.125f, 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f};
static const int RATCHET_TABLE[8] = {1, 2, 3, 4, 6, 8, 16, 32};

// small deterministic hash for seeded branch transforms
static inline uint32_t hash32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

struct Espalier : Module {
	enum ParamId {
		ROOT_PARAM,
		RATE_PARAM,
		GROW_PARAM,
		ROUTE_PARAM,
		EVOLVE_PARAM,
		ENUMS(STEP_PARAM, 8),
		ENUMS(STEPBTN_PARAM, 8),
		CHANNEL_PARAM,
		PAGE_PARAM,
		KNOBMODE_PARAM,
		TUNEMODE_PARAM,
		RESEED_PARAM,
		RESET_PARAM,
		SHIFT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ROOT_INPUT,
		RATE_INPUT,
		CLOCK_INPUT,
		RESEED_INPUT,
		RESET_INPUT,
		GROW_INPUT,
		ROUTE_INPUT,
		EVOLVE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(GATE_OUTPUT, 3),
		ENUMS(NOTE_OUTPUT, 3),
		ENUMS(MOD_OUTPUT, 3),
		CLOCK_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(CHANNEL_LIGHT, 3),
		ENUMS(KNOBMODE_LIGHT, 3),
		ENUMS(TUNEMODE_LIGHT, 3),
		SHIFT_LIGHT,
		ENUMS(STEP_LIGHT, 8 * 3),
		ENUMS(PAGE_LIGHT, 8),
		LIGHTS_LEN
	};

	struct Step {
		float note = 24.f;     // semitones, 0..60 (5 octaves)
		bool on = true;
		float gateLen = 0.5f;  // 0.05..1 (1 = tie)
		float slew = 0.f;
		int ratchet = 0;       // index into RATCHET_TABLE
		float mod = 0.5f;
	};

	struct SubNote {
		float frac;
		float semis;
		bool rest;
	};

	struct Channel {
		Step steps[MAX_STEPS];
		int length = 8;
		int scaleIdx = 4; // minor pentatonic, like a fresh unit
		int orderIdx = 0;
		int ratioIdx = 4;
		int resizeIdx = 3;
		int rotate = 0;
		int transpose = 0;     // scale degrees, -12..12
		int modMode = 0;       // 0 shapes, 1 velocity, 2 smooth, 3 envelope
		uint32_t branchSeed = 12345;
		// per-channel knob-layer values [knobMode][grow, route, evolve]:
		// like the hardware, branch/path/mutate (and the micro/performance
		// layers) are channel settings that persist when the knobs are
		// re-purposed by another knob mode or another channel is focused.
		float kv[3][3] = {};
		int passSeed = 0;      // reshuffles random order each pass

		// playback
		int segPos = -1;       // step within current segment
		int segIdx = 0;        // which segment (0=trunk, 1..n = branches)
		int divCount = 0;      // external clock division counter
		int subsLeft = 0;      // multiplied substeps remaining before next edge
		double stepPhase = 0.0;
		float stepDur = 0.5f;
		// current step rendering
		float curNote = 24.f;
		float noteOut = 2.f;
		float targetNote = 2.f;
		float slewRate = 0.f;
		bool gateOn = false;
		float curGateLen = 0.5f;
		int curRatchets = 1;
		float curMod = 0.5f;
		float prevMod = 0.5f;
		bool tied = false;
		// ornaments
		SubNote subs[8];
		int numSubs = 0;
	};

	struct Pattern {
		bool used = false;
		Step steps[MAX_STEPS];
		int length = 8, scaleIdx = 4, orderIdx = 0, ratioIdx = 4;
		int resizeIdx = 3, rotate = 0, transpose = 0, modMode = 0;
	};

	Channel ch[3];
	Pattern patterns[NUM_PATTERNS];
	int patSlot = 0;
	float patFlash = 0.f;
	int resizeAlgo = 0; // 0 spread (hardware default), 1 stretch, 2 clone

	int focus = 0;
	int page = 0;
	int knobMode = 0;  // 0 default, 1 micro, 2 performance
	int tuneMode = 0;  // 0 note, 1 gate len, 2 slew, 3 ratchet, 4 mod

	// relative (encoder-style) editing state: knobs always respond, edits are
	// applied as deltas from the stored data captured at context change.
	bool inited = false;
	int lastContext = -1;
	float knobPrev[8] = {};
	float stepOrig[3][8] = {};
	float stepAcc[3][8] = {};
	float macroPrev[3] = {};
	int macroContext = -1;
	// channel button: tap cycles focus, long-press latches global edit
	// (mouse users can't hold a button while turning a knob)
	float chanHold = -1.f;
	bool chanEdited = false;
	bool globalLatch = false;
	// tune-mode button: press cycles, hold + reseed/reset = combo
	float tuneHold = -1.f;
	bool tuneCombo = false;
	// pattern button (step button 4 while shift held)
	float patHold = -1.f;
	bool patSaved = false;
	bool patShiftAtPress = false;

	// LED feedback when a shift macro is edited (hardware shows the
	// selection on the trunk LEDs)
	int shiftFlashKnob = -1;
	float shiftFlashTimer = 0.f;

	bool pendingReset = false;
	float clockPeriod = 0.5f;
	float lastEdge = 1e9f;
	double clockPhase = 0.0;

	dsp::SchmittTrigger clockTrig, resetTrig, reseedTrig;
	dsp::BooleanTrigger pageBtn, knobModeBtn, reseedBtn, resetBtn, stepBtn[8];
	dsp::ClockDivider lightDivider;

	Espalier() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(ROOT_PARAM, 0.f, 14.f, 0.f, "Root (diatonic transpose)");
		paramQuantities[ROOT_PARAM]->snapEnabled = true;
		configParam(RATE_PARAM, 0.f, 1.f, 0.4f, "Rate (fully left = external clock)");
		struct GrowQuantity : ParamQuantity {
			std::string getDisplayValueString() override {
				int n = (int)(getValue() * 7.f + 0.5f);
				return string::f("%.2f (%d branch%s in default mode)", getValue(), n, n == 1 ? "" : "es");
			}
		};
		configParam<GrowQuantity>(GROW_PARAM, 0.f, 1.f, 0.f, "Grow (branches / micro: ratchet dice / perf: ratchet FX)");
		configParam(ROUTE_PARAM, 0.f, 1.f, 0.f, "Route (branch path / micro: slew dice / perf: slew FX)");
		configParam(EVOLVE_PARAM, 0.f, 1.f, 0.f, "Evolve (mutation / micro: mod dice / perf: ornaments)");
		for (int i = 0; i < 8; i++) {
			configParam(STEP_PARAM + i, 0.f, 1.f, 0.5f, string::f("Step %d (relative encoder)", i + 1));
			configButton(STEPBTN_PARAM + i, string::f("Step %d on/off%s", i + 1,
				(i == 3) ? " (+shift: tap = load pattern, hold = save)" : ""));
		}
		configButton(CHANNEL_PARAM, "Channel focus (long-press: toggle global edit of all channels)");
		configButton(PAGE_PARAM, "Page (cycles the active pages set by LEN)");
		configButton(KNOBMODE_PARAM, "Knob mode (default/micro/performance; +shift: mod output mode)");
		configButton(TUNEMODE_PARAM, "Tune mode (note/gate/slew/ratchet/mod; hold + reseed/reset = combo)");
		configButton(RESEED_PARAM, "Reseed (dice-roll focused channel; +shift: new branches)");
		configButton(RESET_PARAM, "Reset to step 1 (+shift: reset all tune data)");
		configSwitch(SHIFT_PARAM, 0.f, 1.f, 0.f, "Shift", {"Off", "On"});
		configInput(ROOT_INPUT, "Root CV");
		configInput(RATE_INPUT, "Rate CV");
		configInput(CLOCK_INPUT, "Clock");
		configInput(RESEED_INPUT, "Reseed gate");
		configInput(RESET_INPUT, "Reset gate");
		configInput(GROW_INPUT, "Grow CV");
		configInput(ROUTE_INPUT, "Route CV");
		configInput(EVOLVE_INPUT, "Evolve CV");
		for (int c = 0; c < 3; c++) {
			configOutput(GATE_OUTPUT + c, string::f("Channel %d gate", c + 1));
			configOutput(NOTE_OUTPUT + c, string::f("Channel %d note (1V/oct)", c + 1));
			configOutput(MOD_OUTPUT + c, string::f("Channel %d mod", c + 1));
		}
		configOutput(CLOCK_OUTPUT, "Clock");
		lightDivider.setDivision(64);
		onReset();
	}

	void onReset() override {
		// boot with a musical trunk (like powering a fresh unit), not chaos:
		// a minor-pentatonic phrase (C Eb F G Bb)
		static const float bootNotes[8] = {12.f, 15.f, 17.f, 19.f, 22.f, 24.f, 19.f, 15.f};
		for (int c = 0; c < 3; c++) {
			ch[c] = Channel();
			ch[c].branchSeed = random::u32();
			for (int i = 0; i < MAX_STEPS; i++) {
				ch[c].steps[i] = Step();
				ch[c].steps[i].note = bootNotes[i % 8];
			}
		}
		focus = 0;
		page = 0;
		knobMode = 0;
		tuneMode = 0;
		patSlot = 0;
		lastContext = -1;
		inited = false;
	}

	void reseedChannel(int c) {
		Channel& C = ch[c];
		for (int i = 0; i < MAX_STEPS; i++) {
			Step& s = C.steps[i];
			s.note = (float)quantizeSemis(c, 12 + (int)(random::u32() % 25));
			s.on = random::uniform() < 0.85f;
			s.gateLen = 0.3f + random::uniform() * 0.4f;
			s.slew = (random::uniform() < 0.1f) ? random::uniform() * 0.6f : 0.f;
			s.ratchet = (random::uniform() < 0.15f) ? 1 + (int)(random::u32() % 2) : 0;
			s.mod = random::uniform();
		}
		lastContext = -1;
	}

	// reseed only the focused tune mode's data (current page, or all steps)
	void reseedTuneData(int c, bool allSteps) {
		Channel& C = ch[c];
		int i0 = allSteps ? 0 : page * PAGE_STEPS;
		int i1 = allSteps ? MAX_STEPS : std::min(MAX_STEPS, i0 + PAGE_STEPS);
		for (int i = i0; i < i1; i++) {
			Step& s = C.steps[i];
			switch (tuneMode) {
				case 0: s.note = (float)quantizeSemis(c, 12 + (int)(random::u32() % 25)); break;
				case 1: s.gateLen = 0.1f + random::uniform() * 0.9f; break;
				case 2: s.slew = (random::uniform() < 0.4f) ? random::uniform() * 0.8f : 0.f; break;
				case 3: s.ratchet = (random::uniform() < 0.4f) ? 1 + (int)(random::u32() % 4) : 0; break;
				default: s.mod = random::uniform(); break;
			}
		}
		lastContext = -1;
	}

	void resetTuneData(int c, bool allModes) {
		Channel& C = ch[c];
		for (int i = 0; i < MAX_STEPS; i++) {
			Step& s = C.steps[i];
			if (allModes || tuneMode == 0) s.note = 24.f;
			if (allModes || tuneMode == 1) s.gateLen = 0.5f;
			if (allModes || tuneMode == 2) s.slew = 0.f;
			if (allModes || tuneMode == 3) s.ratchet = 0;
			if (allModes || tuneMode == 4) s.mod = 0.5f;
		}
		lastContext = -1;
	}

	void savePattern(int slot) {
		Pattern& P = patterns[clamp(slot, 0, NUM_PATTERNS - 1)];
		Channel& C = ch[focus];
		P.used = true;
		for (int i = 0; i < MAX_STEPS; i++)
			P.steps[i] = C.steps[i];
		P.length = C.length;
		P.scaleIdx = C.scaleIdx;
		P.orderIdx = C.orderIdx;
		P.ratioIdx = C.ratioIdx;
		P.resizeIdx = C.resizeIdx;
		P.rotate = C.rotate;
		P.transpose = C.transpose;
		P.modMode = C.modMode;
	}

	void loadPattern(int slot) {
		Pattern& P = patterns[clamp(slot, 0, NUM_PATTERNS - 1)];
		if (!P.used)
			return;
		Channel& C = ch[focus];
		for (int i = 0; i < MAX_STEPS; i++)
			C.steps[i] = P.steps[i];
		C.length = P.length;
		C.scaleIdx = P.scaleIdx;
		C.orderIdx = P.orderIdx;
		C.ratioIdx = P.ratioIdx;
		C.resizeIdx = P.resizeIdx;
		C.rotate = P.rotate;
		C.transpose = P.transpose;
		C.modMode = P.modMode;
		lastContext = -1;
	}

	int scaleSize(int c) {
		const int* sc = SCALE_TABLE[ch[c].scaleIdx];
		int n = 0;
		while (sc[n] >= 0)
			n++;
		return n;
	}

	float quantizeSemis(int c, float semis) {
		const int* sc = SCALE_TABLE[ch[c].scaleIdx];
		if (sc[0] < 0)
			return semis;
		int n = scaleSize(c);
		float oct = std::floor(semis / 12.f);
		float in = semis - oct * 12.f;
		float best = sc[0], bestD = 1e9f;
		for (int i = 0; i < n; i++) {
			float d = std::fabs(in - sc[i]);
			if (d < bestD) {
				bestD = d;
				best = sc[i];
			}
		}
		if (std::fabs(in - 12.f) < bestD)
			return (oct + 1.f) * 12.f;
		return oct * 12.f + best;
	}

	// move semis by `deg` scale degrees
	float degreeShift(int c, float semis, int deg) {
		const int* sc = SCALE_TABLE[ch[c].scaleIdx];
		int n = scaleSize(c);
		if (n == 0)
			return semis + deg; // unquantized: semitone steps
		int oct = (int)std::floor(semis / 12.f);
		float in = semis - oct * 12.f;
		int idx = 0;
		float bestD = 1e9f;
		for (int i = 0; i < n; i++) {
			float d = std::fabs(in - sc[i]);
			if (d < bestD) {
				bestD = d;
				idx = i;
			}
		}
		int total = oct * n + idx + deg;
		int no = (total >= 0) ? total / n : (total - n + 1) / n;
		int ni = total - no * n;
		return no * 12.f + sc[ni];
	}

	// effective trunk length after resize
	int effLength(int c) {
		float f = RESIZE_TABLE[ch[c].resizeIdx];
		int L = std::max(1, (int)std::round(ch[c].length * f));
		return std::min(L, 512);
	}

	// map playback index (within resized/rotated trunk) to a trunk step
	int trunkIndex(int c, int idx) {
		float f = RESIZE_TABLE[ch[c].resizeIdx];
		int raw;
		if (f > 1.f && resizeAlgo == 2)
			raw = idx % ch[c].length; // clone: repeat the whole sequence
		else
			raw = (int)(idx / f);
		raw = (raw + ch[c].rotate) % ch[c].length;
		if (raw < 0)
			raw += ch[c].length;
		return raw;
	}

	// spread resize (hardware default) leaves empty steps between notes
	bool resizeMuted(int c, int idx) {
		float f = RESIZE_TABLE[ch[c].resizeIdx];
		if (f <= 1.f || resizeAlgo != 0)
			return false;
		return (idx % (int)f) != 0;
	}

	// playback order mapping within a segment of length L
	int orderIndex(int c, int pos, int L) {
		switch (ch[c].orderIdx) {
			case 1: return L - 1 - pos % L; // reverse
			case 2: { // pendulum
				int cyc = std::max(1, 2 * L - 2);
				int p = pos % cyc;
				return (p < L) ? p : cyc - p;
			}
			case 3: // random: a new shuffle every pass
				return (int)(hash32(ch[c].branchSeed ^ (pos * 2654435761u) ^ (ch[c].passSeed * 97u)) % L);
			case 4: { // converge
				int p = pos % L;
				return (p & 1) ? L - 1 - p / 2 : p / 2;
			}
			case 5: { // diverge
				int p = pos % L;
				int half = L / 2;
				int v = (p & 1) ? half + p / 2 : half - 1 - p / 2;
				return clamp(v, 0, L - 1);
			}
			case 6: { // converge & diverge
				int cyc = 2 * L;
				int p = pos % cyc;
				if (p < L)
					return (p & 1) ? L - 1 - p / 2 : p / 2;
				p -= L;
				int half = L / 2;
				int v = (p & 1) ? half + p / 2 : half - 1 - p / 2;
				return clamp(v, 0, L - 1);
			}
			case 7: { // page jump: random page each cycle
				int pg = (int)(hash32(ch[c].branchSeed ^ ((pos / std::max(1, L)) * 7919u) ^ (ch[c].passSeed * 131u)) % std::max(1, (L + 7) / 8));
				return (pg * 8 + pos % 8) % L;
			}
			default: return pos % L;
		}
	}

	// per-channel macro value (stored layer + CV on the focused channel)
	float macroVal(int c, int layer, int k, int cvInput) {
		float v = ch[c].kv[layer][k];
		if (c == focus)
			v += inputs[cvInput].getVoltage() / 10.f;
		return clamp(v, 0.f, 1.f);
	}

	// Branch-transformed step data. seg 0 = trunk; each branch applies one
	// transformation to the previous branch (chained, like the hardware).
	// The route value picks one of 128 binary-tree paths: at each branch
	// level the path bit chooses between two seeded transform variants.
	// Branches always keep the trunk's rhythmic skeleton — the rests stay
	// put (the demo's signature "notice how it's keeping the rests").
	// dataIdx returns the trunk step the branch lands on, so per-step
	// gate length / slew / ratchet / mod data travel with the melody.
	void branchStep(int c, int seg, int idx, float& note, bool& on, int& dataIdx) {
		Channel& C = ch[c];
		int L = effLength(c);
		float route = macroVal(c, 0, 1, ROUTE_INPUT);
		int pathBits = (int)(route * 127.f + 0.5f);
		int types[MAX_BRANCHES];
		bool usedRandom = false;
		for (int b = 0; b < seg && b < MAX_BRANCHES; b++) {
			int variant = (pathBits >> b) & 1;
			// weighted pick: reverse/inverse/transpose/mutate are twice as
			// likely as full randomize, so deep branch chains stay related
			int t = (int)(hash32(C.branchSeed + b * 101u + variant * 7561u) % 9);
			int ty = (t < 8) ? (t >> 1) : 4;
			if (b == 0 && ty > 2)
				ty = t % 3; // first branch: always a clearly audible transform
			if (ty == 4 && usedRandom)
				ty = 3; // at most one randomize per chain
			if (b > 0 && ty == types[b - 1] && ty != 3)
				ty = (ty + 1) % 3; // no self-cancelling/stacking repeats
			if (ty == 4)
				usedRandom = true;
			types[b] = ty;
		}
		// index transforms (reverse) applied from outermost in
		int curIdx = idx;
		for (int b = std::min(seg, MAX_BRANCHES) - 1; b >= 0; b--) {
			if (types[b] == 0)
				curIdx = L - 1 - curIdx;
		}
		dataIdx = trunkIndex(c, curIdx);
		Step& s = C.steps[dataIdx];
		note = s.note;
		on = s.on;
		for (int b = 0; b < seg && b < MAX_BRANCHES; b++) {
			int variant = (pathBits >> b) & 1;
			uint32_t h = hash32(C.branchSeed ^ (b * 7907u + variant * 593u + curIdx * 31u));
			switch (types[b]) {
				case 1: // inverse: mirror around the center of the range
					note = clamp(48.f - note, 0.f, 60.f);
					break;
				case 2: // transpose up an octave (folds back at top)
					note = (note + 12.f > 60.f) ? note - 12.f : note + 12.f;
					break;
				case 3: // mutate ~35% of steps (seeded)
					if ((h & 0xff) < 90)
						note = clamp(note + (float)((int)(h >> 8 & 7) - 3) * 2.f, 0.f, 60.f);
					break;
				case 4: { // randomize notes within the trunk's own register —
					// the rhythm stays intact and the result stays melodic
					float lo = 60.f, hi = 0.f;
					for (int i = 0; i < C.length; i++) {
						lo = std::min(lo, C.steps[i].note);
						hi = std::max(hi, C.steps[i].note);
					}
					if (hi < lo) {
						lo = 24.f;
						hi = 36.f;
					}
					float range = std::max(7.f, hi - lo);
					note = lo + (float)(h % 100) / 99.f * range;
				} break;
				default:
					break;
			}
		}
		note = quantizeSemis(c, note);
	}

	// segment length in steps for the current playback order
	int segSteps(int c, int L) {
		switch (ch[c].orderIdx) {
			case 2: return std::max(1, 2 * L - 2); // pendulum
			case 6: return 2 * L;                  // converge & diverge
			default: return L;
		}
	}

	// ---- step advance for one channel ----
	void advanceStep(int c) {
		Channel& C = ch[c];
		int L = effLength(c);
		// branch/path/mutate are the channel's stored default-layer values:
		// they persist regardless of which knob mode is currently selected.
		float grow = macroVal(c, 0, 0, GROW_INPUT);
		float evolve = macroVal(c, 0, 2, EVOLVE_INPUT);

		int numBranches = (int)(grow * MAX_BRANCHES + 0.5f);
		int numSegs = 1 + numBranches;

		C.segPos++;
		if (C.segPos >= segSteps(c, L)) {
			C.segPos = 0;
			C.segIdx = (C.segIdx + 1) % numSegs;
			C.passSeed++;
		}
		if (C.segIdx >= numSegs)
			C.segIdx = 0;

		int idx = orderIndex(c, C.segPos, L);

		float note;
		bool on;
		int dataIdx;
		branchStep(c, C.segIdx, idx, note, on, dataIdx);
		if (resizeMuted(c, idx))
			on = false;
		// per-step data (gate len, slew, ratchet, mod) follows the branch
		Step& s = C.steps[dataIdx];

		// default-layer mutation: destructive evolution of the active step.
		// Zone 1 (lower half): note mutation only, reaching 100% at the top
		// of the zone. Zone 2: notes settle at 75% while gate mutations rise
		// to 100% (70% length changes / 30% on-off flips).
		if (evolve > 0.003f) {
			float noteP = (evolve <= 0.5f) ? evolve * 2.f : 0.75f;
			if (random::uniform() < noteP) {
				s.note = quantizeSemis(c, clamp(s.note + (float)((int)(random::u32() % 9) - 4) * 2.f, 0.f, 60.f));
			}
			if (evolve > 0.5f) {
				float gateP = (evolve - 0.5f) * 2.f;
				if (random::uniform() < gateP) {
					if (random::uniform() < 0.7f)
						s.gateLen = 0.1f + random::uniform() * 0.85f;
					else
						s.on = !s.on;
				}
			}
		}
		// micro-mutate layer: destructive per-step ratchet/slew/mod dice
		{
			float mg = macroVal(c, 1, 0, GROW_INPUT);
			float mr = macroVal(c, 1, 1, ROUTE_INPUT);
			float mm = macroVal(c, 1, 2, EVOLVE_INPUT);
			if (mg > 0.003f && random::uniform() < mg)
				s.ratchet = (int)(random::u32() % 5);
			if (mr > 0.003f && random::uniform() < mr)
				s.slew = random::uniform() * 0.9f;
			if (mm > 0.003f && random::uniform() < mm)
				s.mod = random::uniform();
		}

		// gather step rendering data
		float gateLen = s.gateLen;
		int ratchets = RATCHET_TABLE[clamp(s.ratchet, 0, 7)];
		float slew = s.slew;
		C.numSubs = 0;
		bool wantOrnament = false;

		// performance layer: non-destructive effects
		{
			float pg = macroVal(c, 2, 0, GROW_INPUT);
			float pr = macroVal(c, 2, 1, ROUTE_INPUT);
			float pe = macroVal(c, 2, 2, EVOLVE_INPUT);
			// ratchet effect: zone 1 probabilistic (up to x6), zone 2 fixed
			if (pg > 0.003f) {
				if (pg <= 0.5f) {
					if (random::uniform() < pg * 2.f)
						ratchets = std::max(ratchets, RATCHET_TABLE[1 + (int)(random::u32() % 4)]);
				}
				else {
					ratchets = std::max(ratchets, RATCHET_TABLE[(int)((pg - 0.5f) * 2.f * 5.f)]);
				}
			}
			// slew effect
			if (pr > 0.003f) {
				if (pr <= 0.5f) {
					if (random::uniform() < pr * 2.f)
						slew = std::max(slew, random::uniform() * 0.9f);
				}
				else {
					slew = std::max(slew, (pr - 0.5f) * 1.8f);
				}
			}
			// mutation / ornaments
			if (pe > 0.003f) {
				if (pe <= 0.5f) {
					if (random::uniform() < pe * 2.f * 0.5f) {
						note = quantizeSemis(c, clamp(note + (float)((int)(random::u32() % 9) - 4) * 2.f, 0.f, 60.f));
						if (random::uniform() < 0.3f)
							on = !on;
					}
				}
				else if (on && random::uniform() < 0.5f + (pe - 0.5f)) {
					wantOrnament = true;
				}
			}
		}

		// root + transpose (diatonic)
		int rootDeg = (int)clamp(params[ROOT_PARAM].getValue() + inputs[ROOT_INPUT].getVoltage() * 2.8f, 0.f, 14.f);
		float prevNote = C.curNote;
		note = clamp(degreeShift(c, note, rootDeg), 0.f, 60.f);

		if (wantOrnament) {
			// peek the next step's (root-shifted) note for direction-aware flourishes
			float nextNote;
			bool nextOn;
			int nextIdx;
			branchStep(c, C.segIdx, orderIndex(c, C.segPos + 1, L), nextNote, nextOn, nextIdx);
			nextNote = clamp(degreeShift(c, nextNote, rootDeg), 0.f, 60.f);
			float pe = macroVal(c, 2, 2, EVOLVE_INPUT);
			makeOrnament(c, note, prevNote, nextNote, clamp((pe - 0.5f) * 2.f, 0.f, 1.f));
		}

		// apply
		bool wasTied = C.tied;
		C.tied = on && gateLen >= 0.99f;
		C.curNote = note;
		C.targetNote = note / 12.f;
		if (slew > 0.005f) {
			float slewTime = slew * C.stepDur * 0.8f;
			C.slewRate = std::fabs(C.targetNote - C.noteOut) / std::max(0.001f, slewTime);
		}
		else {
			C.slewRate = 0.f;
			C.noteOut = C.targetNote;
		}
		C.gateOn = on;
		C.curGateLen = gateLen;
		C.curRatchets = clamp(ratchets, 1, 32);
		C.prevMod = C.curMod;
		C.curMod = s.mod;
		C.stepPhase = 0.0;
		(void)wasTied;
	}

	// Classical flourishes, built from the manual's ornament list. `zone`
	// (0..1 across the upper half of the knob) opens up two-step, then
	// four-step, then trill ornaments.
	void makeOrnament(int c, float base, float prev, float next, float zone) {
		Channel& C = ch[c];
		int dir = (next >= base) ? 1 : -1;
		int maxType = (zone < 0.34f) ? 7 : (zone < 0.68f) ? 14 : 16;
		int type = (int)(random::u32() % maxType);
		auto deg = [&](float n, int d) { return clamp(degreeShift(c, n, d), 0.f, 60.f); };
		switch (type) {
			case 0: // anticipation: next step's note an eighth early
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.5f, next, false};
				C.numSubs = 2;
				break;
			case 1: // suspension: hold previous note, resolve to this one
				C.subs[0] = {0.f, prev, false};
				C.subs[1] = {0.5f, base, false};
				C.numSubs = 2;
				break;
			case 2: // syncopation: rest an eighth, then play
				C.subs[0] = {0.f, base, true};
				C.subs[1] = {0.5f, base, false};
				C.numSubs = 2;
				break;
			case 3: // octave up, an eighth later
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.5f, (base + 12.f <= 60.f) ? base + 12.f : base, false};
				C.numSubs = 2;
				break;
			case 4: // fifth up
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.5f, deg(base, 4), false};
				C.numSubs = 2;
				break;
			case 5: // half turn toward: a degree beyond the next note
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.5f, deg(next, dir), false};
				C.numSubs = 2;
				break;
			case 6: // half turn away
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.5f, deg(base, -dir), false};
				C.numSubs = 2;
				break;
			case 7: // run toward
				for (int i = 0; i < 4; i++)
					C.subs[i] = {i * 0.25f, deg(base, i * dir), false};
				C.numSubs = 4;
				break;
			case 8: // run away
				for (int i = 0; i < 4; i++)
					C.subs[i] = {i * 0.25f, deg(base, -i * dir), false};
				C.numSubs = 4;
				break;
			case 9: // turn
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.25f, deg(base, 1), false};
				C.subs[2] = {0.5f, deg(base, -1), false};
				C.subs[3] = {0.75f, base, false};
				C.numSubs = 4;
				break;
			case 10: // arp toward
				for (int i = 0; i < 4; i++)
					C.subs[i] = {i * 0.25f, deg(base, i * 2 * dir), false};
				C.numSubs = 4;
				break;
			case 11: // arp away
				for (int i = 0; i < 4; i++)
					C.subs[i] = {i * 0.25f, deg(base, -i * 2 * dir), false};
				C.numSubs = 4;
				break;
			case 12: // mordent up
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.15f, deg(base, 1), false};
				C.subs[2] = {0.3f, base, false};
				C.numSubs = 3;
				break;
			case 13: // mordent down
				C.subs[0] = {0.f, base, false};
				C.subs[1] = {0.15f, deg(base, -1), false};
				C.subs[2] = {0.3f, base, false};
				C.numSubs = 3;
				break;
			default: // trill (8 steps)
				for (int i = 0; i < 8; i++)
					C.subs[i] = {i * 0.125f, (i & 1) ? deg(base, 1) : base, false};
				C.numSubs = 8;
				break;
		}
	}

	// ---- step data normalized read/write (relative encoder editing) ----
	float dataNorm(int c, int k) {
		Channel& C = ch[c];
		bool shift = params[SHIFT_PARAM].getValue() > 0.f;
		if (shift) {
			switch (k) {
				case 0: return (C.length - 1) / 63.f;
				case 1: return C.scaleIdx / 7.f;
				case 2: return C.orderIdx / 7.f;
				case 3: return patSlot / (float)(NUM_PATTERNS - 1);
				case 4: return C.ratioIdx / 8.f;
				case 5: return C.resizeIdx / 6.f;
				case 6: return C.rotate / 63.f;
				default: return (C.transpose + 12) / 24.f;
			}
		}
		Step& s = C.steps[clamp(page * PAGE_STEPS + k, 0, MAX_STEPS - 1)];
		switch (tuneMode) {
			case 0: return s.note / 60.f;
			case 1: return s.gateLen;
			case 2: return s.slew;
			case 3: return s.ratchet / 7.f;
			default: return s.mod;
		}
	}

	void writeData(int c, int k, float v) {
		Channel& C = ch[c];
		bool shift = params[SHIFT_PARAM].getValue() > 0.f;
		if (shift) {
			shiftFlashKnob = k;
			shiftFlashTimer = 1.2f;
			switch (k) {
				case 0: C.length = 1 + (int)(v * 63.f + 0.5f); break;
				case 1: C.scaleIdx = (int)(v * 7.f + 0.5f); break;
				case 2: C.orderIdx = (int)(v * 7.f + 0.5f); break;
				case 3: if (c == focus) patSlot = (int)(v * (NUM_PATTERNS - 1) + 0.5f); break;
				case 4: C.ratioIdx = (int)(v * 8.f + 0.5f); break;
				case 5: C.resizeIdx = (int)(v * 6.f + 0.5f); break;
				case 6: C.rotate = (int)(v * 63.f + 0.5f); break;
				default: {
					// destructive diatonic transpose of the trunk (hardware
					// behavior): rewrites the step notes, clamped at the
					// 5-octave ends
					int newT = (int)(v * 24.f + 0.5f) - 12;
					int diff = newT - C.transpose;
					if (diff != 0) {
						for (int i = 0; i < MAX_STEPS; i++)
							C.steps[i].note = clamp(degreeShift(c, C.steps[i].note, diff), 0.f, 60.f);
						C.transpose = newT;
					}
				} break;
			}
			return;
		}
		Step& s = C.steps[clamp(page * PAGE_STEPS + k, 0, MAX_STEPS - 1)];
		switch (tuneMode) {
			case 0: s.note = quantizeSemis(c, clamp(v, 0.f, 1.f) * 60.f); break;
			case 1: s.gateLen = clamp(v, 0.05f, 1.f); break;
			case 2: s.slew = clamp(v, 0.f, 1.f); break;
			case 3: s.ratchet = (int)(clamp(v, 0.f, 1.f) * 7.f + 0.5f); break;
			default: s.mod = clamp(v, 0.f, 1.f); break;
		}
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		bool shift = params[SHIFT_PARAM].getValue() > 0.f;
		bool chanHeldNow = params[CHANNEL_PARAM].getValue() > 0.f;
		bool tuneHeldNow = params[TUNEMODE_PARAM].getValue() > 0.f;

		if (!inited) {
			inited = true;
			for (int k = 0; k < 8; k++)
				knobPrev[k] = params[STEP_PARAM + k].getValue();
			macroPrev[0] = params[GROW_PARAM].getValue();
			macroPrev[1] = params[ROUTE_PARAM].getValue();
			macroPrev[2] = params[EVOLVE_PARAM].getValue();
		}

		// ---- channel button: tap cycles focus (or exits global edit);
		// long-press toggles the global-edit latch ----
		if (chanHeldNow) {
			if (chanHold < 0.f) {
				chanHold = 0.f;
				chanEdited = false;
			}
			else {
				chanHold += dt;
				if (!chanEdited && chanHold > 0.6f) {
					globalLatch = !globalLatch;
					chanEdited = true;
				}
			}
		}
		else {
			if (chanHold >= 0.f && !chanEdited) {
				if (globalLatch)
					globalLatch = false;
				else
					focus = (focus + 1) % 3;
			}
			chanHold = -1.f;
		}
		bool global = chanHeldNow || globalLatch;

		// ---- tune-mode button: tap cycles, hold + reseed/reset = combo ----
		if (tuneHeldNow) {
			if (tuneHold < 0.f) {
				tuneHold = 0.f;
				tuneCombo = false;
			}
			else
				tuneHold += dt;
		}
		else {
			if (tuneHold >= 0.f && !tuneCombo)
				tuneMode = (tuneMode + 1) % 5;
			tuneHold = -1.f;
		}

		// page cycles the active pages set by LEN; clamp if LEN shrank
		int activePages = std::max(1, (ch[focus].length + PAGE_STEPS - 1) / PAGE_STEPS);
		if (page >= activePages)
			page = activePages - 1;
		if (pageBtn.process(params[PAGE_PARAM].getValue() > 0.f))
			page = (page + 1) % activePages;
		if (knobModeBtn.process(params[KNOBMODE_PARAM].getValue() > 0.f)) {
			if (shift) {
				// shift + knob mode cycles the mod output mode (like hardware)
				if (global) {
					int m = (ch[focus].modMode + 1) % 4;
					for (int c = 0; c < 3; c++)
						ch[c].modMode = m;
					chanEdited = true;
				}
				else
					ch[focus].modMode = (ch[focus].modMode + 1) % 4;
			}
			else
				knobMode = (knobMode + 1) % 3;
		}
		if (reseedBtn.process(params[RESEED_PARAM].getValue() > 0.f) ||
		    reseedTrig.process(inputs[RESEED_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (tuneHold >= 0.f) {
				tuneCombo = true;
				if (global) {
					chanEdited = true;
					for (int c = 0; c < 3; c++)
						reseedTuneData(c, shift);
				}
				else
					reseedTuneData(focus, shift);
			}
			else if (shift) {
				if (global) {
					chanEdited = true;
					for (int c = 0; c < 3; c++)
						ch[c].branchSeed = random::u32();
				}
				else
					ch[focus].branchSeed = random::u32();
			}
			else if (global) {
				chanEdited = true;
				for (int c = 0; c < 3; c++)
					reseedChannel(c);
			}
			else
				reseedChannel(focus);
		}
		if (resetBtn.process(params[RESET_PARAM].getValue() > 0.f) ||
		    resetTrig.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
			if (tuneHold >= 0.f) {
				tuneCombo = true;
				resetTuneData(focus, false);
			}
			else if (shift) {
				resetTuneData(focus, true);
			}
			else {
				pendingReset = true; // consumed instantly on internal clock
			}
		}

		// ---- macro knobs: write-through absolute pots. When the knob mode
		// or focused channel changes, the knobs snap to that layer's stored
		// values, so the knob position always shows the live value. ----
		static const int macroParams[3] = {GROW_PARAM, ROUTE_PARAM, EVOLVE_PARAM};
		int mCtx = focus * 10 + knobMode;
		if (mCtx != macroContext) {
			macroContext = mCtx;
			for (int k = 0; k < 3; k++) {
				float v = ch[focus].kv[knobMode][k];
				params[macroParams[k]].setValue(v);
				macroPrev[k] = v;
			}
		}
		for (int k = 0; k < 3; k++) {
			float v = params[macroParams[k]].getValue();
			if (v != macroPrev[k]) {
				macroPrev[k] = v;
				if (global) {
					chanEdited = true;
					for (int c = 0; c < 3; c++)
						ch[c].kv[knobMode][k] = v;
				}
				else
					ch[focus].kv[knobMode][k] = v;
			}
		}

		// ---- step knobs: relative editing from the data captured at the
		// last context change ----
		int context = focus * 1000 + page * 100 + tuneMode * 10 + (shift ? 1 : 0);
		if (context != lastContext) {
			lastContext = context;
			for (int k = 0; k < 8; k++) {
				knobPrev[k] = params[STEP_PARAM + k].getValue();
				for (int c = 0; c < 3; c++) {
					stepOrig[c][k] = dataNorm(c, k);
					stepAcc[c][k] = 0.f;
				}
			}
		}
		for (int k = 0; k < 8; k++) {
			float v = params[STEP_PARAM + k].getValue();
			float d = v - knobPrev[k];
			knobPrev[k] = v;
			if (d != 0.f) {
				if (global)
					chanEdited = true;
				for (int c = 0; c < 3; c++) {
					if (!global && c != focus)
						continue;
					float nv = clamp(stepOrig[c][k] + stepAcc[c][k] + d, 0.f, 1.f);
					stepAcc[c][k] = nv - stepOrig[c][k];
					writeData(c, k, nv);
				}
			}
			// step on/off buttons (button 4 + shift = pattern load/save)
			bool pressed = params[STEPBTN_PARAM + k].getValue() > 0.f;
			if (k == 3) {
				if (pressed && patHold < 0.f) {
					patHold = 0.f;
					patSaved = false;
					patShiftAtPress = shift;
				}
				else if (pressed && patHold >= 0.f) {
					patHold += dt;
					if (patShiftAtPress && !patSaved && patHold > 1.5f) {
						savePattern(patSlot);
						patSaved = true;
						patFlash = 1.f;
					}
				}
				else if (!pressed && patHold >= 0.f) {
					if (patShiftAtPress) {
						if (!patSaved) {
							loadPattern(patSlot);
							patFlash = 1.f;
						}
					}
					else {
						int idx = clamp(page * PAGE_STEPS + k, 0, MAX_STEPS - 1);
						if (global) {
							chanEdited = true;
							for (int c = 0; c < 3; c++)
								ch[c].steps[idx].on = !ch[c].steps[idx].on;
						}
						else
							ch[focus].steps[idx].on = !ch[focus].steps[idx].on;
					}
					patHold = -1.f;
				}
			}
			else if (stepBtn[k].process(pressed) && !shift) {
				int idx = clamp(page * PAGE_STEPS + k, 0, MAX_STEPS - 1);
				if (global) {
					chanEdited = true;
					for (int c = 0; c < 3; c++)
						ch[c].steps[idx].on = !ch[c].steps[idx].on;
				}
				else
					ch[focus].steps[idx].on = !ch[focus].steps[idx].on;
			}
		}
		if (patFlash > 0.f)
			patFlash = std::max(0.f, patFlash - dt * 2.f);

		// ---- clock ----
		float rate = clamp(params[RATE_PARAM].getValue() + inputs[RATE_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		lastEdge += dt;
		bool edge = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
		if (edge) {
			if (lastEdge < 8.f && lastEdge > 0.0005f)
				clockPeriod = clamp(lastEdge, 0.0005f, 8.f);
			lastEdge = 0.f;
		}
		bool external = rate < 0.02f;
		float basePeriod;
		if (external)
			basePeriod = clockPeriod;
		else
			basePeriod = 1.f / (0.1f * std::pow(600.f, rate)); // 0.1 Hz .. 60 Hz
		if (external && edge)
			clockPhase = 0.0; // lock the clock output to the input edges
		clockPhase += dt / basePeriod;
		if (clockPhase >= 1.0)
			clockPhase -= 1.0;
		// hardware levels: +5V clock, silent when externally clocked with no
		// pulses arriving
		bool clockAlive = !external || lastEdge < 4.f;
		outputs[CLOCK_OUTPUT].setVoltage((clockAlive && clockPhase < 0.5) ? 5.f : 0.f);

		// reset: instant on the internal clock, on the next pulse externally
		if (pendingReset && (!external || edge)) {
			pendingReset = false;
			for (int c = 0; c < 3; c++) {
				ch[c].segPos = -1;
				ch[c].segIdx = 0;
				ch[c].divCount = 0;
				ch[c].subsLeft = 0;
				if (!external)
					ch[c].stepPhase = 1e9;
			}
			if (!external)
				clockPhase = 0.0;
		}

		// ---- channels ----
		for (int c = 0; c < 3; c++) {
			Channel& C = ch[c];
			float r = RATIO_TABLE[clamp(C.ratioIdx, 0, 8)];
			C.stepDur = basePeriod / r;
			C.stepPhase += dt;
			if (!external) {
				if (C.stepPhase >= C.stepDur)
					advanceStep(c);
			}
			else {
				// edge-synced stepping: divisions count pulses, multiples
				// schedule substeps inside the measured period
				if (edge) {
					if (r < 0.999f) {
						int everyN = (int)std::round(1.f / r);
						if (++C.divCount >= everyN) {
							C.divCount = 0;
							advanceStep(c);
						}
					}
					else {
						advanceStep(c);
						C.subsLeft = (int)std::round(r) - 1;
					}
				}
				else if (C.subsLeft > 0 && C.stepPhase >= C.stepDur) {
					C.subsLeft--;
					advanceStep(c);
				}
			}

			float p = clamp((float)(C.stepPhase / C.stepDur), 0.f, 1.f);

			// note output with slew + ornaments
			float noteTarget = C.targetNote;
			bool subRest = false;
			if (C.numSubs > 0) {
				int si = 0;
				for (int i = 0; i < C.numSubs; i++)
					if (p >= C.subs[i].frac)
						si = i;
				noteTarget = clamp(C.subs[si].semis, 0.f, 60.f) / 12.f;
				subRest = C.subs[si].rest;
			}
			if (C.slewRate > 0.f) {
				float dd = noteTarget - C.noteOut;
				float step = C.slewRate * dt;
				C.noteOut += clamp(dd, -step, step);
			}
			else {
				C.noteOut = noteTarget;
			}
			outputs[NOTE_OUTPUT + c].setVoltage(clamp(C.noteOut, 0.f, 5.f));

			// gate output with ratchets / ties / ornaments
			bool gate = false;
			if (C.gateOn) {
				if (C.numSubs > 0) {
					float subLen = 1.f / C.numSubs;
					float lp = std::fmod(p, subLen) / subLen;
					gate = !subRest && lp < 0.6f;
				}
				else if (C.tied) {
					gate = true;
				}
				else if (C.curRatchets > 1) {
					gate = std::fmod(p * C.curRatchets, 1.f) < 0.5f;
				}
				else {
					gate = p < C.curGateLen;
				}
			}
			outputs[GATE_OUTPUT + c].setVoltage(gate ? 5.f : 0.f); // hardware: +5V gates

			// mod output (0 shapes, 1 velocity, 2 smooth, 3 envelope)
			float mod = 0.f;
			switch (C.modMode) {
				case 0: { // shapes: per-step LFO shape selected by the step value
					int shape = clamp((int)(C.curMod * 7.99f), 0, 7);
					switch (shape) {
						case 0: mod = p; break;
						case 1: mod = (p < 0.25f) ? p * 4.f : 1.f - (p - 0.25f) / 0.75f; break;
						case 2: mod = 1.f - std::fabs(2.f * p - 1.f); break;
						case 3: mod = std::fabs(2.f * p - 1.f); break;
						case 4: mod = (p < 0.5f) ? 0.f : (p - 0.5f) * 2.f; break;
						case 5: mod = (p < 0.5f) ? 1.f - p * 2.f : 0.f; break;
						case 6: mod = 0.5f - 0.5f * std::cos(2.f * M_PI * p); break;
						default: mod = std::exp(-4.f * p) * (std::fmod(p * 8.f, 1.f) < 0.5f ? 1.f : 0.6f); break;
					}
					if (!C.gateOn)
						mod = 0.f;
				} break;
				case 1: // velocity: held value
					mod = C.gateOn ? C.curMod : 0.f;
					break;
				case 2: // smooth: interpolate between step values
					mod = C.prevMod + (C.curMod - C.prevMod) * std::min(1.f, p * 2.f);
					break;
				default: { // envelope: AD with morphing peak position
					float peak = clamp(C.curMod, 0.02f, 0.98f);
					mod = (p < peak) ? p / peak : (1.f - p) / (1.f - peak);
					if (!C.gateOn)
						mod = 0.f;
				} break;
			}
			outputs[MOD_OUTPUT + c].setVoltage(clamp(mod, 0.f, 1.f) * 5.f);
		}

		// ---- lights ----
		if (lightDivider.process()) {
			static const float chColors[3][3] = {{0.1f, 0.4f, 1.f}, {0.1f, 1.f, 0.2f}, {1.f, 0.15f, 0.3f}};
			// branch tint targets: ch1 blue->gold, ch2 green->purple, ch3 rose->cyan
			static const float brColors[3][3] = {{1.f, 0.65f, 0.05f}, {0.6f, 0.1f, 1.f}, {0.1f, 0.9f, 0.9f}};
			static const float kmColors[3][3] = {{0.1f, 0.4f, 1.f}, {0.1f, 1.f, 0.2f}, {1.f, 0.1f, 0.4f}};
			static const float mmColors[4][3] = {
				{0.1f, 0.4f, 1.f}, {0.1f, 1.f, 0.2f}, {1.f, 0.7f, 0.f}, {0.7f, 0.f, 1.f}};
			static const float tmColors[5][3] = {
				{0.1f, 0.4f, 1.f}, {0.1f, 1.f, 0.2f}, {1.f, 0.7f, 0.f}, {0.7f, 0.f, 1.f}, {1.f, 1.f, 1.f}};
			for (int k = 0; k < 3; k++) {
				// white = global edit latched (long-press of CHAN)
				lights[CHANNEL_LIGHT + k].setBrightness(globalLatch ? 1.f : chColors[focus][k]);
				// while shift is held the knob-mode LED previews the mod output mode
				lights[KNOBMODE_LIGHT + k].setBrightness(
					shift ? mmColors[ch[focus].modMode][k] : kmColors[knobMode][k]);
				lights[TUNEMODE_LIGHT + k].setBrightness(tmColors[tuneMode][k]);
			}
			lights[SHIFT_LIGHT].setBrightness(shift);
			Channel& C = ch[focus];
			float growF = macroVal(focus, 0, 0, GROW_INPUT);
			int numBr = (int)(growF * MAX_BRANCHES + 0.5f);
			float blend = (numBr > 0) ? clamp((float)C.segIdx / numBr, 0.f, 1.f) : 0.f;
			int playIdx = orderIndex(focus, std::max(0, C.segPos), effLength(focus));
			int playTrunk = trunkIndex(focus, playIdx);
			// editing a shift macro flashes its value across the step LEDs
			// (like the hardware showing the scale selection on the trunk)
			shiftFlashTimer = std::max(0.f, shiftFlashTimer - 64.f * APP->engine->getSampleTime());
			int flashIdx = -1;
			if (shift && shiftFlashTimer > 0.f && shiftFlashKnob >= 0) {
				switch (shiftFlashKnob) {
					case 0: flashIdx = (C.length - 1) / 8; break;
					case 1: flashIdx = C.scaleIdx; break;
					case 2: flashIdx = C.orderIdx; break;
					case 3: flashIdx = patSlot % 8; break;
					case 4: flashIdx = std::min(7, C.ratioIdx); break;
					case 5: flashIdx = C.resizeIdx; break;
					case 6: flashIdx = C.rotate * 8 / 64; break;
					default: flashIdx = clamp((C.transpose + 12) * 7 / 24, 0, 7); break;
				}
			}
			for (int k = 0; k < 8; k++) {
				float b;
				if (flashIdx >= 0) {
					b = (k == flashIdx) ? 1.f : 0.04f;
					for (int j = 0; j < 3; j++)
						lights[STEP_LIGHT + k * 3 + j].setBrightness(b);
					continue;
				}
				int idx = page * PAGE_STEPS + k;
				b = 0.f;
				if (idx < C.length) {
					b = C.steps[idx].on ? 0.35f : 0.06f;
					if (idx == playTrunk)
						b = 1.f;
				}
				for (int j = 0; j < 3; j++) {
					float col = chColors[focus][j] + (brColors[focus][j] - chColors[focus][j]) * blend;
					lights[STEP_LIGHT + k * 3 + j].setBrightness(col * b);
				}
			}
			for (int pg = 0; pg < 8; pg++) {
				float b;
				if (shift) {
					// shift: page LEDs show the pattern slot (bright = bank 1,
					// dim+used flash = bank 2 of 8)
					b = (pg == patSlot % 8) ? (patSlot < 8 ? 1.f : 0.4f) : 0.f;
					if (patFlash > 0.f)
						b = std::max(b, patFlash);
				}
				else if (numBr > 0) {
					// branches active: the page LEDs count the passes —
					// LED 1 is the trunk, LEDs 2..8 the branches as they play
					b = (pg == C.segIdx) ? 1.f : (pg <= numBr ? 0.12f : 0.f);
				}
				else {
					b = pg == page ? 1.f : (pg < (C.length + 7) / 8 ? 0.15f : 0.f);
				}
				lights[PAGE_LIGHT + pg].setBrightness(b);
			}
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "focus", json_integer(focus));
		json_object_set_new(root, "knobMode", json_integer(knobMode));
		json_object_set_new(root, "tuneMode", json_integer(tuneMode));
		json_object_set_new(root, "patSlot", json_integer(patSlot));
		json_object_set_new(root, "resizeAlgo", json_integer(resizeAlgo));
		json_object_set_new(root, "clockPeriod", json_real(clockPeriod));
		json_t* chans = json_array();
		for (int c = 0; c < 3; c++) {
			Channel& C = ch[c];
			json_t* jc = json_object();
			json_object_set_new(jc, "length", json_integer(C.length));
			json_object_set_new(jc, "scale", json_integer(C.scaleIdx));
			json_object_set_new(jc, "order", json_integer(C.orderIdx));
			json_object_set_new(jc, "ratio", json_integer(C.ratioIdx));
			json_object_set_new(jc, "resize", json_integer(C.resizeIdx));
			json_object_set_new(jc, "rotate", json_integer(C.rotate));
			json_object_set_new(jc, "transpose", json_integer(C.transpose));
			json_object_set_new(jc, "modMode", json_integer(C.modMode));
			json_object_set_new(jc, "seed", json_integer((int64_t)C.branchSeed));
			json_t* kvArr = json_array();
			for (int m = 0; m < 3; m++)
				for (int k = 0; k < 3; k++)
					json_array_append_new(kvArr, json_real(C.kv[m][k]));
			json_object_set_new(jc, "kv", kvArr);
			json_object_set_new(jc, "steps", stepsToJson(C.steps));
			json_array_append_new(chans, jc);
		}
		json_object_set_new(root, "channels", chans);
		json_t* pats = json_array();
		for (int i = 0; i < NUM_PATTERNS; i++) {
			Pattern& P = patterns[i];
			json_t* jp = json_object();
			json_object_set_new(jp, "used", json_boolean(P.used));
			if (P.used) {
				json_object_set_new(jp, "length", json_integer(P.length));
				json_object_set_new(jp, "scale", json_integer(P.scaleIdx));
				json_object_set_new(jp, "order", json_integer(P.orderIdx));
				json_object_set_new(jp, "ratio", json_integer(P.ratioIdx));
				json_object_set_new(jp, "resize", json_integer(P.resizeIdx));
				json_object_set_new(jp, "rotate", json_integer(P.rotate));
				json_object_set_new(jp, "transpose", json_integer(P.transpose));
				json_object_set_new(jp, "modMode", json_integer(P.modMode));
				json_object_set_new(jp, "steps", stepsToJson(P.steps));
			}
			json_array_append_new(pats, jp);
		}
		json_object_set_new(root, "patterns", pats);
		return root;
	}

	json_t* stepsToJson(Step* steps) {
		json_t* arr = json_array();
		for (int i = 0; i < MAX_STEPS; i++) {
			json_t* js = json_object();
			json_object_set_new(js, "n", json_real(steps[i].note));
			json_object_set_new(js, "o", json_boolean(steps[i].on));
			json_object_set_new(js, "g", json_real(steps[i].gateLen));
			json_object_set_new(js, "s", json_real(steps[i].slew));
			json_object_set_new(js, "r", json_integer(steps[i].ratchet));
			json_object_set_new(js, "m", json_real(steps[i].mod));
			json_array_append_new(arr, js);
		}
		return arr;
	}

	void stepsFromJson(json_t* arr, Step* steps) {
		if (!arr)
			return;
		for (int i = 0; i < MAX_STEPS && i < (int)json_array_size(arr); i++) {
			json_t* js = json_array_get(arr, i);
			json_t* f;
			if ((f = json_object_get(js, "n"))) steps[i].note = json_real_value(f);
			if ((f = json_object_get(js, "o"))) steps[i].on = json_boolean_value(f);
			if ((f = json_object_get(js, "g"))) steps[i].gateLen = json_real_value(f);
			if ((f = json_object_get(js, "s"))) steps[i].slew = json_real_value(f);
			if ((f = json_object_get(js, "r"))) steps[i].ratchet = json_integer_value(f);
			if ((f = json_object_get(js, "m"))) steps[i].mod = json_real_value(f);
		}
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "focus"))) focus = json_integer_value(j);
		if ((j = json_object_get(root, "knobMode"))) knobMode = json_integer_value(j);
		if ((j = json_object_get(root, "tuneMode"))) tuneMode = json_integer_value(j);
		if ((j = json_object_get(root, "patSlot"))) patSlot = json_integer_value(j);
		if ((j = json_object_get(root, "resizeAlgo"))) resizeAlgo = json_integer_value(j);
		if ((j = json_object_get(root, "clockPeriod"))) clockPeriod = json_real_value(j);
		json_t* chans = json_object_get(root, "channels");
		if (chans) {
			for (int c = 0; c < 3 && c < (int)json_array_size(chans); c++) {
				json_t* jc = json_array_get(chans, c);
				Channel& C = ch[c];
				if ((j = json_object_get(jc, "length"))) C.length = json_integer_value(j);
				if ((j = json_object_get(jc, "scale"))) C.scaleIdx = json_integer_value(j);
				if ((j = json_object_get(jc, "order"))) C.orderIdx = json_integer_value(j);
				if ((j = json_object_get(jc, "ratio"))) C.ratioIdx = json_integer_value(j);
				if ((j = json_object_get(jc, "resize"))) C.resizeIdx = json_integer_value(j);
				if ((j = json_object_get(jc, "rotate"))) C.rotate = json_integer_value(j);
				if ((j = json_object_get(jc, "transpose"))) C.transpose = json_integer_value(j);
				if ((j = json_object_get(jc, "modMode"))) C.modMode = json_integer_value(j);
				if ((j = json_object_get(jc, "seed"))) C.branchSeed = (uint32_t)json_integer_value(j);
				json_t* kvArr = json_object_get(jc, "kv");
				if (kvArr)
					for (int m = 0; m < 3; m++)
						for (int k = 0; k < 3; k++)
							C.kv[m][k] = json_real_value(json_array_get(kvArr, m * 3 + k));
				stepsFromJson(json_object_get(jc, "steps"), C.steps);
			}
		}
		json_t* pats = json_object_get(root, "patterns");
		if (pats) {
			for (int i = 0; i < NUM_PATTERNS && i < (int)json_array_size(pats); i++) {
				json_t* jp = json_array_get(pats, i);
				Pattern& P = patterns[i];
				if ((j = json_object_get(jp, "used"))) P.used = json_boolean_value(j);
				if (!P.used)
					continue;
				if ((j = json_object_get(jp, "length"))) P.length = json_integer_value(j);
				if ((j = json_object_get(jp, "scale"))) P.scaleIdx = json_integer_value(j);
				if ((j = json_object_get(jp, "order"))) P.orderIdx = json_integer_value(j);
				if ((j = json_object_get(jp, "ratio"))) P.ratioIdx = json_integer_value(j);
				if ((j = json_object_get(jp, "resize"))) P.resizeIdx = json_integer_value(j);
				if ((j = json_object_get(jp, "rotate"))) P.rotate = json_integer_value(j);
				if ((j = json_object_get(jp, "transpose"))) P.transpose = json_integer_value(j);
				if ((j = json_object_get(jp, "modMode"))) P.modMode = json_integer_value(j);
				stepsFromJson(json_object_get(jp, "steps"), P.steps);
			}
		}
		lastContext = -1;
		macroContext = -1;
		inited = false;
	}

	void restoreDefaultTrunk(int c) {
		static const float bootNotes[8] = {12.f, 15.f, 17.f, 19.f, 22.f, 24.f, 19.f, 15.f};
		for (int i = 0; i < MAX_STEPS; i++) {
			ch[c].steps[i] = Step();
			ch[c].steps[i].note = bootNotes[i % 8];
		}
		lastContext = -1;
	}
};

struct EspalierWidget : ModuleWidget {
	EspalierWidget(Espalier* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Espalier.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace elayout;

		// row A
		addParam(createParamCentered<Trimpot>(mm2px(Vec(J8[0], YA)), module, Espalier::ROOT_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[1], YA)), module, Espalier::ROOT_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(J8[2], YA)), module, Espalier::RESEED_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[3], YA)), module, Espalier::RESEED_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[4], YA)), module, Espalier::RESET_INPUT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(J8[5], YA)), module, Espalier::RESET_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[6], YA)), module, Espalier::RATE_INPUT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(J8[7], YA)), module, Espalier::RATE_PARAM));

		// row B
		addParam(createParamCentered<VCVButton>(mm2px(Vec(J8[0], YB)), module, Espalier::CHANNEL_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(J8[0] + 6.5f, YB)), module, Espalier::CHANNEL_LIGHT));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(GROW_X, YB)), module, Espalier::GROW_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(ROUTE_X, YB)), module, Espalier::ROUTE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(EVOLVE_X, YB)), module, Espalier::EVOLVE_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(J8[7], YB)), module, Espalier::PAGE_PARAM));

		// mode row
		addParam(createParamCentered<VCVButton>(mm2px(Vec(J8[0], YM)), module, Espalier::KNOBMODE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(J8[0] + 6.5f, YM)), module, Espalier::KNOBMODE_LIGHT));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(J8[7], YM)), module, Espalier::TUNEMODE_PARAM));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(J8[7] - 6.5f, YM)), module, Espalier::TUNEMODE_LIGHT));
		for (int pg = 0; pg < 8; pg++)
			addChild(createLightCentered<SmallLight<WhiteLight>>(mm2px(Vec(PAGE_LIGHT_X0 + pg * PAGE_LIGHT_DX, PAGE_LIGHT_Y)), module, Espalier::PAGE_LIGHT + pg));

		// step grid
		for (int k = 0; k < 8; k++) {
			float x = SC[k % 4];
			float y = (k < 4) ? SY1 : SY2;
			addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(x, y)), module, Espalier::STEP_PARAM + k));
			addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(x + STEP_LIGHT_DX, y + STEP_LIGHT_DY)), module, Espalier::STEP_LIGHT + k * 3));
			addParam(createParamCentered<TL1105>(mm2px(Vec(x + STEP_BTN_DX, y + STEP_BTN_DY)), module, Espalier::STEPBTN_PARAM + k));
		}

		// jacks
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[0], R1)), module, Espalier::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[1], R1)), module, Espalier::GROW_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[2], R1)), module, Espalier::ROUTE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J8[3], R1)), module, Espalier::EVOLVE_INPUT));
		for (int c = 0; c < 3; c++) {
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J8[4 + c], R1)), module, Espalier::GATE_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J8[1 + c], R2)), module, Espalier::MOD_OUTPUT + c));
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J8[4 + c], R2)), module, Espalier::NOTE_OUTPUT + c));
		}
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J8[7], R1)), module, Espalier::CLOCK_OUTPUT));
		addParam(createParamCentered<VCVLatch>(mm2px(Vec(J8[0], R2)), module, Espalier::SHIFT_PARAM));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(J8[0] + 6.f, R2 - 5.f)), module, Espalier::SHIFT_LIGHT));
	}

	void appendContextMenu(Menu* menu) override {
		Espalier* module = getModule<Espalier>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexPtrSubmenuItem("Focused channel", {"1", "2", "3"}, &module->focus));
		menu->addChild(createIndexPtrSubmenuItem("Knob mode",
			{"Default (branches)", "Micro mutate", "Performance"}, &module->knobMode));
		menu->addChild(createIndexPtrSubmenuItem("Tune mode",
			{"Note", "Gate length", "Slew", "Ratchet", "Mod"}, &module->tuneMode));
		menu->addChild(createIndexPtrSubmenuItem("Scale (focused channel; also shift + SCALE knob)",
			{"Chromatic", "Major", "Minor", "Major pentatonic", "Minor pentatonic",
			 "Harmonic minor", "Whole tone", "Unquantized"},
			&module->ch[module->focus].scaleIdx));
		menu->addChild(createIndexPtrSubmenuItem("Mod output (focused channel)",
			{"Shapes", "Velocity", "Smooth", "Envelope"}, &module->ch[module->focus].modMode));
		menu->addChild(createIndexPtrSubmenuItem("Resize algorithm",
			{"Spread (default)", "Stretch", "Clone"}, &module->resizeAlgo));
		std::vector<std::string> slotNames;
		for (int i = 0; i < NUM_PATTERNS; i++)
			slotNames.push_back(string::f("%d%s", i + 1, module->patterns[i].used ? " *" : ""));
		menu->addChild(createIndexPtrSubmenuItem("Pattern slot", slotNames, &module->patSlot));
		menu->addChild(createMenuItem("Save focused channel to pattern slot", "",
			[=]() { module->savePattern(module->patSlot); }));
		menu->addChild(createMenuItem("Load pattern slot into focused channel", "",
			[=]() { module->loadPattern(module->patSlot); }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Global edit (all channels)", "", &module->globalLatch));
		menu->addChild(createMenuItem("Reseed focused tune data (this page)", "",
			[=]() { module->reseedTuneData(module->focus, false); }));
		menu->addChild(createMenuItem("Reseed focused tune data (all 64 steps)", "",
			[=]() { module->reseedTuneData(module->focus, true); }));
		menu->addChild(createMenuItem("Reset focused tune data", "",
			[=]() { module->resetTuneData(module->focus, false); }));
		menu->addChild(createMenuItem("Reset all tune data", "",
			[=]() { module->resetTuneData(module->focus, true); }));
		menu->addChild(createMenuItem("Restore default trunk melody", "",
			[=]() { module->restoreDefaultTrunk(module->focus); }));
	}
};

Model* modelEspalier = createModel<Espalier, EspalierWidget>("Espalier");
