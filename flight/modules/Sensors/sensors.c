/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Sensors
 * @brief Acquires sensor data
 * @{
 *
 * @file       sensors.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2015.
 * @brief      Module to handle fetch and preprocessing of sensor data
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref GyroSensor @ref AccelSensor @ref MagSensor
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include <openpilot.h>
#include <pios_sensors.h>
#include <homelocation.h>

#include <magsensor.h>
#include <accelsensor.h>
#include <gyrosensor.h>
#include <barosensor.h>
#include <flightstatus.h>

#include <attitudesettings.h>
#include <revocalibration.h>
#include <auxmagsettings.h>
#include <auxmagsensor.h>
#include <auxmagsupport.h>
#include <accelgyrosettings.h>
#include <revosettings.h>

#include <mathmisc.h>
#include <taskinfo.h>
#include <pios_math.h>
#include <pios_constants.h>
#include <CoordinateConversions.h>
#include <pios_board_info.h>
#include <string.h>

// Private constants
#define STACK_SIZE_BYTES         1000
#define TASK_PRIORITY            (tskIDLE_PRIORITY + 3)

#define MAX_SENSORS_PER_INSTANCE 2
#ifdef PIOS_INCLUDE_WDG
#define RELOAD_WDG()   PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS)
#define REGISTER_WDG() PIOS_WDG_RegisterFlag(PIOS_WDG_SENSORS)
#else
#define RELOAD_WDG()
#define REGISTER_WDG()
#endif

static const TickType_t sensor_period_ticks = ((uint32_t) (1000.0f / PIOS_SENSOR_RATE / (float) portTICK_RATE_MS));
#define AUX_MAG_SKIP ((int) ((((PIOS_SENSOR_RATE < 76) ? 76 : PIOS_SENSOR_RATE) + 74) / 75))  /* (AMS at least 2) 75 is mag ODR output data rate in pios_board.c */

#define AUX_MAG_LOCAL_SUPPORT

// Interval in number of sample to recalculate temp bias
#define TEMP_CALIB_INTERVAL      30

// LPF
#define TEMP_DT_GYRO_ACCEL       (1.0f / PIOS_SENSOR_RATE)
#define TEMP_LPF_FC_GYRO_ACCEL   5.0f
static const float temp_alpha_gyro_accel = LPF_ALPHA(TEMP_DT_GYRO_ACCEL, TEMP_LPF_FC_GYRO_ACCEL);

// Interval in number of sample to recalculate temp bias
#define BARO_TEMP_CALIB_INTERVAL 10

// LPF
#define TEMP_DT_BARO             (1.0f / 120.0f)
#define TEMP_LPF_FC_BARO         5.0f
static const float temp_alpha_baro = TEMP_DT_BARO / (TEMP_DT_BARO + 1.0f / (2.0f * M_PI_F * TEMP_LPF_FC_BARO));

#define ZERO_ROT_ANGLE           0.00001f

// Private types
typedef struct {
    // used to accumulate all samples in a task iteration
    Vector3i32 accum[2];
    int32_t    temperature;
    uint32_t   count;
} sensor_fetch_context;

#define MAX_SENSOR_DATA_SIZE (sizeof(PIOS_SENSORS_3Axis_SensorsWithTemp) + MAX_SENSORS_PER_INSTANCE * sizeof(Vector3i16))
typedef union {
    PIOS_SENSORS_3Axis_SensorsWithTemp sensorSample3Axis;
    PIOS_SENSORS_1Axis_SensorsWithTemp sensorSample1Axis;
} sensor_data;

#define PIOS_INSTRUMENT_MODULE
#include <pios_instrumentation_helper.h>

PERF_DEFINE_COUNTER(counterAccelSamples);
PERF_DEFINE_COUNTER(counterAccelPeriod);
PERF_DEFINE_COUNTER(counterMagPeriod);
PERF_DEFINE_COUNTER(counterBaroPeriod);
PERF_DEFINE_COUNTER(counterSensorPeriod);
PERF_DEFINE_COUNTER(counterSensorResets);

// Private functions
static void SensorsTask(void *parameters);
static void settingsUpdatedCb(UAVObjEvent *objEv);

static void accumulateSamples(sensor_fetch_context *sensor_context, sensor_data *sample);
static void processSamples3d(sensor_fetch_context *sensor_context, const PIOS_SENSORS_Instance *sensor);
static void processSamples1d(PIOS_SENSORS_1Axis_SensorsWithTemp *sample, const PIOS_SENSORS_Instance *sensor);

static void clearContext(sensor_fetch_context *sensor_context);

static void handleAccel(float *samples, float temperature);
static void handleGyro(float *samples, float temperature);
static void handleMag(float *samples, float temperature);
static void handleAuxMag(float *samples);
static void handleBaro(float sample, float temperature);

static void updateAccelTempBias(float temperature);
static void updateGyroTempBias(float temperature);
static void updateBaroTempBias(float temperature);

// Private variables
static sensor_data *source_data;
static xTaskHandle sensorsTaskHandle;
RevoCalibrationData cal;
#ifdef AUX_MAG_LOCAL_SUPPORT
AuxMagSettingsData auxmagcal;
#endif
AccelGyroSettingsData agcal;

// These values are initialized by settings but can be updated by the attitude algorithm
static float mag_bias[3] = { 0, 0, 0 };
static float mag_transform[3][3] = {
    { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }
};
#ifdef AUX_MAG_LOCAL_SUPPORT
static float auxmag_bias[3] = { 0, 0, 0 };
static float auxmag_transform[3][3] = {
    { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }
};
#endif

// Variables used to handle accel/gyro temperature bias
static volatile bool gyro_temp_calibrated  = false;
static volatile bool accel_temp_calibrated = false;

static float accel_temperature  = NAN;
static float gyro_temperature   = NAN;
static float accel_temp_bias[3] = { 0 };
static float gyro_temp_bias[3] = { 0 };
static uint8_t accel_temp_calibration_count = 0;
static uint8_t gyro_temp_calibration_count  = 0;

// The user specified "Rotate virtual attitude relative to board"
static float R[3][3] = {
    { 0 }
};

// Variables used to handle baro temperature bias
static RevoSettingsBaroTempCorrectionPolynomialData baroCorrection;
static RevoSettingsBaroTempCorrectionExtentData baroCorrectionExtent;
static volatile bool baro_temp_correction_enabled;
static float baro_temp_bias   = 0;
static float baro_temperature = NAN;
static uint8_t baro_temp_calibration_count = 0;

// this is set, but not used
// it was intended to be a flag to avoid rotation calculation if the rotation was zero
static int8_t rotate = 0;

/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsInitialize(void)
{
    source_data = (sensor_data *)pios_malloc(MAX_SENSOR_DATA_SIZE);
    GyroSensorInitialize();
    AccelSensorInitialize();
    MagSensorInitialize();
    BaroSensorInitialize();
    RevoCalibrationInitialize();
#ifdef AUX_MAG_LOCAL_SUPPORT
    AuxMagSettingsInitialize();
#endif
    RevoSettingsInitialize();
    AttitudeSettingsInitialize();
    AccelGyroSettingsInitialize();

    rotate = 0;

    RevoSettingsConnectCallback(&settingsUpdatedCb);
    RevoCalibrationConnectCallback(&settingsUpdatedCb);
#ifdef AUX_MAG_LOCAL_SUPPORT
    AuxMagSettingsConnectCallback(&settingsUpdatedCb);
#endif
    AttitudeSettingsConnectCallback(&settingsUpdatedCb);
    AccelGyroSettingsConnectCallback(&settingsUpdatedCb);

    return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsStart(void)
{
    // Start main task
    xTaskCreate(SensorsTask, "Sensors", STACK_SIZE_BYTES / 4, NULL, TASK_PRIORITY, &sensorsTaskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_SENSORS, sensorsTaskHandle);
    REGISTER_WDG();
    return 0;
}

MODULE_INITCALL(SensorsInitialize, SensorsStart);

int32_t accel_test;
int32_t gyro_test;
int32_t mag_test;
// int32_t pressure_test;


/**
 * The sensor task.  This polls the gyros at 500 Hz and pumps that data to
 * stabilization and to the attitude loop
 *
 */

uint32_t sensor_dt_us;
static void SensorsTask(__attribute__((unused)) void *parameters)
{
    portTickType lastSysTime;
    sensor_fetch_context sensor_context;
    bool error = false;
    const PIOS_SENSORS_Instance *sensors_list = PIOS_SENSORS_GetList();
    PIOS_SENSORS_Instance *sensor;
    uint8_t aux_mag_skip = 0;

    AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);
    settingsUpdatedCb(NULL);

    // Performance counters
    PERF_INIT_COUNTER(counterAccelSamples, 0x53000001);
    PERF_INIT_COUNTER(counterAccelPeriod, 0x53000002);
    PERF_INIT_COUNTER(counterMagPeriod, 0x53000003);
    PERF_INIT_COUNTER(counterBaroPeriod, 0x53000004);
    PERF_INIT_COUNTER(counterSensorPeriod, 0x53000005);
    PERF_INIT_COUNTER(counterSensorResets, 0x53000006);

    // Test sensors
    bool sensors_test = true;
    uint8_t count     = 0;
    LL_FOREACH((PIOS_SENSORS_Instance *)sensors_list, sensor) {
        RELOAD_WDG();  // mag tests on I2C have 200+(7x10)ms delay calls in them
        sensors_test &= PIOS_SENSORS_Test(sensor);
        count++;
    }

    PIOS_Assert(count);
    RELOAD_WDG();
    if (!sensors_test) {
        AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
        while (1) {
            vTaskDelay(10);
        }
    }

    // Main task loop
    lastSysTime = xTaskGetTickCount();
    uint32_t reset_counter = 0;

    while (1) {
        // TODO: add timeouts to the sensor reads and set an error if the fail
        if (error) {
            RELOAD_WDG();
            lastSysTime = xTaskGetTickCount();
            vTaskDelayUntil(&lastSysTime, sensor_period_ticks);
            AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
            error = false;
        } else {
            AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);
        }


        // reset the fetch context
        clearContext(&sensor_context);
        aux_mag_skip = (aux_mag_skip + 1) % AUX_MAG_SKIP;
        LL_FOREACH((PIOS_SENSORS_Instance *)sensors_list, sensor) {
            // we will wait on the sensor that's marked as primary( that means the sensor with higher sample rate)
            bool is_primary = (sensor->type & PIOS_SENSORS_TYPE_3AXIS_ACCEL);

            if (sensor->type != PIOS_SENSORS_TYPE_3AXIS_AUXMAG || aux_mag_skip == 0) {
                if (!sensor->driver->is_polled) {
                    const QueueHandle_t queue = PIOS_SENSORS_GetQueue(sensor);
                    while (xQueueReceive(queue,
                                         (void *)source_data,
                                         (is_primary && !sensor_context.count) ? sensor_period_ticks : 0) == pdTRUE) {
                        accumulateSamples(&sensor_context, source_data);
                    }
                    if (sensor_context.count) {
                        processSamples3d(&sensor_context, sensor);
                        clearContext(&sensor_context);
                    } else if (is_primary) {
                        PIOS_SENSOR_Reset(sensor);
                        reset_counter++;
                        PERF_TRACK_VALUE(counterSensorResets, reset_counter);
                        error = true;
                    }
                } else {
                    if (PIOS_SENSORS_Poll(sensor)) {
                        PIOS_SENSOR_Fetch(sensor, (void *)source_data, MAX_SENSORS_PER_INSTANCE);
                        if (sensor->type & PIOS_SENSORS_TYPE_3D) {
                            accumulateSamples(&sensor_context, source_data);
                            processSamples3d(&sensor_context, sensor);
                        } else {
                            processSamples1d(&source_data->sensorSample1Axis, sensor);
                        }
                        clearContext(&sensor_context);
                    }
                }
            }
        }
        PERF_MEASURE_PERIOD(counterSensorPeriod);
        RELOAD_WDG();
        vTaskDelayUntil(&lastSysTime, sensor_period_ticks);
    }
}

