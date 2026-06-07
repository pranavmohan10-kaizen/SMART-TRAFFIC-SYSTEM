/* ============================================================================
 *  traffic_engine.c
 *  ----------------------------------------------------------------------------
 *  The intelligent traffic controller. It continuously:
 *      1. reads the latest sensor snapshot (live_sensor_data.txt),
 *      2. runs a DETERMINISTIC DECISION TREE to choose signal timings,
 *      3. drives the signals through their phases (green -> amber -> red),
 *      4. paints a live ANSI dashboard, and
 *      5. appends a timestamped audit trail to traffic_log.txt.
 *
 *  DECISION TREE (evaluated top-down every cycle):
 *
 *      ROOT
 *      |
 *      +-- [Branch 1] Emergency vehicle present?
 *      |        YES -> PRIORITY OVERRIDE: that lane GREEN, all others RED,
 *      |               all pedestrian crossings HELD.            (run_emergency)
 *      |        NO  v
 *      +-- [Branch 2] Night condition? (ALL lanes LOW & all peds FEW)
 *      |        YES -> RAPID CYCLE / DYNAMIC SKIP: 3-5s greens,
 *      |               empty lanes skipped, NO blinking amber.   (run_night)
 *      |        NO  v
 *      +-- [Branch 3] NORMAL ADAPTIVE control
 *               |-- Node: classify each lane by density  -> base green
 *               |-- Node: classify each crossing by crowd -> walk time
 *               |-- Node: BUBBLE SORT lanes by density (busiest first)
 *               |-- Node: serve lanes in that priority order
 *               +-- Node: COORDINATION - WALK only while the lane is RED
 *                                                            (run_normal)
 *
 *  Pure C (C11 + POSIX). No external libraries.
 * ==========================================================================*/

#include "traffic_system.h"
#include <ctype.h>
#include <stdarg.h>
#include <signal.h>

/* ============================================================================
 *  GLOBAL STATE
 * ==========================================================================*/
static LaneData g_live[NUM_LANES];           /* most recent sensor snapshot   */
static volatile sig_atomic_t g_running = 1;  /* cleared by Ctrl+C handler     */

/* Rolling "live decision feed" shown at the bottom of the dashboard.         */
static struct { char text[256]; const char *color; } g_feed[FEED_LINES];
static int g_feed_count = 0;

/* Forward declaration so the phase code can request a repaint. */
static void render(const Plan *p, const char *phase, int remaining, int total);

/* ============================================================================
 *  SECTION 1 - small string / time utilities
 * ==========================================================================*/

/* Safe append: concatenate src onto dst without overflowing cap. */
static void sapp(char *dst, size_t cap, const char *src)
{
    size_t len = strlen(dst);
    if (len < cap - 1) snprintf(dst + len, cap - len, "%s", src);
}

/* Centre an ASCII string inside a field of `w` visible columns (truncates if
 * too long). Cell text is ASCII-only, so byte length == visible width.       */
static void center(char *out, size_t cap, const char *s, int w)
{
    int len = (int)strlen(s);
    if (len >= w) { snprintf(out, cap, "%.*s", w, s); return; }
    int pad = w - len, left = pad / 2, right = pad - left;
    snprintf(out, cap, "%*s%s%*s", left, "", s, right, "");
}

/* Left-justify (and hard-truncate) to exactly w columns. */
static void ljust(char *out, size_t cap, const char *s, int w)
{
    snprintf(out, cap, "%-*.*s", w, w, s);
}

static void timestamp(char *buf, size_t n)
{
    time_t t = time(NULL);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", localtime(&t));
}

/* ============================================================================
 *  SECTION 2 - logging + live feed
 *  notify()   : writes a timestamped line to traffic_log.txt AND pushes a
 *               coloured entry onto the on-screen feed (used for decisions,
 *               emergencies, mode changes, skips).
 *  log_stats(): appends a detailed per-lane statistics block (log only).
 * ==========================================================================*/

static void feed_push(const char *color, const char *line)
{
    if (g_feed_count < FEED_LINES) {
        snprintf(g_feed[g_feed_count].text, sizeof g_feed[0].text, "%s", line);
        g_feed[g_feed_count].color = color;
        g_feed_count++;
    } else {
        for (int i = 1; i < FEED_LINES; i++) g_feed[i - 1] = g_feed[i];
        snprintf(g_feed[FEED_LINES - 1].text, sizeof g_feed[0].text, "%s", line);
        g_feed[FEED_LINES - 1].color = color;
    }
}

static void notify(const char *level, const char *color, const char *fmt, ...)
{
    char msg[180];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* (a) persistent audit log */
    char ts[32]; timestamp(ts, sizeof ts);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) { fprintf(f, "[%s] [%-9s] %s\n", ts, level, msg); fclose(f); }

    /* (b) on-screen feed, prefixed with a short HH:MM:SS */
    char hms[16];
    time_t t = time(NULL);
    strftime(hms, sizeof hms, "%H:%M:%S", localtime(&t));
    char line[256];
    snprintf(line, sizeof line, "[%s] [%s] %s", hms, level, msg);
    feed_push(color, line);
}

