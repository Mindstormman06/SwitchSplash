#include "app_monitor.h"
#include "log.h"

#include <string.h>

/* pm returns this when there is no application process (seen in sys-clk/sys-tune). */
#define RESULT_PM_NOT_FOUND 0x20F

static u64 g_cur_pid = 0;
static u64 g_cur_program_id = 0;
static Result g_last_poll_rc = 0;

/* pm:info 65002 AtmosphereGetProcessInfo(pid) -> (ncm::ProgramLocation, cfg::OverrideStatus)
 * See stratosphere/pm/source/pm_info_service.hpp. libnx doesn't wrap it. */
static Result pminfoAtmosphereGetProcessInfo(u64 pid, NcmProgramLocation *out_loc, AmsOverrideStatus *out_status) {
    struct {
        NcmProgramLocation loc;
        AmsOverrideStatus status;
    } out;
    _Static_assert(sizeof(out) == 0x20, "AtmosphereGetProcessInfo output size");

    Result rc = serviceDispatchInOut(pminfoGetServiceSession(), 65002, pid, out);
    if (R_SUCCEEDED(rc)) {
        *out_loc = out.loc;
        *out_status = out.status;
    }
    return rc;
}

Result appMonitorInitialize(void) {
    Result rc = pmdmntInitialize();
    if (R_FAILED(rc))
        return rc;

    rc = pminfoInitialize();
    if (R_FAILED(rc)) {
        pmdmntExit();
        return rc;
    }
    return 0;
}

void appMonitorExit(void) {
    pminfoExit();
    pmdmntExit();
}

bool appMonitorPoll(AppEvent *out) {
    u64 pid = 0;
    Result rc = pmdmntGetApplicationProcessId(&pid);

    if (R_FAILED(rc)) {
        if (rc != RESULT_PM_NOT_FOUND && rc != g_last_poll_rc)
            logWrite("WARN  pmdmntGetApplicationProcessId failed: 0x%x", rc);
        pid = 0;
    }
    g_last_poll_rc = rc;

    if (pid == g_cur_pid)
        return false;

    memset(out, 0, sizeof(*out));
    out->tick = armGetSystemTick();

    /* Report the exit of the previous process first, so a launch is never swallowed. */
    if (g_cur_pid != 0) {
        out->type = AppEvent_Exited;
        out->pid = g_cur_pid;
        out->program_id = g_cur_program_id;
        g_cur_pid = 0;
        g_cur_program_id = 0;
        return true;
    }

    u64 program_id = 0;
    rc = pminfoGetProgramId(&program_id, pid);
    if (R_FAILED(rc)) {
        /* The process may have died between the two calls. Leave g_cur_pid = 0 so the
         * next poll tries again if it's still around. */
        logWrite("WARN  pminfoGetProgramId(pid=%lu) failed: 0x%x", pid, rc);
        return false;
    }

    out->type = AppEvent_Launched;
    out->pid = pid;
    out->program_id = program_id;

    NcmProgramLocation loc;
    if (R_SUCCEEDED(pminfoAtmosphereGetProcessInfo(pid, &loc, &out->status))) {
        out->has_ams_info = true;
        out->storage_id = loc.storageID;
    }

    g_cur_pid = pid;
    g_cur_program_id = program_id;
    return true;
}
