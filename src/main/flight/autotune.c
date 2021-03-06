/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#ifdef AUTOTUNE

#include "common/axis.h"
#include "common/maths.h"

#include "drivers/system.h"

#include "flight/flight.h"

extern int16_t debug[4];

/*
 * Authors
 * Brad Quick - initial implementation in BradWii
 * Dominic Clifton - baseflight port & cleanup.
 *
 * Autotune in BradWii thread here: http://www.rcgroups.com/forums/showthread.php?t=1922423
 *
 * We start with two input parameters. The first is our target angle. By default it's 20 degrees, so we will bank to 20 degrees,
 * see how the system reacts, then bank to -20 degrees and evaluate again. We repeat this over and over. The second input is
 * how much oscillation we can tolerate. This can range from 2 degrees to 5 or more degrees. This defaults to 4 degrees. The
 * higher this value is, the more agressive the result of the tuning will be.
 *
 * First, we turn the I gain down to zero so that we don't have to try to figure out how much overshoot is caused by the I term
 * vs. the P term.
 *
 * Then, we move to the target of 20 degrees and analyze the results. Our goal is to have no overshoot and to keep the bounce
 * back within the 4 degrees. By working to get zero overshoot, we can isolate the effects of the P and D terms. If we don't
 * overshoot, then the P term never works in the opposite direction, so we know that any bounce we get is caused by the D term.
 *
 * If we overshoot the target 20 degrees, then we know our P term is too high or our D term is too low. We can determine
 * which one to change by looking at how much bounce back (or the amplitude of the oscillation) we get. If it bounces back
 * more than the 4 degrees, then our D is already too high, so we can't increase it, so instead we decrease P.
 *
 * If we undershoot, then either our P is too low or our D is too high. Again, we can determine which to change by looking at
 * how much bounce we get.
 *
 * Once we have the P and D terms set, we then set the I term by repeating the same test above and measuring the overshoot.
 * If our maximum oscillation is set to 4 degrees, then we keep increasing the I until we get an overshoot of 2 degrees, so
 * that our oscillations are now centered around our target (in theory).
 *
 * In the BradWii software, it alternates between doing the P and D step and doing the I step so you can quit whenever you
 * want without having to tell it specifically to do the I term. The sequence is actually P&D, P&D, I, P&D, P&D, I...
 *
 * Note: The 4 degrees mentioned above is the value of AUTOTUNE_MAX_OSCILLATION.  In the BradWii code at the time of writing
 * the default value was 1.0f instead of 4.0f.
 *
 * To adjust how aggressive the tuning is, adjust the AUTOTUNEMAXOSCILLATION value.  A larger value will result in more
 * aggressive tuning. A lower value will result in softer tuning. It will rock back and forth between -AUTOTUNE_TARGET_ANGLE
 * and AUTOTUNE_TARGET_ANGLE degrees
 * AUTOTUNE_D_MULTIPLIER is a multiplier that puts in a little extra D when autotuning is done. This helps damp  the wobbles
 * after a quick angle change.
 * Always autotune on a full battery.
 */

#define AUTOTUNE_MAX_OSCILLATION_ANGLE 2.0f
#define AUTOTUNE_TARGET_ANGLE 20.0f
#define AUTOTUNE_D_MULTIPLIER 1.2f
#define AUTOTUNE_SETTLING_DELAY_MS 250  // 1/4 of a second.

#define AUTOTUNE_INCREASE_MULTIPLIER 1.03f
#define AUTOTUNE_DECREASE_MULTIPLIER 0.97f

#define AUTOTUNE_MINIMUM_I_VALUE 0.001f

#define YAW_GAIN_MULTIPLIER 2.0f


typedef enum {
    PHASE_IDLE = 0,
    PHASE_TUNE_ROLL,
    PHASE_TUNE_PITCH,
    PHASE_SAVE_OR_RESTORE_PIDS,
} autotunePhase_e;

static const pidIndex_e angleIndexToPidIndexMap[] = {
    PIDROLL,
    PIDPITCH
};

#define AUTOTUNE_PHASE_MAX PHASE_SAVE_OR_RESTORE_PIDS
#define AUTOTUNE_PHASE_COUNT (AUTOTUNE_PHASE_MAX + 1)

#define FIRST_TUNE_PHASE PHASE_TUNE_ROLL

static pidProfile_t *pidProfile;
static pidProfile_t pidBackup;
static uint8_t pidController;
static uint8_t pidIndex;
static bool rising;
static uint8_t cycleCount; // TODO can we replace this with an enum to improve readability.
static uint32_t timeoutAt;
static angle_index_t autoTuneAngleIndex;
static autotunePhase_e phase = PHASE_IDLE;
static autotunePhase_e nextPhase = FIRST_TUNE_PHASE;