static const char *density_name(DensityClass d);   /* fwd */
static const char *ped_name(PedClass p);            /* fwd */
static const char *mode_name(SystemMode m);         /* fwd */
static int         night_green(int vehicles);       /* fwd */

static void log_stats(const Plan *p)
{
    char ts[32]; timestamp(ts, sizeof ts);
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;

    fprintf(f, "[%s] [STATS    ] ----- Cycle %ld  Mode=%s -----\n",
            ts, p->cycle, mode_name(p->mode));
    for (int i = 0; i < NUM_LANES; i++) {
        const LanePlan *L = &p->lanes[i];
        /* Report the green each lane will actually receive: the compressed
         * rapid-cycle value in night mode, else the rule-based classification. */
        int gshow = (p->mode == MODE_NIGHT) ? night_green(L->vehicles)
                                            : L->green_time;
        fprintf(f,
            "[%s] [STATS    ]   Lane %c | veh=%-3d (%-6s) green=%2ds | "
            "ped=%-2d (%-11s) walk=%2ds | emg=%d\n",
            ts, L->name, L->vehicles, density_name(L->density), gshow,
            L->pedestrians, ped_name(L->ped_class), L->walk_time, L->emergency);
    }
    /* bubble-sorted priority queue, for the record */
    fprintf(f, "[%s] [STATS    ]   Priority queue (desc):", ts);
    for (int k = 0; k < NUM_LANES; k++)
        fprintf(f, " %c(%d)", p->lanes[p->order[k]].name,
                              p->lanes[p->order[k]].vehicles);
    fprintf(f, "\n");
    fclose(f);
}

/* ============================================================================
 *  SECTION 3 - DECISION-TREE CLASSIFICATION NODES
 * ==========================================================================*/

/* Vehicle density -> tier. */
static DensityClass classify_density(int v)
{
    if (v <= DENS_LOW_MAX)    return DENS_LOW;
    if (v <= DENS_MEDIUM_MAX) return DENS_MEDIUM;
    if (v <= DENS_HIGH_MAX)   return DENS_HIGH;     /* gap-filler tier */
    return DENS_SEVERE;                              /* >= 60 */
}

static int green_for_density(DensityClass d)
{
    switch (d) {
        case DENS_LOW:    return BASE_GREEN_LOW;
        case DENS_MEDIUM: return BASE_GREEN_MEDIUM;
        case DENS_HIGH:   return BASE_GREEN_HIGH;
        case DENS_SEVERE: return BASE_GREEN_SEVERE;
    }
    return BASE_GREEN_LOW;
}

/* Pedestrian crowd -> tier. */
static PedClass classify_ped(int p)
{
    if (p <= PED_FEW_MAX)      return PED_FEW;
    if (p <= PED_MODERATE_MAX) return PED_MODERATE;
    if (p <= PED_CROWDED_MAX)  return PED_CROWDED;
    return PED_OVERCROWDED;
}

static int walk_for_ped(PedClass c)
{
    switch (c) {
        case PED_FEW:         return WALK_FEW;
        case PED_MODERATE:    return WALK_MODERATE;
        case PED_CROWDED:     return WALK_CROWDED;
        case PED_OVERCROWDED: return WALK_OVERCROWDED;
    }
    return WALK_FEW;
}

static const char *density_name(DensityClass d)
{
    switch (d) {
        case DENS_LOW:    return "LOW";
        case DENS_MEDIUM: return "MEDIUM";
        case DENS_HIGH:   return "HIGH";
        case DENS_SEVERE: return "SEVERE";
    }
    return "?";
}
static const char *ped_name(PedClass p)
{
    switch (p) {
        case PED_FEW:         return "FEW";
        case PED_MODERATE:    return "MODERATE";
        case PED_CROWDED:     return "CROWDED";
        case PED_OVERCROWDED: return "OVERCROWDED";
    }
    return "?";
}
static const char *mode_name(SystemMode m)
{
    switch (m) {
        case MODE_NORMAL:    return "NORMAL";
        case MODE_EMERGENCY: return "EMERGENCY";
        case MODE_NIGHT:     return "NIGHT";
    }
    return "?";
}

/* ============================================================================
 *  SECTION 4 - DECISION-TREE ALGORITHM NODES
 * ==========================================================================*/

/* --- PRIORITY SORTING NODE --------------------------------------------------
 *  Classic BUBBLE SORT on the index array `order[]`, ordering lanes from the
 *  highest vehicle count to the lowest so the worst bottleneck is served
 *  first. (Bubble sort is intentional - simple, in-place, and easy to audit
 *  for a small fixed N=6.)
 * --------------------------------------------------------------------------*/
