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
//   chainProtected = every existing path component from the volume root down
//   is a plain directory, admin/SYSTEM-owned, and grants no non-admin the
//   rights that could substitute it (delete / delete-child / WRITE_DAC /
//   WRITE_OWNER).  Create-only rights on ancestors are harmless: they cannot
//   displace an existing protected child.  Components that do not exist yet are
//   safe by construction - the elevated setup creates them beneath the
//   verified chain and hardens the leaf before any payload lands in it.
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
    // be missing; the policy treats that tail as safe by construction (the
    // elevated setup creates it beneath the verified chain) when the last
    // existing component grants nothing dangerous to non-admins.
    bool exists;
    bool is_reparse;
    // Owner is admin-equivalent (Administrators, SYSTEM, TrustedInstaller, or
    // an account that is a member of the local Administrators group).
    bool owner_admin_trusted;
    // Some non-admin principal holds substitution-capable rights against this
    // component: DELETE (rename it away and plant a replacement), delete-child
    // (remove it regardless of its own DACL), WRITE_DAC / WRITE_OWNER (re-ACL
    // it to grant the above).  Create-only rights do NOT count: they cannot
    // displace an existing protected child.  On the volume root (index 0) a
    // bare DELETE grant is ignored - a volume root can be neither renamed nor
    // deleted - but every other bit still counts.
    bool non_admin_danger;
    // Some non-admin principal holds dangerous rights that INHERIT to children
    // created beneath this component.  An inherit-only ACE grants nothing on
    // the component itself but flows into whatever is created there, which is
    // exactly what the constructed tail would pick up (real default ACLs do
    // this: a data drive root hands Authenticated Users inheritable Modify).
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
    GcPathComponentFacts components[GC_PATH_CHAIN_MAX_COMPONENTS];
    int component_count;   // components[0] is the volume root
    bool chain_complete;   // walked to the target without gaps or overflow
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
#define GC_PATH_PROTECTION_REMEDY_BEFORE_PATH \
    "To fully protect this location, run in an admin console: icacls \""
#define GC_PATH_PROTECTION_REMEDY_AFTER_PATH \
    " /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F /grant:r *S-1-5-18:(OI)(CI)F /grant:r *S-1-5-32-545:(OI)(CI)RX /setowner *S-1-5-32-544"
#define GC_PATH_PROTECTION_REMEDY_CREATE_BEFORE_PATH \
    "To fully protect this location, run in an admin console: mkdir \""
#define GC_PATH_PROTECTION_REMEDY_CREATE_MID_PATH \
    "\" & icacls \""
// (the same grant line as the existing-folder spelling follows)
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
            if (!component->owner_admin_trusted) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_NOT_ADMIN_OWNED;
                out->first_unsafe_component = i;
                break;
            }
            // The install leaf's own DACL is replaced with the protected one
            // before anything is written into it, so a pre-existing non-admin
            // grant on the LEAF is neutralized by construction.  On every
            // ancestor it is not - that is the substitution path this whole
            // policy exists to close.  The leaf is always the last component.
            if (component->non_admin_danger && !isLeaf) {
                chainProtected = false;
                out->reason = GC_PATH_RISK_COMPONENT_USER_WRITABLE;
                out->first_unsafe_component = i;
                break;
            }
        }
        // The missing tail is created beneath the last existing component; it
        // inherits that component's inheritable ACLs.  A tail under
        // inheritable non-admin danger would land still-substitutable (a
        // created folder inheriting DELETE can be renamed away and replaced),
        // so such a chain is not protected.  The actionable component is the
        // first missing one: created protected, it breaks the inheritance.
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
            if (!seen_existing) {
                // Nothing exists at all: there is no verified chain for the
                // construction to rest on.
                chainProtected = false;
                out->reason = GC_PATH_RISK_CHAIN_INCOMPLETE;
                out->first_unsafe_component = firstMissing >= 0 ? firstMissing : 0;
            } else if (firstMissing >= 0 &&
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
