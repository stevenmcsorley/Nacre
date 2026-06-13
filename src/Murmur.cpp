// Murmur — a polyphonic macro oscillator for VCV Rack: up to 16 voices of
// Émilie Gillet's open-source (MIT-licensed) Plaits DSP, driven by
// polyphonic V/oct and trigger cables. The Plaits engine code is vendored
// verbatim under eurorack/plaits with its license intact; this file is the
// original Rack integration and polyphony wrapper.

#include "plugin.hpp"
#include "layout_murmur.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "plaits/dsp/voice.h"
#pragma GCC diagnostic pop

static const int MAX_VOICES = 16;
static const int NUM_ENGINES = 24;
static const float PLAITS_SR = 48000.f;

struct Murmur : Module {
	enum ParamId {
		MODEL_PARAM,
		FREQ_PARAM,
		DECAY_PARAM,
		LPG_PARAM,
		HARM_PARAM,
		TIMBRE_PARAM,
		MORPH_PARAM,
		FM_ATT_PARAM,
		TIMBRE_ATT_PARAM,
		MORPH_ATT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		MODEL_INPUT,
		HARM_INPUT,
		TIMBRE_INPUT,
		MORPH_INPUT,
		FM_INPUT,
		LEVEL_INPUT,
		VOCT_INPUT,
		TRIG_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_OUTPUT,
		AUX_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(ENGINE_LIGHT, 8 * 3),
		LIGHTS_LEN
	};

	struct PolyVoice {
		plaits::Voice voice;
		plaits::Patch patch = {};
		plaits::Modulations mods = {};
		char sharedBuffer[16384] = {};
		// 48 kHz block fifo + linear resampler state
		plaits::Voice::Frame frames[plaits::kBlockSize];
		int frameIdx = plaits::kBlockSize;
		float prevOut = 0.f, prevAux = 0.f;
		float curOut = 0.f, curAux = 0.f;
		double srcPhase = 1.0;

		void init() {
			stmlib::BufferAllocator allocator(sharedBuffer, sizeof(sharedBuffer));
			voice.Init(&allocator);
		}
	};

	PolyVoice* voices[MAX_VOICES] = {};
	dsp::ClockDivider lightDivider;

