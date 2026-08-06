# ================================================================
# PLC DSL v2.1 — Mathematically Secure & Functional Logic
# ================================================================

# 1. User Defined Type (UDT)
STRUCT MotorTelemetry
  current
  vibration
  temperature
END_STRUCT

# 2. Function Block with Pre/Post conditions
FUNCTION_BLOCK SafeHeater
  VAR_INPUT
    target_temp
    enable
  END_VAR
  VAR_OUTPUT
    heater_on
  END_VAR
  
  PRE enable = ON
  POST heater_on = ON OR heater_on = OFF
  
  IF target_temp > 50 THEN
    heater_on = ON
  ELSE
    heater_on = OFF
  END
END_FUNCTION_BLOCK

# 3. State Machine for Industrial Mixer
STATE_MACHINE MixerControl
  STATE Idle
    ENTRY mixer_motor = OFF; light = OFF
    TRANSITION TO Running IF start_button = ON
  END
  
  STATE Running
    ENTRY mixer_motor = ON; light = ON
    TRANSITION TO Idle IF stop_button = ON OR timer_done = ON
  END
END_STATE_MACHINE

# 4. Formal Verification Assertions
ASSERT NOT (mixer_motor = ON AND emergency_stop = ON) "Safety Violation: Motor running during E-Stop!"

# 5. Contradiction Detection (will be caught by Safety Engine)
# ASSERT emergency_stop = ON AND emergency_stop = OFF
