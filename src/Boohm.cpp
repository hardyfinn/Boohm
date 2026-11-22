// Boohm: a kick drum voice for VCV Rack, made as a companion to the Ohmforce
// Bohm Eurorack module. The controls follow the Bohm's panel (HIT, VELOCITY,
// LENGTH, SUSTAIN, ATTACK, PITCH, CURVE, COLOR, TRS DECAY, TRS TONE, FX,
// STEREO); the sound engines are our own (see BoohmDSP.hpp).
#include "plugin.hpp"
#include "BoohmDSP.hpp"

// ---- LAYOUT (same numbers in res/make_panel.py) ----
static const float COL[6] = {12.5f, 33.9f, 55.3f, 76.7f, 98.1f, 119.5f};
static const float R1_Y = 27.0f, R2_Y = 49.0f, R3_Y = 69.0f;
static const float J1_Y = 87.0f, J2_Y = 101.5f, J3_Y = 116.0f;
static const float LIST_LX = 6.0f;          // x of the selector lights
static const float MODEL_Y0 = 25.0f, FX_Y0 = 55.5f, LIST_DY = 4.0f;
static const float SEL_BX = 21.6f;          // x of the two little "next" buttons
static const float MODEL_BY = 18.4f, FX_BY = 48.4f;

static const char* const MODEL_NAMES[boohm::NUM_MODELS] = {"ANLG", "FM-2", "MEMB", "GRIT"};
static const char* const MODEL_LONG[boohm::NUM_MODELS] = {
	"ANLG (analogue-style shapes + click)", "FM-2 (two-operator FM)",
	"MEMB (struck membrane + room)", "GRIT (digital wavetables + metal tick)"};
static const char* const FX_NAMES[boohm::NUM_FX] = {"TUBE", "BASS", "SOFT", "HARD", "FOLD", "CRUSH"};
static const float FM_RATIOS[] = {0.5f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f, 12.f, 15.f};
static const int NUM_RATIOS = 13;

struct Boohm : Module {
	enum ParamIds {
		MODEL_PARAM, MODEL_NEXT_PARAM,
		FXTYPE_PARAM, FXTYPE_NEXT_PARAM,
		PITCH_PARAM, CURVE_PARAM, LENGTH_PARAM, ATTACK_PARAM, SUSTAIN_PARAM,
		COLOR_PARAM, TRSDECAY_PARAM, TRSTONE_PARAM, VELOCITY_PARAM, FX_PARAM,
		PITCH_ATV_PARAM, STEREO_PARAM, RAND_PARAM, HIT_PARAM, SUSTAIN_ATV_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		MODEL_INPUT, PITCH_INPUT, CURVE_INPUT, LENGTH_INPUT, ATTACK_INPUT, SUSTAIN_INPUT,
		FXTYPE_INPUT, COLOR_INPUT, TRSDECAY_INPUT, TRSTONE_INPUT, VELOCITY_INPUT, FX_INPUT,
		HIT_INPUT, RAND_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		ENV_OUTPUT, ENVINV_OUTPUT, LEFT_OUTPUT, RIGHT_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(MODEL_LIGHT, boohm::NUM_MODELS),
		ENUMS(FX_LIGHT, boohm::NUM_FX),
		HIT_LIGHT, RAND_LIGHT,
		NUM_LIGHTS
	};

	boohm::Voice voice;
	boohm::Controls ctl;
	dsp::SchmittTrigger hitTrig, hitButtonTrig, randTrig, randButtonTrig, modelNext, fxNext;
	dsp::PulseGenerator randFlash;
	dsp::ClockDivider ctlDivider, lightDivider;
	int hitDelay = 0;          // samples until a pending hit plays

	// right-click options
	bool tracking = false;     // PITCH CV is 1 V/oct (0 V + PITCH fully left = C1)
	bool gateSustain = true;   // a HIT held high keeps the kick sustaining
	int ratioIndex = 2;        // FM-2 modulator ratio (default 2)
	bool randModel = false;    // RANDOM also picks a new model

