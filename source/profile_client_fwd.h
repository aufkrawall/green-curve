// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
#pragma once
// Client/profile declarations for the Windows amalgam; types precede this include.
// Profile I/O
static bool load_desired_settings_from_ini(const char* path, DesiredSettings* desired, char* err, size_t errSize);
static bool load_profile_from_config(const char* path, int slot, DesiredSettings* desired, char* err, size_t errSize, ProfileReadMode mode = PROFILE_READ_FOR_EDITOR);
static bool save_profile_to_config(const char* path, int slot, const DesiredSettings* desired, char* err, size_t errSize);
static bool clear_profile_from_config(const char* path, int slot, char* err, size_t errSize);
static bool is_profile_slot_saved(const char* path, int slot);
static void refresh_profile_controls_from_config();
static void update_share_all_users_check_state();
static void gui_profile_tools_service_changed();
static void gui_profile_tools_observe_service(const ServiceResponse* response);
static void gui_profile_capture_service_changed();
#ifdef GREEN_CURVE_SERVICE_BINARY
static constexpr bool gui_profile_capture_active() { return false; }
static constexpr bool gui_profile_transfer_active() { return false; }
static constexpr void gui_profile_capture_begin(int) {}
#else
static bool gui_profile_capture_active();
static bool gui_profile_transfer_active();
static void gui_profile_capture_begin(int preferredSlot);
#endif
static void gui_profile_tools_append_menu(HMENU menu);
static void migrate_legacy_config_if_needed(const char* path);
static void merge_desired_settings(DesiredSettings* base, const DesiredSettings* override);
static bool desired_has_any_action(const DesiredSettings* desired);
static int desired_curve_point_count(const DesiredSettings* desired);
static bool desired_updates_curve_or_gpu_offset_state(const DesiredSettings* desired);
static bool desired_has_nonfan_apply_fields(const DesiredSettings* desired);
static bool desired_is_fan_only_apply_request(const DesiredSettings* desired);
static bool desired_settings_match_active_service_intent(const DesiredSettings* profile, const DesiredSettings* active, char* detail, size_t detailSize, bool relaxedOwnershipRead = false);
static bool capture_gui_apply_settings(DesiredSettings* desired, OcApplyBaseline* baselineOut, char* err, size_t errSize);
static void capture_applied_curve_baseline(const DesiredSettings* desired);
static void set_profile_status_text(const char* fmt, ...);
static void update_profile_state_label();
static void update_profile_action_buttons();
static void update_background_service_controls();
static bool maybe_confirm_profile_load_replace(int slot);
static void sync_applied_profile_from_service_metadata();
#ifndef GREEN_CURVE_SERVICE_BINARY
static bool g_appliedProfileSyncVerifyDiskStamp = true;
struct AppliedProfileSyncStampScope {
    bool previous;
    explicit AppliedProfileSyncStampScope(bool verifyDiskStamp)
        : previous(g_appliedProfileSyncVerifyDiskStamp) {
        g_appliedProfileSyncVerifyDiskStamp = verifyDiskStamp;
    }
    ~AppliedProfileSyncStampScope() {
        g_appliedProfileSyncVerifyDiskStamp = previous;
    }
};
#endif
static void maybe_load_app_launch_profile_to_gui();