	Murmur() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(MODEL_PARAM, 0.f, NUM_ENGINES - 1, 8.f, "Model");
		paramQuantities[MODEL_PARAM]->snapEnabled = true;
		configParam(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " oct");
		configParam(DECAY_PARAM, 0.f, 1.f, 0.5f, "LPG decay");
		configParam(LPG_PARAM, 0.f, 1.f, 0.5f, "LPG colour (VCA to VCF)");
		configParam(HARM_PARAM, 0.f, 1.f, 0.5f, "Harmonics");
		configParam(TIMBRE_PARAM, 0.f, 1.f, 0.5f, "Timbre");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.5f, "Morph");
		configParam(FM_ATT_PARAM, -1.f, 1.f, 0.f, "FM attenuverter");
		configParam(TIMBRE_ATT_PARAM, -1.f, 1.f, 0.f, "Timbre CV attenuverter");
		configParam(MORPH_ATT_PARAM, -1.f, 1.f, 0.f, "Morph CV attenuverter");
		configInput(MODEL_INPUT, "Model CV");
		configInput(HARM_INPUT, "Harmonics CV (polyphonic)");
		configInput(TIMBRE_INPUT, "Timbre CV (polyphonic)");
		configInput(MORPH_INPUT, "Morph CV (polyphonic)");
		configInput(FM_INPUT, "FM (polyphonic)");
		configInput(LEVEL_INPUT, "Level (polyphonic, opens the LPG)");
		configInput(VOCT_INPUT, "1V/oct (polyphonic)");
		configInput(TRIG_INPUT, "Trigger (polyphonic)");
		configOutput(OUT_OUTPUT, "Main (polyphonic)");
		configOutput(AUX_OUTPUT, "Auxiliary (polyphonic)");
		lightDivider.setDivision(256);
		for (int i = 0; i < MAX_VOICES; i++) {
			voices[i] = new PolyVoice;
			voices[i]->init();
		}
	}

	~Murmur() override {
		for (int i = 0; i < MAX_VOICES; i++)
			delete voices[i];
	}

	void process(const ProcessArgs& args) override {
		int channels = std::max({1, inputs[VOCT_INPUT].getChannels(), inputs[TRIG_INPUT].getChannels()});
		channels = std::min(channels, MAX_VOICES);
		outputs[OUT_OUTPUT].setChannels(channels);
		outputs[AUX_OUTPUT].setChannels(channels);

		int engine = clamp((int)std::round(params[MODEL_PARAM].getValue() + inputs[MODEL_INPUT].getVoltage() * 2.4f), 0, NUM_ENGINES - 1);
		float freqOct = params[FREQ_PARAM].getValue();
		bool trigPatched = inputs[TRIG_INPUT].isConnected();
		bool timbrePatched = inputs[TIMBRE_INPUT].isConnected();
		bool morphPatched = inputs[MORPH_INPUT].isConnected();
		bool fmPatched = inputs[FM_INPUT].isConnected();
		bool levelPatched = inputs[LEVEL_INPUT].isConnected();

		double srcInc = (double)PLAITS_SR * args.sampleTime;

		for (int c = 0; c < channels; c++) {
			PolyVoice& v = *voices[c];
			// per-channel CV (mono cables broadcast channel 0)
			auto chV = [&](int inputId) {
				int n = inputs[inputId].getChannels();
				return inputs[inputId].getVoltage(n > 1 ? std::min(c, n - 1) : 0);
			};

			v.patch.engine = engine;
			v.patch.note = 60.f + freqOct * 12.f;
			v.patch.harmonics = params[HARM_PARAM].getValue();
			v.patch.timbre = params[TIMBRE_PARAM].getValue();
			v.patch.morph = params[MORPH_PARAM].getValue();
			v.patch.frequency_modulation_amount = params[FM_ATT_PARAM].getValue();
			v.patch.timbre_modulation_amount = params[TIMBRE_ATT_PARAM].getValue();
			v.patch.morph_modulation_amount = params[MORPH_ATT_PARAM].getValue();
			v.patch.decay = params[DECAY_PARAM].getValue();
			v.patch.lpg_colour = params[LPG_PARAM].getValue();

			v.mods.engine = 0.f;
			v.mods.note = chV(VOCT_INPUT) * 12.f;
			v.mods.frequency = fmPatched ? chV(FM_INPUT) * 6.f : 0.f;
			v.mods.harmonics = chV(HARM_INPUT) / 5.f;
			v.mods.timbre = timbrePatched ? chV(TIMBRE_INPUT) / 8.f : 0.f;
			v.mods.morph = morphPatched ? chV(MORPH_INPUT) / 8.f : 0.f;
			v.mods.trigger = trigPatched ? (chV(TRIG_INPUT) / 3.f) : 0.f;
			v.mods.level = levelPatched ? clamp(chV(LEVEL_INPUT) / 8.f, 0.f, 1.f) : 1.f;
			v.mods.frequency_patched = fmPatched;
			v.mods.timbre_patched = timbrePatched;
			v.mods.morph_patched = morphPatched;
			v.mods.trigger_patched = trigPatched;
			v.mods.level_patched = levelPatched;

			// resample 48 kHz blocks to the engine rate (linear)
			v.srcPhase += srcInc;
			while (v.srcPhase >= 1.0) {
				v.srcPhase -= 1.0;
				v.frameIdx++;
				if (v.frameIdx >= plaits::kBlockSize) {
					v.voice.Render(v.patch, v.mods, v.frames, plaits::kBlockSize);
					v.frameIdx = 0;
				}
				v.prevOut = v.curOut;
				v.prevAux = v.curAux;
				v.curOut = v.frames[v.frameIdx].out / 32768.f;
				v.curAux = v.frames[v.frameIdx].aux / 32768.f;
			}
			float fr = (float)v.srcPhase;
			outputs[OUT_OUTPUT].setVoltage((v.prevOut + (v.curOut - v.prevOut) * fr) * 5.f, c);
			outputs[AUX_OUTPUT].setVoltage((v.prevAux + (v.curAux - v.prevAux) * fr) * 5.f, c);
		}

		// engine bank display: position 1-8, color = bank (green/yellow/red)
		if (lightDivider.process()) {
			int pos = engine % 8;
			int bank = engine / 8;
			for (int i = 0; i < 8; i++) {
				bool on = (i == pos);
				lights[ENGINE_LIGHT + i * 3 + 0].setBrightness(on && bank >= 1 ? 1.f : 0.f);
				lights[ENGINE_LIGHT + i * 3 + 1].setBrightness(on && bank != 2 ? 1.f : 0.f);
				lights[ENGINE_LIGHT + i * 3 + 2].setBrightness(0.f);
			}
		}
	}
};

