// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Pure policy: how well is an install/service-binary location protected?
//
// The LocalSystem service is registered with the SCM by absolute path and the
// driver-recovery design auto-restarts it, so whoever can replace that binary
// gets SYSTEM code execution (F-SEC-1).  A hardened DACL on the install
// directory alone does not prove this: a non-admin who holds delete/create/
// WRITE_DAC rights on ANY ancestor can substitute the directory or re-ACL it
// from above.  "Direct child of Program Files" was the old proxy for the real
// property; this header states the property itself:
//
//   chainProtected = every existing path component from the FILESYSTEM root
//   down is a plain directory, admin/SYSTEM-owned, and grants no non-admin the
//   rights that could substitute it (delete / delete-child / WRITE_DAC /
//   WRITE_OWNER).  Create-only rights on ancestors are harmless: they cannot
//   displace an existing protected child - but on the LEAF they are not, since
//   a file dropped beside the service binary is a DLL-search-order hijack, so
//   the leaf counts create rights too.  A component that does not exist yet is
//   safe only when the elevated setup will both create AND harden it, which it
//   does for the final component alone: an intermediate directory it creates
//   keeps whatever it inherited from the last existing ancestor.
//
// "From the filesystem root" means the drive root (X:\), never the mount path
// a volume happens to be reachable through.  A volume mounted into a directory
// (C:\mnt\data) has ordinary, renameable directories above it, and they are
// part of the proof; only a true drive root gets the "cannot be renamed or
// deleted" DELETE relaxation.
//
// Everything else is INFORMATION, not a gate: the administrator decides.  The
// classifier's one hard obligation is to err toward warning - unproven is never
// reported as protected, because a false "protected" is the one verdict that
// lets someone install a LocalSystem service into a hijackable folder while
// believing they were told it was safe.  The consent wording names the exact
// risk: anything that can write the folder - other accounts or software running
// as the user - can replace the LocalSystem service and gain SYSTEM rights.
//
// Host-neutral on purpose: the regression harness pins the decision matrix on
// every host; service_path_chain.cpp gathers the facts from Win32.

#ifndef GREEN_CURVE_SERVICE_PATH_CHAIN_POLICY_H
#define GREEN_CURVE_SERVICE_PATH_CHAIN_POLICY_H

// Windows allows far deeper chains; anything past this is refused as unproven
// rather than walked, which keeps the fact tables statically sized.
#define GC_PATH_CHAIN_MAX_COMPONENTS 64
#define GC_PATH_CHAIN_MAX_PATH_CHARS 520

// Facts about one path component (one directory from the volume root down to
// the target).  Gathered by service_path_chain.cpp.  The safe-failing values
// are the DANGEROUS ones: a zeroed struct classifies as not protected.
struct GcPathComponentFacts {
    // False for components that do not exist yet.  Only a contiguous TAIL may
    // be missing, and only its FINAL component is safe by construction (the
    // elevated setup creates it and immediately replaces its DACL); anything
    // above it in that tail is created with inherited permissions.
    bool exists;
    bool is_reparse;
    // A plain directory, not a file.  An existing non-directory component can
    // never carry the install and is treated as unproven.
    bool is_directory;
    // Owner is admin-equivalent (Administrators, SYSTEM, TrustedInstaller, or
    // an account that is a member of the local Administrators group).
    bool owner_admin_trusted;
    // Some non-admin principal holds substitution-capable rights against this
    // component: DELETE (rename it away and plant a replacement), delete-child
    // (remove it regardless of its own DACL), WRITE_DAC / WRITE_OWNER (re-ACL
    // it to grant the above).  Create-only rights do NOT count here: they
    // cannot displace an existing protected child.  On the filesystem root
    // (index 0, always a drive root) a bare DELETE grant is ignored - a drive
    // root can be neither renamed nor deleted - but every other bit counts.
    bool non_admin_danger;
    // Some non-admin principal may CREATE inside this component (add-file /
    // add-subdirectory / generic write).  Harmless on an ancestor, decisive on
    // the leaf: the leaf is the directory the LocalSystem service binary loads
    // its DLLs from, so a non-admin who can drop `version.dll` beside it gets
    // SYSTEM without ever touching the protected binary.
    bool non_admin_create_danger;
    // Some non-admin principal holds dangerous rights that INHERIT to children
    // created beneath this component.  An inherit-only ACE grants nothing on
    // the component itself but flows into whatever is created there, which is
    // exactly what a constructed intermediate would pick up (real default ACLs
    // do this: a drive root hands Authenticated Users inheritable Modify).
    bool non_admin_inherit_danger;
    // Owner and DACL could actually be read.  False => assume dangerous.
    bool facts_complete;
};

