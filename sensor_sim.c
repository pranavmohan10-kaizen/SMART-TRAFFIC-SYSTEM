/* ============================================================================
 *  sensor_sim.c
 *  ----------------------------------------------------------------------------
 *  Independent executable that emulates the field hardware (inductive loops,
 *  pedestrian buttons, and emergency-vehicle pre-emption beacons) for all six
 *  approaches A..F. It streams a fresh snapshot into live_sensor_data.txt
 *  every SENSOR_UPDATE_SECS seconds, forever, so that traffic_engine can read
 *  real-time-like data.
 *
 *  SYNCHRONISATION STRATEGY  (the "file lock / structured overwrite" requirement)
 *  ----------------------------------------------------------------------------
 *  Rather than a platform-specific advisory lock (flock/fcntl/LockFileEx), we
 *  use the classic ATOMIC-REPLACE pattern:
 *        1. write the full snapshot to a temporary file (SENSOR_TMP)
 *        2. fflush + fclose it
 *        3. rename() it onto SENSOR_FILE
 *  rename() is atomic within a filesystem, so the reader (traffic_engine)
 *  ALWAYS sees either the previous complete snapshot or the new complete
 *  snapshot, never a half-written/torn file. This is race-free, portable, and
 *  needs no locking handshake between the two processes.
 *
 *  DEMO SCENARIO INJECTOR
 *  ----------------------------------------------------------------------------
 *  Purely uniform random data would almost never satisfy the strict NIGHT-mode
 *  condition (ALL lanes <=15 vehicles AND all pedestrians <=5), so a live
 *  examiner might never see that branch fire. To guarantee every controller
 *  branch is demonstrable, the simulator occasionally forces a representative
 *  "scenario" (quiet night / rush hour) and otherwise emits fully random data.
 *  In a real deployment this block would simply be replaced by hardware reads.
 *
 *  Pure C (C11 + POSIX). No external libraries.
 * ==========================================================================*/

#include "traffic_system.h"
#include <ctype.h>

/* Probability (percent) that a fully-random cycle injects ONE emergency. */
#define EMERGENCY_CHANCE_PCT   18

/* Scenario kinds chosen by the demo injector.
 * SCN_MANUAL is set when the web frontend sends an INJECT/UPDATE command,
 * and prevents auto-generation from overwriting the manually-set values. */
typedef enum { SCN_RANDOM = 0, SCN_NIGHT, SCN_RUSH, SCN_MANUAL } Scenario;

/* ----------------------------------------------------------------------------
 *  rnd_range : inclusive uniform integer in [lo, hi]
 * --------------------------------------------------------------------------*/
static int rnd_range(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + rand() % (hi - lo + 1);
}

/* ----------------------------------------------------------------------------
 *  generate_snapshot
 *  Fills lanes[] for one cycle according to the chosen demo scenario.
 * --------------------------------------------------------------------------*/
