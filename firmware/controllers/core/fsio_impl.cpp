/**
 * @file fsio_impl.cpp
 * @brief FSIO as it's used for GPIO
 *
 * @date Oct 5, 2014
 * @author Andrey Belomutskiy, (c) 2012-2017
 */

#include "main.h"

#if EFI_FSIO || defined(__DOXYGEN__)

#include "fsio_impl.h"
#include "allsensors.h"
#include "rpm_calculator.h"
#include "efiGpio.h"

#define NO_PWM 0

fsio8_Map3D_f32t fsioTable1("fsio#1");
fsio8_Map3D_u8t fsioTable2("fsio#2");
fsio8_Map3D_u8t fsioTable3("fsio#3");
fsio8_Map3D_u8t fsioTable4("fsio#4");


/**
 * Here we define all rusEfi-specific methods
 */
static LENameOrdinalPair leRpm(LE_METHOD_RPM, "rpm");
static LENameOrdinalPair leTps(LE_METHOD_TPS, "tps");
static LENameOrdinalPair leMaf(LE_METHOD_MAF, "maf");
static LENameOrdinalPair leMap(LE_METHOD_MAP, "map");
static LENameOrdinalPair leVBatt(LE_METHOD_VBATT, "vbatt");
static LENameOrdinalPair leFan(LE_METHOD_FAN, "fan");
static LENameOrdinalPair leCoolant(LE_METHOD_COOLANT, "coolant");
static LENameOrdinalPair leIsCoolantBroken(LE_METHOD_IS_COOLANT_BROKEN, "is_clt_broken");
static LENameOrdinalPair leAcToggle(LE_METHOD_AC_TOGGLE, "ac_on_switch");
static LENameOrdinalPair leFanOnSetting(LE_METHOD_FAN_ON_SETTING, "fan_on_setting");
static LENameOrdinalPair leFanOffSetting(LE_METHOD_FAN_OFF_SETTING, "fan_off_setting");
static LENameOrdinalPair leTimeSinceBoot(LE_METHOD_TIME_SINCE_BOOT, "time_since_boot");
static LENameOrdinalPair leFsioSetting(LE_METHOD_FSIO_SETTING, "fsio_setting");
static LENameOrdinalPair leFsioTable(LE_METHOD_FSIO_TABLE, "fsio_table");
static LENameOrdinalPair leFsioAnalogInput(LE_METHOD_FSIO_ANALOG_INPUT, "fsio_input");
static LENameOrdinalPair leKnock(LE_METHOD_KNOCK, "knock");
static LENameOrdinalPair leIntakeVVT(LE_METHOD_INTAKE_VVT, "ivvt");
static LENameOrdinalPair leExhaustVVT(LE_METHOD_EXHAUST_VVT, "evvt");
static LENameOrdinalPair leCrankingRpm(LE_METHOD_CRANKING_RPM, "cranking_rpm");
static LENameOrdinalPair leStartupFuelPumpDuration(LE_METHOD_STARTUP_FUEL_PUMP_DURATION, "startup_fuel_pump_duration");
static LENameOrdinalPair leInShutdown(LE_METHOD_IN_SHUTDOWN, "in_shutdown");

#define LE_EVAL_POOL_SIZE 32

extern EnginePins enginePins;

static LECalculator evalCalc;
static LEElement evalPoolElements[LE_EVAL_POOL_SIZE];
static LEElementPool evalPool(evalPoolElements, LE_EVAL_POOL_SIZE);

#define SYS_ELEMENT_POOL_SIZE 128
#define UD_ELEMENT_POOL_SIZE 128

static LEElement sysElements[SYS_ELEMENT_POOL_SIZE] CCM_OPTIONAL;
LEElementPool sysPool(sysElements, SYS_ELEMENT_POOL_SIZE);

static LEElement userElements[UD_ELEMENT_POOL_SIZE] CCM_OPTIONAL;
LEElementPool userPool(userElements, UD_ELEMENT_POOL_SIZE);
static LEElement * fsioLogics[FSIO_COMMAND_COUNT] CCM_OPTIONAL;

static LEElement * acRelayLogic;
static LEElement * fuelPumpLogic;
static LEElement * radiatorFanLogic;
static LEElement * alternatorLogic;