static void clearContext(sensor_fetch_context *sensor_context)
{
    // clear the context once it has finished
    for (uint32_t i = 0; i < MAX_SENSORS_PER_INSTANCE; i++) {
        sensor_context->accum[i].x = 0;
        sensor_context->accum[i].y = 0;
        sensor_context->accum[i].z = 0;
    }
    sensor_context->temperature = 0;
    sensor_context->count = 0;
}

static void accumulateSamples(sensor_fetch_context *sensor_context, sensor_data *sample)
{
    for (uint32_t i = 0; (i < MAX_SENSORS_PER_INSTANCE) && (i < sample->sensorSample3Axis.count); i++) {
        sensor_context->accum[i].x += sample->sensorSample3Axis.sample[i].x;
        sensor_context->accum[i].y += sample->sensorSample3Axis.sample[i].y;
        sensor_context->accum[i].z += sample->sensorSample3Axis.sample[i].z;
    }
    sensor_context->temperature += sample->sensorSample3Axis.temperature;
    sensor_context->count++;
}

static void processSamples3d(sensor_fetch_context *sensor_context, const PIOS_SENSORS_Instance *sensor)
{
    float samples[3];
    float temperature;
    float scales[MAX_SENSORS_PER_INSTANCE];

    PIOS_SENSORS_GetScales(sensor, scales, MAX_SENSORS_PER_INSTANCE);
    float inv_count = 1.0f / (float)sensor_context->count;
    if ((sensor->type & PIOS_SENSORS_TYPE_3AXIS_ACCEL) ||
        (sensor->type == PIOS_SENSORS_TYPE_3AXIS_MAG) ||
        (sensor->type == PIOS_SENSORS_TYPE_3AXIS_AUXMAG)) {
        float t = inv_count * scales[0];
        samples[0]  = ((float)sensor_context->accum[0].x * t);
        samples[1]  = ((float)sensor_context->accum[0].y * t);
        samples[2]  = ((float)sensor_context->accum[0].z * t);
        temperature = (float)sensor_context->temperature * inv_count * 0.01f;
        switch (sensor->type) {
        case PIOS_SENSORS_TYPE_3AXIS_MAG:
            handleMag(samples, temperature);
            PERF_MEASURE_PERIOD(counterMagPeriod);
            return;
        case PIOS_SENSORS_TYPE_3AXIS_AUXMAG:
            handleAuxMag(samples);
            PERF_MEASURE_PERIOD(counterMagPeriod);
            return;
        default:
            PERF_TRACK_VALUE(counterAccelSamples, sensor_context->count);
            PERF_MEASURE_PERIOD(counterAccelPeriod);
            handleAccel(samples, temperature);
            break;
        }
    }

    if (sensor->type & PIOS_SENSORS_TYPE_3AXIS_GYRO) {
        uint8_t index = 0;
        if (sensor->type == PIOS_SENSORS_TYPE_3AXIS_GYRO_ACCEL) {
            index = 1;
        }
        float t = inv_count * scales[index];
        samples[0]  = ((float)sensor_context->accum[index].x * t);
        samples[1]  = ((float)sensor_context->accum[index].y * t);
        samples[2]  = ((float)sensor_context->accum[index].z * t);
        temperature = (float)sensor_context->temperature * inv_count * 0.01f;
        handleGyro(samples, temperature);
        return;
    }
}

