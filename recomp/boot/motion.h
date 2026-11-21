/* Motion sensors, for the minigames (hle_motion.c): gyro aiming (the
 * pointer moved by turning the controller) and gyro steering (the
 * controller tilted like a wheel). Shared by input_win32.c (a controller's
 * sensors, through SDL3) and input_android.c (the handheld's own), which
 * give it readings in the controller's axes: x to the right, y out of its
 * face, z toward the player (along it, toward the near edge) - SDL's. For
 * a handheld held up, the screen is the face.
 *
 * Accelerometer readings are of gravity's push, any unit: at rest they
 * point up. Turning goes by the gyroscope (radians a second, about each
 * axis by the right hand): left and right as turning about the way up,
 * whatever way the controller is held, so a handheld held upright and a
 * controller held flat both aim by turning as you would to look; up and
 * down as turning about the controller's x axis. Tilt goes by the way up
 * alone: steering is how far the right side is down, climbing how far the
 * controller is turned about its x axis, from how it was held (the minigame
 * takes the start as level). */
#pragma once
#include <math.h>

#include "host_settings.h"

#define MOTION_STALE 0.25    /* seconds without readings: none */
#define MOTION_WANTED 1.0    /* seconds the sensors stay on after the game last asked */
#define MOTION_STILL 0.03f   /* rad/s: slower turning is taken as none (the gyro's drift) */
#define MOTION_SETTLE 0.06f  /* seconds: how quickly the way up follows the accelerometer */

static struct {
    float up[3];               /* the way up, in the controller's axes (unit) */
    int haveUp;
    float yaw, pitch;          /* turning since the pointer was last read, right and up */
    double lastReading;        /* when the last gyro or accelerometer reading came */
    double aimAsked, steerAsked; /* when the game last asked */
} sMotion;

/* Whether the sensors should be on: the game asked for them lately, with
 * the setting on. */
static int motion_wanted(double now) {
    return (g_settings.gyro_aim && now - sMotion.aimAsked < MOTION_WANTED) ||
           (g_settings.gyro_steer && now - sMotion.steerAsked < MOTION_WANTED);
}

/* The sensors turned off: what they said is forgotten. */
static void motion_off(void) {
    sMotion.haveUp = 0;
    sMotion.yaw = sMotion.pitch = 0;
    sMotion.lastReading = 0;
}

static void motion_accel(const float a[3], float dt, double now) {
    float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), k;
    int i;
    if (len <= 0) return;
    k = sMotion.haveUp ? 1.0f - expf(-(dt > 0 ? dt : 0) / MOTION_SETTLE) : 1.0f;
    for (i = 0; i < 3; i++) sMotion.up[i] += (a[i] / len - sMotion.up[i]) * k;
    len = sqrtf(sMotion.up[0] * sMotion.up[0] + sMotion.up[1] * sMotion.up[1] + sMotion.up[2] * sMotion.up[2]);
    if (len > 0)
        for (i = 0; i < 3; i++) sMotion.up[i] /= len;
    sMotion.haveUp = 1;
    sMotion.lastReading = now;
}

static void motion_gyro(const float g[3], float dt, double now) {
    float yaw, pitch;
    if (!sMotion.haveUp || dt <= 0 || dt > 0.1f) return;
    /* Left about the way up is positive (the right hand), so right is the other way. */
    yaw = -(g[0] * sMotion.up[0] + g[1] * sMotion.up[1] + g[2] * sMotion.up[2]);
    /* About x: the face turns toward the player, the far edge up. */
    pitch = g[0];
    if (fabsf(yaw) < MOTION_STILL) yaw = 0;
    if (fabsf(pitch) < MOTION_STILL) pitch = 0;
    sMotion.yaw += yaw * dt;
    sMotion.pitch += pitch * dt;
    sMotion.lastReading = now;
}

/* g_host_gyro_read: the turning since the last call, in radians, right and
 * up, times the gyro speed setting; 0 with gyro aiming off or no sensor. */
static int motion_gyro_read(float* yaw, float* pitch, double now) {
    *yaw = *pitch = 0;
    sMotion.aimAsked = now;
    if (!g_settings.gyro_aim || !sMotion.lastReading || now - sMotion.lastReading > MOTION_STALE) {
        sMotion.yaw = sMotion.pitch = 0;
        return 0;
    }
    *yaw = sMotion.yaw * g_settings.gyro_speed;
    *pitch = sMotion.pitch * g_settings.gyro_speed;
    if (g_settings.invert_tilt) *pitch = -*pitch;
    sMotion.yaw = sMotion.pitch = 0;
    return 1;
}

/* g_host_tilt_read: the tilt now, in radians: the right side down
 * (positive) or up, and the turn about x (the far edge up: more positive),
 * level or not; 0 with gyro steering off or no sensor. */
static int motion_tilt_read(float* roll, float* pitch, double now) {
    float x;
    *roll = *pitch = 0;
    sMotion.steerAsked = now;
    if (!g_settings.gyro_steer || !sMotion.haveUp || now - sMotion.lastReading > MOTION_STALE) return 0;
    x = sMotion.up[0] > 1 ? 1 : sMotion.up[0] < -1 ? -1 : sMotion.up[0];
    *roll = asinf(-x);
    *pitch = atan2f(-sMotion.up[2], sMotion.up[1]);
    return 1;
}
