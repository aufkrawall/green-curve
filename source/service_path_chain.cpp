// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// Win32 fact-gathering for the path-protection classifier.
//
// service_path_chain_policy.h states the property being measured and owns
// every decision; this file only reports what the filesystem says.  Two rules
// govern the gathering:
//
//   1. Nothing here trusts a name.  Every principal is a SID (the German
//      Windows this project is developed on calls the groups
//      "VORDEFINIERT\Administratoren" and "NT-AUTORITÄT\Authentifizierte
//      Benutzer"), except for one lookup that resolves the LOCALIZED name of
//      the Administrators group from its SID to enumerate its members.
//   2. An unreadable fact is a DANGEROUS fact.  A failure to open a component
//      or read its owner/DACL marks it unproven and the policy then warns.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <strsafe.h>

#include "service_acl.h"

// (local array-count helper; GC_ARRAY_COUNT lives in installer_common.h,
// which this translation unit deliberately does not include)
#define GC_PATH_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

namespace {

// The one netapi32 call used here is taken by GetProcAddress so no link line
// changes; these are the four items otherwise pulled in from lm.h (which is
// not included to keep this translation unit lean).
typedef DWORD NET_API_STATUS_LOCAL;
constexpr NET_API_STATUS_LOCAL kNetErrSuccess = 0;             // NERR_Success
constexpr DWORD kNetMaxPreferredLength = (DWORD)-1;            // MAX_PREFERRED_LENGTH
struct GcLocalGroupMembersInfo0 {                              // LOCALGROUP_MEMBERS_INFO_0
    PSID lgrmi0_sid;
};
typedef DWORD (WINAPI* NetLocalGroupGetMembersFn)(LPCWSTR, LPCWSTR, DWORD, LPBYTE*,
                                                  DWORD, LPDWORD, LPDWORD, PDWORD_PTR);
typedef NET_API_STATUS_LOCAL (WINAPI* NetApiBufferFreeFn)(LPVOID);

// Substitution-capable rights; see GcPathComponentFacts.  DELETE renames the
// component away and plants a replacement; FILE_DELETE_CHILD removes a child
// regardless of the child's own DACL; WRITE_DAC/WRITE_OWNER re-ACL the
// component to grant either.  Create-only bits are deliberately absent here:
// they cannot displace an existing protected child.
const DWORD kSubstituteMask =
    DELETE | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

// Rights that let a principal put a NEW file or directory inside a component.
// Irrelevant on an ancestor, decisive on the leaf: the leaf is the directory
// the LocalSystem service binary resolves its DLL imports from, so adding
// `version.dll` beside it is SYSTEM code execution without ever touching the
// hardened binary.  FILE_WRITE_EA / FILE_WRITE_ATTRIBUTES are NOT here - they
// change metadata on existing entries and plant nothing.
const DWORD kCreateMask =
    FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL;

bool build_well_known_sid(WELL_KNOWN_SID_TYPE type, BYTE* buf, DWORD bufSize) {
    DWORD size = bufSize;
    return CreateWellKnownSid(type, nullptr, buf, &size) != FALSE;
}

// CREATOR OWNER / OWNER RIGHTS ACEs grant whatever they carry to the object's
// owner at access-check time.  Owner trust is measured per object instead, so
// these SIDs are neither admin nor non-admin evidence and are skipped.  Owner
// Rights (S-1-3-4) is resolved by string because older SDK headers do not
// spell a WELL_KNOWN_SID_TYPE for it.
bool sid_is_owner_placeholder(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    BYTE creatorOwner[SECURITY_MAX_SID_SIZE] = {};
    BYTE creatorOwnerServer[SECURITY_MAX_SID_SIZE] = {};
    if (build_well_known_sid(WinCreatorOwnerSid, creatorOwner, sizeof(creatorOwner)) &&
        EqualSid(sid, creatorOwner)) return true;
    if (build_well_known_sid(WinCreatorOwnerServerSid, creatorOwnerServer, sizeof(creatorOwnerServer)) &&
        EqualSid(sid, creatorOwnerServer)) return true;
    PSID ownerRights = nullptr;
    if (ConvertStringSidToSidW(L"S-1-3-4", &ownerRights)) {
        bool match = EqualSid(sid, ownerRights);
        LocalFree(ownerRights);
        if (match) return true;
    }
    return false;
}

// Accounts that are trusted with the machine: SYSTEM, BUILTIN\Administrators,
// TrustedInstaller, and every direct member of the local Administrators group
// (a member can take ownership of anything anyway, so an owner or ACE grant
// held by one is not an escalation path).  Nested domain membership is not
// resolved; such an owner classifies untrusted, which errs toward warning.
struct GcAdminTrustSet {
    BYTE sids[32][SECURITY_MAX_SID_SIZE];
    int count;