static float targetAngle = 0;
static float targetAngleAtPeak;
static float secondPeakAngle, firstPeakAngle; // deci dgrees, 180 deg = 1800

typedef struct fp_pid {
    float p;
    float i;
    float d;
} fp_pid_t;

static fp_pid_t pid;

// These are used to convert between multiwii integer values to the float pid values used by the autotuner.
#define MULTIWII_P_MULTIPLIER 10.0f    // e.g 0.4 * 10 = 40
#define MULTIWII_I_MULTIPLIER 1000.0f  // e.g 0.030 * 1000 = 30
// Note there is no D multiplier since D values are stored and used AS-IS

bool isAutotuneIdle(void)
{
    return phase == PHASE_IDLE;
}

static void startNewCycle(void)
{
    rising = !rising;
    secondPeakAngle = firstPeakAngle = 0;
}

static void updatePidIndex(void)
{
    pidIndex = angleIndexToPidIndexMap[autoTuneAngleIndex];
}

static void updateTargetAngle(void)
{
    if (rising) {
        targetAngle = AUTOTUNE_TARGET_ANGLE;
    }
    else  {
        targetAngle = -AUTOTUNE_TARGET_ANGLE;
    }

#if 0
    debug[2] = DEGREES_TO_DECIDEGREES(targetAngle);
#endif
}

float autotune(angle_index_t angleIndex, const rollAndPitchInclination_t *inclination, float errorAngle)
{
    float currentAngle;

    if (!(phase == PHASE_TUNE_ROLL || phase == PHASE_TUNE_PITCH) || autoTuneAngleIndex != angleIndex) {
        return errorAngle;
    }

    if (pidController == 2) {
        // TODO support new baseflight pid controller
        return errorAngle;
    }

    debug[0] = 0;

#if 0
    debug[1] = inclination->rawAngles[angleIndex];
#endif

    updatePidIndex();

    if (rising) {
        currentAngle = DECIDEGREES_TO_DEGREES(inclination->raw[angleIndex]);
    } else {
        targetAngle = -targetAngle;
        currentAngle = DECIDEGREES_TO_DEGREES(-inclination->raw[angleIndex]);
    }

#if 1
    debug[1] = DEGREES_TO_DECIDEGREES(currentAngle);
    debug[2] = DEGREES_TO_DECIDEGREES(targetAngle);
#endif

    if (firstPeakAngle == 0) {
        // The peak will be when our angular velocity is negative.  To be sure we are in the right place,
        // we also check to make sure our angle position is greater than zero.

        if (currentAngle > secondPeakAngle) {
            // we are still going up
            secondPeakAngle = currentAngle;
            targetAngleAtPeak = targetAngle;

            debug[3] = DEGREES_TO_DECIDEGREES(secondPeakAngle);

        } else if (secondPeakAngle > 0) {
            if (cycleCount == 0) {
                // when checking the I value, we would like to overshoot the target position by half of the max oscillation.
                if (currentAngle - targetAngle < AUTOTUNE_MAX_OSCILLATION_ANGLE / 2) {
                    pid.i *= AUTOTUNE_INCREASE_MULTIPLIER;
                } else {
                    pid.i *= AUTOTUNE_DECREASE_MULTIPLIER;
                    if (pid.i < AUTOTUNE_MINIMUM_I_VALUE) {
                        pid.i = AUTOTUNE_MINIMUM_I_VALUE;
                    }
                }

                // go back to checking P and D
                cycleCount = 1;
                pidProfile->I8[pidIndex] = 0;
                startNewCycle();
            } else {
                // we are checking P and D values
                // set up to look for the 2nd peak
                firstPeakAngle = currentAngle;
                timeoutAt = millis() + AUTOTUNE_SETTLING_DELAY_MS;
            }
        }
    } else {
        // we saw the first peak. looking for the second

        if (currentAngle < firstPeakAngle) {
            firstPeakAngle = currentAngle;
            debug[3] = DEGREES_TO_DECIDEGREES(firstPeakAngle);
        }

        float oscillationAmplitude = secondPeakAngle - firstPeakAngle;

        uint32_t now = millis();
        int32_t signedDiff = now - timeoutAt;
        bool timedOut = signedDiff >= 0L;

        // stop looking for the 2nd peak if we time out or if we change direction again after moving by more than half the maximum oscillation
        if (timedOut || (oscillationAmplitude > AUTOTUNE_MAX_OSCILLATION_ANGLE / 2 && currentAngle > firstPeakAngle)) {
            // analyze the data
            // Our goal is to have zero overshoot and to have AUTOTUNEMAXOSCILLATION amplitude

            if (secondPeakAngle > targetAngleAtPeak) {
                // overshot
                debug[0] = 1;

#ifdef PREFER_HIGH_GAIN_SOLUTION
                if (oscillationAmplitude > AUTOTUNE_MAX_OSCILLATION_ANGLE) {
                    // we have too much oscillation, so we can't increase D, so decrease P
                    pid.p *= AUTOTUNE_DECREASE_MULTIPLIER;
                } else {
                    // we don't have too much oscillation, so we can increase D
                    pid.d *= AUTOTUNE_INCREASE_MULTIPLIER;
                }
#else
				pid.p *= AUTOTUNE_DECREASE_MULTIPLIER;
				pid.d *= AUTOTUNE_INCREASE_MULTIPLIER;
#endif
            } else {
                // undershot
                debug[0] = 2;

                if (oscillationAmplitude > AUTOTUNE_MAX_OSCILLATION_ANGLE) {
                    // we have too much oscillation
                    pid.d *= AUTOTUNE_DECREASE_MULTIPLIER;
                } else {
                    // we don't have too much oscillation
                    pid.p *= AUTOTUNE_INCREASE_MULTIPLIER;
                }
            }

            pidProfile->P8[pidIndex] = pid.p * MULTIWII_P_MULTIPLIER;
            pidProfile->D8[pidIndex] = pid.d;

            // switch to the other direction and start a new cycle
            startNewCycle();

            if (++cycleCount == 3) {
                // switch to testing I value
                cycleCount = 0;

                pidProfile->I8[pidIndex] = pid.i  * MULTIWII_I_MULTIPLIER;
            }
        }
    }

    if (angleIndex == AI_ROLL) {
        debug[0] += 100;
    }

    updateTargetAngle();

    return targetAngle - DECIDEGREES_TO_DEGREES(inclination->raw[angleIndex]);
}

