// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT

#ifndef GREEN_CURVE_MAIN_LAYOUT_POLICY_H
#define GREEN_CURVE_MAIN_LAYOUT_POLICY_H

// Pure main-window layout policy.  Inputs and outputs are physical pixels so
// the same code can be tested without creating a window.  Runtime control
// placement is kept in ui_main_layout.cpp.

enum {
    MAIN_LAYOUT_BASE_WIDTH_LOGICAL = 1180,
    MAIN_LAYOUT_MIN_VIEWPORT_WIDTH_LOGICAL = 640,
    MAIN_LAYOUT_MIN_VIEWPORT_HEIGHT_LOGICAL = 480,
    MAIN_LAYOUT_MIN_COLUMNS = 6,
    MAIN_LAYOUT_MAX_COLUMNS = 12,
    MAIN_LAYOUT_COLUMN_WIDTH_LOGICAL = 192,
    MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL = 260,
    MAIN_LAYOUT_GRAPH_PREFERRED_HEIGHT_LOGICAL = 420,
    MAIN_LAYOUT_GRAPH_MAX_HEIGHT_LOGICAL = 520,
};

struct MainLayoutPlan {
    int dpi;
    int viewportWidth;
    int viewportHeight;
    int contentWidth;
    int contentHeight;
    int graphHeight;
    int columns;
    int rowsPerColumn;
    int pointStartY;
    int globalControlsY;
    int buttonsY;
    int profileY;
    int autoY;
    int sharedY;
    int serviceY;
    int hintY;
    int statusY;
    bool horizontalOverflow;
    bool verticalOverflow;
};

struct MainLayoutPointCell {
    int left;
    int top;
    int right;
    int bottom;
};

struct MainLayoutSize {
    int width;
    int height;
};

struct MainLayoutRect {
    int left;
    int top;
    int right;
    int bottom;
};

static inline int main_layout_scale_px(int logicalPixels, int dpi) {
    if (dpi <= 0) dpi = 96;
    return (int)(((long long)logicalPixels * dpi) / 96);
}

static inline int main_layout_max_int(int a, int b) {
    return a > b ? a : b;
}

static inline int main_layout_min_int(int a, int b) {
    return a < b ? a : b;
}

static inline int main_layout_clamp_int(int value, int lo, int hi) {
    return main_layout_max_int(lo, main_layout_min_int(value, hi));
}

static inline MainLayoutPlan main_layout_build_plan(
    int viewportWidth, int viewportHeight, int dpi, int pointCount) {
    MainLayoutPlan plan = {};
    if (dpi <= 0) dpi = 96;
    if (viewportWidth < 1) viewportWidth = 1;
    if (viewportHeight < 1) viewportHeight = 1;
    if (pointCount < 0) pointCount = 0;

    plan.dpi = dpi;
    plan.viewportWidth = viewportWidth;
    plan.viewportHeight = viewportHeight;
    plan.contentWidth = main_layout_max_int(
        viewportWidth, main_layout_scale_px(MAIN_LAYOUT_BASE_WIDTH_LOGICAL, dpi));

    const int sideAllowance = main_layout_scale_px(16, dpi);
    const int columnWidth = main_layout_max_int(
        1, main_layout_scale_px(MAIN_LAYOUT_COLUMN_WIDTH_LOGICAL, dpi));
    int availableColumns = (plan.contentWidth - sideAllowance) / columnWidth;
    int columnCapacity = main_layout_clamp_int(
        availableColumns, MAIN_LAYOUT_MIN_COLUMNS, MAIN_LAYOUT_MAX_COLUMNS);
    plan.rowsPerColumn = pointCount > 0
        ? (pointCount + columnCapacity - 1) / columnCapacity
        : 0;
    plan.columns = plan.rowsPerColumn > 0
        ? (pointCount + plan.rowsPerColumn - 1) / plan.rowsPerColumn
        : 0;

    const int rowHeight = main_layout_scale_px(20, dpi);
    // Everything below the graph, including the final breathing room.  Keep
    // this expressed from the same durable row offsets used by runtime layout.
    const int fixedHeight =
        main_layout_scale_px(20, dpi) + plan.rowsPerColumn * rowHeight +
        main_layout_scale_px(6, dpi) + main_layout_scale_px(56, dpi) +
        main_layout_scale_px(40, dpi) + main_layout_scale_px(34, dpi) +
        main_layout_scale_px(26, dpi) + main_layout_scale_px(26, dpi) +
        main_layout_scale_px(26, dpi) + main_layout_scale_px(40, dpi) +
        main_layout_scale_px(18, dpi) + main_layout_scale_px(12, dpi);
    const int graphMin = main_layout_scale_px(MAIN_LAYOUT_GRAPH_MIN_HEIGHT_LOGICAL, dpi);
    const int graphMax = main_layout_scale_px(MAIN_LAYOUT_GRAPH_MAX_HEIGHT_LOGICAL, dpi);
    plan.graphHeight = main_layout_clamp_int(
        viewportHeight - fixedHeight, graphMin, graphMax);

    plan.pointStartY = plan.graphHeight + main_layout_scale_px(20, dpi);
    plan.globalControlsY = plan.pointStartY +
        plan.rowsPerColumn * rowHeight + main_layout_scale_px(6, dpi);
    plan.buttonsY = plan.globalControlsY + main_layout_scale_px(56, dpi);
    plan.profileY = plan.buttonsY + main_layout_scale_px(40, dpi);
    plan.autoY = plan.profileY + main_layout_scale_px(34, dpi);
    plan.sharedY = plan.autoY + main_layout_scale_px(26, dpi);
    plan.serviceY = plan.sharedY + main_layout_scale_px(26, dpi);
    plan.hintY = plan.serviceY + main_layout_scale_px(26, dpi);
    plan.statusY = plan.hintY + main_layout_scale_px(40, dpi);
    plan.contentHeight = plan.statusY + main_layout_scale_px(18 + 12, dpi);
    plan.horizontalOverflow = plan.contentWidth > viewportWidth;
    plan.verticalOverflow = plan.contentHeight > viewportHeight;
    return plan;
}