static void processSamples1d(PIOS_SENSORS_1Axis_SensorsWithTemp *sample, const PIOS_SENSORS_Instance *sensor)
{
    switch (sensor->type) {
    case PIOS_SENSORS_TYPE_1AXIS_BARO:
        PERF_MEASURE_PERIOD(counterBaroPeriod);
        handleBaro(sample->sample, sample->temperature);
        return;

    default:
        PIOS_Assert(0);
    }
}

static void handleAccel(float *samples, float temperature)
{
    AccelSensorData accelSensorData;

    updateAccelTempBias(temperature);
    float accels_out[3] = { (samples[0] - agcal.accel_bias.X) * agcal.accel_scale.X - accel_temp_bias[0],
                            (samples[1] - agcal.accel_bias.Y) * agcal.accel_scale.Y - accel_temp_bias[1],
                            (samples[2] - agcal.accel_bias.Z) * agcal.accel_scale.Z - accel_temp_bias[2] };

    rot_mult(R, accels_out, samples);
    accelSensorData.x = samples[0];
    accelSensorData.y = samples[1];
    accelSensorData.z = samples[2];
    accelSensorData.temperature = temperature;
    AccelSensorSet(&accelSensorData);
}

static void handleGyro(float *samples, float temperature)
{
    GyroSensorData gyroSensorData;

    updateGyroTempBias(temperature);
    float gyros_out[3] = { samples[0] * agcal.gyro_scale.X - agcal.gyro_bias.X - gyro_temp_bias[0],
                           samples[1] * agcal.gyro_scale.Y - agcal.gyro_bias.Y - gyro_temp_bias[1],
                           samples[2] * agcal.gyro_scale.Z - agcal.gyro_bias.Z - gyro_temp_bias[2] };

    rot_mult(R, gyros_out, samples);
    gyroSensorData.temperature = temperature;
    gyroSensorData.x = samples[0];
    gyroSensorData.y = samples[1];
    gyroSensorData.z = samples[2];

    GyroSensorSet(&gyroSensorData);
}