static void bubble_sort_by_density(int order[], const LanePlan lanes[])
{
    for (int i = 0; i < NUM_LANES - 1; i++) {
        for (int j = 0; j < NUM_LANES - 1 - i; j++) {
            if (lanes[order[j]].vehicles < lanes[order[j + 1]].vehicles) {
                int tmp      = order[j];
                order[j]     = order[j + 1];
                order[j + 1] = tmp;
            }
        }
    }
}

/* --- EMERGENCY DETECTION ----------------------------------------------------
 *  Returns the index of the emergency lane, or -1 if none. If several lanes
 *  report a priority vehicle, the busiest one wins (deterministic tie-break).
 * --------------------------------------------------------------------------*/
static int detect_emergency(const LaneData d[])
{
    int best = -1;
    for (int i = 0; i < NUM_LANES; i++) {
        if (d[i].emergency) {
            if (best < 0 || d[i].vehicles > d[best].vehicles) best = i;
        }
    }
    return best;
}

/* --- NIGHT-MODE CONDITION ---------------------------------------------------
 *  TRUE only when EVERY lane is LOW (<=15 vehicles) AND every crossing is
 *  FEW (<=5 pedestrians).
 * --------------------------------------------------------------------------*/
static int is_night_mode(const LaneData d[])
{
    for (int i = 0; i < NUM_LANES; i++) {
        if (d[i].vehicles > DENS_LOW_MAX)  return 0;
        if (d[i].pedestrians > PED_FEW_MAX) return 0;
    }
    return 1;
}

/* Compressed night-mode green: 3-5s, scaled mildly by how busy the (still LOW)
 * lane is. */
static int night_green(int vehicles)
{
    int g = NIGHT_GREEN_MIN + (vehicles >= 8 ? 1 : 0) + (vehicles >= 13 ? 1 : 0);
    if (g > NIGHT_GREEN_MAX) g = NIGHT_GREEN_MAX;
    return g;
}

/* ============================================================================
 *  SECTION 5 - PLAN CONSTRUCTION + COORDINATION
 * ==========================================================================*/

/* Run the classification nodes + the sort, and select the decision-tree
 * branch (mode). Signals are set later by the phase that actually drives
 * them. */
static void build_plan(const LaneData in[], Plan *p, long cycle)
{
    p->cycle       = cycle;
    p->active_lane = -1;

    for (int i = 0; i < NUM_LANES; i++) {
        LanePlan *L = &p->lanes[i];
        L->index       = i;
        L->name        = LANE_NAMES[i];
        L->vehicles    = in[i].vehicles;
        L->pedestrians = in[i].pedestrians;
        L->emergency   = in[i].emergency;
        L->density     = classify_density(L->vehicles);
        L->green_time  = green_for_density(L->density);
        L->ped_class   = classify_ped(L->pedestrians);
        L->walk_time   = walk_for_ped(L->ped_class);
        L->signal      = SIG_RED;
        L->walk_active = 0;
        L->skipped     = 0;
        p->order[i]    = i;
    }

    bubble_sort_by_density(p->order, p->lanes);   /* priority sorting node */

    /* Top-down branch selection. */
    if (detect_emergency(in) >= 0)      p->mode = MODE_EMERGENCY;
    else if (is_night_mode(in))         p->mode = MODE_NIGHT;
    else                                p->mode = MODE_NORMAL;
}

/* Recompute the *live* counts / classifications from g_live WITHOUT disturbing
 * the signals chosen by the active phase. Lets the metric tables update in
 * real time while a green is still counting down. */
static void refresh_counts(Plan *p)
{
    for (int i = 0; i < NUM_LANES; i++) {
        LanePlan *L = &p->lanes[i];
        L->vehicles    = g_live[i].vehicles;
        L->pedestrians = g_live[i].pedestrians;
        L->emergency   = g_live[i].emergency;
        L->density     = classify_density(L->vehicles);
        /* In night mode the assigned green is the compressed rapid-cycle value
         * (3-5s); otherwise it is the rule-based classification time. This keeps
         * the dashboard's "Green(s)" column honest about what each lane gets. */
        L->green_time  = (p->mode == MODE_NIGHT)
                           ? night_green(L->vehicles)
                           : green_for_density(L->density);
        L->ped_class   = classify_ped(L->pedestrians);
        L->walk_time   = walk_for_ped(L->ped_class);
    }
}

/* --- COORDINATION LOGIC NODE ------------------------------------------------
 *  Safety rule: a pedestrian WALK is permitted on a crossing ONLY while that
 *  approach's vehicle signal is RED. During an emergency override every
 *  crossing is HELD so the intersection is clear for the priority vehicle.
 * --------------------------------------------------------------------------*/
static void apply_coordination(Plan *p)
{
    for (int i = 0; i < NUM_LANES; i++) {
        if (p->mode == MODE_EMERGENCY)
            p->lanes[i].walk_active = 0;                 /* hold all peds */
        else
            p->lanes[i].walk_active = (p->lanes[i].signal == SIG_RED) ? 1 : 0;
    }
}