#if EFI_MAIN_RELAY_CONTROL || defined(__DOXYGEN__)
static LEElement * mainRelayLogic;
#endif /* EFI_MAIN_RELAY_CONTROL */

EXTERN_ENGINE
;

#if EFI_PROD_CODE || EFI_SIMULATOR
static Logging *logger;

float getEngineValue(le_action_e action DECLARE_ENGINE_PARAMETER_SUFFIX) {
	efiAssert(engine!=NULL, "getLEValue", NAN);
	switch (action) {
	case LE_METHOD_FAN:
		return enginePins.fanRelay.getLogicValue();
	case LE_METHOD_AC_TOGGLE:
		return getAcToggle(PASS_ENGINE_PARAMETER_SIGNATURE);
	case LE_METHOD_COOLANT:
		return getCoolantTemperature(PASS_ENGINE_PARAMETER_SIGNATURE);
	case LE_METHOD_IS_COOLANT_BROKEN:
		return engine->isCltBroken;
	case LE_METHOD_INTAKE_AIR:
		return getIntakeAirTemperature(PASS_ENGINE_PARAMETER_SIGNATURE);
	case LE_METHOD_RPM:
		return engine->rpmCalculator.getRpm();
	case LE_METHOD_MAF:
		return getMaf(PASS_ENGINE_PARAMETER_SIGNATURE);
	case LE_METHOD_MAP:
		return getMap();
	case LE_METHOD_INTAKE_VVT:
	case LE_METHOD_EXHAUST_VVT:
		return engine->triggerCentral.vvtPosition;
	case LE_METHOD_TIME_SINCE_BOOT:
		return getTimeNowSeconds();
	case LE_METHOD_FAN_OFF_SETTING:
		return engineConfiguration->fanOffTemperature;
	case LE_METHOD_FAN_ON_SETTING:
		return engineConfiguration->fanOnTemperature;
	case LE_METHOD_CRANKING_RPM:
		return engineConfiguration->cranking.rpm;
	case LE_METHOD_STARTUP_FUEL_PUMP_DURATION:
		// todo: remove default value check and finish migration to startUpFuelPumpDuration param.
		return (engineConfiguration->startUpFuelPumpDuration == 0) ? 4 : engineConfiguration->startUpFuelPumpDuration;
	case LE_METHOD_IN_SHUTDOWN:
		return engine->isInShutdownMode();
	case LE_METHOD_VBATT:
		return getVBatt(PASS_ENGINE_PARAMETER_SIGNATURE);
	default:
		warning(CUSTOM_FSIO_UNEXPECTED, "FSIO unexpected %d", action);
		return NAN;
	}
}


#if EFI_PROD_CODE || defined(__DOXYGEN__)

#include "pin_repository.h"
#include "pwm_generator.h"
// todo: that's about bench test mode, wrong header for sure!
#include "injector_central.h"

static void setFsioInputPin(const char *indexStr, const char *pinName) {
	int index = atoi(indexStr) - 1;
	if (index < 0 || index >= FSIO_COMMAND_COUNT) {
		scheduleMsg(logger, "invalid FSIO index: %d", index);
		return;
	}
	brain_pin_e pin = parseBrainPin(pinName);
	// todo: extract method - code duplication with other 'set_xxx_pin' methods?
	if (pin == GPIO_INVALID) {
		scheduleMsg(logger, "invalid pin name [%s]", pinName);
		return;
	}
	boardConfiguration->fsioDigitalInputs[index] = pin;
	scheduleMsg(logger, "FSIO input pin #%d [%s]", (index + 1), hwPortname(pin));
}

static void setFsioPidOutputPin(const char *indexStr, const char *pinName) {
	int index = atoi(indexStr) - 1;
	if (index < 0 || index >= AUX_PID_COUNT) {
		scheduleMsg(logger, "invalid AUX index: %d", index);
		return;
	}
	brain_pin_e pin = parseBrainPin(pinName);
	// todo: extract method - code duplication with other 'set_xxx_pin' methods?
	if (pin == GPIO_INVALID) {
		scheduleMsg(logger, "invalid pin name [%s]", pinName);
		return;
	}
	engineConfiguration->auxPidPins[index] = pin;
	scheduleMsg(logger, "FSIO aux pin #%d [%s]", (index + 1), hwPortname(pin));
}