struct GcPathVolumeFacts {
    bool is_unc;                // \\server\share spelling
    bool is_remote;             // UNC or a network-mapped drive
    bool has_persistent_acls;   // NTFS/ReFS; false for FAT/exFAT where no
                                // DACL can be applied or verified at all
    bool facts_complete;
};

struct GcPathProtectionFacts {
    GcPathVolumeFacts volume;
    bool under_user_profile;
    bool preflight_mode;   // true during installer preflight where the leaf DACL will be replaced
    GcPathComponentFacts components[GC_PATH_CHAIN_MAX_COMPONENTS];
    int component_count;   // components[0] is the filesystem (drive) root
    bool chain_complete;   // walked all the way to the target, no overflow
};

enum GcPathRiskReason {
    GC_PATH_RISK_NONE = 0,
    GC_PATH_RISK_REMOTE,
    GC_PATH_RISK_NO_PERSISTENT_ACLS,
    GC_PATH_RISK_CHAIN_INCOMPLETE,
    GC_PATH_RISK_VOLUME_UNREADABLE,
    GC_PATH_RISK_COMPONENT_MISSING_FACTS,
    GC_PATH_RISK_COMPONENT_REPARSE,
    GC_PATH_RISK_COMPONENT_NOT_ADMIN_OWNED,
    GC_PATH_RISK_COMPONENT_USER_WRITABLE,
};

// The classification shown to the user.  chain_protected is the only green
// state; every other flag is a reason to inform and (interactively) to require
// an explicit acknowledgment before installing there.
struct GcPathProtection {
    bool chain_protected;
    bool standard_writable;         // fail-safe class: everything not proven
    bool user_profile;
    bool remote;
    bool no_filesystem_permissions;
    GcPathRiskReason reason;
    int first_unsafe_component;     // index into components, -1 when none
};

// Win32-side gathering result: the facts, the derived verdict, and the first
// unsafe component's full path (for the remediation line).  Wide because every
// Win32 path API is.
struct GcPathProtectionReport {
    GcPathProtectionFacts facts;
    GcPathProtection verdict;
    wchar_t firstUnsafeComponent[GC_PATH_CHAIN_MAX_PATH_CHARS];
};

// The risk sentence every consent surface must carry verbatim.  Pinned by the
// build gates so it cannot silently disappear from the UI.
#define GC_PATH_RISK_ESCALATION_SENTENCE "anything that can write this folder - other accounts or software running as you - can replace the LocalSystem background service and gain SYSTEM rights"

#define GC_PATH_PROTECTION_SUMMARY_GREEN \
    "Protected like Program Files: only administrators and SYSTEM can change these folders."
#define GC_PATH_PROTECTION_SUMMARY_UNPROTECTED \
    "Not protected like Program Files: " GC_PATH_RISK_ESCALATION_SENTENCE "."
#define GC_PATH_PROTECTION_NOTE_NO_PERMISSIONS \
    "This drive has no file permissions (for example FAT/exFAT): nothing placed here can be protected."
#define GC_PATH_PROTECTION_NOTE_REMOTE \
    "This is a network location: its files are controlled by the server, and server administrators can replace the background service too."
#define GC_PATH_PROTECTION_NOTE_USER_PROFILE \
    "This is inside a user profile: other accounts on this PC cannot run this copy."
// Shown, never applied: fixing an existing folder's permissions can break other
// software that relies on them, so that is the administrator's call.  Two
// spellings: one for an existing folder at the failing component, one for a
// folder that setup would otherwise create with inherited (user-substitutable)
// permissions - that one is created protected in the same breath.
//
// The exact spelling matters, because this is a line a user pastes:
//   * every path is quote-CLOSED (BEFORE opens the quote, AFTER closes it);
//   * `/setowner` is its OWN icacls invocation - icacls rejects it combined
//     with /grant or /inheritance with "Invalid parameter" (error 87);
//   * every SID:permission argument is quoted, because an unquoted `(OI)(CI)F`
//     is a subexpression to PowerShell, the project's default shell;
//   * `;` separates the invocations, which is what PowerShell wants.
#define GC_PATH_PROTECTION_REMEDY_GRANTS \
    "\" /inheritance:r /grant:r \"*S-1-5-32-544:(OI)(CI)F\" /grant:r \"*S-1-5-18:(OI)(CI)F\"" \
    " /grant:r \"*S-1-5-32-545:(OI)(CI)RX\"; icacls \""