	Boohm() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configSwitch(MODEL_PARAM, 0.f, boohm::NUM_MODELS - 1, 0.f, "Model",
		             {MODEL_LONG[0], MODEL_LONG[1], MODEL_LONG[2], MODEL_LONG[3]});
		configButton(MODEL_NEXT_PARAM, "Next model");
		configSwitch(FXTYPE_PARAM, 0.f, boohm::NUM_FX - 1, 0.f, "FX type",
		             {"Tube", "Bass (lows only)", "Soft clip", "Hard clip", "Wavefold", "Bitcrush"});
		configButton(FXTYPE_NEXT_PARAM, "Next FX type");

		configParam(PITCH_PARAM, 0.f, 1.f, 0.25f, "Pitch", " Hz", 2.f, boohm::C1_HZ);
		configParam(CURVE_PARAM, 0.f, 1.f, 0.35f, "Curve (left 808-style fast sweep, right 909-style)", "%", 0.f, 100.f);
		configParam(LENGTH_PARAM, 0.f, 1.f, 0.55f, "Length", " ms", 62.5f, 40.f);  // 40 ms .. 2.5 s
		configParam(ATTACK_PARAM, 0.f, 1.f, 0.5f, "Attack (click and punch; FM depth on FM-2; beater on MEMB)", "%", 0.f, 100.f);
		configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.8f, "Sustain (body level; room mic on MEMB)", "%", 0.f, 100.f);
		configParam(COLOR_PARAM, 0.f, 1.f, 0.f, "Color (body shape / table)", "%", 0.f, 100.f);
		configParam(TRSDECAY_PARAM, 0.f, 1.f, 0.3f, "Transient decay (FM decay on FM-2; room size on MEMB)", "%", 0.f, 100.f);
		configParam(TRSTONE_PARAM, 0.f, 1.f, 0.5f, "Transient tone (FM wave on FM-2; beater hardness on MEMB)", "%", 0.f, 100.f);
		configParam(VELOCITY_PARAM, 0.f, 1.f, 1.f, "Velocity", "%", 0.f, 100.f);
		configParam(FX_PARAM, 0.f, 1.f, 0.f, "FX amount", "%", 0.f, 100.f);
		configParam(PITCH_ATV_PARAM, -1.f, 1.f, 0.f, "Pitch CV attenuverter", "%", 0.f, 100.f);
		configParam(STEREO_PARAM, 0.f, 1.f, 0.3f, "Stereo width", "%", 0.f, 100.f);
		configButton(RAND_PARAM, "Random sound");
		configButton(HIT_PARAM, "Hit (hold to sustain)");
		configParam(SUSTAIN_ATV_PARAM, -1.f, 1.f, 1.f, "Sustain CV attenuverter", "%", 0.f, 100.f);

		configInput(MODEL_INPUT, "Model CV (2.5 V per model)");
		configInput(PITCH_INPUT, "Pitch CV (through the attenuverter; 1 V/oct when tracking is on)");
		configInput(CURVE_INPUT, "Curve CV");
		configInput(LENGTH_INPUT, "Length CV");
		configInput(ATTACK_INPUT, "Attack CV");
		configInput(SUSTAIN_INPUT, "Sustain CV (through the attenuverter)");
		configInput(FXTYPE_INPUT, "FX type CV (about 1.7 V per type)");
		configInput(COLOR_INPUT, "Color CV");
		configInput(TRSDECAY_INPUT, "Transient decay CV");
		configInput(TRSTONE_INPUT, "Transient tone CV");
		configInput(VELOCITY_INPUT, "Velocity CV (read at each hit, 10 V = full)");
		configInput(FX_INPUT, "FX amount CV");
		configInput(HIT_INPUT, "Hit (trigger, or gate to sustain)");
		configInput(RAND_INPUT, "Random sound trigger");
		configOutput(ENV_OUTPUT, "Envelope (0 to 10 V)");
		configOutput(ENVINV_OUTPUT, "Inverted envelope (10 V down to 0, for ducking)");
		configOutput(LEFT_OUTPUT, "Left (mono when Right is not patched)");
		configOutput(RIGHT_OUTPUT, "Right");

		for (int i = 0; i < boohm::NUM_MODELS; i++) configLight(MODEL_LIGHT + i, MODEL_NAMES[i]);
		for (int i = 0; i < boohm::NUM_FX; i++) configLight(FX_LIGHT + i, FX_NAMES[i]);

		ctlDivider.setDivision(8);
		lightDivider.setDivision(64);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		voice.setSampleRate(e.sampleRate);
	}

	void onAdd(const AddEvent& e) override {
		voice.setSampleRate(APP->engine->getSampleRate());
	}

	float knobCv(int param, int input) {
		return clamp(params[param].getValue() + inputs[input].getVoltage() * 0.1f, 0.f, 1.f);
	}

	int modelIndex() {
		float v = params[MODEL_PARAM].getValue() + inputs[MODEL_INPUT].getVoltage() * 0.4f;
		return clamp((int) std::round(v), 0, boohm::NUM_MODELS - 1);
	}
	int fxIndex() {
		float v = params[FXTYPE_PARAM].getValue() + inputs[FXTYPE_INPUT].getVoltage() * 0.6f;
		return clamp((int) std::round(v), 0, boohm::NUM_FX - 1);
	}

	void randomSound() {
		auto set = [&](int p, float lo, float hi) { params[p].setValue(lo + (hi - lo) * random::uniform()); };
		set(PITCH_PARAM, 0.f, 1.f);
		set(CURVE_PARAM, 0.f, 1.f);
		set(LENGTH_PARAM, 0.25f, 0.8f);
		set(ATTACK_PARAM, 0.1f, 1.f);
		set(SUSTAIN_PARAM, 0.5f, 1.f);
		set(COLOR_PARAM, 0.f, 1.f);
		set(TRSDECAY_PARAM, 0.f, 1.f);
		set(TRSTONE_PARAM, 0.f, 1.f);
		set(FX_PARAM, 0.f, 0.6f);
		set(STEREO_PARAM, 0.f, 0.8f);
		params[FXTYPE_PARAM].setValue((float) (int) (random::uniform() * boohm::NUM_FX * 0.999f));
		if (randModel)
			params[MODEL_PARAM].setValue((float) (int) (random::uniform() * boohm::NUM_MODELS * 0.999f));
		randFlash.trigger(0.2f);
	}

	void updateControls() {
		ctl.model = modelIndex();
		ctl.fxType = fxIndex();
		float pitchKnob = params[PITCH_PARAM].getValue();
		if (tracking) {
			ctl.tracking = true;
			ctl.pitchOct = pitchKnob + inputs[PITCH_INPUT].getVoltage();
		}
		else {
			ctl.tracking = false;
			ctl.pitchOct = pitchKnob + params[PITCH_ATV_PARAM].getValue() * inputs[PITCH_INPUT].getVoltage() * 0.2f;
		}
		ctl.curve = knobCv(CURVE_PARAM, CURVE_INPUT);
		ctl.length = knobCv(LENGTH_PARAM, LENGTH_INPUT);
		ctl.attack = knobCv(ATTACK_PARAM, ATTACK_INPUT);
		ctl.sustain = clamp(params[SUSTAIN_PARAM].getValue()
		                    + params[SUSTAIN_ATV_PARAM].getValue() * inputs[SUSTAIN_INPUT].getVoltage() * 0.2f, 0.f, 1.f);
		ctl.color = knobCv(COLOR_PARAM, COLOR_INPUT);
		ctl.trsDecay = knobCv(TRSDECAY_PARAM, TRSDECAY_INPUT);
		ctl.trsTone = knobCv(TRSTONE_PARAM, TRSTONE_INPUT);
		ctl.velocity = params[VELOCITY_PARAM].getValue();
		ctl.fx = knobCv(FX_PARAM, FX_INPUT);
		ctl.stereo = params[STEREO_PARAM].getValue();
		ctl.fmRatio = FM_RATIOS[clamp(ratioIndex, 0, NUM_RATIOS - 1)];
	}

	static float softLimit(float v) {
		float a = std::fabs(v);
		if (a < 6.f) return v;
		float y = 6.f + 4.f * std::tanh((a - 6.f) / 4.f);
		return v < 0.f ? -y : y;
	}

	void process(const ProcessArgs& args) override {
		if (modelNext.process(params[MODEL_NEXT_PARAM].getValue()))
			params[MODEL_PARAM].setValue((float) (((int) params[MODEL_PARAM].getValue() + 1) % boohm::NUM_MODELS));
		if (fxNext.process(params[FXTYPE_NEXT_PARAM].getValue()))
			params[FXTYPE_PARAM].setValue((float) (((int) params[FXTYPE_PARAM].getValue() + 1) % boohm::NUM_FX));

		bool rb = randButtonTrig.process(params[RAND_PARAM].getValue());
		bool rt = randTrig.process(inputs[RAND_INPUT].getVoltage(), 0.1f, 1.f);
		if (rb || rt)
			randomSound();

		if (ctlDivider.process())
			updateControls();

		float hitV = inputs[HIT_INPUT].getVoltage();
		bool button = params[HIT_PARAM].getValue() > 0.f;
		bool jackHit = hitTrig.process(hitV, 0.1f, 1.f);
		bool buttonHit = hitButtonTrig.process(button ? 1.f : 0.f);
		// Wait 1 ms before playing, so pitch / velocity / model CVs that a
		// sequencer sends a sample or two after its trigger are caught.
		if (jackHit || buttonHit)
			hitDelay = std::max(1, (int) (0.001f * args.sampleRate));
		if (hitDelay > 0 && --hitDelay == 0) {
			updateControls();
			float vel = 1.f;
			if (inputs[VELOCITY_INPUT].isConnected())
				vel = clamp(inputs[VELOCITY_INPUT].getVoltage() * 0.1f, 0.f, 1.f);
			voice.hit(vel, ctl);
		}
		bool gateHigh = hitTrig.isHigh() || button;

		float l = 0.f, r = 0.f;
		voice.process(ctl, gateHigh, gateSustain, l, r);
		l = softLimit(l * 5.f);
		r = softLimit(r * 5.f);

		if (outputs[RIGHT_OUTPUT].isConnected()) {
			outputs[LEFT_OUTPUT].setVoltage(l);
			outputs[RIGHT_OUTPUT].setVoltage(r);
		}
		else {
			outputs[LEFT_OUTPUT].setVoltage(0.5f * (l + r));
		}
		float env = clamp(voice.env, 0.f, 1.f);
		outputs[ENV_OUTPUT].setVoltage(env * 10.f);
		outputs[ENVINV_OUTPUT].setVoltage(10.f - env * 10.f);

		bool flash = randFlash.process(args.sampleTime);
		if (lightDivider.process()) {
			float dt = args.sampleTime * lightDivider.getDivision();
			for (int i = 0; i < boohm::NUM_MODELS; i++)
				lights[MODEL_LIGHT + i].setBrightness(i == ctl.model ? 1.f : 0.f);
			for (int i = 0; i < boohm::NUM_FX; i++)
				lights[FX_LIGHT + i].setBrightness(i == ctl.fxType ? (ctl.fx > 0.001f ? 1.f : 0.35f) : 0.f);
			lights[HIT_LIGHT].setBrightnessSmooth(env, dt, 60.f);
			lights[RAND_LIGHT].setBrightnessSmooth(flash || params[RAND_PARAM].getValue() > 0.f ? 1.f : 0.f, dt, 30.f);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "tracking", json_boolean(tracking));
		json_object_set_new(root, "gateSustain", json_boolean(gateSustain));
		json_object_set_new(root, "ratioIndex", json_integer(ratioIndex));
		json_object_set_new(root, "randModel", json_boolean(randModel));
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* j;
		if ((j = json_object_get(root, "tracking"))) tracking = json_boolean_value(j);
		if ((j = json_object_get(root, "gateSustain"))) gateSustain = json_boolean_value(j);
		if ((j = json_object_get(root, "ratioIndex"))) ratioIndex = clamp((int) json_integer_value(j), 0, NUM_RATIOS - 1);
		if ((j = json_object_get(root, "randModel"))) randModel = json_boolean_value(j);
	}
};