static void setFsioOutputPin(const char *indexStr, const char *pinName) {
	int index = atoi(indexStr) - 1;
	if (index < 0 || index >= FSIO_COMMAND_COUNT) {
		scheduleMsg(logger, "invalid FSIO index: %d", index);
		return;
	}
	brain_pin_e pin = parseBrainPin(pinName);
	// todo: extract method - code duplication with other 'set_xxx_pin' methods?
	if (pin == GPIO_INVALID) {
		scheduleMsg(logger, "invalid pin name [%s]", pinName);
		return;
	}
	boardConfiguration->fsioPins[index] = pin;
	scheduleMsg(logger, "FSIO output pin #%d [%s]", (index + 1), hwPortname(pin));
}
#endif /* EFI_PROD_CODE */

#endif

/**
 * index is between zero and LE_COMMAND_LENGTH-1
 */
void setFsioExt(int index, brain_pin_e pin, const char * exp, int freq DECLARE_ENGINE_PARAMETER_SUFFIX) {
	boardConfiguration->fsioPins[index] = pin;
	int len = strlen(exp);
	if (len >= LE_COMMAND_LENGTH) {
		return;
	}
	strcpy(config->fsioFormulas[index], exp);
	boardConfiguration->fsioFrequency[index] = freq;
}

void setFsio(int index, brain_pin_e pin, const char * exp DECLARE_ENGINE_PARAMETER_SUFFIX) {
	setFsioExt(index, pin, exp, NO_PWM PASS_ENGINE_PARAMETER_SUFFIX);
}

void applyFsioConfiguration(DECLARE_ENGINE_PARAMETER_SIGNATURE) {
	userPool.reset();
	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		brain_pin_e brainPin = boardConfiguration->fsioPins[i];

		if (brainPin != GPIO_UNASSIGNED) {
			const char *formula = config->fsioFormulas[i];
			LEElement *logic = userPool.parseExpression(formula);
			if (logic == NULL) {
				warning(CUSTOM_FSIO_PARSING, "parsing [%s]", formula);
			}

			fsioLogics[i] = logic;
		}
	}
}

#if EFI_PROD_CODE || defined(__DOXYGEN__)

static SimplePwm fsioPwm[FSIO_COMMAND_COUNT] CCM_OPTIONAL;

static LECalculator calc;
extern LEElement * fsioLogics[FSIO_COMMAND_COUNT];

// that's crazy, but what's an alternative? we need const char *, a shared buffer would not work for pin repository
static const char *getGpioPinName(int index) {
	switch (index) {
	case 0:
		return "FSIO_OUT_0";
	case 1:
		return "FSIO_OUT_1";
	case 10:
		return "FSIO_OUT_10";
	case 11:
		return "FSIO_OUT_11";
	case 12:
		return "FSIO_OUT_12";
	case 13:
		return "FSIO_OUT_13";
	case 14:
		return "FSIO_OUT_14";
	case 15:
		return "FSIO_OUT_15";
	case 2:
		return "FSIO_OUT_2";
	case 3:
		return "FSIO_OUT_3";
	case 4:
		return "FSIO_OUT_4";
	case 5:
		return "FSIO_OUT_5";
	case 6:
		return "FSIO_OUT_6";
	case 7:
		return "FSIO_OUT_7";
	case 8:
		return "FSIO_OUT_8";
	case 9:
		return "FSIO_OUT_9";
	}
	return NULL;
}

/**
 * @param index from zero for (FSIO_COMMAND_COUNT - 1)
 */
static void handleFsio(Engine *engine, int index) {
	if (boardConfiguration->fsioPins[index] == GPIO_UNASSIGNED)
		return;

	bool isPwmMode = boardConfiguration->fsioFrequency[index] != NO_PWM;

	float fvalue;
	if (fsioLogics[index] == NULL) {
		warning(CUSTOM_NO_FSIO, "no FSIO for #%d %s", index + 1, hwPortname(boardConfiguration->fsioPins[index]));
		fvalue = NAN;
	} else {
		fvalue = calc.getValue2(engine->fsioLastValue[index], fsioLogics[index] PASS_ENGINE_PARAMETER_SUFFIX);
	}
	engine->fsioLastValue[index] = fvalue;

	if (isPwmMode) {
		fsioPwm[index].setSimplePwmDutyCycle(fvalue);
	} else {
		int value = (int) fvalue;
		if (value != enginePins.fsioOutputs[index].getLogicValue()) {
			//		scheduleMsg(logger, "setting %s %s", getIo_pin_e(pin), boolToString(value));
			enginePins.fsioOutputs[index].setValue(value);
		}
	}
}