/* ============================================================================
 *  SECTION 6 - SENSOR INPUT (file I/O)
 *  Reads SENSOR_FILE. Returns NUM_LANES only when a COMPLETE snapshot of all
 *  six lanes parsed cleanly (so partial/missing files are simply ignored).
 * ==========================================================================*/
static int read_sensors(LaneData out[])
{
    FILE *f = fopen(SENSOR_FILE, "r");
    if (!f) return 0;

    int seen[NUM_LANES] = {0};
    int got = 0;
    char line[256];

    while (fgets(line, sizeof line, f)) {
        char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '#' || *q == '\n' || *q == '\0' || *q == '\r') continue;

        char L; int v, pd, em;
        if (sscanf(q, " %c %d %d %d", &L, &v, &pd, &em) == 4) {
            int idx = -1;
            char up = (char)toupper((unsigned char)L);
            for (int i = 0; i < NUM_LANES; i++)
                if (LANE_NAMES[i] == up) { idx = i; break; }
            if (idx < 0) continue;

            if (v  < 0)       v  = 0;          /* clamp vehicles to [0,VEH_MAX] */
            if (v  > VEH_MAX) v  = VEH_MAX;
            if (pd < 0)       pd = 0;          /* clamp pedestrians to [0,PED_MAX] */
            if (pd > PED_MAX) pd = PED_MAX;
            em = em ? 1 : 0;

            out[idx].name        = LANE_NAMES[idx];
            out[idx].vehicles    = v;
            out[idx].pedestrians = pd;
            out[idx].emergency   = em;
            if (!seen[idx]) { seen[idx] = 1; got++; }
        }
    }
    fclose(f);
    return (got == NUM_LANES) ? got : 0;
}

/* Pull a fresh snapshot into g_live if (and only if) a complete one is
 * available. Returns 1 if g_live was updated this call. */
static int poll_sensors(void)
{
    LaneData tmp[NUM_LANES];
    if (read_sensors(tmp) == NUM_LANES) {
        memcpy(g_live, tmp, sizeof tmp);
        return 1;
    }
    return 0;
}

/* ============================================================================
 *  SECTION 7 - DASHBOARD RENDERING (ANSI)
 * ==========================================================================*/

/* Column widths (visible). Combined width = (5+sumL) + 3 gutter + (5+sumR). */
static const int  LW[4] = { 6, 10, 12, 11 };                 /* left table  */
static const int  RW[4] = { 11, 7, 13, 15 };                 /* right table */
static const char *LH[4] = { "Lane", "Vehicles", "Status", "Green(s)" };
static const char *RH[4] = { "Crosswalk", "Peds", "Action", "Priority" };

#define LEFT_W   (5 + 6 + 10 + 12 + 11)   /* = 44 */
#define RIGHT_W  (5 + 11 + 7 + 13 + 15)   /* = 51 */
#define BOX_IN   96                        /* inner width of banner/feed boxes */

/* Build a horizontal box rule of given per-column widths and connector glyphs.
 * For a single column (n==1) only the left/right glyphs are used.            */
static void build_border(char *o, size_t cap, const int *w, int n,
                          const char *l, const char *m, const char *r,
                          const char *fill)
{
    o[0] = '\0';
    sapp(o, cap, l);
    for (int c = 0; c < n; c++) {
        for (int i = 0; i < w[c]; i++) sapp(o, cap, fill);
        sapp(o, cap, (c == n - 1) ? r : m);
    }
}

/* Build "|cellA|cellB|cellC|cellD|" where each cell is centred to its width
 * and wrapped in its colour. ANSI codes are zero-width, so alignment with the
 * borders (which share the same widths) is preserved. */
static void build_row(char *o, size_t cap, const int *w,
                      const char *col[4], const char *txt[4])
{
    o[0] = '\0';
    sapp(o, cap, "\u2502");                       /* │ */
    for (int c = 0; c < 4; c++) {
        char cell[80];
        center(cell, sizeof cell, txt[c], w[c]);
        sapp(o, cap, col[c]);
        sapp(o, cap, cell);
        sapp(o, cap, C_RESET);
        sapp(o, cap, "\u2502");                   /* │ */
    }
}

/* --- colour pickers --------------------------------------------------------*/
static const char *dens_color(DensityClass d)
{
    switch (d) {
        case DENS_LOW:    return FG_GREEN;
        case DENS_MEDIUM: return FG_BYELLOW;
        default:          return FG_BRED;          /* HIGH / SEVERE */
    }
}
static const char *ped_color(PedClass p)
{
    switch (p) {
        case PED_FEW:      return FG_GREEN;
        case PED_MODERATE: return FG_BYELLOW;
        default:           return FG_BRED;
    }
}
static const char *sig_color(SignalState s)
{
    switch (s) {
        case SIG_GREEN:  return FG_BGREEN;
        case SIG_YELLOW: return FG_BYELLOW;
        default:         return FG_BRED;
    }
}
static const char *sig_text(SignalState s)
{
    switch (s) {
        case SIG_GREEN:  return "GREEN";
        case SIG_YELLOW: return "AMBER";
        default:         return "RED";
    }
}

