# ================================================================
# PLC DSL Program — Industrial Control Logic
# Compatible with: Siemens TIA Portal / CODESYS / Rockwell
# ================================================================

# Rule 1: Motor control with timer delay (5 seconds)
IF switch = ON FOR 5 seconds THEN motor = ON ELSE motor = OFF END

# Rule 2: Fan control with compound AND condition
IF temp >= 50 AND switch = ON THEN fan = ON ELSE fan = OFF END

# Rule 3: Pump control based on level sensor
IF level <= 20 THEN pump1 = ON ELSE pump1 = OFF END

# Rule 4: Lighting control based on light sensor (numeric threshold)
IF light_sensor < 300 THEN light = ON ELSE light = OFF END

# Rule 5: Alarm on high temperature
IF temp >= 80 THEN alarm = ON ELSE alarm = OFF END

# Rule 6: Valve open when pressure is within safe range
IF pressure >= 2 AND pressure <= 10 THEN valve = ON ELSE valve = OFF END 

# Rule 7: Emergency stop — all off if e_stop is triggered
IF e_stop = ON THEN motor = OFF ELSE motor = ON END
