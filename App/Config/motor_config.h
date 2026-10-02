#ifndef APP_CONFIG_MOTOR_CONFIG_H
#define APP_CONFIG_MOTOR_CONFIG_H

/* Static configuration for the fitted 5010 motor and MT6835 installation. */
#define MOTOR_POLE_PAIRS 7.0f
#define MOTOR_SPEED_MAX_RPM 8600.0f
#define MOTOR_CURRENT_MAX_A 8.0f
#define MOTOR_PHASE_CURRENT_TRIP_A 10.0f
#define MOTOR_RESISTANCE_OHM 0.12f
#define MOTOR_INDUCTANCE_H 50e-6f
#define MOTOR_FLUX_WB 0.0021f
#define MOTOR_ENCODER_HARMONIC_DEG 0.52f
#define MOTOR_CURRENT_KP 0.1884955592f
#define MOTOR_CURRENT_KI_TS 0.0226194671f
#define MOTOR_CURRENT_ANTI_WINDUP 0.12f
#define MOTOR_ALIGN_VOLTAGE_V 0.6f

/* Low-speed bench candidate; gains are A/rpm and A/(rpm*s). */
#define MOTOR_SPEED_KP 0.1f
#define MOTOR_SPEED_KI 2.0f
#define MOTOR_POSITION_KP 4.0f
/* Experimental speed controller; see App/speed_control.md for measured limits. */
#define MOTOR_SPEED_REFERENCE_WEIGHT 0.8f
#define MOTOR_SPEED_ACCEL_RPM_S_A 4685.833948f
#define MOTOR_OBSERVER_ANGLE_GAIN 75.0f
#define MOTOR_OBSERVER_SPEED_GAIN 312.5f
#define MOTOR_OBSERVER_LOAD_GAIN (-0.555753084f)
#define MOTOR_COGGING_FULL_RPM 50.0f
#define MOTOR_COGGING_OFF_RPM 200.0f

#endif