struct MurmurWidget : ModuleWidget {
	MurmurWidget(Murmur* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Murmur.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		using namespace ulayout;

		for (int i = 0; i < 8; i++)
			addChild(createLightCentered<SmallLight<RedGreenBlueLight>>(mm2px(Vec(ELED_X0 + i * ELED_DX, ELED_Y)), module, Murmur::ENGINE_LIGHT + i * 3));

		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(MODEL_X, MODEL_Y)), module, Murmur::MODEL_PARAM));
		addParam(createParamCentered<Rogan3PSWhite>(mm2px(Vec(FREQ_X, FREQ_Y)), module, Murmur::FREQ_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(DECAY_X, DECAY_Y)), module, Murmur::DECAY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(LPG_X, LPG_Y)), module, Murmur::LPG_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(HARM_X, KB_Y)), module, Murmur::HARM_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(TIMB_X, KB_Y)), module, Murmur::TIMBRE_PARAM));
		addParam(createParamCentered<Rogan1PSWhite>(mm2px(Vec(MORPH_X, KB_Y)), module, Murmur::MORPH_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(FMATT_X, ATT_Y)), module, Murmur::FM_ATT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(TIMB_X, ATT_Y)), module, Murmur::TIMBRE_ATT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(MORPH_X, ATT_Y)), module, Murmur::MORPH_ATT_PARAM));

		static const int r1[6] = {Murmur::MODEL_INPUT, Murmur::HARM_INPUT, Murmur::TIMBRE_INPUT,
		                          Murmur::MORPH_INPUT, Murmur::FM_INPUT, Murmur::LEVEL_INPUT};
		for (int i = 0; i < 6; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[i], R1)), module, r1[i]));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[0], R2)), module, Murmur::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(J6[1], R2)), module, Murmur::TRIG_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[4], R2)), module, Murmur::OUT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(J6[5], R2)), module, Murmur::AUX_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Murmur* module = getModule<Murmur>();
		menu->addChild(new MenuSeparator);
		static const char* engineNames[NUM_ENGINES] = {
			"Virtual analog VCF", "Phase distortion", "Six-op FM A", "Six-op FM B",
			"Six-op FM C", "Wave terrain", "String machine", "Chiptune",
			"Virtual analog", "Waveshaping", "Two-op FM", "Granular formant",
			"Harmonic", "Wavetable", "Chords", "Vowel/speech",
			"Granular cloud", "Filtered noise", "Particle noise", "Inharmonic string",
			"Modal resonator", "Bass drum", "Snare drum", "Hi-hat"};
		menu->addChild(createIndexSubmenuItem("Engine",
			std::vector<std::string>(engineNames, engineNames + NUM_ENGINES),
			[=]() { return (int)std::round(module->params[Murmur::MODEL_PARAM].getValue()); },
			[=](int v) { module->params[Murmur::MODEL_PARAM].setValue((float)v); }));
	}
};

Model* modelMurmur = createModel<Murmur, MurmurWidget>("Murmur");
