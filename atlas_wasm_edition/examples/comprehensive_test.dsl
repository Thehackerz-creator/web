# ================================================================
# comprehensive_test.dsl — Full compiler test suite
# Tests: IF/ELSE, AND/OR/NOT, timers, WHILE, CASE, STRUCT,
#        FUNCTION_BLOCK, STATE_MACHINE, VAR declarations,
#        edge cases, safety interlocks, nested logic
# ================================================================


# ── SECTION 1: Basic IF / ELSE ───────────────────────────────────

IF switch = ON THEN motor = ON ELSE motor = OFF END

IF temp = OFF THEN fan = OFF END

# Action without ELSE (should trigger missing-ELSE warning with --require-else)
IF pressure > 10 THEN valve = OFF END


# ── SECTION 2: Comparators ───────────────────────────────────────

IF temp >= 50 THEN heater = OFF ELSE heater = ON END
IF temp <= 20 THEN heater = ON  ELSE heater = OFF END
IF level > 90  THEN pump1 = OFF ELSE pump1 = ON  END
IF level < 10  THEN pump1 = ON  ELSE pump1 = OFF END
IF speed != 0  THEN brake = OFF ELSE brake = ON  END
IF flow = 0    THEN alarm = ON  ELSE alarm = OFF END


# ── SECTION 3: Compound conditions (AND / OR / NOT) ──────────────

IF temp >= 50 AND switch = ON THEN fan = ON ELSE fan = OFF END

IF pressure > 8 OR temp > 90 THEN alarm = ON ELSE alarm = OFF END

IF NOT sensor = ON THEN light = ON ELSE light = OFF END

IF temp >= 60 AND pressure <= 5 AND switch = ON THEN
  motor = ON
  fan = ON
ELSE
  motor = OFF
  fan = OFF
END

IF NOT e_stop = ON AND switch = ON THEN
  conveyor = ON
ELSE
  conveyor = OFF
END

# Parenthesised groups
IF (temp > 70 OR pressure > 9) AND switch = ON THEN
  cooling = ON
  alarm = ON
ELSE
  cooling = OFF
  alarm = OFF
END


# ── SECTION 4: Timers ────────────────────────────────────────────

IF switch = ON FOR 5 SECONDS THEN motor = ON ELSE motor = OFF END

IF temp >= 80 FOR 2 MINUTES THEN heater = OFF ELSE heater = ON END

IF vibration = ON FOR 500 MILLISECONDS THEN alarm = ON ELSE alarm = OFF END

# Timer with compound condition
IF temp >= 75 AND switch = ON FOR 10 SECONDS THEN
  cooling = ON
ELSE
  cooling = OFF
END


# ── SECTION 5: Multiple actions per branch ───────────────────────

IF e_stop = ON THEN
  motor    = OFF
  fan      = OFF
  pump1    = OFF
  pump2    = OFF
  valve    = OFF
  conveyor = OFF
  alarm    = ON
  light    = ON
ELSE
  alarm = OFF
END


# ── SECTION 6: Numeric values ────────────────────────────────────

IF speed >= 1500 THEN motor = OFF ELSE motor = ON END
IF temp >= 98.6  THEN cooler = ON  ELSE cooler = OFF END
IF flow <= 0.5   THEN pump1 = ON   ELSE pump1 = OFF END
IF counter = 100 THEN reset_flag = ON ELSE reset_flag = OFF END

# Assigning numeric output
IF mode = 1 THEN setpoint = 75 END
IF mode = 2 THEN setpoint = 50 END


# ── SECTION 7: WHILE loops ───────────────────────────────────────

WHILE system_active = ON DO
  IF temp >= 95 THEN emergency_cool = ON ELSE emergency_cool = OFF END
END

WHILE pump_running = ON DO
  IF flow < 5 THEN low_flow_alarm = ON ELSE low_flow_alarm = OFF END
  IF pressure > 12 THEN overpressure_alarm = ON ELSE overpressure_alarm = OFF END
END

# Nested WHILE
WHILE system_active = ON DO
  WHILE zone_enabled = ON DO
    IF temp >= 80 THEN zone_cool = ON ELSE zone_cool = OFF END
  END
END


# ── SECTION 8: CASE statements ───────────────────────────────────

CASE mode OF
  1: motor = ON;  fan = OFF
  2: motor = ON;  fan = ON
  3: motor = OFF; fan = ON
  DEFAULT: motor = OFF; fan = OFF
END_CASE

CASE speed_level OF
  1: setpoint = 25
  2: setpoint = 50
  3: setpoint = 75
  4: setpoint = 100
  DEFAULT: setpoint = 0
