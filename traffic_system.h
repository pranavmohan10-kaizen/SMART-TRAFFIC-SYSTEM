/* ============================================================================
 *  traffic_system.h
 *  ----------------------------------------------------------------------------
 *  Shared contract between the two independent executables of the
 *  "AI-Based Smart Traffic Management System":
 *
 *      sensor_sim      -> produces  live_sensor_data.txt   (the data producer)
 *      traffic_engine  -> consumes  live_sensor_data.txt   (the AI controller)
 *
 *  Because the two programs are SEPARATE processes (not linked together), this
 *  header only carries things that must agree across the process boundary:
 *      - the on-disk data format constants / file names
 *      - the numeric decision thresholds (so producer & judge speak the same
 *        language about "what a SEVERE lane is")
 *      - the shared data structures
 *      - ANSI colour macros and a portable millisecond sleep
 *
 *  Pure C only (C11 + POSIX). No C++, no external libraries.
 * ==========================================================================*/

#ifndef TRAFFIC_SYSTEM_H
#define TRAFFIC_SYSTEM_H

/* Expose POSIX nanosleep / clock_gettime under a strict -std=c11 build. ------*/
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------------------
 *  CROSS-PLATFORM SLEEP HELPERS
 *  We accelerate the live demo by treating one "signal second" as SIM_TICK_MS
 *  of real wall-clock time (see below). These helpers hide the platform call.
 * --------------------------------------------------------------------------*/
#ifdef _WIN32
    #include <windows.h>
    static inline void sleep_ms(int ms)      { if (ms > 0) Sleep((DWORD)ms); }
    static inline void sleep_seconds(int s)  { if (s  > 0) Sleep((DWORD)s * 1000u); }