    bool add(PSID sid) {
        if (!sid || !IsValidSid(sid) || count >= (int)GC_PATH_ARRAY_COUNT(sids)) return false;
        DWORD length = GetLengthSid(sid);
        if (length == 0 || length > SECURITY_MAX_SID_SIZE) return false;
        if (!CopySid(length, (PSID)sids[count], sid)) return false;
        count++;
        return true;
    }
    bool contains(PSID sid) const {
        if (!sid || !IsValidSid(sid)) return false;
        for (int i = 0; i < count; i++) {
            if (EqualSid(sid, (const PSID)sids[i])) return true;
        }
        return false;
    }
};

void admin_trust_add_local_administrators(GcAdminTrustSet* trust) {
    BYTE admins[SECURITY_MAX_SID_SIZE] = {};
    if (!build_well_known_sid(WinBuiltinAdministratorsSid, admins, sizeof(admins))) return;

    // NetLocalGroupGetMembers wants the group NAME, and the built-in group
    // names are localized ("Administratoren" on German Windows); resolve it
    // from the SID first.
    WCHAR account[256] = {};
    WCHAR domain[256] = {};
    DWORD accountLength = GC_PATH_ARRAY_COUNT(account);
    DWORD domainLength = GC_PATH_ARRAY_COUNT(domain);
    SID_NAME_USE use = SidTypeUnknown;
    if (!LookupAccountSidW(nullptr, admins, account, &accountLength,
                           domain, &domainLength, &use)) return;

    HMODULE netapi = LoadLibraryExW(L"netapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!netapi) return;
    auto getMembers = (NetLocalGroupGetMembersFn)GetProcAddress(netapi, "NetLocalGroupGetMembers");
    auto bufferFree = (NetApiBufferFreeFn)GetProcAddress(netapi, "NetApiBufferFree");
    if (!getMembers || !bufferFree) {
        FreeLibrary(netapi);
        return;
    }
    LPBYTE buffer = nullptr;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD_PTR resume = 0;
    NET_API_STATUS_LOCAL status = getMembers(nullptr, account, 0, &buffer,
                                             kNetMaxPreferredLength, &entriesRead,
                                             &totalEntries, &resume);
    if (status == kNetErrSuccess && buffer) {
        for (DWORD i = 0; i < entriesRead; i++) {
            trust->add(((GcLocalGroupMembersInfo0*)buffer)[i].lgrmi0_sid);
        }
    }
    if (buffer) bufferFree(buffer);
    FreeLibrary(netapi);
}

void admin_trust_build(GcAdminTrustSet* trust) {
    trust->count = 0;
    BYTE systemSid[SECURITY_MAX_SID_SIZE] = {};
    BYTE admins[SECURITY_MAX_SID_SIZE] = {};
    if (build_well_known_sid(WinLocalSystemSid, systemSid, sizeof(systemSid))) trust->add(systemSid);
    if (build_well_known_sid(WinBuiltinAdministratorsSid, admins, sizeof(admins))) trust->add(admins);
    // The owner of the Windows and Program Files trees.  Resolved by string
    // because there is no WELL_KNOWN_SID_TYPE for the TrustedInstaller
    // service SID.
    PSID trustedInstaller = nullptr;
    if (ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
                               &trustedInstaller)) {
        trust->add(trustedInstaller);
        LocalFree(trustedInstaller);
    }
    admin_trust_add_local_administrators(trust);
}

// The trust set costs a LoadLibrary plus a NetLocalGroupGetMembers round trip
// (SAM, and a domain controller for domain members of the local group), which
// is far too expensive to repeat: setup reclassifies on every keystroke in the
// folder page.  Build it once per process.  Administrators-group membership
// changing mid-process is not a case worth re-querying for - and a stale set
// only ever mis-trusts an account that was an administrator moments ago.
INIT_ONCE g_adminTrustOnce = INIT_ONCE_STATIC_INIT;
GcAdminTrustSet g_adminTrust = {};

BOOL CALLBACK admin_trust_init_once(PINIT_ONCE, PVOID, PVOID*) {
    admin_trust_build(&g_adminTrust);
    return TRUE;
}

const GcAdminTrustSet& admin_trust_shared() {
    InitOnceExecuteOnce(&g_adminTrustOnce, admin_trust_init_once, nullptr, nullptr);
    return g_adminTrust;
}

// Scan one component's DACL.  `isRoot` relaxes a bare DELETE grant and is only
// ever true for a real drive root (X:\), which can be neither renamed nor
// deleted; a directory a volume happens to be MOUNTED at is an ordinary
// renameable directory and is walked as one.
void scan_dacl_for_danger(PACL dacl, bool isRoot, const GcAdminTrustSet& trust,
                          bool* nonAdminDanger, bool* nonAdminCreateDanger,
                          bool* nonAdminInheritDanger) {
    // A null DACL grants everyone full control; the caller treats it as
    // dangerous before ever getting here.
    if (!dacl) return;
    DWORD selfMask = isRoot ? (kSubstituteMask & ~DELETE) : kSubstituteMask;
    for (DWORD i = 0; i < dacl->AceCount; i++) {
        void* aceRaw = nullptr;
        if (!GetAce(dacl, i, &aceRaw) || !aceRaw) {
            *nonAdminDanger = true;
            *nonAdminCreateDanger = true;
            *nonAdminInheritDanger = true;
            return;
        }
        ACE_HEADER* header = (ACE_HEADER*)aceRaw;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE ||
            header->AceType == ACCESS_DENIED_OBJECT_ACE_TYPE ||
            header->AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE ||
            header->AceType == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE) {
            continue;  // deny ACEs only restrict
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            // Object/callback allow ACEs (and anything unrecognized) carry the
            // mask at the standard offset but their SID elsewhere; rather than
            // guessing at layouts, treat them as dangerous unproven.  File
            // DACLs essentially never contain them.
            *nonAdminDanger = true;
            *nonAdminCreateDanger = true;
            *nonAdminInheritDanger = true;
            return;
        }
        ACCESS_ALLOWED_ACE* ace = (ACCESS_ALLOWED_ACE*)aceRaw;
        PSID sid = (PSID)&ace->SidStart;
        if (sid_is_owner_placeholder(sid)) continue;
        if (trust.contains(sid)) continue;
        bool inheritOnly = (header->AceFlags & INHERIT_ONLY_ACE) != 0;
        bool inheritable =
            (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != 0;
        if (!inheritOnly && (ace->Mask & selfMask)) *nonAdminDanger = true;
        if (!inheritOnly && (ace->Mask & kCreateMask)) *nonAdminCreateDanger = true;
        if (inheritable && (ace->Mask & kSubstituteMask)) *nonAdminInheritDanger = true;
    }
}

bool path_prefix_matches(const WCHAR* path, const WCHAR* prefix) {
    if (!path || !prefix || !prefix[0]) return false;
    size_t prefixLength = 0;
    while (prefix[prefixLength]) prefixLength++;
    while (prefixLength > 0 &&
           (prefix[prefixLength - 1] == L'\\' || prefix[prefixLength - 1] == L'/'))
        prefixLength--;
    if (prefixLength == 0) return false;
    size_t pathLength = 0;
    while (path[pathLength]) pathLength++;
    if (pathLength < prefixLength) return false;
    if (_wcsnicmp(path, prefix, prefixLength) != 0) return false;
    return pathLength == prefixLength || path[prefixLength] == L'\\' ||
           path[prefixLength] == L'/';
}

void gather_component_facts(const WCHAR* full, size_t prefixEnd, bool isRoot,
                            const GcAdminTrustSet& trust, GcPathComponentFacts* facts) {
    WCHAR buffer[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
    if (prefixEnd == 0 || prefixEnd >= GC_PATH_CHAIN_MAX_PATH_CHARS) {
        facts->facts_complete = false;
        return;
    }
    for (size_t i = 0; i < prefixEnd; i++) buffer[i] = full[i];
    buffer[prefixEnd] = 0;

    DWORD attributes = GetFileAttributesW(buffer);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD failure = GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) {
            facts->exists = false;
            facts->facts_complete = true;
            return;
        }
        // Access denied on a parent traversal is not "missing": an existing
        // component we cannot inspect must fail the proof, not join a tail.
        facts->exists = true;
        facts->facts_complete = false;
        return;
    }
    facts->exists = true;
    facts->is_reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    facts->is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    HANDLE handle = CreateFileW(buffer, READ_CONTROL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        facts->facts_complete = false;
        return;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                   OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                   &owner, nullptr, &dacl, nullptr, &sd);
    if (status != ERROR_SUCCESS || !sd) {
        CloseHandle(handle);
        facts->facts_complete = false;
        return;
    }
    facts->owner_admin_trusted = owner && IsValidSid(owner) && trust.contains(owner);
    bool nonAdminDanger = false;
    bool nonAdminCreateDanger = false;
    bool nonAdminInheritDanger = false;
    if (dacl) {
        scan_dacl_for_danger(dacl, isRoot, trust, &nonAdminDanger, &nonAdminCreateDanger,
                             &nonAdminInheritDanger);
    } else {
        // A null DACL grants everyone full control: dangerous in the most
        // direct way possible.
        nonAdminDanger = true;
        nonAdminCreateDanger = true;
        nonAdminInheritDanger = true;
    }
    facts->non_admin_danger = nonAdminDanger;
    facts->non_admin_create_danger = nonAdminCreateDanger;
    facts->non_admin_inherit_danger = nonAdminInheritDanger;
    facts->facts_complete = true;
    LocalFree(sd);
    CloseHandle(handle);
}

}  // namespace