static void handleMag(float *samples, float temperature)
{
    MagSensorData mag;
    float mags[3] = { (float)samples[0] - mag_bias[0],
                      (float)samples[1] - mag_bias[1],
                      (float)samples[2] - mag_bias[2] };

    rot_mult(mag_transform, mags, samples);

    mag.x = samples[0];
    mag.y = samples[1];
    mag.z = samples[2];
    mag.temperature = temperature;

    MagSensorSet(&mag);
}

static void handleAuxMag(float *samples)
{
#ifdef AUX_MAG_LOCAL_SUPPORT
    AuxMagSensorData mag;
    float mags[3] = { (float)samples[0] - auxmag_bias[0],
                      (float)samples[1] - auxmag_bias[1],
                      (float)samples[2] - auxmag_bias[2] };

    rot_mult(auxmag_transform, mags, samples);

    mag.x = samples[0];
    mag.y = samples[1];
    mag.z = samples[2];
    mag.Status = AUXMAGSENSOR_STATUS_OK;

    AuxMagSensorSet(&mag);
#else
    auxmagsupport_publish_samples(samples, AUXMAGSENSOR_STATUS_OK);
#endif
}

static void handleBaro(float sample, float temperature)
{
    updateBaroTempBias(temperature);
    sample -= baro_temp_bias;

    float altitude = 44330.0f * (1.0f - powf((sample) / PIOS_CONST_MKS_STD_ATMOSPHERE_F, (1.0f / 5.255f)));

    if (!isnan(altitude)) {
        BaroSensorData data;
        data.Altitude    = altitude;
        data.Temperature = temperature;
        data.Pressure    = sample;
        // Update the BasoSensor UAVObject
        BaroSensorSet(&data);
    }
}

