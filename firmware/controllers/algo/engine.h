/**
 * @file	engine.h
 *
 * @date May 21, 2014
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#pragma once

#include "globalaccess.h"
#include "engine_state.h"
#include "rpm_calculator.h"
#include "event_registry.h"
#include "table_helper.h"
#include "listener_array.h"
#include "accel_enrichment.h"
#include "trigger_central.h"
#include "local_version_holder.h"

#if EFI_SIGNAL_EXECUTOR_ONE_TIMER
// PROD real firmware uses this implementation
#include "single_timer_executor.h"
#endif /* EFI_SIGNAL_EXECUTOR_ONE_TIMER */
#if EFI_SIGNAL_EXECUTOR_SLEEP
#include "signal_executor_sleep.h"
#endif /* EFI_SIGNAL_EXECUTOR_SLEEP */
#if EFI_UNIT_TEST
#include "global_execution_queue.h"
#endif /* EFI_UNIT_TEST */

#define FAST_CALLBACK_PERIOD_MS 5

class RpmCalculator;

#define MAF_DECODING_CACHE_SIZE 256

#define MAF_DECODING_CACHE_MULT (MAF_DECODING_CACHE_SIZE / 5.0)

/**
 * I am not sure if this needs to be configurable.
 *
 * Also technically the whole feature might be implemented as cranking fuel coefficient curve by TPS.
 */
// todo: not great location for these
#define CLEANUP_MODE_TPS 90
#define STEPPER_PARKING_TPS CLEANUP_MODE_TPS

#define CYCLE_ALTERNATION 2

class IEtbController;

class Engine : public TriggerStateListener {
public:
	explicit Engine(persistent_config_s *config);
	Engine();

	IEtbController *etbControllers[ETB_COUNT];

	cyclic_buffer<int> triggerErrorDetection;

	void OnTriggerStateDecodingError();
	void OnTriggerStateProperState(efitick_t nowNt) override;
	void OnTriggerSyncronization(bool wasSynchronized) override;
	void OnTriggerInvalidIndex(int currentIndex) override;

	void setConfig(persistent_config_s *config);
	injection_mode_e getCurrentInjectionMode(DECLARE_ENGINE_PARAMETER_SIGNATURE);

	LocalVersionHolder versionForConfigurationListeners;
	LocalVersionHolder auxParametersVersion;
	operation_mode_e getOperationMode(DECLARE_ENGINE_PARAMETER_SIGNATURE);

	AuxActor auxValves[AUX_DIGITAL_VALVE_COUNT][2];

#if EFI_UNIT_TEST
	bool needTdcCallback = true;
#endif /* EFI_UNIT_TEST */

	/**
	 * if 2nd TPS is not configured we do not run 2nd ETB
	 */
	int etbActualCount = 0;

	/**
	 * By the way 32-bit value should hold at least 400 hours of events at 6K RPM x 12 events per revolution
	 */
	int globalSparkIdCounter = 0;

	// this is useful at least for real hardware integration testing - maybe a proper solution would be to simply
	// GND input pins instead of leaving them floating
	bool hwTriggerInputEnabled = true;


#if !EFI_PROD_CODE
	float mockMapValue = 0;
	// for historical reasons we have options to mock TPS on different layers :(
	int mockTpsAdcValue = 0;
	float mockTpsValue = NAN;
#endif

	int getGlobalConfigurationVersion(void) const;
	/**
	 * true if a recent configuration change has changed any of the trigger settings which
	 * we have not adjusted for yet
	 */
	bool isTriggerConfigChanged = false;
	LocalVersionHolder triggerVersion;

	// a pointer with interface type would make this code nicer but would carry extra runtime
	// cost to resolve pointer, we use instances as a micro optimization
#if EFI_SIGNAL_EXECUTOR_ONE_TIMER
	SingleTimerExecutor executor;
#endif
#if EFI_SIGNAL_EXECUTOR_SLEEP
	SleepExecutor executor;
#endif
#if EFI_UNIT_TEST
	TestExecutor executor;
#endif

#if EFI_ENGINE_CONTROL
	FuelSchedule injectionEvents;
	IgnitionEventList ignitionEvents;
#endif /* EFI_ENGINE_CONTROL */