#define GC_PATH_PROTECTION_REMEDY_BEFORE_PATH \
    "To fully protect this location, run in an elevated PowerShell: icacls \""
#define GC_PATH_PROTECTION_REMEDY_AFTER_PATH \
    GC_PATH_PROTECTION_REMEDY_GRANTS
#define GC_PATH_PROTECTION_REMEDY_TAIL_PATH \
    "\" /setowner \"*S-1-5-32-544\""
#define GC_PATH_PROTECTION_REMEDY_CREATE_BEFORE_PATH \
    "To fully protect this location, run in an elevated PowerShell: mkdir \""
#define GC_PATH_PROTECTION_REMEDY_CREATE_MID_PATH \
    "\"; icacls \""
// Both spellings end with AFTER_PATH, the path again, then TAIL_PATH.
#define GC_PATH_PROTECTION_ACKNOWLEDGMENT_LABEL \
    "I understand the risk and want to install here anyway"

static inline void gc_path_protection_classify(const GcPathProtectionFacts* facts,
                                              GcPathProtection* out) {
    if (!out) return;
    GcPathProtection blank = {};
    blank.first_unsafe_component = -1;
    blank.reason = GC_PATH_RISK_NONE;
    if (!facts) {
        // No facts at all is the extreme unproven case: warn.
        blank.standard_writable = true;
        blank.reason = GC_PATH_RISK_CHAIN_INCOMPLETE;
        *out = blank;
        return;
    }

    out->remote = facts->volume.is_unc || facts->volume.is_remote;
    out->no_filesystem_permissions =
        facts->volume.facts_complete && !facts->volume.has_persistent_acls;
    out->user_profile = facts->under_user_profile;
    out->first_unsafe_component = -1;
    out->reason = GC_PATH_RISK_NONE;

    bool chainProtected = false;
    if (out->remote) {
        out->reason = GC_PATH_RISK_REMOTE;
    } else if (!facts->volume.facts_complete) {
        out->reason = GC_PATH_RISK_VOLUME_UNREADABLE;
    } else if (out->no_filesystem_permissions) {
        out->reason = GC_PATH_RISK_NO_PERSISTENT_ACLS;
    } else if (!facts->chain_complete || facts->component_count <= 0) {
        out->reason = GC_PATH_RISK_CHAIN_INCOMPLETE;
    } else {
        chainProtected = true;  // disproved below
        bool seen_missing = false;
        bool seen_existing = false;
        for (int i = 0; i < facts->component_count; i++) {
            const GcPathComponentFacts* component = &facts->components[i];
            if (!component->exists) {
                // Missing components are only safe as a contiguous tail: they
                // are created by the elevated setup beneath the verified
                // chain (see the header comment).
                seen_missing = true;
                continue;
            }
            seen_existing = true;
            if (seen_missing) {
                // A gap (missing parent, existing child) cannot happen in a
                // real walk; treat the chain as unproven rather than trusting
                // an inconsistent fact table.
                chainProtected = false;
                out->reason = GC_PATH_RISK_CHAIN_INCOMPLETE;
                out->first_unsafe_component = i;
                break;
            }
            bool isLeaf = (i == facts->component_count - 1);
            if (!component->facts_complete) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_MISSING_FACTS;
                out->first_unsafe_component = i;
                break;
            }
            if (component->is_reparse) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_REPARSE;
                out->first_unsafe_component = i;
                break;
            }
            if (!component->is_directory) {
                // An existing file where a directory must be: nothing about the
                // install can be proven through it.
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_MISSING_FACTS;
                out->first_unsafe_component = i;
                break;
            }
            if (!component->owner_admin_trusted) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_NOT_ADMIN_OWNED;
                out->first_unsafe_component = i;
                break;
            }
            // The install leaf's own DACL is replaced with the protected one
            // before anything is written into it, so a pre-existing non-admin
            // grant on the LEAF is neutralized by construction ONLY in preflight
            // mode.  At runtime, service startup, and in post-hardening checks,
            // non-admin substitution rights on the leaf directory itself are an
            // active escalation risk.
            bool exemptLeaf = isLeaf && facts->preflight_mode;
            if (component->non_admin_danger && !exemptLeaf) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_USER_WRITABLE;
                out->first_unsafe_component = i;
                break;
            }
            // Create-only rights are harmless on an ancestor and decisive on
            // the leaf: that is the directory the LocalSystem service binary
            // resolves its DLL imports from, so a non-admin who can add a file
            // there owns SYSTEM without touching the hardened binary.
            if (isLeaf && component->non_admin_create_danger && !exemptLeaf) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_USER_WRITABLE;
                out->first_unsafe_component = i;
                break;
            }
        }
        // What setup will BUILD, versus what it will HARDEN.  It creates the
        // whole missing tail but replaces the DACL of the final component
        // only, so:
        //   * a tail of exactly one is fully covered - the created leaf gets a
        //     protected DACL that drops every inherited ACE, which is why the
        //     common "C:\Green Curve under a stock drive root" case is green
        //     rather than a warning the user learns to click through;
        //   * a longer tail leaves intermediates carrying whatever the last
        //     existing ancestor hands down, so inheritable non-admin danger
        //     there lands a renameable parent above a hardened child.
        // Outside preflight nothing is about to be created at all, so a
        // missing component simply means the target is not there: unproven.
        if (chainProtected && facts->component_count > 0) {
            int lastExisting = -1;
            int firstMissing = -1;
            for (int i = 0; i < facts->component_count; i++) {
                if (facts->components[i].exists) {
                    lastExisting = i;
                } else if (firstMissing < 0) {
                    firstMissing = i;
                }
            }
            int tailLength = firstMissing >= 0 ? facts->component_count - firstMissing : 0;
            if (!seen_existing) {
                // Nothing exists at all: there is no verified chain for the
                // construction to rest on.
                chainProtected = false;
                out->reason = GC_PATH_RISK_CHAIN_INCOMPLETE;
                out->first_unsafe_component = firstMissing >= 0 ? firstMissing : 0;
            } else if (firstMissing >= 0 && !facts->preflight_mode) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_CHAIN_INCOMPLETE;
                out->first_unsafe_component = firstMissing;
            } else if (firstMissing >= 0 && tailLength > 1 &&
                       facts->components[lastExisting].non_admin_inherit_danger) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_USER_WRITABLE;
                out->first_unsafe_component = firstMissing;
            }
        }
    }

    out->chain_protected = chainProtected;
    // The fail-safe class: everything that was not positively proven safe.
    out->standard_writable = !chainProtected;
}