/* A progress bar of `width` cells reflecting done/total. */
static void build_bar(char *o, size_t cap, int done, int total, int width)
{
    o[0] = '\0';
    if (total <= 0) { for (int i = 0; i < width; i++) sapp(o, cap, " "); return; }
    int fill = (int)((double)done / total * width + 0.5);
    if (fill > width) fill = width;
    if (fill < 0) fill = 0;
    for (int i = 0; i < fill; i++)         sapp(o, cap, "\u2593");  /* ▓ */
    for (int i = 0; i < width - fill; i++) sapp(o, cap, "\u2591");  /* ░ */
}

/* --- the ASCII banner ------------------------------------------------------*/
static void render_banner(void)
{
    int one = BOX_IN;
    char top[512], bot[512];
    build_border(top, sizeof top, &one, 1, "\u2554", "", "\u2557", "\u2550"); /* ╔═╗ */
    build_border(bot, sizeof bot, &one, 1, "\u255A", "", "\u255D", "\u2550"); /* ╚═╝ */

    /* traffic-light motif, centred over the box */
    int pad = (BOX_IN - 11) / 2 + 1;
    printf("\n%*s" FG_BRED C_BOLD "(\u25CF) " C_RESET
                    FG_BYELLOW C_BOLD "(\u25CF) " C_RESET
                    FG_BGREEN  C_BOLD "(\u25CF)" C_RESET "\n", pad, "");

    char t1[256], t2[256];
    center(t1, sizeof t1,
           "A I - B A S E D   S M A R T   T R A F F I C   "
           "M A N A G E M E N T   S Y S T E M", BOX_IN);
    center(t2, sizeof t2,
           "Decision-Tree AI   *   Rule-Based Control   *   "
           "Adaptive Timing   *   Live Demo", BOX_IN);

    printf(FG_BCYAN "%s\n" C_RESET, top);
    printf(FG_BCYAN "\u2551" C_RESET C_BOLD FG_BWHITE "%s" C_RESET FG_BCYAN "\u2551\n" C_RESET, t1);
    printf(FG_BCYAN "\u2551" C_RESET FG_CYAN        "%s" C_RESET FG_BCYAN "\u2551\n" C_RESET, t2);
    printf(FG_BCYAN "%s\n" C_RESET, bot);
}

/* --- top status strip (mode / active lane / phase / countdown bar) ---------*/
static void render_status(const Plan *p, const char *phase, int remaining, int total)
{
    const char *mc = (p->mode == MODE_EMERGENCY) ? FG_BRED
                   : (p->mode == MODE_NIGHT)     ? FG_BBLUE
                                                 : FG_BGREEN;
    char active[8];
    if (p->active_lane >= 0) snprintf(active, sizeof active, "Lane %c", p->lanes[p->active_lane].name);
    else                     snprintf(active, sizeof active, "--");

    char bar[64];
    build_bar(bar, sizeof bar, (total > 0 ? total - remaining : 0), total, 14);

    char ts[32]; timestamp(ts, sizeof ts);

    printf("  " C_BOLD "MODE:" C_RESET " %s%-9s" C_RESET
           "  " C_BOLD "ACTIVE:" C_RESET " " FG_BWHITE "%-7s" C_RESET
           "  " C_BOLD "PHASE:" C_RESET " %s%-6s" C_RESET
           "  " C_BOLD "TIME:" C_RESET " " FG_BCYAN "%2d/%-2d" C_RESET
           "  [%s%s" C_RESET "]\n",
           mc, mode_name(p->mode), active,
           sig_color(phase[0] == 'A' ? SIG_YELLOW :
                     phase[0] == 'R' ? SIG_RED : SIG_GREEN), phase,
           remaining, total, mc, bar);
    printf("  " C_DIM "Cycle %-5ld  %s" C_RESET "\n\n", p->cycle, ts);
}