void autotuneReset(void)
{
    targetAngle = 0;
    phase = PHASE_IDLE;
    nextPhase = FIRST_TUNE_PHASE;
}

void backupPids(pidProfile_t *pidProfileToTune)
{
    memcpy(&pidBackup, pidProfileToTune, sizeof(pidBackup));
}

void restorePids(pidProfile_t *pidProfileToTune)
{
    memcpy(pidProfileToTune, &pidBackup, sizeof(pidBackup));
}

void autotuneBeginNextPhase(pidProfile_t *pidProfileToTune, uint8_t pidControllerInUse)
{
    phase = nextPhase;

    if (phase == PHASE_SAVE_OR_RESTORE_PIDS) {
        restorePids(pidProfileToTune);
        return;
    }

    if (phase == FIRST_TUNE_PHASE) {
        backupPids(pidProfileToTune);
    }

    if (phase == PHASE_TUNE_ROLL) {
        autoTuneAngleIndex = AI_ROLL;
    } if (phase == PHASE_TUNE_PITCH) {
        autoTuneAngleIndex = AI_PITCH;
    }

    rising = true;
    cycleCount = 1;
    secondPeakAngle = firstPeakAngle = 0;

    pidProfile = pidProfileToTune;
    pidController = pidControllerInUse;

    updatePidIndex();
    updateTargetAngle();

    pid.p = pidProfile->P8[pidIndex] / MULTIWII_P_MULTIPLIER;
    pid.i = pidProfile->I8[pidIndex] / MULTIWII_I_MULTIPLIER;
    // divide by D multiplier to get our working value.  We'll multiply by D multiplier when we are done.
    pid.d = pidProfile->D8[pidIndex] * (1.0f / AUTOTUNE_D_MULTIPLIER);

    pidProfile->D8[pidIndex] = pid.d;
    pidProfile->I8[pidIndex] = 0;
}

void autotuneEndPhase(void)
{
    if (phase == PHASE_TUNE_ROLL || phase == PHASE_TUNE_PITCH) {

        // we leave P alone, just update I and D

        pidProfile->I8[pidIndex] = pid.i * MULTIWII_I_MULTIPLIER;

        // multiply by D multiplier.  The best D is usually a little higher than what the algroithm produces.
        pidProfile->D8[pidIndex] = (pid.d * AUTOTUNE_D_MULTIPLIER);

        pidProfile->P8[PIDYAW] = pidProfile->P8[PIDROLL] * YAW_GAIN_MULTIPLIER;
        pidProfile->I8[PIDYAW] = pidProfile->I8[PIDROLL];
        pidProfile->D8[PIDYAW] = pidProfile->D8[PIDROLL];
    }

    if (phase == AUTOTUNE_PHASE_MAX) {
        phase = PHASE_IDLE;
        nextPhase = FIRST_TUNE_PHASE;
    } else {
        nextPhase++;
    }
}

bool havePidsBeenUpdatedByAutotune(void)
{
    return targetAngle != 0;
}

#endif