	WallFuel wallFuel[INJECTION_PIN_COUNT];
	bool needToStopEngine(efitick_t nowNt) const;
	bool etbAutoTune = false;
	/**
	 * That's the linked list of pending events scheduled in relation to trigger
	 * At the moment we iterate over the whole list while looking for events for specific trigger index
	 * We can make it an array of lists per trigger index, but that would take some RAM and probably not needed yet.
	 */
	AngleBasedEvent *angleBasedEventsHead = nullptr;
	/**
	 * this is based on isEngineChartEnabled and engineSnifferRpmThreshold settings
	 */
	bool isEngineChartEnabled = false;
	/**
	 * this is based on sensorChartMode and sensorSnifferRpmThreshold settings
	 */
	sensor_chart_e sensorChartMode = SC_OFF;
	/**
	 * based on current RPM and isAlternatorControlEnabled setting
	 */
	bool isAlternatorControlEnabled = false;

	bool isCltBroken = false;
	bool slowCallBackWasInvoked = false;

	/**
	 * remote telemetry: if not zero, time to stop flashing 'CALL FROM PIT STOP' light
	 * todo: looks like there is a bug here? 64 bit storage an 32 bit time logic? anyway this feature is mostly a dream at this point
	 */
	efitimems64_t callFromPitStopEndTime = 0;

	/**
	 * This flag indicated a big enough problem that engine control would be
	 * prohibited if this flag is set to true.
	 */
	bool withError = false;

	RpmCalculator rpmCalculator;
	persistent_config_s *config = nullptr;
	/**
	 * we use funny unique name to make sure that compiler is not confused between global variable and class member
	 * todo: this variable is probably a sign of some problem, should we even have it?
	 */
	engine_configuration_s *engineConfigurationPtr = nullptr;

	/**
	 * this is about 'stopengine' command
	 */
	efitick_t stopEngineRequestTimeNt = 0;

	/**
	 * this is needed by getTimeIgnitionSeconds() and checkShutdown()
	 */
	efitick_t ignitionOnTimeNt = 0;

	/**
	 * This counter is incremented every time user adjusts ECU parameters online (either via rusEfi console or other
	 * tuning software)
	 */
	volatile int globalConfigurationVersion = 0;

	/**
	 * always 360 or 720, never zero
	 */
	angle_t engineCycle;

	LoadAccelEnrichment engineLoadAccelEnrichment;
	TpsAccelEnrichment tpsAccelEnrichment;

	TriggerCentral triggerCentral;

	/**
	 * Each individual fuel injection duration for current engine cycle, without wall wetting
	 * including everything including injector lag, both cranking and running
	 * @see getInjectionDuration()
	 */
	floatms_t injectionDuration = 0;

	/**
	 * This one with wall wetting accounted for, used for logging.
	 */
	floatms_t actualLastInjection = 0;

	void periodicFastCallback(DECLARE_ENGINE_PARAMETER_SIGNATURE);
	void periodicSlowCallback(DECLARE_ENGINE_PARAMETER_SIGNATURE);
	void updateSlowSensors(DECLARE_ENGINE_PARAMETER_SIGNATURE);
	void initializeTriggerWaveform(Logging *logger DECLARE_ENGINE_PARAMETER_SUFFIX);

	bool clutchUpState = false;
	bool clutchDownState = false;
	bool brakePedalState = false;

	// todo: extract some helper which would contain boolean state and most recent toggle time?
	bool acSwitchState = false;
	efitimeus_t acSwitchLastChangeTime = 0;

	bool isRunningPwmTest = false;


	FsioState fsioState;

	/**
	 * Are we experiencing knock right now?
	 */
	bool knockNow = false;
	/**
	 * Have we experienced knock since engine was started?
	 */
	bool knockEver = false;
	/**
     * KnockCount is directly proportional to the degrees of ignition
     * advance removed
     */
    int knockCount = 0;