/* --- the two side-by-side metric tables ------------------------------------*/
static void render_tables(const Plan *p)
{
    char ltop[512], lmid[512], lbot[512];
    char rtop[512], rmid[512], rbot[512];

    build_border(ltop, sizeof ltop, LW, 4, "\u250C", "\u252C", "\u2510", "\u2500");
    build_border(lmid, sizeof lmid, LW, 4, "\u251C", "\u253C", "\u2524", "\u2500");
    build_border(lbot, sizeof lbot, LW, 4, "\u2514", "\u2534", "\u2518", "\u2500");
    build_border(rtop, sizeof rtop, RW, 4, "\u250C", "\u252C", "\u2510", "\u2500");
    build_border(rmid, sizeof rmid, RW, 4, "\u251C", "\u253C", "\u2524", "\u2500");
    build_border(rbot, sizeof rbot, RW, 4, "\u2514", "\u2534", "\u2518", "\u2500");

    /* header rows */
    const char *cyan4[4] = { FG_BCYAN, FG_BCYAN, FG_BCYAN, FG_BCYAN };
    char lhdr[256], rhdr[256];
    build_row(lhdr, sizeof lhdr, LW, cyan4, LH);
    build_row(rhdr, sizeof rhdr, RW, cyan4, RH);

    /* section captions sit above each table */
    printf("  " FG_BCYAN C_BOLD "%-*s" C_RESET "   " FG_BCYAN C_BOLD "%s\n" C_RESET,
           LEFT_W, "LANE METRICS", "PEDESTRIAN & EMERGENCY STATUS");

    printf("  %s   %s\n", ltop, rtop);
    printf("  %s   %s\n", lhdr, rhdr);
    printf("  %s   %s\n", lmid, rmid);

    for (int i = 0; i < NUM_LANES; i++) {
        const LanePlan *L = &p->lanes[i];

        /* ---- left row cells ---- */
        char veh[16], grn[16], name[8];
        snprintf(name, sizeof name, "%c", L->name);
        snprintf(veh, sizeof veh, "%d", L->vehicles);
        if (L->skipped) snprintf(grn, sizeof grn, "--");
        else            snprintf(grn, sizeof grn, "%d", L->green_time);

        const char *lname_c = (i == p->active_lane) ? FG_BGREEN : FG_BWHITE;
        const char *stat_c, *stat_t;
        if (L->skipped) { stat_c = FG_DIM;            stat_t = "SKIP"; }
        else            { stat_c = sig_color(L->signal); stat_t = sig_text(L->signal); }

        const char *lcol[4] = { lname_c, dens_color(L->density), stat_c, FG_BCYAN };
        const char *ltxt[4] = { name, veh, stat_t, grn };
        char lrow[256];
        build_row(lrow, sizeof lrow, LW, lcol, ltxt);

        /* ---- right row cells ---- */
        char cross[16], peds[16];
        snprintf(cross, sizeof cross, "Cross-%c", L->name);
        snprintf(peds, sizeof peds, "%d", L->pedestrians);

        const char *act_c, *act_t;
        if (p->mode == MODE_EMERGENCY) { act_c = FG_BYELLOW; act_t = "HOLD"; }
        else if (L->skipped)           { act_c = FG_DIM;     act_t = "--"; }
        else if (L->signal == SIG_RED) { act_c = FG_BGREEN;  act_t = "WALK"; }
        else                           { act_c = FG_BRED;    act_t = "DONT WALK"; }

        const char *pri_c, *pri_t;
        if (L->emergency)                                   { pri_c = FG_BRED;   pri_t = "** EMERGENCY **"; }
        else if (i == p->active_lane && L->signal == SIG_GREEN){ pri_c = FG_BGREEN; pri_t = "> SERVING"; }
        else                                                { pri_c = FG_DIM;    pri_t = "-"; }

        const char *rcol[4] = { FG_BWHITE, ped_color(L->ped_class), act_c, pri_c };
        const char *rtxt[4] = { cross, peds, act_t, pri_t };
        char rrow[256];
        build_row(rrow, sizeof rrow, RW, rcol, rtxt);

        printf("  %s   %s\n", lrow, rrow);
    }
    printf("  %s   %s\n", lbot, rbot);
}

/* --- bubble-sorted priority queue strip ------------------------------------*/
static void render_priority(const Plan *p)
{
    printf("  " FG_BCYAN C_BOLD "PRIORITY QUEUE" C_RESET
           " " C_DIM "(bubble-sorted by density)" C_RESET ":  ");
    for (int k = 0; k < NUM_LANES; k++) {
        const LanePlan *L = &p->lanes[p->order[k]];
        printf(FG_BWHITE "%c" C_RESET "%s(%d)" C_RESET,
               L->name, dens_color(L->density), L->vehicles);
        if (k < NUM_LANES - 1) printf(C_DIM " > " C_RESET);
    }
    printf("\n\n");
}

/* --- the live decision feed (boxed) ----------------------------------------*/
static void render_feed(void)
{
    int one = BOX_IN;
    char top[512], mid[512], bot[512];
    build_border(top, sizeof top, &one, 1, "\u250C", "", "\u2510", "\u2500");
    build_border(mid, sizeof mid, &one, 1, "\u251C", "", "\u2524", "\u2500");
    build_border(bot, sizeof bot, &one, 1, "\u2514", "", "\u2518", "\u2500");

    char cap[128];
    center(cap, sizeof cap, "L I V E   D E C I S I O N   F E E D", BOX_IN);

    printf("  " FG_BCYAN "%s\n" C_RESET, top);
    printf("  " FG_BCYAN "\u2502" C_RESET C_BOLD FG_BCYAN "%s" C_RESET FG_BCYAN "\u2502\n" C_RESET, cap);
    printf("  " FG_BCYAN "%s\n" C_RESET, mid);

    for (int i = 0; i < FEED_LINES; i++) {
        char body[256];
        if (i < g_feed_count) {
            char padded[160];
            ljust(padded, sizeof padded, g_feed[i].text, BOX_IN - 1);
            snprintf(body, sizeof body, "%s %s%s",
                     g_feed[i].color ? g_feed[i].color : C_RESET, padded, C_RESET);
        } else {
            char padded[160];
            ljust(padded, sizeof padded, "", BOX_IN - 1);
            snprintf(body, sizeof body, " %s", padded);
        }
        printf("  " FG_BCYAN "\u2502" C_RESET "%s" FG_BCYAN "\u2502\n" C_RESET, body);
    }
    printf("  " FG_BCYAN "%s\n" C_RESET, bot);
}