static void updateAccelTempBias(float temperature)
{
    if (isnan(accel_temperature)) {
        accel_temperature = temperature;
    }
    accel_temperature = temp_alpha_gyro_accel * (temperature - accel_temperature) + accel_temperature;

    if ((accel_temp_calibrated) && !accel_temp_calibration_count) {
        accel_temp_calibration_count = TEMP_CALIB_INTERVAL;
        if (accel_temp_calibrated) {
            float ctemp = boundf(accel_temperature,
                                 agcal.temp_calibrated_extent.max,
                                 agcal.temp_calibrated_extent.min);

            accel_temp_bias[0] = agcal.accel_temp_coeff.X * ctemp;
            accel_temp_bias[1] = agcal.accel_temp_coeff.Y * ctemp;
            accel_temp_bias[2] = agcal.accel_temp_coeff.Z * ctemp;
        }
    }
    accel_temp_calibration_count--;
}

static void updateGyroTempBias(float temperature)
{
    if (isnan(gyro_temperature)) {
        gyro_temperature = temperature;
    }

    gyro_temperature = temp_alpha_gyro_accel * (temperature - gyro_temperature) + gyro_temperature;

    if (gyro_temp_calibrated && !gyro_temp_calibration_count) {
        gyro_temp_calibration_count = TEMP_CALIB_INTERVAL;

        if (gyro_temp_calibrated) {
            float ctemp = boundf(gyro_temperature, agcal.temp_calibrated_extent.max, agcal.temp_calibrated_extent.min);
            gyro_temp_bias[0] = (agcal.gyro_temp_coeff.X + agcal.gyro_temp_coeff.X2 * ctemp) * ctemp;
            gyro_temp_bias[1] = (agcal.gyro_temp_coeff.Y + agcal.gyro_temp_coeff.Y2 * ctemp) * ctemp;
            gyro_temp_bias[2] = (agcal.gyro_temp_coeff.Z + agcal.gyro_temp_coeff.Z2 * ctemp) * ctemp;
        }
    }
    gyro_temp_calibration_count--;
}

static void updateBaroTempBias(float temperature)
{
    if (isnan(baro_temperature)) {
        baro_temperature = temperature;
    }

    baro_temperature = temp_alpha_baro * (temperature - baro_temperature) + baro_temperature;

    if (baro_temp_correction_enabled && !baro_temp_calibration_count) {
        baro_temp_calibration_count = BARO_TEMP_CALIB_INTERVAL;
        // pressure bias = A + B*t + C*t^2 + D * t^3
        // in case the temperature is outside of the calibrated range, uses the nearest extremes
        float ctemp = boundf(baro_temperature, baroCorrectionExtent.max, baroCorrectionExtent.min);
        baro_temp_bias = baroCorrection.a + ((baroCorrection.d * ctemp + baroCorrection.c) * ctemp + baroCorrection.b) * ctemp;
    }
    baro_temp_calibration_count--;
}

