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

/* Probability (percent) that a fully-random cycle injects ONE emergency. */
#define EMERGENCY_CHANCE_PCT   18

/* Scenario kinds chosen by the demo injector. */
typedef enum { SCN_RANDOM = 0, SCN_NIGHT, SCN_RUSH } Scenario;

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

    if (rename(SENSOR_TMP, SENSOR_FILE) != 0) {  /* the atomic step */
        perror("sensor_sim: rename");
        return 0;
    }
    return 1;
}

/* ----------------------------------------------------------------------------
 *  Small presentation helpers for the SENSOR terminal's own panel.
 * --------------------------------------------------------------------------*/
static const char *scenario_name(Scenario s)
{
    switch (s) {
        case SCN_NIGHT: return "QUIET / NIGHT";
        case SCN_RUSH:  return "RUSH HOUR";
        default:        return "RANDOM TRAFFIC";
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
 * ==========================================================================*/
int main(void)
{
    /* Seed with time XOR clock so rapid restarts still diverge. */
    srand((unsigned)(time(NULL) ^ (clock() << 8)));

    LaneData lanes[NUM_LANES];
    long cycle = 0;

    for (;;) {
        /* --- pick a scenario for this cycle (demo injector) --------------- */
        int roll = rnd_range(0, 99);
        Scenario scn = (roll < 15) ? SCN_NIGHT
                     : (roll < 30) ? SCN_RUSH
                                   : SCN_RANDOM;

        generate_snapshot(lanes, scn);
        write_snapshot_atomic(lanes, cycle);
        render_sensor_panel(lanes, cycle, scn);

        cycle++;
        sleep_seconds(SENSOR_UPDATE_SECS);
    }
    return 0; /* unreachable */
}
