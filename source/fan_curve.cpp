#include "fan_curve.h"

static int clamp_int(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void sort_enabled_points(FanCurvePoint* points, int count) {
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (points[j].temperatureC < points[i].temperatureC) {
                FanCurvePoint temp = points[i];
                points[i] = points[j];
                points[j] = temp;
            }
        }
    }
}

void fan_curve_set_default(FanCurveConfig* config) {
    if (!config) return;

    memset(config, 0, sizeof(*config));
    config->pollIntervalMs = 1000;
    config->hysteresisC = 2;

    config->points[0] = { true, 30, 20 };
    config->points[1] = { true, 45, 35 };
    config->points[2] = { true, 60, 55 };
    config->points[3] = { true, 72, 72 };
    config->points[4] = { true, 84, 90 };
    config->points[5] = { false, 90, 95 };
    config->points[6] = { false, 95, 100 };
    config->points[7] = { false, 100, 100 };
}

int fan_curve_active_count(const FanCurveConfig* config) {
    if (!config) return 0;

    int count = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (config->points[i].enabled) count++;
    }
    return count;
}

void fan_curve_normalize(FanCurveConfig* config) {
    if (!config) return;

    config->pollIntervalMs = clamp_int(config->pollIntervalMs, 250, 5000);
    config->pollIntervalMs = ((config->pollIntervalMs + 125) / 250) * 250;
    config->hysteresisC = clamp_int(config->hysteresisC, 0, 5);

    FanCurvePoint enabled[FAN_CURVE_MAX_POINTS] = {};
    FanCurvePoint disabled[FAN_CURVE_MAX_POINTS] = {};
    int enabledCount = 0;
    int disabledCount = 0;

    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        config->points[i].temperatureC = clamp_int(config->points[i].temperatureC, 0, 100);
        config->points[i].fanPercent = clamp_int(config->points[i].fanPercent, 0, 100);
        if (config->points[i].enabled) enabled[enabledCount++] = config->points[i];
        else disabled[disabledCount++] = config->points[i];
    }

    if (enabledCount < 2) {
        FanCurveConfig defaults = {};
        fan_curve_set_default(&defaults);
        enabled[0] = defaults.points[0];
        enabled[1] = defaults.points[1];
        enabledCount = 2;
    }

    sort_enabled_points(enabled, enabledCount);

    for (int i = 0; i < enabledCount; i++) {
        config->points[i] = enabled[i];
        config->points[i].enabled = true;
    }
    for (int i = 0; i < disabledCount; i++) {
        config->points[enabledCount + i] = disabled[i];
        config->points[enabledCount + i].enabled = false;
    }
    for (int i = enabledCount + disabledCount; i < FAN_CURVE_MAX_POINTS; i++) {
        config->points[i] = { false, 100, 100 };
    }
}

bool fan_curve_validate(const FanCurveConfig* config, char* err, size_t errSize) {
    if (!config) {
        set_message(err, errSize, "No fan curve config");
        return false;
    }

    if (config->pollIntervalMs < 250 || config->pollIntervalMs > 5000 || (config->pollIntervalMs % 250) != 0) {
        set_message(err, errSize, "Fan curve poll interval must be 0.25s to 5.0s in 0.25s steps");
        return false;
    }
    if (config->hysteresisC < 0 || config->hysteresisC > 5) {
        set_message(err, errSize, "Fan curve hysteresis must be between 0C and 5C");
        return false;
    }

    FanCurvePoint active[FAN_CURVE_MAX_POINTS] = {};
    int activeCount = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        const FanCurvePoint* point = &config->points[i];
        if (point->temperatureC < 0 || point->temperatureC > 100) {
            set_message(err, errSize, "Fan curve temperatures must be between 0C and 100C");
            return false;
        }
        if (point->fanPercent < 0 || point->fanPercent > 100) {
            set_message(err, errSize, "Fan curve percentages must be between 0%% and 100%%");
            return false;
        }
        if (point->enabled) active[activeCount++] = *point;
    }

    if (activeCount < 2) {
        set_message(err, errSize, "Enable at least two fan curve points");
        return false;
    }

    sort_enabled_points(active, activeCount);
    for (int i = 1; i < activeCount; i++) {
        if (active[i].temperatureC <= active[i - 1].temperatureC) {
            set_message(err, errSize, "Enabled fan curve temperatures must be strictly increasing");
            return false;
        }
    }

    return true;
}

int fan_curve_interpolate_percent(const FanCurveConfig* config, int temperatureC) {
    if (!config) return 0;

    FanCurvePoint active[FAN_CURVE_MAX_POINTS] = {};
    int activeCount = 0;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (config->points[i].enabled) active[activeCount++] = config->points[i];
    }

    if (activeCount < 1) return 0;
    sort_enabled_points(active, activeCount);
    if (activeCount == 1) return clamp_int(active[0].fanPercent, 0, 100);

    temperatureC = clamp_int(temperatureC, 0, 100);
    if (temperatureC <= active[0].temperatureC) return clamp_int(active[0].fanPercent, 0, 100);
    if (temperatureC >= active[activeCount - 1].temperatureC) return clamp_int(active[activeCount - 1].fanPercent, 0, 100);

    for (int i = 1; i < activeCount; i++) {
        const FanCurvePoint* left = &active[i - 1];
        const FanCurvePoint* right = &active[i];
        if (temperatureC > right->temperatureC) continue;
        int span = right->temperatureC - left->temperatureC;
        if (span <= 0) return clamp_int(right->fanPercent, 0, 100);
        int offset = temperatureC - left->temperatureC;
        int pct = left->fanPercent + (offset * (right->fanPercent - left->fanPercent) + span / 2) / span;
        return clamp_int(pct, 0, 100);
    }

    return clamp_int(active[activeCount - 1].fanPercent, 0, 100);
}

void fan_curve_format_summary(const FanCurveConfig* config, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;
    if (!config) {
        buffer[0] = 0;
        return;
    }

    StringCchPrintfA(buffer, bufferSize, "%d pts | %.2fs | %dC hyst",
        fan_curve_active_count(config),
        (double)config->pollIntervalMs / 1000.0,
        config->hysteresisC);
}

bool fan_curve_equals(const FanCurveConfig* lhs, const FanCurveConfig* rhs) {
    if (!lhs || !rhs) return false;
    if (lhs->pollIntervalMs != rhs->pollIntervalMs || lhs->hysteresisC != rhs->hysteresisC) return false;
    for (int i = 0; i < FAN_CURVE_MAX_POINTS; i++) {
        if (lhs->points[i].enabled != rhs->points[i].enabled) return false;
        if (lhs->points[i].temperatureC != rhs->points[i].temperatureC) return false;
        if (lhs->points[i].fanPercent != rhs->points[i].fanPercent) return false;
    }
    return true;
}