    float knockVolts = 0;

    bool knockDebug = false;

	efitimeus_t timeOfLastKnockEvent = 0;

	/**
	 * are we running any kind of functional test? this affect
	 * some areas
	 */
	bool isTestMode = false;

	void resetEngineSnifferIfInTestMode();

	/**
	 * pre-calculated offset for given sequence index within engine cycle
	 * (not cylinder ID)
	 */
	angle_t ignitionPositionWithinEngineCycle[IGNITION_PIN_COUNT];
	/**
	 * pre-calculated reference to which output pin should be used for
	 * given sequence index within engine cycle
	 * todo: update documentation
	 */
	int ignitionPin[IGNITION_PIN_COUNT];

	// Store current ignition mode for prepareIgnitionPinIndices()
	ignition_mode_e ignitionModeForPinIndices = Force_4_bytes_size_ignition_mode;

	/**
	 * this is invoked each time we register a trigger tooth signal
	 */
	void onTriggerSignalEvent(efitick_t nowNt);
	EngineState engineState;
	SensorsState sensors;
	efitick_t lastTriggerToothEventTimeNt = 0;


	/**
	 * This coefficient translates ADC value directly into voltage adjusted according to
	 * voltage divider configuration with just one multiplication. This is a future (?) performance optimization.
	 */
	float adcToVoltageInputDividerCoefficient = NAN;

	/**
	 * This field is true if we are in 'cylinder cleanup' state right now
	 * see isCylinderCleanupEnabled
	 */
	bool isCylinderCleanupMode = false;

	/**
	 * value of 'triggerShape.getLength()'
	 * pre-calculating this value is a performance optimization
	 */
	uint32_t engineCycleEventCount = 0;

	void preCalculate(DECLARE_ENGINE_PARAMETER_SIGNATURE);

	void watchdog();

	/**
	 * Needed by EFI_MAIN_RELAY_CONTROL to shut down the engine correctly.
	 * This method cancels shutdown if the ignition voltage is detected.
	 */
	void checkShutdown(DECLARE_ENGINE_PARAMETER_SIGNATURE);

	/**
	 * Allows to finish some long-term shutdown procedures (stepper motor parking etc.)
	   Called when the ignition switch is turned off (vBatt is too low).
	   Returns true if some operations are in progress on background.
	 */
	bool isInShutdownMode(DECLARE_ENGINE_PARAMETER_SIGNATURE) const;

	/**
	 * The stepper does not work if the main relay is turned off (it requires +12V).
	 * Needed by the stepper motor code to detect if it works.
	 */
	bool isMainRelayEnabled(DECLARE_ENGINE_PARAMETER_SIGNATURE) const;
	
	/**
	 * Needed by EFI_MAIN_RELAY_CONTROL to handle fuel pump and shutdown timings correctly.
	 * This method returns the number of seconds since the ignition voltage is present.
	 * The return value is float for more FSIO flexibility.
	 */
	float getTimeIgnitionSeconds(void) const;

	monitoring_timestamps_s m;

	void knockLogic(float knockVolts DECLARE_ENGINE_PARAMETER_SUFFIX);
	void printKnockState(void);

private:
	/**
	 * By the way:
	 * 'cranking' means engine is not stopped and the rpm are below crankingRpm
	 * 'running' means RPM are above crankingRpm
	 * 'spinning' means the engine is not stopped
	 */
	bool isSpinning = false;
	void reset();
};

void prepareShapes(DECLARE_ENGINE_PARAMETER_SIGNATURE);
void applyNonPersistentConfiguration(Logging * logger DECLARE_ENGINE_PARAMETER_SUFFIX);
void prepareOutputSignals(DECLARE_ENGINE_PARAMETER_SIGNATURE);

void validateConfiguration(DECLARE_ENGINE_PARAMETER_SIGNATURE);
void doScheduleStopEngine(DECLARE_ENGINE_PARAMETER_SIGNATURE);