bool gc_path_is_under_user_profile(const wchar_t* path) {
    if (!path || !path[0]) return false;
    WCHAR full[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
    DWORD length = GetFullPathNameW(path, GC_PATH_CHAIN_MAX_PATH_CHARS, full, nullptr);
    if (length == 0 || length >= GC_PATH_CHAIN_MAX_PATH_CHARS) return false;

    bool under = false;
    const KNOWNFOLDERID* roots[] = { &FOLDERID_Profile, &FOLDERID_UserProfiles };
    for (const KNOWNFOLDERID* root : roots) {
        PWSTR resolved = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*root, 0, nullptr, &resolved)) && resolved) {
            if (path_prefix_matches(full, resolved)) under = true;
            CoTaskMemFree(resolved);
        }
        if (under) break;
    }
    return under;
}

void classify_path_protection(const wchar_t* path, GcPathProtectionReport* out,
                              bool preflightMode) {
    if (!out) return;
    GcPathProtectionReport blank = {};
    *out = blank;
    out->facts.preflight_mode = preflightMode;

    if (!path || !path[0]) {
        gc_path_protection_classify(nullptr, &out->verdict);
        return;
    }

    WCHAR full[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
    DWORD fullLength = GetFullPathNameW(path, GC_PATH_CHAIN_MAX_PATH_CHARS, full, nullptr);
    if (fullLength == 0 || fullLength >= GC_PATH_CHAIN_MAX_PATH_CHARS) {
        out->facts.chain_complete = false;
        gc_path_protection_classify(&out->facts, &out->verdict);
        return;
    }

    out->facts.volume.is_unc = full[0] == L'\\' && full[1] == L'\\';
    if (out->facts.volume.is_unc) {
        // A remote location is server-controlled: the verdict is fixed at
        // "not protected, remote" and nothing is probed.  Beyond the honesty
        // of server-side ACLs, every probe would be a network round trip on a
        // UI thread that classifies as the user types.  Short-circuit before
        // the volume lookup below, which can also touch a dead server.
        out->facts.volume.is_remote = true;
        gc_path_protection_classify(&out->facts, &out->verdict);
        return;
    }

    WCHAR volume[GC_PATH_CHAIN_MAX_PATH_CHARS] = {};
    if (!GetVolumePathNameW(full, volume, GC_PATH_CHAIN_MAX_PATH_CHARS)) {
        out->facts.volume.facts_complete = false;
        gc_path_protection_classify(&out->facts, &out->verdict);
        return;
    }
    UINT driveType = GetDriveTypeW(volume);
    out->facts.volume.is_remote = driveType == DRIVE_REMOTE;

    // User-profile reachability is an extra warning, not part of the security
    // proof; a failed lookup just omits it.  Pure path arithmetic against the
    // shell's answers: no volume is touched.
    out->facts.under_user_profile = gc_path_is_under_user_profile(full);

    // A network-mapped drive is server-controlled exactly like a UNC path;
    // same fixed verdict, same refusal to probe.  This return must come BEFORE
    // GetVolumeInformationW: that call is a round trip to the redirector, and
    // setup's folder page classifies on every keystroke, so a dead mapped
    // drive would stall typing.  GetDriveTypeW above is the only question the
    // redirector is asked.
    if (out->facts.volume.is_remote) {
        gc_path_protection_classify(&out->facts, &out->verdict);
        return;
    }

    DWORD fsFlags = 0;
    if (GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, &fsFlags,
                              nullptr, 0)) {
        out->facts.volume.facts_complete = true;
        out->facts.volume.has_persistent_acls = (fsFlags & FILE_PERSISTENT_ACLS) != 0;
    }

    const GcAdminTrustSet& trust = admin_trust_shared();

    // components[0] is the FILESYSTEM root, which for a local path is always
    // the drive root ("D:\").  Deliberately NOT GetVolumePathNameW's answer:
    // that is the path a volume is REACHABLE through, and a volume mounted
    // into a directory ("C:\mnt\data") hangs below ordinary directories that
    // a non-admin with DELETE can rename away, taking the whole install with
    // them.  Starting at the drive root walks those directories like any
    // other, and confines the "cannot be renamed or deleted" DELETE
    // relaxation to a root where it is actually true.
    if (!(full[0] && full[1] == L':' && (full[2] == L'\\' || full[2] == L'/'))) {
        // Not a drive-rooted local path (device namespace, a bare "X:" that
        // GetFullPathNameW did not expand): unproven rather than guessed at.
        out->facts.chain_complete = false;
        gc_path_protection_classify(&out->facts, &out->verdict);
        return;
    }
    const size_t rootEnd = 3;  // keep the separator: "D:" names a directory, "D:\" the root

    // Every component down to the target is probed, including ones that do not
    // exist yet: the policy distinguishes a tail setup will create AND harden
    // (the final component) from intermediates it would create with inherited
    // permissions, and it can only do that if the whole tail is on the table.
    int componentCount = 0;
    size_t componentEnds[GC_PATH_CHAIN_MAX_COMPONENTS] = {};
    bool overflow = false;
    size_t cursor = rootEnd;
    for (;;) {
        if (componentCount >= GC_PATH_CHAIN_MAX_COMPONENTS) {
            overflow = true;
            break;
        }
        size_t end = cursor;
        if (componentCount > 0) {
            while (full[end] && full[end] != L'\\' && full[end] != L'/') end++;
        }
        componentEnds[componentCount] = end;
        GcPathComponentFacts* facts = &out->facts.components[componentCount];
        *facts = GcPathComponentFacts{};
        bool isRoot = componentCount == 0;
        gather_component_facts(full, end, isRoot, trust, facts);
        componentCount++;
        if (end >= fullLength) break;
        cursor = end;
        while (full[cursor] == L'\\' || full[cursor] == L'/') cursor++;
        if (!full[cursor]) break;
    }
    out->facts.component_count = componentCount;
    out->facts.chain_complete = !overflow && componentCount > 0;

    gc_path_protection_classify(&out->facts, &out->verdict);

    // The first unsafe component's path is what the remediation line acts on.
    int unsafeIndex = out->verdict.first_unsafe_component;
    if (unsafeIndex >= 0 && unsafeIndex < componentCount) {
        size_t end = componentEnds[unsafeIndex];
        if (end >= GC_PATH_CHAIN_MAX_PATH_CHARS) end = GC_PATH_CHAIN_MAX_PATH_CHARS - 1;
        for (size_t i = 0; i < end; i++) out->firstUnsafeComponent[i] = full[i];
        out->firstUnsafeComponent[end] = 0;
    }
}