static const char * action2String(le_action_e action) {
	static char buffer[_MAX_FILLER];
	switch(action) {
		case LE_METHOD_RPM:
			return "RPM";
		case LE_METHOD_CRANKING_RPM:
			return "cranking_rpm";
		case LE_METHOD_COOLANT:
			return "CLT";
		case LE_METHOD_FAN_ON_SETTING:
			return "fan_on";
		case LE_METHOD_FAN_OFF_SETTING:
			return "fan_off";
		case LE_METHOD_FAN:
			return "fan";
		case LE_METHOD_STARTUP_FUEL_PUMP_DURATION:
			return "startup_fuel_pump_duration";
		case LE_METHOD_IN_SHUTDOWN:
			return "in_shutdown";

		default: {
			// this is here to make compiler happy
		}
	}
	itoa10(buffer, (int)action);
	return buffer;
}

static void setPinState(const char * msg, OutputPin *pin, LEElement *element) {
	if (element == NULL) {
		warning(CUSTOM_OBD_11, "invalid expression for %s", msg);
	} else {
		int value = (int)calc.getValue2(pin->getLogicValue(), element PASS_ENGINE_PARAMETER_SUFFIX);
		if (pin->isInitialized() && value != pin->getLogicValue()) {
			if (isRunningBenchTest()) {
				return; // let's not mess with bench testing
			}

			for (int i = 0;i < calc.currentCalculationLogPosition;i++) {
				scheduleMsg(logger, "calc %d: action %s value %f", i, action2String(calc.calcLogAction[i]), calc.calcLogValue[i]);
			}

			scheduleMsg(logger, "setPin %s %s", msg, value ? "on" : "off");
			pin->setValue(value);
		}
	}
}

static void setFsioFrequency(int index, int frequency) {
	index--;
	if (index < 0 || index >= FSIO_COMMAND_COUNT) {
		scheduleMsg(logger, "invalid FSIO index: %d", index);
		return;
	}
	boardConfiguration->fsioFrequency[index] = frequency;
	if (frequency == 0) {
		scheduleMsg(logger, "FSIO output #%d@%s set to on/off mode", index + 1, hwPortname(boardConfiguration->fsioPins[index]));
	} else {
		scheduleMsg(logger, "Setting FSIO frequency %dHz on #%d@%s", frequency, index + 1, hwPortname(boardConfiguration->fsioPins[index]));
	}
}

void runFsio(void) {
	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		handleFsio(engine, i);
	}

#if EFI_FUEL_PUMP || defined(__DOXYGEN__)
	if (boardConfiguration->fuelPumpPin != GPIO_UNASSIGNED) {
		setPinState("pump", &enginePins.fuelPumpRelay, fuelPumpLogic);
	}
#endif /* EFI_FUEL_PUMP */

#if EFI_MAIN_RELAY_CONTROL || defined(__DOXYGEN__)
	if (boardConfiguration->mainRelayPin != GPIO_UNASSIGNED)
		setPinState("main_relay", &enginePins.mainRelay, mainRelayLogic);
#else /* EFI_MAIN_RELAY_CONTROL */
	/**
	 * main relay is always on if ECU is on, that's a good enough initial implementation
	 */
	if (boardConfiguration->mainRelayPin != GPIO_UNASSIGNED)
		enginePins.mainRelay.setValue(true);
#endif /* EFI_MAIN_RELAY_CONTROL */

	enginePins.o2heater.setValue(engine->rpmCalculator.isRunning());

	if (boardConfiguration->acRelayPin != GPIO_UNASSIGNED) {
		setPinState("A/C", &enginePins.acRelay, acRelayLogic);
	}