#else
    static inline void sleep_ms(int ms) {
        if (ms <= 0) return;
        struct timespec ts;
        ts.tv_sec  =  ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    static inline void sleep_seconds(int s) { sleep_ms(s * 1000); }
#endif

/* ============================================================================
 *  GLOBAL SYSTEM CONFIGURATION
 * ==========================================================================*/

#define NUM_LANES            6          /* Lanes / approaches A .. F           */

/* Lane labels. 'static const' in a header is safe: each translation unit gets
 * its own private copy (internal linkage), so there is no multiple-definition
 * link error, and BOTH executables read identical labels.                    */
static const char LANE_NAMES[NUM_LANES] = { 'A', 'B', 'C', 'D', 'E', 'F' };

/* Raw sensor value ranges (used by the simulator and for clamping on read).  */
#define VEH_MAX              200        /* vehicles per lane   : 0 .. 200      */
#define PED_MAX              50         /* pedestrians/crossing: 0 .. 50       */

/* --- Shared on-disk state files -------------------------------------------- */
#define SENSOR_FILE          "live_sensor_data.txt"   /* current snapshot      */
#define SENSOR_TMP           "live_sensor_data.txt.tmp"/* atomic-write staging  */
#define LOG_FILE             "traffic_log.txt"         /* append-only audit log */

/* ============================================================================
 *  DECISION-TREE THRESHOLDS  (the "rule book" of the AI)
 *  ----------------------------------------------------------------------------
 *  Vehicle density -> base GREEN time:
 *     LOW      0  - 15  -> 10 s
 *     MEDIUM   16 - 35  -> 25 s
 *     HIGH     36 - 59  -> 45 s   (*) gap-filler tier, see note below
 *     SEVERE   60 +     -> 65 s
 *
 *  (*) The brief specified only LOW / MEDIUM / SEVERE and left the band
 *      36-59 undefined. To keep the controller TOTAL (every possible count
 *      maps to exactly one rule, no undefined behaviour), we add a HIGH tier
 *      covering that gap. This is clearly isolated so it is trivial to remove.
 * ==========================================================================*/
#define DENS_LOW_MAX         15         /* <= 15            => LOW             */
#define DENS_MEDIUM_MAX      35         /* 16 .. 35         => MEDIUM          */
#define DENS_HIGH_MAX        59         /* 36 .. 59         => HIGH  (added)   */
#define DENS_SEVERE_MIN      60         /* >= 60            => SEVERE          */

#define BASE_GREEN_LOW       10
#define BASE_GREEN_MEDIUM    25
#define BASE_GREEN_HIGH      45
#define BASE_GREEN_SEVERE    65

/* Pedestrian crowd -> WALK time.
 *     FEW         0  - 5    -> 10 s
 *     MODERATE    6  - 20   -> 15 s
 *     CROWDED     21 - 40   -> 25 s
 *     OVERCROWDED 41 +      -> 30 s
 *  (The brief listed "40+" for OVERCROWDED which overlapped CROWDED's "21-40".
 *   We resolve the overlap cleanly: 40 stays CROWDED, 41+ is OVERCROWDED.)   */
#define PED_FEW_MAX          5
#define PED_MODERATE_MAX     20
#define PED_CROWDED_MAX      40

#define WALK_FEW             10
#define WALK_MODERATE        15
#define WALK_CROWDED         25
#define WALK_OVERCROWDED     30

/* ============================================================================
 *  TIMING / SIMULATION TUNABLES
 *  ----------------------------------------------------------------------------
 *  SIM_TICK_MS lets the demo run in "accelerated time": the dashboard still
 *  shows the REAL algorithmic second-counts (10s, 25s, 65s ...), but each of
 *  those counted-down seconds elapses in SIM_TICK_MS of wall-clock time so a
 *  live examiner is not forced to wait a literal 65 seconds.
 *      Set SIM_TICK_MS = 1000 for true real-time operation.
 * ==========================================================================*/
#define SIM_TICK_MS          3000  /* wall-clock ms per simulated second  */
#define SENSOR_UPDATE_SECS   30       /* sensor_sim refresh period (real s)  */

#define EMERGENCY_GREEN      12         /* GREEN held for a priority vehicle   */
#define YELLOW_TIME          3          /* fixed amber clearance (normal mode) */
#define ALLRED_CLEAR         1          /* all-red safety gap between phases   */
#define NIGHT_GREEN_MIN      3          /* rapid-cycle compressed GREEN (min)  */
#define NIGHT_GREEN_MAX      5          /* rapid-cycle compressed GREEN (max)  */

#define FEED_LINES           6          /* rows shown in the live decision feed*/

/* ============================================================================
 *  ENUMERATED TYPES
 * ==========================================================================*/
typedef enum { SIG_RED = 0, SIG_YELLOW, SIG_GREEN }          SignalState;
typedef enum { DENS_LOW = 0, DENS_MEDIUM, DENS_HIGH, DENS_SEVERE } DensityClass;
typedef enum { PED_FEW = 0, PED_MODERATE, PED_CROWDED, PED_OVERCROWDED } PedClass;
typedef enum { MODE_NORMAL = 0, MODE_EMERGENCY, MODE_NIGHT } SystemMode;

/* ============================================================================
 *  DATA STRUCTURES
 * ==========================================================================*/

/* Raw snapshot of one lane, exactly as the sensor layer reports it.          */
typedef struct {
    char name;           /* 'A' .. 'F'                                        */
    int  vehicles;       /* 0 .. VEH_MAX                                      */
    int  pedestrians;    /* 0 .. PED_MAX                                      */
    int  emergency;      /* structural flag: 0 = none, 1 = priority vehicle   */
} LaneData;

/* The controller's per-lane working record: raw data + every value derived
 * by the decision tree (classification, allocated timings, live signal).     */
typedef struct {
    int          index;        /* original index 0..5 (kept across sorting)    */
    char         name;         /* 'A' .. 'F'                                   */
    int          vehicles;
    int          pedestrians;
    int          emergency;
    DensityClass density;      /* node: classify density                       */
    PedClass     ped_class;    /* node: classify pedestrian crowd              */
    int          green_time;   /* allocated GREEN seconds for this lane        */
    int          walk_time;    /* allocated WALK seconds for this crossing     */
    SignalState  signal;       /* current vehicle signal in the active plan    */
    int          walk_active;  /* 1 if pedestrians may currently WALK          */
    int          skipped;      /* 1 if night-mode dynamic-skip dropped it      */
} LanePlan;

/* The complete decision produced for one moment in time.                     */
typedef struct {
    SystemMode mode;               /* which decision-tree branch is active     */
    long       cycle;              /* monotonically increasing cycle counter   */
    int        active_lane;        /* index currently GREEN, or -1 (all-red)   */
    LanePlan   lanes[NUM_LANES];   /* stable A..F order for the display tables */
    int        order[NUM_LANES];   /* priority order (bubble-sorted, desc)     */
} Plan;

/* ============================================================================
 *  ANSI ESCAPE / COLOUR MACROS  (for the live dashboard)
 * ==========================================================================*/
#define C_RESET        "\033[0m"
#define C_BOLD         "\033[1m"
#define C_DIM          "\033[2m"
#define FG_DIM         C_DIM             /* alias: faint/grey text (skipped lanes) */

#define FG_RED         "\033[31m"
#define FG_GREEN       "\033[32m"
#define FG_YELLOW      "\033[33m"
#define FG_BLUE        "\033[34m"
#define FG_CYAN        "\033[36m"
#define FG_WHITE       "\033[37m"

#define FG_BRED        "\033[91m"   /* bright red    : stopped / emergency     */
#define FG_BGREEN      "\033[92m"   /* bright green  : open lanes / walk       */
#define FG_BYELLOW     "\033[93m"   /* bright yellow : warnings / amber        */
#define FG_BBLUE       "\033[94m"   /* bright blue   : night / info            */
#define FG_BCYAN       "\033[96m"   /* bright cyan   : metrics / headers       */
#define FG_BWHITE      "\033[97m"

#define CLEAR_SCREEN   "\033[2J\033[H"   /* clear + home (no scrollback churn) */
#define CURSOR_HOME    "\033[H"
#define HIDE_CURSOR    "\033[?25l"
#define SHOW_CURSOR    "\033[?25h"

#endif /* TRAFFIC_SYSTEM_H */