struct BoohmWidget : ModuleWidget {
	BoohmWidget(Boohm* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Boohm.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// selectors (model and FX type): a button steps through, lights show which
		addParam(createParamCentered<TL1105>(mm2px(Vec(SEL_BX, MODEL_BY)), module, Boohm::MODEL_NEXT_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(SEL_BX, FX_BY)), module, Boohm::FXTYPE_NEXT_PARAM));
		for (int i = 0; i < boohm::NUM_MODELS; i++)
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(LIST_LX, MODEL_Y0 + i * LIST_DY)), module, Boohm::MODEL_LIGHT + i));
		for (int i = 0; i < boohm::NUM_FX; i++)
			addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(LIST_LX, FX_Y0 + i * LIST_DY)), module, Boohm::FX_LIGHT + i));

		const int r1[5] = {Boohm::PITCH_PARAM, Boohm::CURVE_PARAM, Boohm::LENGTH_PARAM, Boohm::ATTACK_PARAM, Boohm::SUSTAIN_PARAM};
		const int r2[5] = {Boohm::COLOR_PARAM, Boohm::TRSDECAY_PARAM, Boohm::TRSTONE_PARAM, Boohm::VELOCITY_PARAM, Boohm::FX_PARAM};
		for (int i = 0; i < 5; i++) {
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(COL[i + 1], R1_Y)), module, r1[i]));
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(COL[i + 1], R2_Y)), module, r2[i]));
		}
		addParam(createParamCentered<Trimpot>(mm2px(Vec(COL[1], R3_Y)), module, Boohm::PITCH_ATV_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(COL[2], R3_Y)), module, Boohm::STEREO_PARAM));
		addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(COL[3], R3_Y)), module, Boohm::RAND_PARAM, Boohm::RAND_LIGHT));
		addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(COL[4], R3_Y)), module, Boohm::HIT_PARAM, Boohm::HIT_LIGHT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(COL[5], R3_Y)), module, Boohm::SUSTAIN_ATV_PARAM));

		const int j1[6] = {Boohm::MODEL_INPUT, Boohm::PITCH_INPUT, Boohm::CURVE_INPUT, Boohm::LENGTH_INPUT, Boohm::ATTACK_INPUT, Boohm::SUSTAIN_INPUT};
		const int j2[6] = {Boohm::FXTYPE_INPUT, Boohm::COLOR_INPUT, Boohm::TRSDECAY_INPUT, Boohm::TRSTONE_INPUT, Boohm::VELOCITY_INPUT, Boohm::FX_INPUT};
		for (int i = 0; i < 6; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(COL[i], J1_Y)), module, j1[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(COL[i], J2_Y)), module, j2[i]));
		}
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(COL[0], J3_Y)), module, Boohm::HIT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(COL[1], J3_Y)), module, Boohm::RAND_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL[2], J3_Y)), module, Boohm::ENV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL[3], J3_Y)), module, Boohm::ENVINV_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL[4], J3_Y)), module, Boohm::LEFT_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(COL[5], J3_Y)), module, Boohm::RIGHT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Boohm* module = dynamic_cast<Boohm*>(this->module);
		if (!module)
			return;
		menu->addChild(new MenuSeparator);
		menu->addChild(createIndexSubmenuItem("Model",
			{MODEL_LONG[0], MODEL_LONG[1], MODEL_LONG[2], MODEL_LONG[3]},
			[=]() { return (size_t) module->params[Boohm::MODEL_PARAM].getValue(); },
			[=](size_t i) { module->params[Boohm::MODEL_PARAM].setValue((float) i); }));
		menu->addChild(createIndexSubmenuItem("FX type",
			{"Tube", "Bass (lows only)", "Soft clip", "Hard clip", "Wavefold", "Bitcrush"},
			[=]() { return (size_t) module->params[Boohm::FXTYPE_PARAM].getValue(); },
			[=](size_t i) { module->params[Boohm::FXTYPE_PARAM].setValue((float) i); }));
		menu->addChild(createIndexSubmenuItem("FM-2 ratio",
			{"0.5", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "12", "15"},
			[=]() { return (size_t) module->ratioIndex; },
			[=](size_t i) { module->ratioIndex = (int) i; }));
		menu->addChild(createBoolPtrMenuItem("PITCH CV tracks 1 V/oct (PITCH knob = offset above C1)", "", &module->tracking));
		menu->addChild(createBoolPtrMenuItem("Held HIT sustains the kick", "", &module->gateSustain));
		menu->addChild(createBoolPtrMenuItem("RANDOM also changes the model", "", &module->randModel));
	}
};

Model* modelBoohm = createModel<Boohm, BoohmWidget>("Boohm");