/* The full repaint. */
static void render(const Plan *p, const char *phase, int remaining, int total)
{
    fputs(CLEAR_SCREEN, stdout);
    render_banner();
    render_status(p, phase, remaining, total);
    render_tables(p);
    render_priority(p);
    render_feed();
    fflush(stdout);
}

/* "Waiting for sensor feed" splash before the first snapshot exists. */
static void render_waiting(int ticks)
{
    fputs(CLEAR_SCREEN, stdout);
    render_banner();
    const char *dots = (ticks % 3 == 0) ? "." : (ticks % 3 == 1) ? ".." : "...";
    printf("\n  " FG_BYELLOW "Waiting for sensor feed (%s)%s" C_RESET "\n",
           SENSOR_FILE, dots);
    printf("  " C_DIM "Start the producer in another terminal:  ./sensor_sim" C_RESET "\n");
    fflush(stdout);
}

/* ============================================================================
 *  SECTION 8 - PHASE DRIVERS  (the decision-tree branches in action)
 *  Each driver counts a phase down one simulated second per SIM_TICK_MS, and
 *  re-polls the sensors every tick so an emergency pre-empts within ~1 tick.
 *  Return value: 1 = pre-empted by emergency, 0 = ran to completion.
 * ==========================================================================*/

/* Count down one signal phase, repainting each simulated second. Returns 1 if
 * an emergency appeared mid-phase (caller should bail out and re-plan). */
static int countdown(Plan *p, const char *phase, int seconds, int watch_emergency)
{
    for (int rem = seconds; rem > 0 && g_running; rem--) {
        poll_sensors();                 /* refresh live data each tick      */
        refresh_counts(p);              /* update tables without changing   */
                                        /* the signals this phase set       */
        if (watch_emergency && detect_emergency(g_live) >= 0) {
            notify("EMERGENCY", FG_BRED,
                   "Priority vehicle detected mid-cycle - pre-empting");
            return 1;
        }
        render(p, phase, rem, seconds);
        sleep_ms(SIM_TICK_MS);
    }
    return 0;
}

/* --- BRANCH 1 : EMERGENCY PRIORITY OVERRIDE -------------------------------- */
static void run_emergency(Plan *p)
{
    int el = detect_emergency(g_live);
    if (el < 0) return;                 /* defensive */

    p->mode = MODE_EMERGENCY;
    for (int i = 0; i < NUM_LANES; i++)
        p->lanes[i].signal = (i == el) ? SIG_GREEN : SIG_RED;
    p->active_lane = el;
    apply_coordination(p);              /* all crossings HOLD */

    notify("EMERGENCY", FG_BRED,
           "OVERRIDE: Lane %c GREEN, all others RED, pedestrians HELD",
           p->lanes[el].name);

    /* Hold GREEN for the emergency window, but exit early if it clears or the
     * priority vehicle moves to a different approach. */
    for (int rem = EMERGENCY_GREEN; rem > 0 && g_running; rem--) {
        poll_sensors();
        refresh_counts(p);

        int now = detect_emergency(g_live);
        if (now < 0) {
            notify("SYSTEM", FG_BGREEN,
                   "Emergency cleared - resuming adaptive control");
            break;
        }
        if (now != el) {                /* re-target the override */
            el = now;
            for (int i = 0; i < NUM_LANES; i++)
                p->lanes[i].signal = (i == el) ? SIG_GREEN : SIG_RED;
            p->active_lane = el;
            apply_coordination(p);
            notify("EMERGENCY", FG_BRED, "Re-targeting override to Lane %c",
                   p->lanes[el].name);
        }
        render(p, "GREEN", rem, EMERGENCY_GREEN);
        sleep_ms(SIM_TICK_MS);
    }

    /* All-red clearance before normal control resumes. */
    for (int i = 0; i < NUM_LANES; i++) p->lanes[i].signal = SIG_RED;
    p->active_lane = -1;
    apply_coordination(p);
    countdown(p, "RED", ALLRED_CLEAR, 0);
}