static void generate_snapshot(LaneData lanes[], Scenario scn)
{
    /* Reset emergency flags every cycle (at most one lane is flagged below). */
    for (int i = 0; i < NUM_LANES; i++) {
        lanes[i].name      = LANE_NAMES[i];
        lanes[i].emergency = 0;
    }

    switch (scn) {

    /* -- QUIET NIGHT: every lane LOW (0..15), every crossing FEW (0..5),
     *    and a couple of lanes deliberately EMPTY so the engine's
     *    dynamic-skip behaviour is visible. No emergencies in night data.  */
    case SCN_NIGHT:
        for (int i = 0; i < NUM_LANES; i++) {
            lanes[i].vehicles    = rnd_range(0, DENS_LOW_MAX);   /* 0..15 */
            lanes[i].pedestrians = rnd_range(0, PED_FEW_MAX);    /* 0..5  */
        }
        /* Force two random lanes to be completely empty. */
        lanes[rnd_range(0, NUM_LANES - 1)].vehicles = 0;
        lanes[rnd_range(0, NUM_LANES - 1)].vehicles = 0;
        break;

    /* -- RUSH HOUR: a few SEVERE bottleneck lanes plus busy crossings, with
     *    a small chance of an emergency vehicle threading through traffic.  */
    case SCN_RUSH:
        for (int i = 0; i < NUM_LANES; i++) {
            lanes[i].vehicles    = rnd_range(20, VEH_MAX);
            lanes[i].pedestrians = rnd_range(0, PED_MAX);
        }
        /* Guarantee at least two clearly-SEVERE lanes. */
        lanes[rnd_range(0, NUM_LANES - 1)].vehicles = rnd_range(DENS_SEVERE_MIN, VEH_MAX);
        lanes[rnd_range(0, NUM_LANES - 1)].vehicles = rnd_range(DENS_SEVERE_MIN, VEH_MAX);
        if (rnd_range(0, 99) < EMERGENCY_CHANCE_PCT)
            lanes[rnd_range(0, NUM_LANES - 1)].emergency = 1;
        break;

    /* -- FULLY RANDOM: the everyday case. */
    case SCN_RANDOM:
    default:
        for (int i = 0; i < NUM_LANES; i++) {
            lanes[i].vehicles    = rnd_range(0, VEH_MAX);
            lanes[i].pedestrians = rnd_range(0, PED_MAX);
        }
        if (rnd_range(0, 99) < EMERGENCY_CHANCE_PCT)
            lanes[rnd_range(0, NUM_LANES - 1)].emergency = 1;
        break;
    }
}

/* ----------------------------------------------------------------------------
 *  write_snapshot_atomic
 *  Implements the atomic-replace described in the file header.
 *  Returns 1 on success, 0 on failure.
 * --------------------------------------------------------------------------*/
static int write_snapshot_atomic(const LaneData lanes[], long cycle)
{
    FILE *f = fopen(SENSOR_TMP, "w");
    if (!f) { perror("sensor_sim: fopen(tmp)"); return 0; }

    /* Human-readable, easily parsed line format:  LANE VEH PED EMERGENCY */
    fprintf(f, "# AI Smart Traffic - live sensor feed\n");
    fprintf(f, "# fields: LANE VEHICLES PEDESTRIANS EMERGENCY\n");
    for (int i = 0; i < NUM_LANES; i++) {
        fprintf(f, "%c %d %d %d\n",
                lanes[i].name, lanes[i].vehicles,
                lanes[i].pedestrians, lanes[i].emergency);
    }
    fprintf(f, "# cycle %ld ts %ld\n", cycle, (long)time(NULL));

    fflush(f);          /* push C buffers to the OS ...            */
    fclose(f);          /* ... and close before renaming.          */

#ifdef _WIN32
    /* MoveFileExA with MOVEFILE_REPLACE_EXISTING atomically replaces the
       destination on NTFS.  MSVCRT rename() fails when the target exists,
       which is the root cause of the data-desync bug on Windows. */
    if (!MoveFileExA(SENSOR_TMP, SENSOR_FILE, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "sensor_sim: MoveFileEx error %lu\n", GetLastError());
        return 0;
    }
#else
    if (rename(SENSOR_TMP, SENSOR_FILE) != 0) {  /* the atomic step */
        perror("sensor_sim: rename");
        return 0;
    }
#endif
    return 1;
}

/* ----------------------------------------------------------------------------
 *  Small presentation helpers for the SENSOR terminal's own panel.
 * --------------------------------------------------------------------------*/
static const char *scenario_name(Scenario s)
{
    switch (s) {
        case SCN_NIGHT:  return "QUIET / NIGHT";
        case SCN_RUSH:   return "RUSH HOUR";
        case SCN_MANUAL: return "MANUAL OVERRIDE";
        default:         return "RANDOM TRAFFIC";
    }
}