END_CASE


# ── SECTION 9: VAR declarations ──────────────────────────────────

VAR_INPUT
  switch;
  temp;
  pressure;
  level;
  flow;
  speed;
  sensor;
END_VAR

VAR_OUTPUT
  motor;
  fan;
  pump1;
  pump2;
  valve;
  alarm;
  heater;
  cooler;
  light;
  conveyor;
END_VAR

VAR
  mode;
  setpoint;
  counter;
  reset_flag;
  system_active;
END_VAR


# ── SECTION 10: STRUCT declarations ──────────────────────────────

STRUCT PumpConfig
  max_speed;
  min_pressure;
  enabled;
END_STRUCT

STRUCT ZoneControl
  setpoint;
  actual;
  deviation;
  active;
END_STRUCT


# ── SECTION 11: FUNCTION_BLOCK ───────────────────────────────────

FUNCTION_BLOCK MotorController
  VAR_INPUT
    enable;
    speed_ref;
  END_VAR
  VAR_OUTPUT
    running;
    fault;
  END_VAR

  IF enable = ON AND fault = OFF THEN
    running = ON
  ELSE
    running = OFF
  END
END_FUNCTION_BLOCK

FUNCTION_BLOCK SafetyMonitor
  VAR_INPUT
    e_stop;
    overtemp;
    overpressure;
  END_VAR
  VAR_OUTPUT
    safe_state;
    trip_relay;
  END_VAR

  IF e_stop = ON OR overtemp = ON OR overpressure = ON THEN
    safe_state = OFF
    trip_relay = ON
  ELSE
    safe_state = ON
    trip_relay = OFF
  END
END_FUNCTION_BLOCK


# ── SECTION 12: STATE_MACHINE ─────────────────────────────────────

STATE_MACHINE ProcessControl
  STATE Idle
    ENTRY motor = OFF; fan = OFF END
    TRANSITION TO Running IF switch = ON
  END

  STATE Running
    ENTRY motor = ON END
    EXIT  motor = OFF END
    IF temp >= 80 THEN fan = ON ELSE fan = OFF END
    TRANSITION TO Fault IF e_stop = ON
    TRANSITION TO Idle  IF switch = OFF
  END

  STATE Fault
    ENTRY alarm = ON; motor = OFF; fan = OFF END
    TRANSITION TO Idle IF reset_flag = ON AND e_stop = OFF
  END
END_STATE_MACHINE


# ── SECTION 13: ASSERT / contracts ───────────────────────────────

ASSERT temp >= 0
ASSERT pressure >= 0
ASSERT speed >= 0

PRE  switch = ON
POST motor = ON

INVARIANT e_stop = OFF


# ── SECTION 14: Safety interlock (E-STOP covers all outputs) ─────

IF e_stop = ON THEN
  motor    = OFF
  fan      = OFF
  pump1    = OFF
  pump2    = OFF
  valve    = OFF
  heater   = OFF
  cooler   = OFF
  conveyor = OFF
  alarm    = ON
ELSE
  alarm = OFF
END


# ── SECTION 15: Optimizer bait ───────────────────────────────────

# Duplicate condition (should warn)
IF temp >= 50 AND switch = ON THEN fan = ON ELSE fan = OFF END

# Redundant sub-expression in AND (both sides identical — should warn)
IF switch = ON AND switch = ON THEN motor = ON ELSE motor = OFF END

# Dead branch (THEN == ELSE — should be pruned)
IF pressure > 5 THEN alarm = OFF ELSE alarm = OFF END


# ── SECTION 16: String literals (lexer test) ─────────────────────

# These appear in ASSERT messages or comments; lexer must handle them
# "hello world"
# "line with \"escaped\" quotes"
# "path/to/file"


# ── SECTION 17: Edge — deeply chained OR ─────────────────────────

IF s1 = ON OR s2 = ON OR s3 = ON OR s4 = ON OR s5 = ON THEN
  master_alarm = ON
ELSE
  master_alarm = OFF
END


# ── SECTION 18: Edge — variable used as both input and output ─────

# feedback_bit read in condition, written in action → should reclassify as MEMORY
IF feedback_bit = ON THEN feedback_bit = OFF ELSE feedback_bit = ON END


# ── SECTION 19: Edge — float threshold on INT-like variable ──────

IF temp >= 37.5 THEN fever_alarm = ON ELSE fever_alarm = OFF END


# ── SECTION 20: End-of-file with no trailing newline (lexer edge) #