static inline bool gc_path_protection_requires_acknowledgment(const GcPathProtection* p) {
    if (!p) return true;  // fail-safe
    return !p->chain_protected || p->user_profile || p->remote ||
           p->no_filesystem_permissions;
}

static inline const char* gc_path_protection_headline(const GcPathProtection* p) {
    if (p && p->chain_protected && !gc_path_protection_requires_acknowledgment(p)) {
        return GC_PATH_PROTECTION_SUMMARY_GREEN;
    }
    return GC_PATH_PROTECTION_SUMMARY_UNPROTECTED;
}

// Extra warning lines beyond the headline, in display order (index 0, 1, 2),
// "" when there is nothing left to say.  Iterate until the empty string.
static inline const char* gc_path_protection_extra_note(const GcPathProtection* p, int index) {
    if (!p || index < 0) return "";
    int slot = 0;
    if (p->no_filesystem_permissions) {
        if (slot == index) return GC_PATH_PROTECTION_NOTE_NO_PERMISSIONS;
        slot++;
    }
    if (p->remote) {
        if (slot == index) return GC_PATH_PROTECTION_NOTE_REMOTE;
        slot++;
    }
    if (p->user_profile) {
        if (slot == index) return GC_PATH_PROTECTION_NOTE_USER_PROFILE;
        slot++;
    }
    return "";
}

// The remediation line only applies to a local, ACL-capable location whose
// chain an administrator can still fix; a server-controlled or ACL-less volume
// cannot be repaired with icacls, and a user profile must never be re-ACLed by
// advice (it would break the account's own file ownership).
static inline bool gc_path_protection_wants_remedy(const GcPathProtection* p) {
    return p && p->standard_writable && !p->remote && !p->no_filesystem_permissions &&
           !p->user_profile;
}

// True when the component to remediate does not exist yet, so the caller
// spells the remedy as "mkdir ... & icacls ..." instead of a bare icacls.
static inline bool gc_path_protection_remedy_needs_create(const GcPathProtectionFacts* facts,
                                                         const GcPathProtection* p) {
    if (!facts || !p || p->first_unsafe_component < 0) return false;
    if (p->first_unsafe_component >= facts->component_count) return false;
    return !facts->components[p->first_unsafe_component].exists;
}

#endif // GREEN_CURVE_SERVICE_PATH_CHAIN_POLICY_H