//	if (boardConfiguration->alternatorControlPin != GPIO_UNASSIGNED) {
//		setPinState("alternator", &enginePins.alternatorField, alternatorLogic, engine);
//	}

	if (boardConfiguration->fanPin != GPIO_UNASSIGNED) {
		setPinState("fan", &enginePins.fanRelay, radiatorFanLogic);
	}

}

static pin_output_mode_e defa = OM_DEFAULT;

#endif /* EFI_PROD_CODE */

static void showFsio(const char *msg, LEElement *element) {
#if EFI_PROD_CODE || EFI_SIMULATOR
	if (msg != NULL)
		scheduleMsg(logger, "%s:", msg);
	while (element != NULL) {
		scheduleMsg(logger, "action %d: fValue=%f iValue=%d", element->action, element->fValue, element->iValue);
		element = element->next;
	}
	scheduleMsg(logger, "<end>");
#endif
}

static void showFsioInfo(void) {
#if EFI_PROD_CODE || EFI_SIMULATOR
	scheduleMsg(logger, "sys used %d/user used %d", sysPool.getSize(), userPool.getSize());
	showFsio("a/c", acRelayLogic);
	showFsio("fuel", fuelPumpLogic);
	showFsio("fan", radiatorFanLogic);
	showFsio("alt", alternatorLogic);

	for (int i = 0; i < AUX_PID_COUNT ; i++) {
		brain_pin_e pin = engineConfiguration->auxPidPins[i];
		if (pin != GPIO_UNASSIGNED) {
			scheduleMsg(logger, "FSIO aux #%d [%s]", (i + 1),
					hwPortname(pin));

		}
	}



	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		char * exp = config->fsioFormulas[i];
		if (exp[0] != 0) {
			/**
			 * in case of FSIO user interface indexes are starting with 0, the argument for that
			 * is the fact that the target audience is more software developers
			 */
			scheduleMsg(logger, "FSIO #%d [%s] at %s@%dHz value=%f", (i + 1), exp,
					hwPortname(boardConfiguration->fsioPins[i]), boardConfiguration->fsioFrequency[i],
					engine->fsioLastValue[i]);
//			scheduleMsg(logger, "user-defined #%d value=%f", i, engine->engineConfiguration2->fsioLastValue[i]);
			showFsio(NULL, fsioLogics[i]);
		}
	}
	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		float v = boardConfiguration->fsio_setting[i];
		if (!cisnan(v)) {
			scheduleMsg(logger, "user property #%d: %f", i + 1, v);
		}
	}
	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		brain_pin_e inputPin = boardConfiguration->fsioDigitalInputs[i];
		if (inputPin != GPIO_UNASSIGNED) {
			scheduleMsg(logger, "FSIO digital input #%d: %s", i, hwPortname(inputPin));
		}
	}
#endif
}

/**
 * set_fsio_setting 1 0.11
 */
static void setFsioSetting(float humanIndexF, float value) {
#if EFI_PROD_CODE || EFI_SIMULATOR
	int index = (int)humanIndexF - 1;
	if (index < 0 || index >= FSIO_COMMAND_COUNT) {
		scheduleMsg(logger, "invalid FSIO index: %d", (int)humanIndexF);
		return;
	}
	engineConfiguration->bc.fsio_setting[index] = value;
	showFsioInfo();
#endif
}

static void setFsioExpression(const char *indexStr, const char *quotedLine, Engine *engine) {
#if EFI_PROD_CODE || EFI_SIMULATOR
	int index = atoi(indexStr) - 1;
	if (index < 0 || index >= FSIO_COMMAND_COUNT) {
		scheduleMsg(logger, "invalid FSIO index: %d", index);
		return;
	}
	char * l = unquote((char*) quotedLine);
	if (strlen(l) > LE_COMMAND_LENGTH - 1) {
		scheduleMsg(logger, "Too long %d", strlen(l));
		return;
	}

	scheduleMsg(logger, "setting user out #%d to [%s]", index + 1, l);
	strcpy(engine->config->fsioFormulas[index], l);
	// this would apply the changes
	applyFsioConfiguration(PASS_ENGINE_PARAMETER_SIGNATURE);
	showFsioInfo();
#endif
}