static inline int main_layout_preferred_client_height(int contentWidth, int dpi, int pointCount) {
    const int generousHeight = main_layout_scale_px(4096, dpi);
    MainLayoutPlan wide = main_layout_build_plan(contentWidth, generousHeight, dpi, pointCount);
    return wide.contentHeight - wide.graphHeight +
        main_layout_scale_px(MAIN_LAYOUT_GRAPH_PREFERRED_HEIGHT_LOGICAL, dpi);
}

static inline MainLayoutPointCell main_layout_point_cell(
    const MainLayoutPlan& plan, int visibleIndex) {
    MainLayoutPointCell cell = {};
    if (visibleIndex < 0 || plan.rowsPerColumn <= 0) return cell;
    int column = visibleIndex / plan.rowsPerColumn;
    int row = visibleIndex % plan.rowsPerColumn;
    cell.left = main_layout_scale_px(8, plan.dpi) +
        column * main_layout_scale_px(MAIN_LAYOUT_COLUMN_WIDTH_LOGICAL, plan.dpi);
    cell.top = plan.pointStartY + row * main_layout_scale_px(20, plan.dpi);
    cell.right = cell.left +
        main_layout_scale_px(MAIN_LAYOUT_COLUMN_WIDTH_LOGICAL, plan.dpi);
    cell.bottom = cell.top + main_layout_scale_px(20, plan.dpi);
    return cell;
}

static inline MainLayoutSize main_layout_grow_size(
    int currentWidth, int currentHeight,
    int preferredWidth, int preferredHeight,
    int workWidth, int workHeight) {
    MainLayoutSize result = {};
    workWidth = main_layout_max_int(1, workWidth);
    workHeight = main_layout_max_int(1, workHeight);
    result.width = main_layout_min_int(
        main_layout_max_int(1, main_layout_max_int(currentWidth, preferredWidth)),
        workWidth);
    result.height = main_layout_min_int(
        main_layout_max_int(1, main_layout_max_int(currentHeight, preferredHeight)),
        workHeight);
    return result;
}

static inline MainLayoutRect main_layout_center_rect(
    MainLayoutRect bounds, int requestedWidth, int requestedHeight) {
    int boundsWidth = main_layout_max_int(1, bounds.right - bounds.left);
    int boundsHeight = main_layout_max_int(1, bounds.bottom - bounds.top);
    int width = main_layout_min_int(
        main_layout_max_int(1, requestedWidth), boundsWidth);
    int height = main_layout_min_int(
        main_layout_max_int(1, requestedHeight), boundsHeight);
    int left = bounds.left + (boundsWidth - width) / 2;
    int top = bounds.top + (boundsHeight - height) / 2;
    MainLayoutRect result = { left, top, left + width, top + height };
    return result;
}

static inline MainLayoutRect main_layout_resize_around_center(
    MainLayoutRect current, int requestedWidth, int requestedHeight) {
    int centerX = current.left + (current.right - current.left) / 2;
    int centerY = current.top + (current.bottom - current.top) / 2;
    int width = main_layout_max_int(1, requestedWidth);
    int height = main_layout_max_int(1, requestedHeight);
    MainLayoutRect result = {
        centerX - width / 2,
        centerY - height / 2,
        centerX - width / 2 + width,
        centerY - height / 2 + height,
    };
    return result;
}

#endif