/* Colour a vehicle count by its density tier (mirrors the engine's rules). */
static const char *veh_colour(int v)
{
    if (v <= DENS_LOW_MAX)    return FG_GREEN;
    if (v <= DENS_MEDIUM_MAX) return FG_BYELLOW;
    return FG_BRED;
}

static void render_sensor_panel(const LaneData lanes[], long cycle, Scenario scn)
{
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));

    fputs(CLEAR_SCREEN, stdout);
    printf(FG_BCYAN C_BOLD
           "+--------------------------------------------------------------+\n"
           "|            TRAFFIC SENSOR SIMULATOR  (data producer)         |\n"
           "+--------------------------------------------------------------+\n" C_RESET);
    printf("  Writing -> " FG_BWHITE "%s" C_RESET
           "   every " FG_BWHITE "%d s" C_RESET "\n", SENSOR_FILE, SENSOR_UPDATE_SECS);
    printf("  Cycle: " FG_BCYAN "%-6ld" C_RESET
           "  Scenario: " FG_BYELLOW "%-16s" C_RESET
           "  %s\n\n", cycle, scenario_name(scn), ts);

    printf(FG_BCYAN "  %-6s %-10s %-12s %-10s\n" C_RESET,
           "Lane", "Vehicles", "Pedestrians", "Emergency");
    printf("  ------------------------------------------\n");
    for (int i = 0; i < NUM_LANES; i++) {
        printf("  %-6c %s%-10d" C_RESET " %-12d %s\n",
               lanes[i].name,
               veh_colour(lanes[i].vehicles), lanes[i].vehicles,
               lanes[i].pedestrians,
               lanes[i].emergency ? FG_BRED C_BOLD "  ** PRIORITY **" C_RESET
                                  : FG_DIM "   --" C_RESET);
    }
    printf("\n  " FG_DIM "Press Ctrl+C to stop the sensor stream." C_RESET "\n");
    fflush(stdout);
}

/* ============================================================================
 *  main : the infinite sensor stream
 *
 *  Extended for web frontend: instead of a plain sleep_seconds() between
 *  sensor updates, the loop wakes every 100 ms to check sensor_control.txt
 *  for commands written by the Node.js server (SCENARIO, INJECT, UPDATE).
 *  The 30-second sensor update cadence is fully preserved.
 * ==========================================================================*/