/**
 * Locally cache some variables from the AtttitudeSettings object
 */
static void settingsUpdatedCb(__attribute__((unused)) UAVObjEvent *objEv)
{
    RevoCalibrationGet(&cal);
    mag_bias[0] = cal.mag_bias.X;
    mag_bias[1] = cal.mag_bias.Y;
    mag_bias[2] = cal.mag_bias.Z;
#ifdef AUX_MAG_LOCAL_SUPPORT
    AuxMagSettingsGet(&auxmagcal);
    auxmag_bias[0] = auxmagcal.mag_bias.X;
    auxmag_bias[1] = auxmagcal.mag_bias.Y;
    auxmag_bias[2] = auxmagcal.mag_bias.Z;
#endif
    AccelGyroSettingsGet(&agcal);

    accel_temp_calibrated = (agcal.temp_calibrated_extent.max - agcal.temp_calibrated_extent.min > .1f) &&
                            (fabsf(agcal.accel_temp_coeff.X) > 1e-9f || fabsf(agcal.accel_temp_coeff.Y) > 1e-9f || fabsf(agcal.accel_temp_coeff.Z) > 1e-9f);

    gyro_temp_calibrated  = (agcal.temp_calibrated_extent.max - agcal.temp_calibrated_extent.min > .1f) &&
                            (fabsf(agcal.gyro_temp_coeff.X) > 1e-9f || fabsf(agcal.gyro_temp_coeff.Y) > 1e-9f ||
                            fabsf(agcal.gyro_temp_coeff.Z) > 1e-9f || fabsf(agcal.gyro_temp_coeff.Z2) > 1e-9f);


    AttitudeSettingsData attitudeSettings;
    AttitudeSettingsGet(&attitudeSettings);

    // Indicates not to expend cycles on rotation
    if (fabsf(attitudeSettings.BoardRotation.Roll) < ZERO_ROT_ANGLE
        && fabsf(attitudeSettings.BoardRotation.Pitch) < ZERO_ROT_ANGLE &&
        fabsf(attitudeSettings.BoardRotation.Yaw) < ZERO_ROT_ANGLE) {
        rotate = 0;
    } else {
        rotate = 1;
    }

    const float rpy[3] = { attitudeSettings.BoardRotation.Roll,
                           attitudeSettings.BoardRotation.Pitch,
                           attitudeSettings.BoardRotation.Yaw };

    float rotationQuat[4];
    RPY2Quaternion(rpy, rotationQuat);

    if (fabsf(attitudeSettings.BoardLevelTrim.Roll) > ZERO_ROT_ANGLE ||
        fabsf(attitudeSettings.BoardLevelTrim.Pitch) > ZERO_ROT_ANGLE) {
        float trimQuat[4];
        float sumQuat[4];
        rotate = 1;

        const float trimRpy[3] = { attitudeSettings.BoardLevelTrim.Roll, attitudeSettings.BoardLevelTrim.Pitch, 0.0f };
        RPY2Quaternion(trimRpy, trimQuat);

        quat_mult(rotationQuat, trimQuat, sumQuat);
        Quaternion2R(sumQuat, R);
    } else {
        Quaternion2R(rotationQuat, R);
    }
    matrix_mult_3x3f((float(*)[3])RevoCalibrationmag_transformToArray(cal.mag_transform), R, mag_transform);
#ifdef AUX_MAG_LOCAL_SUPPORT
    matrix_mult_3x3f((float(*)[3])AuxMagSettingsmag_transformToArray(auxmagcal.mag_transform), R, auxmag_transform);
#endif

    RevoSettingsBaroTempCorrectionPolynomialGet(&baroCorrection);
    RevoSettingsBaroTempCorrectionExtentGet(&baroCorrectionExtent);
    baro_temp_correction_enabled =
        (baroCorrectionExtent.max - baroCorrectionExtent.min > 0.1f &&
         (fabsf(baroCorrection.a) > 1e-9f ||
          fabsf(baroCorrection.b) > 1e-9f ||
          fabsf(baroCorrection.c) > 1e-9f ||
          fabsf(baroCorrection.d) > 1e-9f));
}
/**
 * @}
 * @}
 */