/* --- BRANCH 2 : NIGHT MODE (rapid cycle / dynamic skip) -------------------- */
static int run_night(Plan *p)
{
    p->mode = MODE_NIGHT;
    notify("NIGHT", FG_BBLUE,
           "Night Mode ACTIVE - rapid cycle, dynamic skip, no blinking amber");

    for (int k = 0; k < NUM_LANES && g_running; k++) {
        int idx = p->order[k];

        /* DYNAMIC SKIP: drop completely empty approaches. */
        if (g_live[idx].vehicles == 0) {
            for (int i = 0; i < NUM_LANES; i++) p->lanes[i].signal = SIG_RED;
            p->lanes[idx].skipped = 1;
            p->active_lane = -1;
            apply_coordination(p);
            notify("SYSTEM", FG_BBLUE,
                   "Skipping empty Lane %c (0 vehicles)", p->lanes[idx].name);
            if (countdown(p, "RED", ALLRED_CLEAR, 1)) return 1;
            p->lanes[idx].skipped = 0;
            continue;
        }

        /* Compressed GREEN, others RED. */
        for (int i = 0; i < NUM_LANES; i++)
            p->lanes[i].signal = (i == idx) ? SIG_GREEN : SIG_RED;
        p->active_lane = idx;
        apply_coordination(p);

        int g = night_green(g_live[idx].vehicles);
        notify("NIGHT", FG_BGREEN,
               "Rapid GREEN Lane %c for %ds (%d veh)",
               p->lanes[idx].name, g, g_live[idx].vehicles);
        if (countdown(p, "GREEN", g, 1)) return 1;

        /* No amber in night mode (per spec): a brief all-red only. */
        for (int i = 0; i < NUM_LANES; i++) p->lanes[i].signal = SIG_RED;
        p->active_lane = -1;
        apply_coordination(p);
        if (countdown(p, "RED", ALLRED_CLEAR, 1)) return 1;
    }
    return 0;
}

/* --- BRANCH 3 : NORMAL ADAPTIVE CONTROL ------------------------------------ */
static int run_normal(Plan *p)
{
    p->mode = MODE_NORMAL;

    /* Serve lanes busiest-first (the bubble-sorted order). */
    for (int k = 0; k < NUM_LANES && g_running; k++) {
        int idx = p->order[k];

        /* GREEN for the served lane, RED for the rest -> coordination then
         * lets pedestrians WALK on every RED approach. */
        for (int i = 0; i < NUM_LANES; i++)
            p->lanes[i].signal = (i == idx) ? SIG_GREEN : SIG_RED;
        p->active_lane = idx;
        apply_coordination(p);

        notify("SYSTEM", FG_BGREEN,
               "Lane %c GREEN %ds (%s, %d veh) | WALK on all RED crossings",
               p->lanes[idx].name, p->lanes[idx].green_time,
               density_name(p->lanes[idx].density), p->lanes[idx].vehicles);

        if (countdown(p, "GREEN", p->lanes[idx].green_time, 1)) return 1;

        /* Fixed AMBER clearance (safe, non-blinking), lane still "active". */
        p->lanes[idx].signal = SIG_YELLOW;
        apply_coordination(p);
        if (countdown(p, "AMBER", YELLOW_TIME, 1)) return 1;

        /* All-red gap before the next lane. */
        p->lanes[idx].signal = SIG_RED;
        p->active_lane = -1;
        apply_coordination(p);
        if (countdown(p, "RED", ALLRED_CLEAR, 1)) return 1;
    }
    return 0;
}

/* ============================================================================
 *  SECTION 9 - signal handling + main loop
 * ==========================================================================*/
static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;        /* the phase loops poll this and unwind cleanly */
}

int main(void)
{
    signal(SIGINT, on_sigint);
    fputs(HIDE_CURSOR, stdout);

    notify("SYSTEM", FG_BCYAN, "Controller online - awaiting sensor feed");

    /* Wait until the producer has written a first complete snapshot. */
    int ticks = 0;
    while (g_running && !poll_sensors()) {
        render_waiting(ticks++);
        sleep_seconds(1);
    }

    Plan plan;
    long cycle = 0;

    /* MAIN CONTROL LOOP: read -> decide (decision tree) -> drive -> repeat.  */
    while (g_running) {
        poll_sensors();                       /* freshest data           */
        build_plan(g_live, &plan, cycle++);   /* run the decision tree   */
        log_stats(&plan);                     /* persistent audit block  */

        switch (plan.mode) {                  /* dispatch the chosen branch */
            case MODE_EMERGENCY: run_emergency(&plan); break;
            case MODE_NIGHT:     run_night(&plan);     break;  /* may pre-empt */
            case MODE_NORMAL:    run_normal(&plan);    break;  /* may pre-empt */
        }
        /* On a pre-empt the branch simply returns; the loop re-reads and
         * re-plans, which will now select the EMERGENCY branch. */
    }

    /* Graceful shutdown: restore the terminal. */
    fputs(SHOW_CURSOR C_RESET, stdout);
    fputs(CLEAR_SCREEN, stdout);
    notify("SYSTEM", FG_BCYAN, "Controller shutting down (operator stop)");
    printf("AI Smart Traffic controller stopped. Audit trail: %s\n", LOG_FILE);
    return 0;
}