int main(void)
{
    /* Seed with time XOR clock so rapid restarts still diverge. */
    srand((unsigned)(time(NULL) ^ (clock() << 8)));

    LaneData lanes[NUM_LANES];
    long cycle = 0;

    /* Write the very first snapshot immediately so the engine doesn't wait. */
    {
        int roll = rnd_range(0, 99);
        Scenario scn = (roll < 15) ? SCN_NIGHT
                     : (roll < 30) ? SCN_RUSH
                                   : SCN_RANDOM;
        for (int i = 0; i < NUM_LANES; i++) lanes[i].name = LANE_NAMES[i];
        generate_snapshot(lanes, scn);
        write_snapshot_atomic(lanes, cycle);
        render_sensor_panel(lanes, cycle, scn);
        cycle++;
    }

    Scenario scn = SCN_RANDOM;

    for (;;) {
        /* --- sleep SENSOR_UPDATE_SECS but poll every 100 ms for commands -- */
        int sleep_ticks = SENSOR_UPDATE_SECS * 10; /* 30 s -> 300 x 100 ms  */
        int cmd_received = 0;

        for (int t = 0; t < sleep_ticks; t++) {
            sleep_ms(100);

            /* Try to read and process a command from the web frontend. */
            FILE *cf = fopen("sensor_control.txt", "r");
            if (!cf) continue;

            char cmd_line[256] = {0};
            if (!fgets(cmd_line, sizeof cmd_line, cf)) { fclose(cf); continue; }
            fclose(cf);
            remove("sensor_control.txt"); /* consume it */

            char cmd[32] = {0};
            if (sscanf(cmd_line, "%31s", cmd) != 1) continue;

            if (strcmp(cmd, "SCENARIO") == 0) {
                /* SCENARIO NIGHT | RUSH | RANDOM | MANUAL */
                char scn_name[32] = {0};
                if (sscanf(cmd_line, "SCENARIO %31s", scn_name) == 1) {
                    if      (strcmp(scn_name, "NIGHT")  == 0) scn = SCN_NIGHT;
                    else if (strcmp(scn_name, "RUSH")   == 0) scn = SCN_RUSH;
                    else if (strcmp(scn_name, "RANDOM") == 0) scn = SCN_RANDOM;
                    else if (strcmp(scn_name, "MANUAL") == 0) scn = SCN_MANUAL;
                    if (scn != SCN_MANUAL) generate_snapshot(lanes, scn);
                    write_snapshot_atomic(lanes, cycle);
                    render_sensor_panel(lanes, cycle, scn);
                    cycle++;
                    cmd_received = 1;
                    break; /* reset the 30-s wait */
                }

            } else if (strcmp(cmd, "INJECT") == 0) {
                /* INJECT <lane> - set emergency flag on the named lane */
                char lane_char;
                if (sscanf(cmd_line, "INJECT %c", &lane_char) == 1) {
                    lane_char = (char)toupper((unsigned char)lane_char);
                    for (int i = 0; i < NUM_LANES; i++) {
                        if (LANE_NAMES[i] == lane_char)
                            lanes[i].emergency = 1;
                    }
                    scn = SCN_MANUAL;
                    write_snapshot_atomic(lanes, cycle);
                    render_sensor_panel(lanes, cycle, scn);
                    cycle++;
                    cmd_received = 1;
                    break;
                }

            } else if (strcmp(cmd, "UPDATE") == 0) {
                /* UPDATE <lane> <vehicles> <pedestrians> <emergency> */
                char lane_char;
                int veh, ped, emg;
                if (sscanf(cmd_line, "UPDATE %c %d %d %d",
                           &lane_char, &veh, &ped, &emg) == 4) {
                    lane_char = (char)toupper((unsigned char)lane_char);
                    for (int i = 0; i < NUM_LANES; i++) {
                        if (LANE_NAMES[i] == lane_char) {
                            lanes[i].vehicles    = (veh < 0) ? 0 : (veh > VEH_MAX) ? VEH_MAX : veh;
                            lanes[i].pedestrians = (ped < 0) ? 0 : (ped > PED_MAX) ? PED_MAX : ped;
                            lanes[i].emergency   = emg ? 1 : 0;
                        }
                    }
                    scn = SCN_MANUAL;
                    write_snapshot_atomic(lanes, cycle);
                    render_sensor_panel(lanes, cycle, scn);
                    cycle++;
                    cmd_received = 1;
                    break;
                } else {
                    fprintf(stderr, "\nsensor_sim: Failed to parse UPDATE command line: %s\n", cmd_line);
                    fflush(stderr);
                }
            } else {
                fprintf(stderr, "\nsensor_sim: Unknown command keyword: %s\n", cmd);
                fflush(stderr);
            }
        } /* end 100-ms polling loop */

        /* --- if no command arrived: pick next auto-scenario and write ------ */
        if (!cmd_received) {
            if (scn != SCN_MANUAL) {
                int roll = rnd_range(0, 99);
                scn = (roll < 15) ? SCN_NIGHT
                    : (roll < 30) ? SCN_RUSH
                                  : SCN_RANDOM;
                generate_snapshot(lanes, scn);
            }
            /* In MANUAL mode we re-write the same lanes so the engine re-reads
             * and re-plans (in case it missed the initial write). */
            write_snapshot_atomic(lanes, cycle);
            render_sensor_panel(lanes, cycle, scn);
            cycle++;
        }
    }
    return 0; /* unreachable */
}
