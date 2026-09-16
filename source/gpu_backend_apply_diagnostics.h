// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The post-apply curve diagnostics: what the VF table actually looks like once
// the batch has landed, next to what was asked for.
//
// Split out of gpu_backend_apply.cpp for the file-size guidance; included from
// the exact position it occupied so the amalgamated ordering is unchanged.
//
// This is not decoration.  The 2026-09-13 incident was only reconstructable
// afterwards by summing 51 per-point readback lines by hand, which is why the
// tail bookends and the OK/OFF summary exist as single lines: a post-mortem
// needs to answer "where did the curve actually end up" without the log having
// to carry every point.  The voltage-drift check is here for the same reason --
// voltage is immutable on an NVIDIA VF table, so any movement means a driver
// state shift or an unstable read, and knowing which is what tells a crash
// report apart from a bad offset.
//
// It also produces the flatten tallies the outcome severity consumes, so it
// runs on every verified apply rather than only when something looks wrong.
#ifndef GREEN_CURVE_GPU_BACKEND_APPLY_DIAGNOSTICS_H
#define GREEN_CURVE_GPU_BACKEND_APPLY_DIAGNOSTICS_H

static void apply_log_post_apply_curve_diagnostics(
    const DesiredSettings* verifyDesiredIn, const bool* originalCurvePopulated,
    const unsigned int* originalCurveVoltUv, const bool* lockedTailMask,
    bool hasLock, unsigned int lockMhz, int& flattenApplied, int& flattenFailed) {
    const DesiredSettings& verifyDesired = *verifyDesiredIn;
                // Log voltage consistency diagnostic: compare post-apply voltage
                // against the pre-apply snapshot to detect unexpected voltage drift.
                // Voltage is inherently immutable on NVIDIA VF tables, so any change
                // indicates a driver state shift or NVAPI read instability.
                {
                    const unsigned int VOLTAGE_DRIFT_TOLERANCE_uV = 10000; // 10 mV
                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        if (!originalCurvePopulated[ci]) continue;
                        unsigned int originalUv = originalCurveVoltUv[ci];
                        unsigned int currentUv = g_app.curve[ci].volt_uV;
                        if (originalUv == 0 || currentUv == 0) continue;
                        unsigned int driftUv = (originalUv > currentUv)
                            ? (originalUv - currentUv) : (currentUv - originalUv);
                        if (driftUv > VOLTAGE_DRIFT_TOLERANCE_uV) {
                            debug_log("voltage consistency: point %d drifted by %u uV (original=%u uV current=%u uV)\n",
                                ci, driftUv, originalUv, currentUv);
                        }
                    }
                }
                // Post-apply curve state dump: log all points with target vs actual
                // to detect weird shifts that differ from the intended VF curve shape.
                // Also always log first/last tail point and any tail drift > 2 MHz.
                {
                    int tailOff = 0, tailOK = 0, nonTailOff = 0, nonTailOK = 0;
                    int firstTail = -1, lastTail = -1;
                    unsigned int firstTailActual = 0, firstTailTarget = 0;
                    unsigned int lastTailActual = 0, lastTailTarget = 0;
                    for (int ci = 0; ci < VF_NUM_POINTS; ci++) {
                        if (!verifyDesired.hasCurvePoint[ci]) continue;
                        if (g_app.curve[ci].freq_kHz == 0) continue;
                        unsigned int actualMHz = displayed_curve_mhz(g_app.curve[ci].freq_kHz);
                        unsigned int targetMHz = (lockedTailMask[ci] && lockMhz > 0) ? lockMhz : verifyDesired.curvePointMHz[ci];
                        unsigned int delta = actualMHz > targetMHz ? actualMHz - targetMHz : targetMHz - actualMHz;
                        int tol = (int)curve_point_verify_tolerance_mhz(ci);
                        bool isTail = (lockedTailMask[ci] && lockMhz > 0);
                        if (delta > (unsigned int)tol) {
                            debug_log("post-apply curve: ci=%d actual=%u target=%u delta=%u tol=%d freqOffs=%d %s\n",
                                ci, actualMHz, targetMHz, delta, tol, g_app.freqOffsets[ci],
                                isTail ? "TAIL" : "BOOST");
                            if (isTail) tailOff++; else nonTailOff++;
                        } else if (isTail && delta > 2) {
                            tailOK++;
                            debug_log("post-apply tail: ci=%d actual=%u target=%u delta=%u tol=%d freqOffs=%d\n",
                                ci, actualMHz, targetMHz, delta, tol, g_app.freqOffsets[ci]);
                        } else if (isTail) {
                            tailOK++;
                        } else {
                            nonTailOK++;
                        }
                        if (isTail) {
                            if (firstTail < 0) {
                                firstTail = ci;
                                firstTailActual = actualMHz;
                                firstTailTarget = targetMHz;
                            }
                            lastTail = ci;
                            lastTailActual = actualMHz;
                            lastTailTarget = targetMHz;
                        }
                    }
                    debug_log("post-apply tail bookends: first=ci%d actual=%u target=%u last=ci%d actual=%u target=%u\n",
                        firstTail, firstTailActual, firstTailTarget,
                        lastTail, lastTailActual, lastTailTarget);
                    if (tailOff > 0 || nonTailOff > 0) {
                        debug_log("post-apply curve summary: tail=%dOK+%dOFF boost=%dOK+%dOFF\n",
                            tailOK, tailOff, nonTailOK, nonTailOff);
                    }
                    if (hasLock && lockMhz > 0) {
                        flattenApplied = tailOK;
                        flattenFailed = tailOff;
                    }
                }
}

#endif  // GREEN_CURVE_GPU_BACKEND_APPLY_DIAGNOSTICS_H