static void rpnEval(char *line) {
#if EFI_PROD_CODE || EFI_SIMULATOR
	line = unquote(line);
	scheduleMsg(logger, "Parsing [%s]", line);
	evalPool.reset();
	LEElement * e = evalPool.parseExpression(line);
	if (e == NULL) {
		scheduleMsg(logger, "parsing failed");
	} else {
		float result = evalCalc.getValue2(0, e PASS_ENGINE_PARAMETER_SUFFIX);
		scheduleMsg(logger, "Evaluate result: %f", result);
	}
#endif
}

void initFsioImpl(Logging *sharedLogger DECLARE_ENGINE_PARAMETER_SUFFIX) {
#if EFI_PROD_CODE || EFI_SIMULATOR
	logger = sharedLogger;
#endif
	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		fsioLogics[i] = NULL;
	}

#if EFI_FUEL_PUMP || defined(__DOXYGEN__)
	fuelPumpLogic = sysPool.parseExpression(FUEL_PUMP_LOGIC);
#endif /* EFI_FUEL_PUMP */

	acRelayLogic = sysPool.parseExpression(AC_RELAY_LOGIC);
	radiatorFanLogic = sysPool.parseExpression(FAN_CONTROL_LOGIC);

	alternatorLogic = sysPool.parseExpression(ALTERNATOR_LOGIC);
	
#if EFI_MAIN_RELAY_CONTROL || defined(__DOXYGEN__)
	if (boardConfiguration->mainRelayPin != GPIO_UNASSIGNED)
		mainRelayLogic = sysPool.parseExpression(MAIN_RELAY_LOGIC);
#endif /* EFI_MAIN_RELAY_CONTROL */

#if EFI_PROD_CODE || defined(__DOXYGEN__)
	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		brain_pin_e brainPin = boardConfiguration->fsioPins[i];

		if (brainPin != GPIO_UNASSIGNED) {
			int frequency = boardConfiguration->fsioFrequency[i];
			if (frequency == 0) {
				enginePins.fsioOutputs[i].initPin(getGpioPinName(i), boardConfiguration->fsioPins[i], &defa);
			} else {
				startSimplePwmExt(&fsioPwm[i], "FSIOpwm", brainPin, &enginePins.fsioOutputs[i], frequency, 0.5f, applyPinState);
			}
		}
	}

	for (int i = 0; i < FSIO_COMMAND_COUNT; i++) {
		brain_pin_e inputPin = boardConfiguration->fsioDigitalInputs[i];

		if (inputPin != GPIO_UNASSIGNED) {
			efiSetPadMode("FSIO input", inputPin, getInputMode(engineConfiguration->fsioInputModes[i]));
		}
	}

	addConsoleActionSS("set_fsio_pid_output_pin", (VoidCharPtrCharPtr) setFsioPidOutputPin);
	addConsoleActionSS("set_fsio_output_pin", (VoidCharPtrCharPtr) setFsioOutputPin);
	addConsoleActionII("set_fsio_output_frequency", (VoidIntInt) setFsioFrequency);
	addConsoleActionSS("set_fsio_input_pin", (VoidCharPtrCharPtr) setFsioInputPin);

#endif /* EFI_PROD_CODE */

#if EFI_PROD_CODE || EFI_SIMULATOR
	addConsoleActionSS("set_rpn_expression", (VoidCharPtrCharPtr) setFsioExpression);
	addConsoleActionFF("set_fsio_setting", setFsioSetting);
	addConsoleAction("fsioinfo", showFsioInfo);
	addConsoleActionS("rpn_eval", (VoidCharPtr) rpnEval);
#endif /* EFI_PROD_CODE || EFI_SIMULATOR */

	fsioTable1.init(config->fsioTable1, config->fsioTable1LoadBins,
			config->fsioTable1RpmBins);
	fsioTable2.init(config->fsioTable2, config->fsioTable2LoadBins,
			config->fsioTable2RpmBins);
	fsioTable3.init(config->fsioTable3, config->fsioTable3LoadBins,
			config->fsioTable3RpmBins);
	fsioTable4.init(config->fsioTable4, config->fsioTable4LoadBins,
			config->fsioTable4RpmBins);

}


#endif /* EFI_FSIO */
