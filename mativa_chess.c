/*
 * mativa_chess.c
 *
 * A compact single-file chess engine built in the same spirit as mativa_ajt.c:
 * one translation unit, no external dependencies, explicit data layout, and
 * minimal hidden startup work. The hot path avoids heap allocation entirely.
 *
 * Build examples:
 *   gcc -O3 -DNDEBUG mativa_chess.c -o mativa_chess.exe -lgdi32
 *   clang -O3 -DNDEBUG mativa_chess.c -o mativa_chess.exe -lgdi32
 *   cl /O2 /DNDEBUG mativa_chess.c /Fe:mativa_chess.exe
 *
 * Launching:
 *   mativa_chess.exe            -> GUI on Windows when stdin is not piped
 *   mativa_chess.exe gui        -> force GUI
 *   mativa_chess.exe --uci      -> force console/UCI mode
 *
 * UCI commands:
 *   uci
 *   isready
 *   ucinewgame
 *   position startpos [moves ...]
 *   position fen <fen> [moves ...]
 *   go depth N
 *   go movetime N
 *   go ponder [wtime <ms> btime <ms> [winc <ms>] [binc <ms>] [movestogo N]]
 *   go wtime <ms> btime <ms> [winc <ms>] [binc <ms>] [movestogo N]
 *   ponderhit
 *   stop
 *   quit
 *
 * Extra commands:
 *   d
 *   perft N
 *   divide N
 */

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#endif

typedef uint64_t U64;
typedef uint32_t U32;
typedef int16_t I16;
typedef int64_t I64;

enum { WHITE = 0, BLACK = 1, BOTH = 2 };
enum { PAWN = 0, KNIGHT = 1, BISHOP = 2, ROOK = 3, QUEEN = 4, KING = 5 };
enum {
    WP = 0, WN, WB, WR, WQ, WK,
    BP, BN, BB, BR, BQ, BK,
    NO_PIECE = -1
};

enum {
    CASTLE_WK = 1,
    CASTLE_WQ = 2,
    CASTLE_BK = 4,
    CASTLE_BQ = 8
};

enum {
    FLAG_NONE = 0,
    FLAG_EP = 1,
    FLAG_CASTLE = 2,
    FLAG_DOUBLE = 4
};

enum {
    TT_NONE = 0,
    TT_ALPHA = 1,
    TT_BETA = 2,
    TT_EXACT = 3
};

#define MAX_MOVES 256
#define MAX_PLY 128
#define MAX_HISTORY 2048
#define INF 32000
#define MATE_SCORE 30000
#define MATE_BOUND 29000
#define TT_BITS 19
#define TT_SIZE (1u << TT_BITS)
#define TT_MASK (TT_SIZE - 1u)
#define EVAL_BITS 16
#define EVAL_SIZE (1u << EVAL_BITS)
#define EVAL_MASK (EVAL_SIZE - 1u)
#define PAWN_BITS 15
#define PAWN_SIZE (1u << PAWN_BITS)
#define PAWN_MASK (PAWN_SIZE - 1u)
#define REUSE_BITS 8
#define REUSE_SIZE (1u << REUSE_BITS)
#define REUSE_MASK (REUSE_SIZE - 1u)
#define ROOK_TABLE_SIZE 4096
#define BISHOP_TABLE_SIZE 512
#define HISTORY_MAX 16384
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define BIT(sq) (1ULL << (sq))
#define FILE_OF(sq) ((sq) & 7)
#define RANK_OF(sq) ((sq) >> 3)
#define SQ(file, rank) (((rank) << 3) | (file))

#define MOVE_ENCODE(from, to, piece, captured, promo, flags) \
    ((U32)(from) | ((U32)(to) << 6) | ((U32)((piece) & 15) << 12) | \
     ((U32)(((captured) + 1) & 15) << 16) | ((U32)((promo) & 15) << 20) | \
     ((U32)((flags) & 15) << 24))
#define MOVE_FROM(m) ((int)((m) & 63))
#define MOVE_TO(m) ((int)(((m) >> 6) & 63))
#define MOVE_PIECE(m) ((int)(((m) >> 12) & 15))
#define MOVE_CAPTURED(m) (((int)(((m) >> 16) & 15)) - 1)
#define MOVE_PROMO(m) ((int)(((m) >> 20) & 15))
#define MOVE_FLAGS(m) ((int)(((m) >> 24) & 15))

typedef struct {
    U32 move;
    int score;
} MoveEntry;

typedef struct {
    MoveEntry moves[MAX_MOVES];
    int count;
} MoveList;

typedef struct {
    int castling;
    int ep;
    int halfmove;
    U64 key;
    U64 pawn_key;
} Undo;

typedef struct {
    int ep;
    int halfmove;
    U64 key;
} NullUndo;

typedef struct {
    U64 key;
    U32 move;
    I16 score;
    int8_t depth;
    uint8_t flag;
} TTEntry;

typedef struct {
    U64 key;
    I16 score;
} EvalEntry;

typedef struct {
    U64 key;
    I16 mg[2];
    I16 eg[2];
    U64 attacks[2];
} PawnEntry;

typedef struct {
    U64 bb[12];
    U64 occ[3];
    int board[64];
    int king_sq[2];
    int side;
    int castling;
    int ep;
    int halfmove;
    int fullmove;
    U64 key;
    U64 pawn_key;
    U64 rep_keys[MAX_HISTORY];
    int rep_len;
} Position;

typedef struct {
    int depth;
    U64 node_limit;
    I64 movetime_ms;
    I64 wtime_ms;
    I64 btime_ms;
    I64 winc_ms;
    I64 binc_ms;
    int movestogo;
    int infinite;
    int ponder;
} SearchLimits;

typedef struct {
    I64 start_ms;
    I64 soft_stop_ms;
    I64 hard_stop_ms;
    U64 node_limit;
    U64 nodes;
    int stop;
    int quit;
    int root_depth;
    U32 pv[MAX_PLY][MAX_PLY];
    int pv_len[MAX_PLY];
    U32 killers[2][MAX_PLY];
    U32 countermove[12][64];
    U32 current_move[MAX_PLY];
    int history[12][64];
    int capture_history[12][64][6];
} SearchInfo;

typedef struct {
    U64 key;
    U32 move;
} SearchReuseEntry;

typedef struct {
    SearchReuseEntry entries[REUSE_SIZE];
} SearchReuse;

typedef struct {
    int valid;
    int is_startpos;
    char base_fen[128];
    char moves[MAX_HISTORY][6];
    int move_count;
    Position pos;
} PositionCache;

typedef struct {
    const char *name;
    const char *fen;
    int depth;
    U64 expected;
} SelfTestCase;

typedef struct {
    const char *name;
    U64 calls;
    U64 sampled_calls;
    U64 est_total_us;
    U32 sample_mask;
} LatencySlot;

typedef struct {
    int enabled;
    LatencySlot slots[16];
} LatencyProfiler;

#ifdef _WIN32
typedef struct {
    RECT board;
    RECT panel;
    RECT btn_time_minus;
    RECT btn_time_plus;
    RECT btn_white;
    RECT btn_black;
    RECT btn_flip;
    int cell;
} GuiLayout;

typedef struct {
    Position pos;
    HWND hwnd;
    HANDLE engine_thread;
    int human_side;
    int flip;
    int selected_sq;
    U64 target_mask;
    U32 last_move;
    int engine_thinking;
    int engine_pondering;
    int game_over;
    int engine_ms;
    unsigned search_cookie;
    U32 ponder_move;
    char live_pv[256];
    HFONT piece_font;
    HFONT text_font;
    int piece_font_px;
    char status[256];
} GuiState;

typedef struct {
    HWND hwnd;
    Position pos;
    SearchLimits limits;
    unsigned cookie;
    U32 best_move;
    U32 ponder_move;
    int mode;
} GuiSearchTask;

typedef struct {
    unsigned cookie;
    int mode;
    int depth;
    int score;
    char pv[256];
} GuiSearchInfoMsg;

typedef struct {
    HWND hwnd;
    unsigned cookie;
    int mode;
    int enabled;
} GuiSearchReportTarget;

#define GUI_SEARCH_MOVE 1
#define GUI_SEARCH_PONDER 2
#define GUI_WM_ENGINE_DONE (WM_APP + 1)
#define GUI_WM_SEARCH_INFO (WM_APP + 2)
#define GUI_CMD_PROMO_QUEEN 1001
#define GUI_CMD_PROMO_ROOK 1002
#define GUI_CMD_PROMO_BISHOP 1003
#define GUI_CMD_PROMO_KNIGHT 1004
#endif

static const char *STARTPOS_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static const SelfTestCase SELFTEST_CASES[] = {
    { "startpos-d5", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609ULL },
    { "kiwipete-d4", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603ULL },
    { "endgame-d4", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238ULL },
    { "midgame-d3", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9467ULL },
    { "buggy-d3", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379ULL }
};

static const char *BENCH_FENS[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    "4rrk1/2p1qppp/p1np1n2/1p2p3/4P3/1NN1BP2/PPP1Q1PP/2KR3R w - - 2 16"
};

static const int PIECE_TYPE[12] = {
    PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING,
    PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING
};

static const int PIECE_COLOR[12] = {
    WHITE, WHITE, WHITE, WHITE, WHITE, WHITE,
    BLACK, BLACK, BLACK, BLACK, BLACK, BLACK
};

static const int TYPE_PHASE[6] = { 0, 1, 1, 2, 4, 0 };
static const int PIECE_VALUE_MG[6] = { 100, 320, 335, 500, 910, 0 };
static const int PIECE_VALUE_EG[6] = { 120, 300, 320, 520, 900, 0 };
static const int PASSED_BONUS_MG[8] = { 0, 5, 10, 20, 35, 55, 85, 0 };
static const int PASSED_BONUS_EG[8] = { 0, 8, 16, 30, 55, 90, 140, 0 };
static const char PIECE_TO_CHAR[] = "PNBRQKpnbrqk";
#ifdef _WIN32
static const WCHAR *GUI_PIECE_GLYPHS[12] = {
    L"\x2659", L"\x2658", L"\x2657", L"\x2656", L"\x2655", L"\x2654",
    L"\x265F", L"\x265E", L"\x265D", L"\x265C", L"\x265B", L"\x265A"
};
#endif

static U64 pawn_attacks[2][64];
static U64 knight_attacks[64];
static U64 king_attacks[64];
static U64 ray_north[64];
static U64 ray_south[64];
static U64 ray_east[64];
static U64 ray_west[64];
static U64 ray_ne[64];
static U64 ray_nw[64];
static U64 ray_se[64];
static U64 ray_sw[64];
static U64 ortho_lines[64];
static U64 diag_lines[64];
static U64 between_masks[64][64];
static uint8_t castle_rights_mask[64];
static U64 rook_occ_masks[64];
static U64 bishop_occ_masks[64];
static U64 rook_attack_table[64][ROOK_TABLE_SIZE];
static U64 bishop_attack_table[64][BISHOP_TABLE_SIZE];
static int8_t rook_rel_index[64][64];
static int8_t bishop_rel_index[64][64];
static U64 file_masks[8];
static U64 isolated_masks[64];
static U64 passed_masks[2][64];

static U64 zobrist_piece[12][64];
static U64 zobrist_castle[16];
static U64 zobrist_ep[8];
static U64 zobrist_side;

static TTEntry g_tt[TT_SIZE];
static EvalEntry g_eval[EVAL_SIZE];
static PawnEntry g_pawn[PAWN_SIZE];
static SearchInfo g_si;
static SearchReuse g_reuse;
static PositionCache g_pos_cache;
static int g_emit_search_info = 1;
static LatencyProfiler g_latprof;
static U64 g_eval_cache_probes;
static U64 g_eval_cache_hits;
static U64 g_pawn_cache_probes;
static U64 g_pawn_cache_hits;
static SearchLimits g_active_limits;
static int g_active_root_side;
static int g_active_ponder;
static int g_active_ponderhit;
static int g_uci_ponder_enabled = 1;
#ifdef _WIN32
static GuiSearchReportTarget g_gui_report_target;
#endif

static int engine_initialized = 0;

static void setup_classic_startpos(Position *pos);
static U32 search_best_move(Position *pos, const SearchLimits *limits);
static void search_activate_ponderhit(void);
static void init_search_limits(SearchLimits *limits);
#ifdef _WIN32
static void gui_start_engine_search(GuiState *gs);
static void gui_cancel_engine_search(GuiState *gs);
#endif
static I64 now_us(void);
static void init_engine(void);
#ifdef _WIN32
static int run_gui(int argc, char **argv);
#endif

enum {
    LAT_INIT_ENGINE = 0,
    LAT_INIT_MASKS,
    LAT_INIT_ZOBRIST,
    LAT_SEARCH_BEST,
    LAT_GENERATE_MOVES,
    LAT_EVALUATE,
    LAT_MAKE_MOVE,
    LAT_UNMAKE_MOVE,
    LAT_SQUARE_ATTACKED,
    LAT_BISHOP_ATTACKS,
    LAT_ROOK_ATTACKS,
    LAT_SLOT_COUNT
};

#define LATENCY_SCOPE(slot_id) \
    U64 __lat_start = 0; \
    LatencySlot *__lat_slot = NULL; \
    int __lat_take = 0; \
    if (g_latprof.enabled) { \
        __lat_slot = &g_latprof.slots[(slot_id)]; \
        __lat_slot->calls++; \
        if ((__lat_slot->calls & __lat_slot->sample_mask) == 0u) { \
            __lat_slot->sampled_calls++; \
            __lat_take = 1; \
            __lat_start = (U64)now_us(); \
        } \
    }

#define LATENCY_SCOPE_END() \
    do { \
        if (__lat_take) { \
            __lat_slot->est_total_us += ((U64)now_us() - __lat_start) * (U64)(__lat_slot->sample_mask + 1u); \
        } \
    } while (0)

static int piece_from_char(int c) {
    switch (c) {
    case 'P': return WP;
    case 'N': return WN;
    case 'B': return WB;
    case 'R': return WR;
    case 'Q': return WQ;
    case 'K': return WK;
    case 'p': return BP;
    case 'n': return BN;
    case 'b': return BB;
    case 'r': return BR;
    case 'q': return BQ;
    case 'k': return BK;
    default: return NO_PIECE;
    }
}

static char promo_char_from_type(int promo) {
    switch (promo) {
    case KNIGHT: return 'n';
    case BISHOP: return 'b';
    case ROOK: return 'r';
    case QUEEN: return 'q';
    default: return '\0';
    }
}

static int piece_for_color_type(int color, int type) {
    return color == WHITE ? type : type + 6;
}

static int lsb_index(U64 bb) {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long idx = 0;
    _BitScanForward64(&idx, bb);
    return (int)idx;
#elif defined(__GNUC__) || defined(__clang__)
    return (int)__builtin_ctzll(bb);
#else
    int idx = 0;
    while ((bb & 1ULL) == 0ULL) {
        bb >>= 1;
        idx++;
    }
    return idx;
#endif
}

static int popcount64(U64 bb) {
#if defined(__GNUC__) || defined(__clang__)
    return (int)__builtin_popcountll(bb);
#else
    int n = 0;
    while (bb) {
        bb &= bb - 1ULL;
        n++;
    }
    return n;
#endif
}

static int pop_lsb(U64 *bb) {
    U64 b = *bb;
    int sq = lsb_index(b);
    *bb = b & (b - 1ULL);
    return sq;
}

static I64 now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (I64)((counter.QuadPart * 1000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (I64)ts.tv_sec * 1000LL + (I64)ts.tv_nsec / 1000000LL;
#endif
}

static I64 now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (I64)((counter.QuadPart * 1000000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (I64)ts.tv_sec * 1000000LL + (I64)ts.tv_nsec / 1000LL;
#endif
}

static void latprof_reset(void) {
    static const struct {
        const char *name;
        U32 mask;
    } init[LAT_SLOT_COUNT] = {
        { "init_engine", 0u },
        { "init_masks", 0u },
        { "init_zobrist", 0u },
        { "search_best_move", 0u },
        { "generate_moves", 255u },
        { "evaluate", 255u },
        { "make_move", 255u },
        { "unmake_move", 255u },
        { "square_attacked", 511u },
        { "bishop_attacks_from", 1023u },
        { "rook_attacks_from", 1023u }
    };
    int i;
    memset(&g_latprof, 0, sizeof(g_latprof));
    g_eval_cache_probes = 0ULL;
    g_eval_cache_hits = 0ULL;
    g_pawn_cache_probes = 0ULL;
    g_pawn_cache_hits = 0ULL;
    for (i = 0; i < LAT_SLOT_COUNT; i++) {
        g_latprof.slots[i].name = init[i].name;
        g_latprof.slots[i].sample_mask = init[i].mask;
    }
}

static void latprof_print_report(void) {
    int order[LAT_SLOT_COUNT];
    U64 total_us = 0ULL;
    int i, j;

    for (i = 0; i < LAT_SLOT_COUNT; i++) {
        order[i] = i;
        total_us += g_latprof.slots[i].est_total_us;
    }
    for (i = 0; i < LAT_SLOT_COUNT; i++) {
        for (j = i + 1; j < LAT_SLOT_COUNT; j++) {
            if (g_latprof.slots[order[j]].est_total_us > g_latprof.slots[order[i]].est_total_us) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    printf("latency report (sampled internal engine costs)\n");
    for (i = 0; i < LAT_SLOT_COUNT; i++) {
        const LatencySlot *slot = &g_latprof.slots[order[i]];
        double pct = total_us ? (100.0 * (double)slot->est_total_us / (double)total_us) : 0.0;
        double avg_ns = slot->calls ? ((double)slot->est_total_us * 1000.0 / (double)slot->calls) : 0.0;
        if (slot->est_total_us == 0ULL && slot->calls == 0ULL) continue;
        printf("  %-20s est=%9.3f ms  share=%6.2f%%  calls=%10llu  sampled=%8llu  avg=%8.1f ns\n",
               slot->name,
               (double)slot->est_total_us / 1000.0,
               pct,
               (unsigned long long)slot->calls,
               (unsigned long long)slot->sampled_calls,
               avg_ns);
    }
    if (g_eval_cache_probes) {
        double hit_pct = (100.0 * (double)g_eval_cache_hits) / (double)g_eval_cache_probes;
        printf("  %-20s hits=%10llu/%-10llu rate=%6.2f%%\n",
               "eval_cache",
               (unsigned long long)g_eval_cache_hits,
               (unsigned long long)g_eval_cache_probes,
               hit_pct);
    }
    if (g_pawn_cache_probes) {
        double hit_pct = (100.0 * (double)g_pawn_cache_hits) / (double)g_pawn_cache_probes;
        printf("  %-20s hits=%10llu/%-10llu rate=%6.2f%%\n",
               "pawn_cache",
               (unsigned long long)g_pawn_cache_hits,
               (unsigned long long)g_pawn_cache_probes,
               hit_pct);
    }
    fflush(stdout);
}

static U64 splitmix64(U64 *state) {
    U64 z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void clear_position(Position *pos) {
    memset(pos, 0, sizeof(*pos));
    memset(pos->board, 0xff, sizeof(pos->board));
    pos->king_sq[WHITE] = -1;
    pos->king_sq[BLACK] = -1;
    pos->ep = -1;
    pos->side = WHITE;
    pos->fullmove = 1;
}

static void place_piece(Position *pos, int piece, int sq) {
    U64 bit = BIT(sq);
    pos->board[sq] = piece;
    pos->bb[piece] |= bit;
    pos->occ[PIECE_COLOR[piece]] |= bit;
    pos->occ[BOTH] |= bit;
    if (PIECE_TYPE[piece] == KING) pos->king_sq[PIECE_COLOR[piece]] = sq;
    pos->key ^= zobrist_piece[piece][sq];
    if (PIECE_TYPE[piece] == PAWN) pos->pawn_key ^= zobrist_piece[piece][sq];
}

static void remove_piece(Position *pos, int piece, int sq) {
    U64 bit = BIT(sq);
    pos->board[sq] = NO_PIECE;
    pos->bb[piece] &= ~bit;
    pos->occ[PIECE_COLOR[piece]] &= ~bit;
    pos->occ[BOTH] &= ~bit;
    if (PIECE_TYPE[piece] == KING) pos->king_sq[PIECE_COLOR[piece]] = -1;
    pos->key ^= zobrist_piece[piece][sq];
    if (PIECE_TYPE[piece] == PAWN) pos->pawn_key ^= zobrist_piece[piece][sq];
}

static void move_piece(Position *pos, int piece, int from, int to) {
    U64 from_bit = BIT(from);
    U64 to_bit = BIT(to);
    U64 move_bits = from_bit | to_bit;
    int color = PIECE_COLOR[piece];

    pos->board[from] = NO_PIECE;
    pos->board[to] = piece;
    pos->bb[piece] ^= move_bits;
    pos->occ[color] ^= move_bits;
    pos->occ[BOTH] ^= move_bits;
    if (PIECE_TYPE[piece] == KING) pos->king_sq[color] = to;
    pos->key ^= zobrist_piece[piece][from] ^ zobrist_piece[piece][to];
    if (PIECE_TYPE[piece] == PAWN) pos->pawn_key ^= zobrist_piece[piece][from] ^ zobrist_piece[piece][to];
}

static void place_piece_state(Position *pos, int piece, int sq) {
    U64 bit = BIT(sq);
    pos->board[sq] = piece;
    pos->bb[piece] |= bit;
    pos->occ[PIECE_COLOR[piece]] |= bit;
    pos->occ[BOTH] |= bit;
    if (PIECE_TYPE[piece] == KING) pos->king_sq[PIECE_COLOR[piece]] = sq;
}

static void remove_piece_state(Position *pos, int piece, int sq) {
    U64 bit = BIT(sq);
    pos->board[sq] = NO_PIECE;
    pos->bb[piece] &= ~bit;
    pos->occ[PIECE_COLOR[piece]] &= ~bit;
    pos->occ[BOTH] &= ~bit;
    if (PIECE_TYPE[piece] == KING) pos->king_sq[PIECE_COLOR[piece]] = -1;
}

static void move_piece_state(Position *pos, int piece, int from, int to) {
    U64 from_bit = BIT(from);
    U64 to_bit = BIT(to);
    U64 move_bits = from_bit | to_bit;
    int color = PIECE_COLOR[piece];

    pos->board[from] = NO_PIECE;
    pos->board[to] = piece;
    pos->bb[piece] ^= move_bits;
    pos->occ[color] ^= move_bits;
    pos->occ[BOTH] ^= move_bits;
    if (PIECE_TYPE[piece] == KING) pos->king_sq[color] = to;
}

static void set_ep_square(Position *pos, int sq) {
    if (pos->ep != -1) pos->key ^= zobrist_ep[FILE_OF(pos->ep)];
    pos->ep = sq;
    if (pos->ep != -1) pos->key ^= zobrist_ep[FILE_OF(pos->ep)];
}

static void set_castling(Position *pos, int rights) {
    pos->key ^= zobrist_castle[pos->castling];
    pos->castling = rights;
    pos->key ^= zobrist_castle[pos->castling];
}

static void update_castling_for_square(Position *pos, int sq) {
    int rights = pos->castling & castle_rights_mask[sq];
    if (rights != pos->castling) set_castling(pos, rights);
}

static U64 build_rook_attack(int sq, U64 occ) {
    U64 attacks = 0ULL;
    int file = FILE_OF(sq);
    int rank = RANK_OF(sq);
    int f, r;

    for (r = rank + 1; r < 8; r++) {
        int to = SQ(file, r);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    for (r = rank - 1; r >= 0; r--) {
        int to = SQ(file, r);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    for (f = file + 1; f < 8; f++) {
        int to = SQ(f, rank);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    for (f = file - 1; f >= 0; f--) {
        int to = SQ(f, rank);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    return attacks;
}

static U64 build_bishop_attack(int sq, U64 occ) {
    U64 attacks = 0ULL;
    int file = FILE_OF(sq);
    int rank = RANK_OF(sq);
    int f, r;

    for (f = file + 1, r = rank + 1; f < 8 && r < 8; f++, r++) {
        int to = SQ(f, r);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    for (f = file - 1, r = rank + 1; f >= 0 && r < 8; f--, r++) {
        int to = SQ(f, r);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    for (f = file + 1, r = rank - 1; f < 8 && r >= 0; f++, r--) {
        int to = SQ(f, r);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    for (f = file - 1, r = rank - 1; f >= 0 && r >= 0; f--, r--) {
        int to = SQ(f, r);
        attacks |= BIT(to);
        if (occ & BIT(to)) break;
    }
    return attacks;
}

static unsigned slider_index_from_occ(U64 occ, const int8_t rel_index[64]) {
    unsigned index = 0u;
    while (occ) {
        int sq = lsb_index(occ);
        occ &= occ - 1ULL;
        index |= 1u << (unsigned)rel_index[sq];
    }
    return index;
}

static U64 rook_attacks_raw(int sq, U64 occ) {
    return rook_attack_table[sq][slider_index_from_occ(occ & rook_occ_masks[sq], rook_rel_index[sq])];
}

static U64 bishop_attacks_raw(int sq, U64 occ) {
    return bishop_attack_table[sq][slider_index_from_occ(occ & bishop_occ_masks[sq], bishop_rel_index[sq])];
}

static U64 rook_attacks_from(int sq, U64 occ) {
    LATENCY_SCOPE(LAT_ROOK_ATTACKS);
    U64 attacks = rook_attacks_raw(sq, occ);
    LATENCY_SCOPE_END();
    return attacks;
}

static U64 bishop_attacks_from(int sq, U64 occ) {
    LATENCY_SCOPE(LAT_BISHOP_ATTACKS);
    U64 attacks = bishop_attacks_raw(sq, occ);
    LATENCY_SCOPE_END();
    return attacks;
}

static int square_attacked(const Position *pos, int sq, int by_side) {
    LATENCY_SCOPE(LAT_SQUARE_ATTACKED);
    U64 occ = pos->occ[BOTH];
    U64 bishops;
    U64 rooks;
    U64 candidates;

    if (by_side == WHITE) {
        if (pawn_attacks[BLACK][sq] & pos->bb[WP]) {
            LATENCY_SCOPE_END();
            return 1;
        }
        if (knight_attacks[sq] & pos->bb[WN]) {
            LATENCY_SCOPE_END();
            return 1;
        }
        if (king_attacks[sq] & pos->bb[WK]) {
            LATENCY_SCOPE_END();
            return 1;
        }
        bishops = pos->bb[WB] | pos->bb[WQ];
        rooks = pos->bb[WR] | pos->bb[WQ];
    } else {
        if (pawn_attacks[WHITE][sq] & pos->bb[BP]) {
            LATENCY_SCOPE_END();
            return 1;
        }
        if (knight_attacks[sq] & pos->bb[BN]) {
            LATENCY_SCOPE_END();
            return 1;
        }
        if (king_attacks[sq] & pos->bb[BK]) {
            LATENCY_SCOPE_END();
            return 1;
        }
        bishops = pos->bb[BB] | pos->bb[BQ];
        rooks = pos->bb[BR] | pos->bb[BQ];
    }

    candidates = bishops & diag_lines[sq];
    while (candidates) {
        int from = pop_lsb(&candidates);
        if ((between_masks[sq][from] & occ) == 0ULL) {
            LATENCY_SCOPE_END();
            return 1;
        }
    }

    candidates = rooks & ortho_lines[sq];
    while (candidates) {
        int from = pop_lsb(&candidates);
        if ((between_masks[sq][from] & occ) == 0ULL) {
            LATENCY_SCOPE_END();
            return 1;
        }
    }
    LATENCY_SCOPE_END();
    return 0;
}

static int king_square(const Position *pos, int side) {
    return pos->king_sq[side];
}

static int in_check(const Position *pos, int side) {
    return square_attacked(pos, king_square(pos, side), side ^ 1);
}

static void add_move(MoveList *list, U32 move) {
    if (list->count < MAX_MOVES) {
        list->moves[list->count].move = move;
        list->moves[list->count].score = 0;
        list->count++;
    }
}

static void add_promotions(MoveList *list, int from, int to, int piece, int captured, int flags) {
    add_move(list, MOVE_ENCODE(from, to, piece, captured, KNIGHT, flags));
    add_move(list, MOVE_ENCODE(from, to, piece, captured, BISHOP, flags));
    add_move(list, MOVE_ENCODE(from, to, piece, captured, ROOK, flags));
    add_move(list, MOVE_ENCODE(from, to, piece, captured, QUEEN, flags));
}

static void generate_moves(const Position *pos, MoveList *list, int captures_only) {
    LATENCY_SCOPE(LAT_GENERATE_MOVES);
    int side = pos->side;
    int enemy = side ^ 1;
    U64 own_occ = pos->occ[side];
    U64 enemy_occ = pos->occ[enemy];
    U64 occ = pos->occ[BOTH];
    U64 bb;
    int from, to, file, rank, captured;
    U64 attacks;

    list->count = 0;

    if (side == WHITE) {
        bb = pos->bb[WP];
        while (bb) {
            from = pop_lsb(&bb);
            file = FILE_OF(from);
            rank = RANK_OF(from);
            if (!captures_only) {
                to = from + 8;
                if (to < 64 && pos->board[to] == NO_PIECE) {
                    if (rank == 6) add_promotions(list, from, to, WP, NO_PIECE, FLAG_NONE);
                    else add_move(list, MOVE_ENCODE(from, to, WP, NO_PIECE, 0, FLAG_NONE));
                    if (rank == 1 && pos->board[from + 16] == NO_PIECE) {
                        add_move(list, MOVE_ENCODE(from, from + 16, WP, NO_PIECE, 0, FLAG_DOUBLE));
                    }
                }
            }
            if (file > 0) {
                to = from + 7;
                if (to < 64 && (enemy_occ & BIT(to))) {
                    captured = pos->board[to];
                    if (rank == 6) add_promotions(list, from, to, WP, captured, FLAG_NONE);
                    else add_move(list, MOVE_ENCODE(from, to, WP, captured, 0, FLAG_NONE));
                } else if (to == pos->ep) {
                    add_move(list, MOVE_ENCODE(from, to, WP, BP, 0, FLAG_EP));
                }
            }
            if (file < 7) {
                to = from + 9;
                if (to < 64 && (enemy_occ & BIT(to))) {
                    captured = pos->board[to];
                    if (rank == 6) add_promotions(list, from, to, WP, captured, FLAG_NONE);
                    else add_move(list, MOVE_ENCODE(from, to, WP, captured, 0, FLAG_NONE));
                } else if (to == pos->ep) {
                    add_move(list, MOVE_ENCODE(from, to, WP, BP, 0, FLAG_EP));
                }
            }
        }

        bb = pos->bb[WN];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = knight_attacks[from] & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, WN, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[WB];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = bishop_attacks_from(from, occ) & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, WB, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[WR];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = rook_attacks_from(from, occ) & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, WR, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[WQ];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = (rook_attacks_from(from, occ) | bishop_attacks_from(from, occ)) & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, WQ, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[WK];
        if (bb) {
            from = lsb_index(bb);
            attacks = king_attacks[from] & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, WK, captured, 0, FLAG_NONE));
            }
            if (!captures_only && from == 4 && !in_check(pos, WHITE)) {
                if ((pos->castling & CASTLE_WK) &&
                    pos->board[5] == NO_PIECE && pos->board[6] == NO_PIECE &&
                    !square_attacked(pos, 5, BLACK) && !square_attacked(pos, 6, BLACK)) {
                    add_move(list, MOVE_ENCODE(4, 6, WK, NO_PIECE, 0, FLAG_CASTLE));
                }
                if ((pos->castling & CASTLE_WQ) &&
                    pos->board[3] == NO_PIECE && pos->board[2] == NO_PIECE && pos->board[1] == NO_PIECE &&
                    !square_attacked(pos, 3, BLACK) && !square_attacked(pos, 2, BLACK)) {
                    add_move(list, MOVE_ENCODE(4, 2, WK, NO_PIECE, 0, FLAG_CASTLE));
                }
            }
        }
    } else {
        bb = pos->bb[BP];
        while (bb) {
            from = pop_lsb(&bb);
            file = FILE_OF(from);
            rank = RANK_OF(from);
            if (!captures_only) {
                to = from - 8;
                if (to >= 0 && pos->board[to] == NO_PIECE) {
                    if (rank == 1) add_promotions(list, from, to, BP, NO_PIECE, FLAG_NONE);
                    else add_move(list, MOVE_ENCODE(from, to, BP, NO_PIECE, 0, FLAG_NONE));
                    if (rank == 6 && pos->board[from - 16] == NO_PIECE) {
                        add_move(list, MOVE_ENCODE(from, from - 16, BP, NO_PIECE, 0, FLAG_DOUBLE));
                    }
                }
            }
            if (file > 0) {
                to = from - 9;
                if (to >= 0 && (enemy_occ & BIT(to))) {
                    captured = pos->board[to];
                    if (rank == 1) add_promotions(list, from, to, BP, captured, FLAG_NONE);
                    else add_move(list, MOVE_ENCODE(from, to, BP, captured, 0, FLAG_NONE));
                } else if (to == pos->ep) {
                    add_move(list, MOVE_ENCODE(from, to, BP, WP, 0, FLAG_EP));
                }
            }
            if (file < 7) {
                to = from - 7;
                if (to >= 0 && (enemy_occ & BIT(to))) {
                    captured = pos->board[to];
                    if (rank == 1) add_promotions(list, from, to, BP, captured, FLAG_NONE);
                    else add_move(list, MOVE_ENCODE(from, to, BP, captured, 0, FLAG_NONE));
                } else if (to == pos->ep) {
                    add_move(list, MOVE_ENCODE(from, to, BP, WP, 0, FLAG_EP));
                }
            }
        }

        bb = pos->bb[BN];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = knight_attacks[from] & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, BN, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[BB];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = bishop_attacks_from(from, occ) & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, BB, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[BR];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = rook_attacks_from(from, occ) & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, BR, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[BQ];
        while (bb) {
            from = pop_lsb(&bb);
            attacks = (rook_attacks_from(from, occ) | bishop_attacks_from(from, occ)) & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, BQ, captured, 0, FLAG_NONE));
            }
        }

        bb = pos->bb[BK];
        if (bb) {
            from = lsb_index(bb);
            attacks = king_attacks[from] & ~own_occ;
            while (attacks) {
                to = pop_lsb(&attacks);
                captured = pos->board[to];
                if (captures_only && captured == NO_PIECE) continue;
                add_move(list, MOVE_ENCODE(from, to, BK, captured, 0, FLAG_NONE));
            }
            if (!captures_only && from == 60 && !in_check(pos, BLACK)) {
                if ((pos->castling & CASTLE_BK) &&
                    pos->board[61] == NO_PIECE && pos->board[62] == NO_PIECE &&
                    !square_attacked(pos, 61, WHITE) && !square_attacked(pos, 62, WHITE)) {
                    add_move(list, MOVE_ENCODE(60, 62, BK, NO_PIECE, 0, FLAG_CASTLE));
                }
                if ((pos->castling & CASTLE_BQ) &&
                    pos->board[59] == NO_PIECE && pos->board[58] == NO_PIECE && pos->board[57] == NO_PIECE &&
                    !square_attacked(pos, 59, WHITE) && !square_attacked(pos, 58, WHITE)) {
                    add_move(list, MOVE_ENCODE(60, 58, BK, NO_PIECE, 0, FLAG_CASTLE));
                }
            }
        }
    }
    LATENCY_SCOPE_END();
}

static int make_move(Position *pos, U32 move, Undo *undo) {
    LATENCY_SCOPE(LAT_MAKE_MOVE);
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    int piece = MOVE_PIECE(move);
    int captured = MOVE_CAPTURED(move);
    int promo = MOVE_PROMO(move);
    int flags = MOVE_FLAGS(move);
    int side = pos->side;
    int king_sq;

    undo->castling = pos->castling;
    undo->ep = pos->ep;
    undo->halfmove = pos->halfmove;
    undo->key = pos->key;
    undo->pawn_key = pos->pawn_key;

    set_ep_square(pos, -1);

    if (captured != NO_PIECE) {
        if (flags & FLAG_EP) {
            int cap_sq = side == WHITE ? to - 8 : to + 8;
            remove_piece(pos, captured, cap_sq);
        } else {
            remove_piece(pos, captured, to);
        }
        update_castling_for_square(pos, to);
        pos->halfmove = 0;
    } else if (PIECE_TYPE[piece] == PAWN) {
        pos->halfmove = 0;
    } else {
        pos->halfmove++;
    }

    update_castling_for_square(pos, from);

    if (flags & FLAG_CASTLE) {
        if (piece == WK) {
            if (to == 6) move_piece(pos, WR, 7, 5);
            else move_piece(pos, WR, 0, 3);
        } else if (piece == BK) {
            if (to == 62) move_piece(pos, BR, 63, 61);
            else move_piece(pos, BR, 56, 59);
        }
    }

    remove_piece(pos, piece, from);
    if (promo) place_piece(pos, piece_for_color_type(side, promo), to);
    else place_piece(pos, piece, to);

    if (flags & FLAG_DOUBLE) {
        set_ep_square(pos, side == WHITE ? from + 8 : from - 8);
    }

    pos->side ^= 1;
    pos->key ^= zobrist_side;
    if (side == BLACK) pos->fullmove++;

    king_sq = pos->king_sq[side];
    if (square_attacked(pos, king_sq, side ^ 1)) {
        pos->side ^= 1;
        if (side == BLACK) pos->fullmove--;

        if (flags & FLAG_DOUBLE) set_ep_square(pos, -1);
        if (promo) {
            remove_piece_state(pos, piece_for_color_type(side, promo), to);
            place_piece_state(pos, piece, from);
        } else {
            move_piece_state(pos, piece, to, from);
        }
        if (flags & FLAG_CASTLE) {
            if (piece == WK) {
                if (to == 6) move_piece_state(pos, WR, 5, 7);
                else move_piece_state(pos, WR, 3, 0);
            } else if (piece == BK) {
                if (to == 62) move_piece_state(pos, BR, 61, 63);
                else move_piece_state(pos, BR, 59, 56);
            }
        }
        if (captured != NO_PIECE) {
            if (flags & FLAG_EP) {
                int cap_sq = side == WHITE ? to - 8 : to + 8;
                place_piece_state(pos, captured, cap_sq);
            } else {
                place_piece_state(pos, captured, to);
            }
        }
        pos->castling = undo->castling;
        pos->ep = undo->ep;
        pos->halfmove = undo->halfmove;
        pos->key = undo->key;
        pos->pawn_key = undo->pawn_key;
        LATENCY_SCOPE_END();
        return 0;
    }

    if (pos->rep_len < MAX_HISTORY) pos->rep_keys[pos->rep_len++] = pos->key;
    LATENCY_SCOPE_END();
    return 1;
}

static void unmake_move(Position *pos, U32 move, const Undo *undo) {
    LATENCY_SCOPE(LAT_UNMAKE_MOVE);
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    int piece = MOVE_PIECE(move);
    int captured = MOVE_CAPTURED(move);
    int promo = MOVE_PROMO(move);
    int flags = MOVE_FLAGS(move);
    int side = pos->side ^ 1;

    if (pos->rep_len > 0) pos->rep_len--;

    pos->side = side;
    if (side == BLACK) pos->fullmove--;

    if (promo) {
        remove_piece_state(pos, piece_for_color_type(side, promo), to);
        place_piece_state(pos, piece, from);
    } else {
        move_piece_state(pos, piece, to, from);
    }

    if (flags & FLAG_CASTLE) {
        if (piece == WK) {
            if (to == 6) move_piece_state(pos, WR, 5, 7);
            else move_piece_state(pos, WR, 3, 0);
        } else if (piece == BK) {
            if (to == 62) move_piece_state(pos, BR, 61, 63);
            else move_piece_state(pos, BR, 59, 56);
        }
    }

    if (captured != NO_PIECE) {
        if (flags & FLAG_EP) {
            int cap_sq = side == WHITE ? to - 8 : to + 8;
            place_piece_state(pos, captured, cap_sq);
        } else {
            place_piece_state(pos, captured, to);
        }
    }

    pos->castling = undo->castling;
    pos->ep = undo->ep;
    pos->halfmove = undo->halfmove;
    pos->key = undo->key;
    pos->pawn_key = undo->pawn_key;
    LATENCY_SCOPE_END();
}

static void make_null_move(Position *pos, NullUndo *undo) {
    undo->ep = pos->ep;
    undo->halfmove = pos->halfmove;
    undo->key = pos->key;
    set_ep_square(pos, -1);
    pos->side ^= 1;
    pos->key ^= zobrist_side;
    pos->halfmove++;
    if (pos->rep_len < MAX_HISTORY) pos->rep_keys[pos->rep_len++] = pos->key;
}

static void unmake_null_move(Position *pos, const NullUndo *undo) {
    if (pos->rep_len > 0) pos->rep_len--;
    pos->side ^= 1;
    pos->ep = undo->ep;
    pos->halfmove = undo->halfmove;
    pos->key = undo->key;
}

static int has_non_pawn_material(const Position *pos, int side) {
    if (side == WHITE) {
        return (pos->bb[WN] | pos->bb[WB] | pos->bb[WR] | pos->bb[WQ]) != 0ULL;
    }
    return (pos->bb[BN] | pos->bb[BB] | pos->bb[BR] | pos->bb[BQ]) != 0ULL;
}

static int insufficient_material(const Position *pos) {
    int wn, wb, bn, bb;
    if ((pos->bb[WP] | pos->bb[BP] | pos->bb[WR] | pos->bb[BR] | pos->bb[WQ] | pos->bb[BQ]) != 0ULL) return 0;
    wn = popcount64(pos->bb[WN]);
    wb = popcount64(pos->bb[WB]);
    bn = popcount64(pos->bb[BN]);
    bb = popcount64(pos->bb[BB]);
    if (wn + wb + bn + bb <= 1) return 1;
    if (wn == 0 && bn == 0 && wb <= 1 && bb <= 1) return 1;
    if (wb == 0 && bb == 0 && ((wn == 2 && bn == 0) || (bn == 2 && wn == 0))) return 1;
    return 0;
}

static int repetition_occurrences(const Position *pos) {
    int i;
    int count = 1;
    int limit = pos->rep_len - 1 - pos->halfmove;
    if (limit < 0) limit = 0;
    for (i = pos->rep_len - 3; i >= limit; i -= 2) {
        if (pos->rep_keys[i] == pos->key) count++;
    }
    return count;
}

static int is_repetition(const Position *pos) {
    return repetition_occurrences(pos) >= 3;
}

static int relative_rank(int color, int sq) {
    int rank = RANK_OF(sq);
    return color == WHITE ? rank : 7 - rank;
}

static int center_bonus(int sq) {
    int file = FILE_OF(sq);
    int rank = RANK_OF(sq);
    int df = file < 4 ? 3 - file : file - 4;
    int dr = rank < 4 ? 3 - rank : rank - 4;
    return 8 - (df + dr) * 2;
}

static int king_shield(const Position *pos, int color, int sq) {
    int file = FILE_OF(sq);
    int rank = RANK_OF(sq);
    int dir = color == WHITE ? 1 : -1;
    int score = 0;
    int f, r, s;
    for (f = file - 1; f <= file + 1; f++) {
        if (f < 0 || f > 7) continue;
        r = rank + dir;
        if (r < 0 || r > 7) continue;
        s = SQ(f, r);
        if (pos->board[s] == piece_for_color_type(color, PAWN)) score += 12;
        else score -= 6;
    }
    return score;
}

static int is_knight_home_square(int color, int sq) {
    return color == WHITE ? (sq == 1 || sq == 6) : (sq == 57 || sq == 62);
}

static int is_bishop_home_square(int color, int sq) {
    return color == WHITE ? (sq == 2 || sq == 5) : (sq == 58 || sq == 61);
}

static int is_rook_home_square(int color, int sq) {
    return color == WHITE ? (sq == 0 || sq == 7) : (sq == 56 || sq == 63);
}

static int is_queen_home_square(int color, int sq) {
    return color == WHITE ? sq == 3 : sq == 59;
}

static int pawn_supported_square(U64 pawn_control, int sq) {
    return (pawn_control & BIT(sq)) != 0ULL;
}

static int enemy_pawn_can_challenge(U64 enemy_pawns, int color, int sq) {
    return (passed_masks[color][sq] & isolated_masks[sq] & enemy_pawns) != 0ULL;
}

static void probe_pawn_eval(const Position *pos, int mg[2], int eg[2], U64 pawn_control[2]) {
    PawnEntry *entry = &g_pawn[(unsigned)(pos->pawn_key & PAWN_MASK)];
    U64 own_pawns[2] = { pos->bb[WP], pos->bb[BP] };
    U64 bb;
    int color;

    if (g_latprof.enabled) g_pawn_cache_probes++;
    if (entry->key == pos->pawn_key) {
        if (g_latprof.enabled) g_pawn_cache_hits++;
        mg[WHITE] = entry->mg[WHITE];
        mg[BLACK] = entry->mg[BLACK];
        eg[WHITE] = entry->eg[WHITE];
        eg[BLACK] = entry->eg[BLACK];
        pawn_control[WHITE] = entry->attacks[WHITE];
        pawn_control[BLACK] = entry->attacks[BLACK];
        return;
    }

    mg[WHITE] = mg[BLACK] = 0;
    eg[WHITE] = eg[BLACK] = 0;
    pawn_control[WHITE] = pawn_control[BLACK] = 0ULL;

    for (color = WHITE; color <= BLACK; color++) {
        bb = own_pawns[color];
        while (bb) {
            int sq = pop_lsb(&bb);
            int rel = relative_rank(color, sq);
            int cen = center_bonus(sq);
            int file = FILE_OF(sq);

            mg[color] += PIECE_VALUE_MG[PAWN] + rel * 8 + cen;
            eg[color] += PIECE_VALUE_EG[PAWN] + rel * 12 + cen;
            pawn_control[color] |= pawn_attacks[color][sq];

            if (popcount64(own_pawns[color] & file_masks[file]) > 1) {
                mg[color] -= 10;
                eg[color] -= 12;
            }
            if ((own_pawns[color] & isolated_masks[sq]) == 0ULL) {
                mg[color] -= 12;
                eg[color] -= 10;
            }
            if ((own_pawns[color ^ 1] & passed_masks[color][sq]) == 0ULL) {
                mg[color] += PASSED_BONUS_MG[rel];
                eg[color] += PASSED_BONUS_EG[rel];
            }
        }
    }

    entry->key = pos->pawn_key;
    entry->mg[WHITE] = (I16)mg[WHITE];
    entry->mg[BLACK] = (I16)mg[BLACK];
    entry->eg[WHITE] = (I16)eg[WHITE];
    entry->eg[BLACK] = (I16)eg[BLACK];
    entry->attacks[WHITE] = pawn_control[WHITE];
    entry->attacks[BLACK] = pawn_control[BLACK];
}

static int evaluate(const Position *pos) {
    EvalEntry *entry = &g_eval[(unsigned)(pos->key & EVAL_MASK)];
    int mg[2] = {0, 0};
    int eg[2] = {0, 0};
    int king_pressure[2] = {0, 0};
    int home_minors[2] = {0, 0};
    int home_rooks[2] = {0, 0};
    int queen_developed[2] = {0, 0};
    int phase = 0;
    U64 own_pawns[2];
    U64 pawn_control[2];
    U64 king_zone[2];
    U64 bb;
    int piece, color, type, sq, rel, cen, mob, file, home_dist;
    U64 occ = pos->occ[BOTH];
    U64 attacks;
    U64 raw_attacks;
    int bishops[2] = {0, 0};

    if (g_latprof.enabled) g_eval_cache_probes++;
    if (entry->key == pos->key) {
        if (g_latprof.enabled) g_eval_cache_hits++;
        return (int)entry->score;
    }

    LATENCY_SCOPE(LAT_EVALUATE);

    own_pawns[WHITE] = pos->bb[WP];
    own_pawns[BLACK] = pos->bb[BP];
    king_zone[WHITE] = king_attacks[pos->king_sq[WHITE]] | BIT(pos->king_sq[WHITE]);
    king_zone[BLACK] = king_attacks[pos->king_sq[BLACK]] | BIT(pos->king_sq[BLACK]);
    probe_pawn_eval(pos, mg, eg, pawn_control);
    king_pressure[WHITE] += popcount64(pawn_control[WHITE] & king_zone[BLACK]) * 2;
    king_pressure[BLACK] += popcount64(pawn_control[BLACK] & king_zone[WHITE]) * 2;

    for (piece = 0; piece < 12; piece++) {
        color = PIECE_COLOR[piece];
        type = PIECE_TYPE[piece];
        if (type == PAWN) continue;
        bb = pos->bb[piece];

        while (bb) {
            sq = pop_lsb(&bb);
            rel = relative_rank(color, sq);
            cen = center_bonus(sq);
            file = FILE_OF(sq);

            mg[color] += PIECE_VALUE_MG[type];
            eg[color] += PIECE_VALUE_EG[type];
            phase += TYPE_PHASE[type];

            if (type == KNIGHT) {
                attacks = knight_attacks[sq];
                mob = popcount64(attacks & ~pos->occ[color]);
                mg[color] += cen * 3 + mob * 4;
                eg[color] += cen * 2 + mob * 5;
                king_pressure[color] += popcount64(attacks & king_zone[color ^ 1]) * 4;
                if (rel >= 3 && file > 0 && file < 7 &&
                    pawn_supported_square(pawn_control[color], sq) &&
                    !enemy_pawn_can_challenge(own_pawns[color ^ 1], color, sq)) {
                    mg[color] += 16 + rel * 2;
                    eg[color] += 10 + rel;
                }
                if (is_knight_home_square(color, sq)) home_minors[color]++;
            } else if (type == BISHOP) {
                bishops[color]++;
                raw_attacks = bishop_attacks_from(sq, occ);
                attacks = raw_attacks & ~pos->occ[color];
                mob = popcount64(attacks);
                mg[color] += cen * 2 + mob * 4;
                eg[color] += cen * 2 + mob * 5;
                king_pressure[color] += popcount64(raw_attacks & king_zone[color ^ 1]) * 3;
                if (rel >= 2 && file > 0 && file < 7 &&
                    pawn_supported_square(pawn_control[color], sq) &&
                    !enemy_pawn_can_challenge(own_pawns[color ^ 1], color, sq)) {
                    mg[color] += 10 + rel * 2;
                    eg[color] += 8 + rel;
                }
                if (is_bishop_home_square(color, sq)) home_minors[color]++;
            } else if (type == ROOK) {
                int no_own = (own_pawns[color] & file_masks[file]) == 0ULL;
                int no_enemy = (own_pawns[color ^ 1] & file_masks[file]) == 0ULL;
                raw_attacks = rook_attacks_from(sq, occ);
                attacks = raw_attacks & ~pos->occ[color];
                mob = popcount64(attacks);
                mg[color] += rel * 2 + mob * 2;
                eg[color] += rel * 4 + mob * 3;
                if (no_own) mg[color] += no_enemy ? 20 : 10;
                if (no_own) eg[color] += no_enemy ? 25 : 12;
                if (rel == 6) {
                    mg[color] += 18;
                    eg[color] += 22;
                }
                if (raw_attacks & pos->bb[piece_for_color_type(color, ROOK)]) {
                    mg[color] += 12;
                    eg[color] += 16;
                }
                if (rel == 0 && mob <= 3) {
                    mg[color] -= 12 + (3 - mob) * 5;
                    eg[color] -= 4 + (3 - mob) * 2;
                    if (!no_own) mg[color] -= 8;
                    if (is_rook_home_square(color, sq) &&
                        ((color == WHITE && pos->king_sq[WHITE] == 4) ||
                         (color == BLACK && pos->king_sq[BLACK] == 60))) {
                        mg[color] -= 10;
                    }
                }
                king_pressure[color] += popcount64(raw_attacks & king_zone[color ^ 1]) * 5;
                if (is_rook_home_square(color, sq)) home_rooks[color]++;
            } else if (type == QUEEN) {
                raw_attacks = rook_attacks_from(sq, occ) | bishop_attacks_from(sq, occ);
                attacks = raw_attacks & ~pos->occ[color];
                mob = popcount64(attacks);
                mg[color] += cen + mob;
                eg[color] += cen * 2 + mob * 2;
                king_pressure[color] += popcount64(raw_attacks & king_zone[color ^ 1]) * 2;
                if (!is_queen_home_square(color, sq)) queen_developed[color] = 1;
            } else if (type == KING) {
                home_dist = color == WHITE ? RANK_OF(sq) : 7 - RANK_OF(sq);
                mg[color] += king_shield(pos, color, sq) - home_dist * 10 +
                             ((file == 1 || file == 6) ? 12 : 0) + (file == 2 ? 10 : 0) - cen * 2;
                eg[color] += cen * 4;
            }
        }
    }

    if (bishops[WHITE] >= 2) {
        mg[WHITE] += 28;
        eg[WHITE] += 36;
    }
    if (bishops[BLACK] >= 2) {
        mg[BLACK] += 28;
        eg[BLACK] += 36;
    }

    if (phase >= 16) {
        mg[WHITE] -= home_minors[WHITE] * 10;
        mg[BLACK] -= home_minors[BLACK] * 10;
        if (home_rooks[WHITE] == 2 && pos->king_sq[WHITE] == 4) mg[WHITE] -= 10;
        if (home_rooks[BLACK] == 2 && pos->king_sq[BLACK] == 60) mg[BLACK] -= 10;
        if (queen_developed[WHITE] && home_minors[WHITE] >= 3) mg[WHITE] -= 14;
        if (queen_developed[BLACK] && home_minors[BLACK] >= 3) mg[BLACK] -= 14;
    }

    mg[WHITE] += king_pressure[WHITE];
    mg[BLACK] += king_pressure[BLACK];

    if (phase > 24) phase = 24;

    {
        int mg_score = mg[WHITE] - mg[BLACK];
        int eg_score = eg[WHITE] - eg[BLACK];
        int score = (mg_score * phase + eg_score * (24 - phase)) / 24;
        score += pos->side == WHITE ? 8 : -8;
        score = pos->side == WHITE ? score : -score;
        entry->key = pos->key;
        entry->score = (I16)score;
        LATENCY_SCOPE_END();
        return score;
    }
}

static int score_to_tt(int score, int ply) {
    if (score > MATE_BOUND) return score + ply;
    if (score < -MATE_BOUND) return score - ply;
    return score;
}

static int score_from_tt(int score, int ply) {
    if (score > MATE_BOUND) return score - ply;
    if (score < -MATE_BOUND) return score + ply;
    return score;
}

static TTEntry *tt_probe(U64 key) {
    return &g_tt[(unsigned)(key & TT_MASK)];
}

static void tt_store(U64 key, int depth, int score, int flag, U32 move, int ply) {
    TTEntry *e = tt_probe(key);
    if (e->key == key && e->depth > depth && flag != TT_EXACT) return;
    e->key = key;
    e->move = move;
    e->score = (I16)score_to_tt(score, ply);
    e->depth = (int8_t)depth;
    e->flag = (uint8_t)flag;
}

static U64 attackers_to_square(const U64 bb[12], U64 occ, int sq, int side) {
    U64 attackers = 0ULL;

    attackers |= pawn_attacks[side ^ 1][sq] & bb[piece_for_color_type(side, PAWN)];
    attackers |= knight_attacks[sq] & bb[piece_for_color_type(side, KNIGHT)];
    attackers |= king_attacks[sq] & bb[piece_for_color_type(side, KING)];
    attackers |= bishop_attacks_raw(sq, occ) &
                 (bb[piece_for_color_type(side, BISHOP)] | bb[piece_for_color_type(side, QUEEN)]);
    attackers |= rook_attacks_raw(sq, occ) &
                 (bb[piece_for_color_type(side, ROOK)] | bb[piece_for_color_type(side, QUEEN)]);
    return attackers;
}

static int least_valuable_attacker_square(const U64 bb[12], U64 attackers, int side, int *piece_out) {
    static const int order[6] = { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
    int i;
    for (i = 0; i < 6; i++) {
        int piece = piece_for_color_type(side, order[i]);
        U64 hits = attackers & bb[piece];
        if (hits) {
            *piece_out = piece;
            return lsb_index(hits);
        }
    }
    *piece_out = NO_PIECE;
    return -1;
}

static int see_value_of_move(const Position *pos, U32 move) {
    U64 bb[12];
    U64 occ;
    int gain[32];
    int side = pos->side;
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    int piece = MOVE_PIECE(move);
    int captured = MOVE_CAPTURED(move);
    int promo = MOVE_PROMO(move);
    int flags = MOVE_FLAGS(move);
    int moving_piece = promo ? piece_for_color_type(side, promo) : piece;
    int cap_sq = (flags & FLAG_EP) ? (side == WHITE ? to - 8 : to + 8) : to;
    int depth = 0;
    int current_piece = moving_piece;
    U64 attackers;

    if (captured == NO_PIECE && !(flags & FLAG_EP) && !promo) return 0;

    memcpy(bb, pos->bb, sizeof(bb));
    occ = pos->occ[BOTH];

    gain[0] = (flags & FLAG_EP) ? PIECE_VALUE_MG[PAWN] :
              (captured != NO_PIECE ? PIECE_VALUE_MG[PIECE_TYPE[captured]] : 0);
    if (promo) gain[0] += PIECE_VALUE_MG[promo] - PIECE_VALUE_MG[PAWN];

    bb[piece] &= ~BIT(from);
    occ &= ~BIT(from);
    if (captured != NO_PIECE) {
        bb[captured] &= ~BIT(cap_sq);
        occ &= ~BIT(cap_sq);
    }
    bb[moving_piece] |= BIT(to);
    occ |= BIT(to);

    side ^= 1;
    while (1) {
        int attacker_piece;
        int attacker_sq;

        attackers = attackers_to_square(bb, occ, to, side);
        attacker_sq = least_valuable_attacker_square(bb, attackers, side, &attacker_piece);
        if (attacker_sq == -1) break;

        depth++;
        if (depth >= ARRAY_LEN(gain)) break;
        gain[depth] = PIECE_VALUE_MG[PIECE_TYPE[current_piece]] - gain[depth - 1];

        bb[current_piece] &= ~BIT(to);
        bb[attacker_piece] &= ~BIT(attacker_sq);
        occ &= ~BIT(attacker_sq);
        bb[attacker_piece] |= BIT(to);
        occ |= BIT(to);
        current_piece = attacker_piece;
        side ^= 1;
    }

    while (--depth > 0) {
        if (-gain[depth] < gain[depth - 1]) gain[depth - 1] = -gain[depth];
    }
    return gain[0];
}

static int score_capture(U32 move) {
    int captured = MOVE_CAPTURED(move);
    int piece = MOVE_PIECE(move);
    int promo = MOVE_PROMO(move);
    int score = 0;
    if (captured != NO_PIECE) {
        score += 10000 + PIECE_VALUE_MG[PIECE_TYPE[captured]] * 16 - PIECE_VALUE_MG[PIECE_TYPE[piece]];
    }
    if (promo) score += 8000 + PIECE_VALUE_MG[promo];
    if (MOVE_FLAGS(move) & FLAG_EP) score += 10000;
    return score;
}

static int capture_history_index(U32 move) {
    int captured = MOVE_CAPTURED(move);
    if (MOVE_FLAGS(move) & FLAG_EP) return PAWN;
    if (captured != NO_PIECE) return PIECE_TYPE[captured];
    return MOVE_PROMO(move) ? MOVE_PROMO(move) : PAWN;
}

static int history_bonus_for_depth(int depth, int cutoff) {
    int bonus = depth * depth * (cutoff ? 32 : 16) + depth * 8;
    if (bonus > HISTORY_MAX / 2) bonus = HISTORY_MAX / 2;
    return bonus;
}

static void update_history_score(int piece, int to, int delta) {
    int *entry = &g_si.history[piece][to];
    int magnitude = delta >= 0 ? delta : -delta;

    if (magnitude > HISTORY_MAX / 2) magnitude = HISTORY_MAX / 2;
    if (delta >= 0) {
        *entry += magnitude - (*entry * magnitude) / HISTORY_MAX;
    } else {
        *entry -= magnitude + (*entry * magnitude) / HISTORY_MAX;
    }
    if (*entry > HISTORY_MAX) *entry = HISTORY_MAX;
    if (*entry < -HISTORY_MAX) *entry = -HISTORY_MAX;
}

static void update_capture_history_score(U32 move, int delta) {
    int piece = MOVE_PIECE(move);
    int to = MOVE_TO(move);
    int idx = capture_history_index(move);
    int *entry = &g_si.capture_history[piece][to][idx];
    int magnitude = delta >= 0 ? delta : -delta;

    if (magnitude > HISTORY_MAX / 2) magnitude = HISTORY_MAX / 2;
    if (delta >= 0) {
        *entry += magnitude - (*entry * magnitude) / HISTORY_MAX;
    } else {
        *entry -= magnitude + (*entry * magnitude) / HISTORY_MAX;
    }
    if (*entry > HISTORY_MAX) *entry = HISTORY_MAX;
    if (*entry < -HISTORY_MAX) *entry = -HISTORY_MAX;
}

static void update_capture_heuristics(U32 best_move, const U32 *captures, int capture_count, int depth, int cutoff) {
    int bonus;
    int i;

    if (!best_move) return;
    bonus = history_bonus_for_depth(depth, cutoff);
    update_capture_history_score(best_move, bonus);
    for (i = 0; i < capture_count; i++) {
        U32 move = captures[i];
        if (move == best_move) continue;
        update_capture_history_score(move, -(bonus / 2));
    }
}

static void update_quiet_heuristics(int ply, int depth, U32 best_move,
                                    const U32 *quiets, int quiet_count, int cutoff) {
    int bonus;
    int i;

    if (!best_move) return;
    bonus = history_bonus_for_depth(depth, cutoff);
    update_history_score(MOVE_PIECE(best_move), MOVE_TO(best_move), bonus);

    if (ply > 0) {
        U32 prev = g_si.current_move[ply - 1];
        if (prev) g_si.countermove[MOVE_PIECE(prev)][MOVE_TO(prev)] = best_move;
    }

    for (i = 0; i < quiet_count; i++) {
        U32 move = quiets[i];
        if (move == best_move) continue;
        update_history_score(MOVE_PIECE(move), MOVE_TO(move), -(bonus / 2));
    }
}

static U32 reuse_move_for_position(const Position *pos) {
    SearchReuseEntry *e = &g_reuse.entries[(unsigned)(pos->key & REUSE_MASK)];
    return e->key == pos->key ? e->move : 0;
}

static void score_moves(const Position *pos, MoveList *list, int ply, U32 tt_move) {
    U32 reuse_move = tt_move ? 0 : reuse_move_for_position(pos);
    U32 countermove = 0;
    int i;

    if (ply > 0) {
        U32 prev = g_si.current_move[ply - 1];
        if (prev) countermove = g_si.countermove[MOVE_PIECE(prev)][MOVE_TO(prev)];
    }

    for (i = 0; i < list->count; i++) {
        U32 move = list->moves[i].move;
        int piece = MOVE_PIECE(move);
        int to = MOVE_TO(move);
        int captured = MOVE_CAPTURED(move);

        if (move == tt_move) list->moves[i].score = 100000000;
        else if (move == reuse_move) list->moves[i].score = 95000000;
        else if (captured != NO_PIECE || MOVE_FLAGS(move) & FLAG_EP || MOVE_PROMO(move)) {
            list->moves[i].score = score_capture(move) +
                                  g_si.capture_history[piece][to][capture_history_index(move)];
        } else if (move == g_si.killers[0][ply]) {
            list->moves[i].score = 9000000;
        } else if (move == g_si.killers[1][ply]) {
            list->moves[i].score = 8000000;
        } else if (move == countermove) {
            list->moves[i].score = 7000000 + g_si.history[piece][to];
        } else {
            list->moves[i].score = g_si.history[piece][to];
        }
    }
    (void)pos;
}

static int move_is_quiet(U32 move) {
    return MOVE_CAPTURED(move) == NO_PIECE && !(MOVE_FLAGS(move) & FLAG_EP) && MOVE_PROMO(move) == 0;
}

static void stage_moves(const Position *pos, MoveList *list) {
    MoveEntry staged[MAX_MOVES];
    int write = 0;
    int stage, i;

    for (stage = 0; stage < 4; stage++) {
        for (i = 0; i < list->count; i++) {
            U32 move = list->moves[i].move;
            int is_quiet = move_is_quiet(move);
            int current_stage;

            if (list->moves[i].score >= 7000000) current_stage = 0;
            else if (is_quiet) current_stage = 2;
            else if (MOVE_PROMO(move)) current_stage = 1;
            else current_stage = see_value_of_move(pos, move) >= 0 ? 1 : 3;

            if (current_stage == stage) staged[write++] = list->moves[i];
        }
    }

    memcpy(list->moves, staged, (size_t)list->count * sizeof(MoveEntry));
}

static void pick_next_move(MoveList *list, int index) {
    int best = index;
    int i;
    MoveEntry tmp;
    for (i = index + 1; i < list->count; i++) {
        if (list->moves[i].score > list->moves[best].score) best = i;
    }
    if (best != index) {
        tmp = list->moves[index];
        list->moves[index] = list->moves[best];
        list->moves[best] = tmp;
    }
}

static int stdin_has_line(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (h == INVALID_HANDLE_VALUE || h == NULL) return 0;
    if (GetFileType(h) != FILE_TYPE_PIPE) return 0;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return 0;
    return avail > 0;
#else
    fd_set readfds;
    struct timeval tv;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) > 0;
#endif
}

static void poll_stop_input(void) {
    char line[256];
    if (!stdin_has_line()) return;
    if (!fgets(line, sizeof(line), stdin)) return;
    if (strncmp(line, "stop", 4) == 0) g_si.stop = 1;
    else if (strncmp(line, "ponderhit", 9) == 0) search_activate_ponderhit();
    else if (strncmp(line, "quit", 4) == 0) {
        g_si.stop = 1;
        g_si.quit = 1;
    } else if (strncmp(line, "isready", 7) == 0) {
        printf("readyok\n");
        fflush(stdout);
    }
}

static void check_search_limits(void) {
    if ((g_si.nodes & 2047ULL) != 0ULL) return;
    if (g_si.hard_stop_ms && now_ms() >= g_si.hard_stop_ms) g_si.stop = 1;
    if (g_si.node_limit && g_si.nodes >= g_si.node_limit) g_si.stop = 1;
    poll_stop_input();
}

static int quiescence(Position *pos, int alpha, int beta, int ply) {
    MoveList list;
    U32 captures[MAX_MOVES];
    TTEntry *tt;
    U32 tt_move = 0;
    int stand_pat;
    int i;
    int legal = 0;
    int capture_count = 0;
    int incheck = in_check(pos, pos->side);
    int stand_ready = 0;
    int old_alpha = alpha;
    U32 best_move = 0;

    g_si.pv_len[ply] = ply;

    if (g_si.stop) return 0;
    g_si.nodes++;
    check_search_limits();
    if (g_si.stop) return 0;

    if (ply >= MAX_PLY - 1) return evaluate(pos);

    tt = tt_probe(pos->key);
    if (tt->key == pos->key) {
        int tt_score = score_from_tt(tt->score, ply);
        tt_move = tt->move;
        if (tt->depth <= 0) {
            if (tt->flag == TT_EXACT) return tt_score;
            if (tt->flag == TT_ALPHA && tt_score <= alpha) return tt_score;
            if (tt->flag == TT_BETA && tt_score >= beta) return tt_score;
        }
    }

    if (!incheck) {
        stand_pat = evaluate(pos);
        stand_ready = 1;
        if (stand_pat >= beta) {
            tt_store(pos->key, 0, beta, TT_BETA, tt_move, ply);
            return beta;
        }
        if (stand_pat > alpha) alpha = stand_pat;
        generate_moves(pos, &list, 1);
    } else {
        generate_moves(pos, &list, 0);
    }

    score_moves(pos, &list, ply, tt_move);

    for (i = 0; i < list.count; i++) {
        Undo undo;
        int score;
        U32 move;
        int gain;
        int see;

        pick_next_move(&list, i);
        move = list.moves[i].move;
        if (stand_ready && alpha > -MATE_BOUND) {
            gain = 0;
            if (MOVE_FLAGS(move) & FLAG_EP) gain += PIECE_VALUE_MG[PAWN];
            else if (MOVE_CAPTURED(move) != NO_PIECE) gain += PIECE_VALUE_MG[PIECE_TYPE[MOVE_CAPTURED(move)]];
            if (MOVE_PROMO(move)) gain += PIECE_VALUE_MG[MOVE_PROMO(move)] - PIECE_VALUE_MG[PAWN];
            if (stand_pat + gain + 120 < alpha) continue;
        }
        if (stand_ready && !incheck) {
            see = see_value_of_move(pos, move);
            if (see < 0 && MOVE_PROMO(move) == 0) continue;
        }
        if (!make_move(pos, move, &undo)) continue;
        g_si.current_move[ply] = move;
        if (capture_count < MAX_MOVES) captures[capture_count++] = move;
        legal++;
        score = -quiescence(pos, -beta, -alpha, ply + 1);
        unmake_move(pos, move, &undo);
        if (g_si.stop) return 0;

        if (score > alpha) {
            alpha = score;
            best_move = move;
            g_si.pv[ply][ply] = move;
            memcpy(&g_si.pv[ply][ply + 1], &g_si.pv[ply + 1][ply + 1],
                   (size_t)(g_si.pv_len[ply + 1] - (ply + 1)) * sizeof(U32));
            g_si.pv_len[ply] = g_si.pv_len[ply + 1];
            if (alpha >= beta) {
                update_capture_heuristics(move, captures, capture_count, 1, 1);
                tt_store(pos->key, 0, beta, TT_BETA, move, ply);
                return beta;
            }
        }
    }

    if (incheck && legal == 0) return -MATE_SCORE + ply;
    if (alpha > old_alpha && best_move) update_capture_heuristics(best_move, captures, capture_count, 1, 0);
    tt_store(pos->key, 0, alpha, alpha > old_alpha ? TT_EXACT : TT_ALPHA, best_move, ply);
    return alpha;
}

static int search(Position *pos, int depth, int alpha, int beta, int ply, int allow_null) {
    TTEntry *tt;
    U32 tt_move = 0;
    U32 captures[MAX_MOVES];
    U32 quiets[MAX_MOVES];
    int old_alpha = alpha;
    int best_score = -INF;
    U32 best_move = 0;
    MoveList list;
    int i;
    int legal = 0;
    int capture_count = 0;
    int quiet_count = 0;
    int incheck;
    int static_eval = 0;
    int static_eval_ready = 0;

    g_si.pv_len[ply] = ply;

    if (g_si.stop) return 0;
    if (ply >= MAX_PLY - 1) return evaluate(pos);

    g_si.nodes++;
    check_search_limits();
    if (g_si.stop) return 0;

    if (pos->halfmove >= 100 || insufficient_material(pos) || is_repetition(pos)) return 0;

    incheck = in_check(pos, pos->side);
    if (incheck) depth++;
    if (depth <= 0) return quiescence(pos, alpha, beta, ply);

    tt = tt_probe(pos->key);
    if (tt->key == pos->key) {
        int tt_score = score_from_tt(tt->score, ply);
        tt_move = tt->move;
        if (tt->depth >= depth) {
            if (tt->flag == TT_EXACT) return tt_score;
            if (tt->flag == TT_ALPHA && tt_score <= alpha) return tt_score;
            if (tt->flag == TT_BETA && tt_score >= beta) return tt_score;
        }
    }

    if (!incheck && depth <= 6) {
        int razor_score;
        static_eval = evaluate(pos);
        static_eval_ready = 1;
        if (depth <= 2 && static_eval - (90 * depth + 60) >= beta) return static_eval;
        if (depth <= 2 && alpha > -MATE_BOUND && static_eval + (140 + depth * 80) <= alpha) {
            razor_score = quiescence(pos, alpha, beta, ply);
            if (razor_score <= alpha) return razor_score;
        }
    }

    if (allow_null && depth >= 3 && !incheck && has_non_pawn_material(pos, pos->side)) {
        NullUndo nu;
        int score;
        make_null_move(pos, &nu);
        score = -search(pos, depth - 1 - 2, -beta, -beta + 1, ply + 1, 0);
        unmake_null_move(pos, &nu);
        if (g_si.stop) return 0;
        if (score >= beta) return beta;
    }

    generate_moves(pos, &list, 0);
    score_moves(pos, &list, ply, tt_move);
    stage_moves(pos, &list);

    for (i = 0; i < list.count; i++) {
        Undo undo;
        U32 move;
        int score;
        int is_quiet;
        int history_score;
        int reduction = 0;
        int lmp_limit = 0;

        pick_next_move(&list, i);
        move = list.moves[i].move;
        is_quiet = MOVE_CAPTURED(move) == NO_PIECE && !(MOVE_FLAGS(move) & FLAG_EP) && MOVE_PROMO(move) == 0;
        history_score = g_si.history[MOVE_PIECE(move)][MOVE_TO(move)];

        if (static_eval_ready && is_quiet && !incheck) {
            if (legal > 0 &&
                depth <= 6 &&
                history_score < 5000 &&
                static_eval + (85 * depth + 18 * depth * depth) <= alpha) {
                continue;
            }
            if (depth <= 6) {
                lmp_limit = 1 + depth * depth + (history_score > 4000 ? 2 : 0);
                if (legal >= lmp_limit &&
                    history_score < 3000 &&
                    static_eval + (55 * depth + 30) <= alpha) {
                    continue;
                }
                if (legal >= lmp_limit + 4 &&
                    history_score < 0 &&
                    static_eval + (35 * depth + 20) <= alpha) {
                    continue;
                }
            }
        }

        if (!make_move(pos, move, &undo)) continue;
        g_si.current_move[ply] = move;
        if (is_quiet && quiet_count < MAX_MOVES) quiets[quiet_count++] = move;
        else if (!is_quiet && capture_count < MAX_MOVES) captures[capture_count++] = move;
        legal++;

        if (legal == 1) {
            score = -search(pos, depth - 1, -beta, -alpha, ply + 1, 1);
        } else {
            if (depth >= 3 && legal > 3 && is_quiet && !incheck) {
                reduction = 1 + (depth >= 4) + (legal > 8) + (legal > 16);
                if (legal > 24) reduction++;
                if (history_score < 0) reduction++;
                if (history_score < -4000) reduction++;
                if (list.moves[i].score >= 8000000 || history_score > 6000) reduction--;
                if (history_score > 10000) reduction--;
                if (reduction < 0) reduction = 0;
                if (reduction > depth - 2) reduction = depth - 2;
            }

            score = -search(pos, depth - 1 - reduction, -alpha - 1, -alpha, ply + 1, 1);
            if (!g_si.stop && score > alpha && reduction > 0) {
                score = -search(pos, depth - 1, -alpha - 1, -alpha, ply + 1, 1);
            }
            if (!g_si.stop && score > alpha && score < beta) {
                score = -search(pos, depth - 1, -beta, -alpha, ply + 1, 1);
            }
        }

        unmake_move(pos, move, &undo);
        if (g_si.stop) return 0;

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }

        if (score > alpha) {
            alpha = score;
            g_si.pv[ply][ply] = move;
            memcpy(&g_si.pv[ply][ply + 1], &g_si.pv[ply + 1][ply + 1],
                   (size_t)(g_si.pv_len[ply + 1] - (ply + 1)) * sizeof(U32));
            g_si.pv_len[ply] = g_si.pv_len[ply + 1];
            if (alpha >= beta) {
                if (is_quiet) {
                    if (g_si.killers[0][ply] != move) {
                        g_si.killers[1][ply] = g_si.killers[0][ply];
                        g_si.killers[0][ply] = move;
                    }
                    update_quiet_heuristics(ply, depth, move, quiets, quiet_count, 1);
                } else {
                    update_capture_heuristics(move, captures, capture_count, depth, 1);
                }
                tt_store(pos->key, depth, beta, TT_BETA, move, ply);
                return beta;
            }
        }
    }

    if (legal == 0) {
        if (incheck) return -MATE_SCORE + ply;
        return 0;
    }

    if (alpha > old_alpha &&
        best_move &&
        MOVE_CAPTURED(best_move) == NO_PIECE &&
        !(MOVE_FLAGS(best_move) & FLAG_EP) &&
        MOVE_PROMO(best_move) == 0) {
        update_quiet_heuristics(ply, depth, best_move, quiets, quiet_count, 0);
    } else if (alpha > old_alpha && best_move) {
        update_capture_heuristics(best_move, captures, capture_count, depth, 0);
    }

    tt_store(pos->key, depth, alpha, alpha > old_alpha ? TT_EXACT : TT_ALPHA, best_move, ply);
    return alpha;
}

static void square_to_string(int sq, char out[3]) {
    out[0] = (char)('a' + FILE_OF(sq));
    out[1] = (char)('1' + RANK_OF(sq));
    out[2] = '\0';
}

static void move_to_string(U32 move, char out[6]) {
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    out[0] = (char)('a' + FILE_OF(from));
    out[1] = (char)('1' + RANK_OF(from));
    out[2] = (char)('a' + FILE_OF(to));
    out[3] = (char)('1' + RANK_OF(to));
    if (MOVE_PROMO(move)) {
        out[4] = promo_char_from_type(MOVE_PROMO(move));
        out[5] = '\0';
    } else {
        out[4] = '\0';
        out[5] = '\0';
    }
}

#ifdef _WIN32
static void gui_begin_search_reporting(const GuiSearchTask *task) {
    g_gui_report_target.hwnd = task->hwnd;
    g_gui_report_target.cookie = task->cookie;
    g_gui_report_target.mode = task->mode;
    g_gui_report_target.enabled = 1;
}

static void gui_end_search_reporting(void) {
    memset(&g_gui_report_target, 0, sizeof(g_gui_report_target));
}

static void gui_post_search_info(int depth, int score) {
    GuiSearchInfoMsg *msg;
    size_t used;
    int i;
    char buf[6];

    if (!g_gui_report_target.enabled || !g_gui_report_target.hwnd) return;

    msg = (GuiSearchInfoMsg *)malloc(sizeof(*msg));
    if (!msg) return;

    memset(msg, 0, sizeof(*msg));
    msg->cookie = g_gui_report_target.cookie;
    msg->mode = g_gui_report_target.mode;
    msg->depth = depth;
    msg->score = score;

    if (score > MATE_BOUND) {
        int mate = (MATE_SCORE - score + 1) / 2;
        used = (size_t)snprintf(msg->pv, sizeof(msg->pv), "d%d mate %d pv", depth, mate);
    } else if (score < -MATE_BOUND) {
        int mate = (-MATE_SCORE - score) / 2;
        used = (size_t)snprintf(msg->pv, sizeof(msg->pv), "d%d mate %d pv", depth, mate);
    } else {
        used = (size_t)snprintf(msg->pv, sizeof(msg->pv), "d%d cp %d pv", depth, score);
    }
    if (used >= sizeof(msg->pv)) used = sizeof(msg->pv) - 1;

    for (i = 0; i < g_si.pv_len[0] && used + 6 < sizeof(msg->pv); i++) {
        move_to_string(g_si.pv[0][i], buf);
        msg->pv[used++] = ' ';
        msg->pv[used] = '\0';
        strncat(msg->pv, buf, sizeof(msg->pv) - used - 1);
        used = strlen(msg->pv);
    }

    if (!PostMessage(g_gui_report_target.hwnd, GUI_WM_SEARCH_INFO, 0, (LPARAM)msg)) free(msg);
}
#else
static void gui_post_search_info(int depth, int score) {
    (void)depth;
    (void)score;
}
#endif

static void print_pv_line(int depth, int score) {
    I64 elapsed = now_ms() - g_si.start_ms;
    U64 nps = elapsed > 0 ? (g_si.nodes * 1000ULL) / (U64)elapsed : g_si.nodes;
    int mate;
    int i;
    char buf[6];

    if (elapsed < 0) elapsed = 0;
    gui_post_search_info(depth, score);
    if (!g_emit_search_info) return;

    if (score > MATE_BOUND) {
        mate = (MATE_SCORE - score + 1) / 2;
        printf("info depth %d score mate %d nodes %llu time %lld nps %llu pv",
               depth, mate, (unsigned long long)g_si.nodes, (long long)elapsed,
               (unsigned long long)nps);
    } else if (score < -MATE_BOUND) {
        mate = (-MATE_SCORE - score) / 2;
        printf("info depth %d score mate %d nodes %llu time %lld nps %llu pv",
               depth, mate, (unsigned long long)g_si.nodes, (long long)elapsed,
               (unsigned long long)nps);
    } else {
        printf("info depth %d score cp %d nodes %llu time %lld nps %llu pv",
               depth, score, (unsigned long long)g_si.nodes, (long long)elapsed,
               (unsigned long long)nps);
    }

    for (i = 0; i < g_si.pv_len[0]; i++) {
        move_to_string(g_si.pv[0][i], buf);
        printf(" %s", buf);
    }
    printf("\n");
    fflush(stdout);
}

static void clear_search_state(void) {
    memset(&g_si.killers, 0, sizeof(g_si.killers));
    memset(&g_si.pv, 0, sizeof(g_si.pv));
    memset(&g_si.pv_len, 0, sizeof(g_si.pv_len));
    memset(&g_si.current_move, 0, sizeof(g_si.current_move));
}

static void reset_search_runtime(void) {
    g_si.start_ms = 0;
    g_si.soft_stop_ms = 0;
    g_si.hard_stop_ms = 0;
    g_si.node_limit = 0;
    g_si.nodes = 0ULL;
    g_si.stop = 0;
    g_si.quit = 0;
    g_si.root_depth = 0;
    clear_search_state();
}

static void clear_search_reuse(void) {
    memset(&g_reuse, 0, sizeof(g_reuse));
}

static void clear_position_cache(void) {
    memset(&g_pos_cache, 0, sizeof(g_pos_cache));
}

static void clear_search_heuristics(void) {
    memset(g_si.history, 0, sizeof(g_si.history));
    memset(g_si.capture_history, 0, sizeof(g_si.capture_history));
    memset(g_si.countermove, 0, sizeof(g_si.countermove));
    clear_search_reuse();
}

static void clear_hash(void) {
    memset(g_tt, 0, sizeof(g_tt));
    memset(g_eval, 0, sizeof(g_eval));
    memset(g_pawn, 0, sizeof(g_pawn));
}

static void update_search_reuse(const Position *root) {
    Position pos = *root;
    int i;

    clear_search_reuse();
    for (i = 0; i < g_si.pv_len[0] && i < MAX_PLY; i++) {
        Undo undo;
        U32 move = g_si.pv[0][i];
        SearchReuseEntry *e = &g_reuse.entries[(unsigned)(pos.key & REUSE_MASK)];
        e->key = pos.key;
        e->move = move;
        if (!make_move(&pos, move, &undo)) {
            clear_search_reuse();
            break;
        }
    }
}

static U64 compute_position_key(const Position *pos) {
    U64 key = 0ULL;
    int piece;
    for (piece = 0; piece < 12; piece++) {
        U64 bb = pos->bb[piece];
        while (bb) {
            int sq = pop_lsb(&bb);
            key ^= zobrist_piece[piece][sq];
        }
    }
    key ^= zobrist_castle[pos->castling & 15];
    if (pos->ep != -1) key ^= zobrist_ep[FILE_OF(pos->ep)];
    if (pos->side == BLACK) key ^= zobrist_side;
    return key;
}

static U64 compute_pawn_key(const Position *pos) {
    U64 key = 0ULL;
    U64 bb = pos->bb[WP];
    while (bb) {
        int sq = pop_lsb(&bb);
        key ^= zobrist_piece[WP][sq];
    }
    bb = pos->bb[BP];
    while (bb) {
        int sq = pop_lsb(&bb);
        key ^= zobrist_piece[BP][sq];
    }
    return key;
}

static int validate_position(const Position *pos, int verbose) {
    U64 bb[12] = {0};
    U64 occ[3] = {0};
    U64 recomputed_key = 0ULL;
    U64 recomputed_pawn_key = 0ULL;
    int seen_king_sq[2] = { -1, -1 };
    int sq;
    int errors = 0;

    for (sq = 0; sq < 64; sq++) {
        int piece = pos->board[sq];
        if (piece == NO_PIECE) continue;
        if (piece < 0 || piece >= 12) {
            if (verbose) printf("check failed: invalid piece id on square %d\n", sq);
            return 0;
        }
        bb[piece] |= BIT(sq);
        occ[PIECE_COLOR[piece]] |= BIT(sq);
        occ[BOTH] |= BIT(sq);
        if (piece == WK) seen_king_sq[WHITE] = sq;
        else if (piece == BK) seen_king_sq[BLACK] = sq;
    }

    for (sq = 0; sq < 12; sq++) {
        if (bb[sq] != pos->bb[sq]) {
            if (verbose) printf("check failed: bitboard mismatch for piece %c\n", PIECE_TO_CHAR[sq]);
            errors = 1;
        }
    }
    if (occ[WHITE] != pos->occ[WHITE] || occ[BLACK] != pos->occ[BLACK] || occ[BOTH] != pos->occ[BOTH]) {
        if (verbose) printf("check failed: occupancy mismatch\n");
        errors = 1;
    }
    if (pos->occ[WHITE] & pos->occ[BLACK]) {
        if (verbose) printf("check failed: white/black occupancy overlap\n");
        errors = 1;
    }
    if (popcount64(pos->bb[WK]) != 1 || popcount64(pos->bb[BK]) != 1) {
        if (verbose) printf("check failed: expected one king per side\n");
        errors = 1;
    }
    if (pos->king_sq[WHITE] != seen_king_sq[WHITE] || pos->king_sq[BLACK] != seen_king_sq[BLACK]) {
        if (verbose) printf("check failed: cached king square mismatch\n");
        errors = 1;
    }
    if (pos->ep != -1) {
        if (pos->ep < 0 || pos->ep >= 64) {
            if (verbose) printf("check failed: invalid en-passant square\n");
            errors = 1;
        } else {
            int ep_rank = RANK_OF(pos->ep);
            int pawn_sq = pos->side == WHITE ? pos->ep - 8 : pos->ep + 8;
            int expected_rank = pos->side == WHITE ? 5 : 2;
            int expected_pawn = pos->side == WHITE ? BP : WP;
            if (ep_rank != expected_rank) {
                if (verbose) printf("check failed: en-passant square rank mismatch\n");
                errors = 1;
            }
            if (pos->board[pos->ep] != NO_PIECE) {
                if (verbose) printf("check failed: en-passant square not empty\n");
                errors = 1;
            }
            if (pawn_sq < 0 || pawn_sq >= 64 || pos->board[pawn_sq] != expected_pawn) {
                if (verbose) printf("check failed: en-passant backing pawn mismatch\n");
                errors = 1;
            }
        }
    }
    recomputed_key = compute_position_key(pos);
    if (recomputed_key != pos->key) {
        if (verbose) printf("check failed: zobrist mismatch (have 0x%016llx expected 0x%016llx)\n",
                            (unsigned long long)pos->key, (unsigned long long)recomputed_key);
        errors = 1;
    }
    recomputed_pawn_key = compute_pawn_key(pos);
    if (recomputed_pawn_key != pos->pawn_key) {
        if (verbose) printf("check failed: pawn hash mismatch (have 0x%016llx expected 0x%016llx)\n",
                            (unsigned long long)pos->pawn_key, (unsigned long long)recomputed_pawn_key);
        errors = 1;
    }
    if (pos->rep_len <= 0 || pos->rep_len > MAX_HISTORY) {
        if (verbose) printf("check failed: invalid repetition history length\n");
        errors = 1;
    } else if (pos->rep_keys[pos->rep_len - 1] != pos->key) {
        if (verbose) printf("check failed: repetition tail key mismatch\n");
        errors = 1;
    }
    if (errors) return 0;
    if (verbose) printf("check ok\n");
    return 1;
}

static int parse_square_text(const char *s) {
    int file, rank;
    if (!s || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return -1;
    file = s[0] - 'a';
    rank = s[1] - '1';
    return SQ(file, rank);
}

static int parse_fen(Position *pos, const char *fen) {
    const char *p = fen;
    int sq = 56;
    int piece;
    clear_position(pos);

    while (*p == ' ') p++;
    while (*p && *p != ' ') {
        if (*p == '/') {
            sq -= 16;
        } else if (*p >= '1' && *p <= '8') {
            sq += *p - '0';
        } else {
            piece = piece_from_char(*p);
            if (piece == NO_PIECE || sq < 0 || sq >= 64) return 0;
            place_piece(pos, piece, sq++);
        }
        p++;
    }

    while (*p == ' ') p++;
    pos->side = (*p == 'b') ? BLACK : WHITE;
    if (pos->side == BLACK) pos->key ^= zobrist_side;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;

    set_castling(pos, 0);
    if (*p == '-') p++;
    else {
        int rights = 0;
        while (*p && *p != ' ') {
            if (*p == 'K') rights |= CASTLE_WK;
            else if (*p == 'Q') rights |= CASTLE_WQ;
            else if (*p == 'k') rights |= CASTLE_BK;
            else if (*p == 'q') rights |= CASTLE_BQ;
            p++;
        }
        set_castling(pos, rights);
    }

    while (*p == ' ') p++;
    if (*p == '-') {
        pos->ep = -1;
        p++;
    } else if (p[0] && p[1]) {
        int ep = parse_square_text(p);
        set_ep_square(pos, ep);
        p += 2;
    }

    while (*p == ' ') p++;
    pos->halfmove = *p ? atoi(p) : 0;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    pos->fullmove = *p ? atoi(p) : 1;
    if (pos->fullmove < 1) pos->fullmove = 1;

    pos->rep_len = 0;
    pos->rep_keys[pos->rep_len++] = pos->key;
    return 1;
}

static int parse_move(Position *pos, const char *text, U32 *out_move) {
    MoveList list;
    int i;
    char buf[6];

    generate_moves(pos, &list, 0);
    for (i = 0; i < list.count; i++) {
        Undo undo;
        U32 move = list.moves[i].move;
        move_to_string(move, buf);
        if (strcmp(buf, text) == 0 && make_move(pos, move, &undo)) {
            unmake_move(pos, move, &undo);
            *out_move = move;
            return 1;
        }
    }
    return 0;
}

static U64 perft(Position *pos, int depth) {
    MoveList list;
    U64 nodes = 0;
    int i;
    if (depth == 0) return 1ULL;

    generate_moves(pos, &list, 0);
    for (i = 0; i < list.count; i++) {
        Undo undo;
        U32 move = list.moves[i].move;
        if (!make_move(pos, move, &undo)) continue;
        nodes += perft(pos, depth - 1);
        unmake_move(pos, move, &undo);
    }
    return nodes;
}

static void divide(Position *pos, int depth) {
    MoveList list;
    int i;
    U64 total = 0;
    char buf[6];

    generate_moves(pos, &list, 0);
    for (i = 0; i < list.count; i++) {
        Undo undo;
        U32 move = list.moves[i].move;
        U64 nodes;
        if (!make_move(pos, move, &undo)) continue;
        nodes = perft(pos, depth - 1);
        unmake_move(pos, move, &undo);
        move_to_string(move, buf);
        printf("%s: %llu\n", buf, (unsigned long long)nodes);
        total += nodes;
    }
    printf("Total: %llu\n", (unsigned long long)total);
    fflush(stdout);
}

static int run_roundtrip_test(void) {
    static const char *seq[] = { "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6" };
    Position pos, base;
    Undo undos[ARRAY_LEN(seq)];
    U32 moves[ARRAY_LEN(seq)];
    int i;

    setup_classic_startpos(&pos);
    base = pos;

    for (i = 0; i < ARRAY_LEN(seq); i++) {
        if (!parse_move(&pos, seq[i], &moves[i])) {
            printf("selftest roundtrip failed: could not parse move %s\n", seq[i]);
            return 0;
        }
        if (!make_move(&pos, moves[i], &undos[i])) {
            printf("selftest roundtrip failed: illegal move %s\n", seq[i]);
            return 0;
        }
        if (!validate_position(&pos, 0)) {
            printf("selftest roundtrip failed: invalid state after %s\n", seq[i]);
            return 0;
        }
    }

    for (i = ARRAY_LEN(seq) - 1; i >= 0; i--) {
        unmake_move(&pos, moves[i], &undos[i]);
        if (!validate_position(&pos, 0)) {
            printf("selftest roundtrip failed: invalid state during undo of %s\n", seq[i]);
            return 0;
        }
    }

    if (memcmp(pos.board, base.board, sizeof(pos.board)) != 0 ||
        memcmp(pos.bb, base.bb, sizeof(pos.bb)) != 0 ||
        memcmp(pos.occ, base.occ, sizeof(pos.occ)) != 0 ||
        pos.side != base.side ||
        pos.castling != base.castling ||
        pos.ep != base.ep ||
        pos.halfmove != base.halfmove ||
        pos.fullmove != base.fullmove ||
        pos.key != base.key) {
        printf("selftest roundtrip failed: position did not restore exactly\n");
        return 0;
    }

    printf("selftest roundtrip passed\n");
    return 1;
}

static int run_repetition_test(void) {
    static const char *seq[] = {
        "g1f3", "g8f6", "f3g1", "f6g8",
        "g1f3", "g8f6", "f3g1", "f6g8"
    };
    Position pos;
    U32 move;
    Undo undo;
    int i;

    setup_classic_startpos(&pos);
    for (i = 0; i < ARRAY_LEN(seq); i++) {
        if (!parse_move(&pos, seq[i], &move)) {
            printf("selftest repetition failed: could not parse move %s\n", seq[i]);
            return 0;
        }
        if (!make_move(&pos, move, &undo)) {
            printf("selftest repetition failed: illegal move %s\n", seq[i]);
            return 0;
        }
        if (!validate_position(&pos, 0)) {
            printf("selftest repetition failed: invalid state after %s\n", seq[i]);
            return 0;
        }
        if (i == 3 && is_repetition(&pos)) {
            printf("selftest repetition failed: twofold repetition counted as draw\n");
            return 0;
        }
    }

    if (!is_repetition(&pos)) {
        printf("selftest repetition failed: threefold repetition not detected\n");
        return 0;
    }

    printf("selftest repetition passed\n");
    return 1;
}

static int run_eval_heuristic_test(void) {
    Position trapped;
    Position active;
    int trapped_score;
    int active_score;

    if (!parse_fen(&trapped, "4k3/8/8/8/8/8/PP6/R3K2R w - - 0 1") ||
        !parse_fen(&active, "4k3/8/8/8/8/R7/PP6/4K2R w - - 0 1")) {
        printf("selftest eval failed: could not parse heuristic test FEN\n");
        return 0;
    }

    trapped_score = evaluate(&trapped);
    active_score = evaluate(&active);
    if (active_score <= trapped_score + 20) {
        printf("selftest eval failed: active rook score %d not above trapped rook score %d\n",
               active_score, trapped_score);
        return 0;
    }

    printf("selftest eval passed\n");
    return 1;
}

static int run_selftest(void) {
    Position pos;
    int i;
    int passed = 0;
    int total = 0;

    printf("selftest begin\n");
    for (i = 0; i < ARRAY_LEN(SELFTEST_CASES); i++) {
        const SelfTestCase *tc = &SELFTEST_CASES[i];
        U64 nodes;
        total++;
        if (!parse_fen(&pos, tc->fen)) {
            printf("selftest %s failed: could not parse FEN\n", tc->name);
            continue;
        }
        if (!validate_position(&pos, 0)) {
            printf("selftest %s failed: validation mismatch\n", tc->name);
            continue;
        }
        nodes = perft(&pos, tc->depth);
        if (nodes != tc->expected) {
            printf("selftest %s failed: expected %llu got %llu\n",
                   tc->name, (unsigned long long)tc->expected, (unsigned long long)nodes);
            continue;
        }
        printf("selftest %s passed (%llu)\n", tc->name, (unsigned long long)nodes);
        passed++;
    }

    total++;
    if (run_roundtrip_test()) passed++;

    total++;
    if (run_repetition_test()) passed++;

    total++;
    if (run_eval_heuristic_test()) passed++;

    if (passed == total) {
        printf("selftest ok %d/%d\n", passed, total);
        return 1;
    }

    printf("selftest failed %d/%d\n", passed, total);
    return 0;
}

static void run_bench(int depth) {
    SearchLimits limits;
    Position pos;
    I64 bench_start;
    U64 total_nodes = 0ULL;
    int i;
    int saved_emit = g_emit_search_info;

    init_search_limits(&limits);
    if (depth > 0) limits.depth = depth;
    else limits.depth = 6;

    clear_hash();
    memset(g_si.history, 0, sizeof(g_si.history));
    g_emit_search_info = 0;
    bench_start = now_ms();

    for (i = 0; i < ARRAY_LEN(BENCH_FENS); i++) {
        U32 best_move;
        char move_buf[6];
        clear_search_heuristics();
        if (!parse_fen(&pos, BENCH_FENS[i])) continue;
        if (!validate_position(&pos, 0)) {
            printf("bench position %d invalid\n", i + 1);
            continue;
        }
        best_move = search_best_move(&pos, &limits);
        total_nodes += g_si.nodes;
        if (best_move) move_to_string(best_move, move_buf);
        else strcpy(move_buf, "0000");
        printf("bench %d/%d bestmove %s nodes %llu\n",
               i + 1, ARRAY_LEN(BENCH_FENS), move_buf, (unsigned long long)g_si.nodes);
    }

    {
        I64 elapsed = now_ms() - bench_start;
        U64 nps = elapsed > 0 ? (total_nodes * 1000ULL) / (U64)elapsed : total_nodes;
        printf("bench done depth %d positions %d nodes %llu time %lld nps %llu\n",
               limits.depth, ARRAY_LEN(BENCH_FENS), (unsigned long long)total_nodes,
               (long long)elapsed, (unsigned long long)nps);
    }
    fflush(stdout);
    g_emit_search_info = saved_emit;
}

static void run_latency_report(int depth) {
    SearchLimits limits;
    Position pos;
    I64 bench_start_us;
    I64 cold_init_start_us;
    I64 cold_init_us;
    U64 total_nodes = 0ULL;
    int i;
    int saved_emit = g_emit_search_info;

    init_search_limits(&limits);
    if (depth > 0) limits.depth = depth;
    else limits.depth = 6;

    latprof_reset();
    g_emit_search_info = 0;
    memset(g_si.history, 0, sizeof(g_si.history));
    engine_initialized = 0;
    g_latprof.enabled = 1;

    cold_init_start_us = now_us();
    init_engine();
    cold_init_us = now_us() - cold_init_start_us;
    clear_hash();

    bench_start_us = now_us();
    for (i = 0; i < ARRAY_LEN(BENCH_FENS); i++) {
        U32 best_move;
        char move_buf[6];
        I64 pos_start_us;
        I64 pos_us;
        U64 pos_nps;

        clear_search_heuristics();
        if (!parse_fen(&pos, BENCH_FENS[i])) continue;
        if (!validate_position(&pos, 0)) {
            printf("latency position %d invalid\n", i + 1);
            continue;
        }

        pos_start_us = now_us();
        best_move = search_best_move(&pos, &limits);
        pos_us = now_us() - pos_start_us;
        total_nodes += g_si.nodes;
        pos_nps = pos_us > 0 ? (g_si.nodes * 1000000ULL) / (U64)pos_us : g_si.nodes;
        if (best_move) move_to_string(best_move, move_buf);
        else strcpy(move_buf, "0000");
        printf("latency pos %d/%d bestmove %s nodes %llu time %.3f ms nps %llu\n",
               i + 1, ARRAY_LEN(BENCH_FENS), move_buf,
               (unsigned long long)g_si.nodes,
               (double)pos_us / 1000.0,
               (unsigned long long)pos_nps);
    }
    g_latprof.enabled = 0;

    {
        I64 elapsed_us = now_us() - bench_start_us;
        U64 nps = elapsed_us > 0 ? (total_nodes * 1000000ULL) / (U64)elapsed_us : total_nodes;
        printf("latency cold-init internal %.3f ms\n", (double)cold_init_us / 1000.0);
        printf("latency bench depth %d positions %d nodes %llu time %.3f ms nps %llu\n",
               limits.depth, ARRAY_LEN(BENCH_FENS), (unsigned long long)total_nodes,
               (double)elapsed_us / 1000.0, (unsigned long long)nps);
    }
    latprof_print_report();
    printf("note: the report is internal engine time only; process creation and shell/pipe overhead are outside it.\n");
    fflush(stdout);
    g_emit_search_info = saved_emit;
}

static void print_board(const Position *pos) {
    int rank, file, sq, piece;
    char sqbuf[3];

    for (rank = 7; rank >= 0; rank--) {
        printf("%d  ", rank + 1);
        for (file = 0; file < 8; file++) {
            sq = SQ(file, rank);
            piece = pos->board[sq];
            printf("%c ", piece == NO_PIECE ? '.' : PIECE_TO_CHAR[piece]);
        }
        printf("\n");
    }
    printf("\n   a b c d e f g h\n");
    printf("side: %s\n", pos->side == WHITE ? "white" : "black");
    printf("castling: %c%c%c%c\n",
           (pos->castling & CASTLE_WK) ? 'K' : '-',
           (pos->castling & CASTLE_WQ) ? 'Q' : '-',
           (pos->castling & CASTLE_BK) ? 'k' : '-',
           (pos->castling & CASTLE_BQ) ? 'q' : '-');
    if (pos->ep != -1) {
        square_to_string(pos->ep, sqbuf);
        printf("ep: %s\n", sqbuf);
    } else {
        printf("ep: -\n");
    }
    printf("halfmove: %d fullmove: %d key: 0x%016llx\n",
           pos->halfmove, pos->fullmove, (unsigned long long)pos->key);
    fflush(stdout);
}

static void setup_classic_startpos(Position *pos) {
    parse_fen(pos, STARTPOS_FEN);
}

static void copy_trimmed_text(char *dst, size_t dst_size, const char *src) {
    size_t len;
    if (!dst_size) return;
    while (*src == ' ' || *src == '\t') src++;
    len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    len = strlen(dst);
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\t')) {
        dst[len - 1] = '\0';
        len--;
    }
}

static int apply_move_text(Position *pos, const char *text) {
    U32 move;
    Undo undo;
    if (!parse_move(pos, text, &move)) return 0;
    return make_move(pos, move, &undo);
}

static void apply_position_command(Position *pos, char *line) {
    char *moves = strstr(line, " moves ");
    char *fen;
    char base_fen[128];
    char parsed_moves[MAX_HISTORY][6];
    int move_count = 0;
    int is_startpos = 1;
    int reused = 0;
    int start_index = 0;
    int i;
    if (moves) {
        *moves = '\0';
        moves += 7;
    }

    if (strstr(line, "startpos")) {
        is_startpos = 1;
    } else if ((fen = strstr(line, "fen ")) != NULL) {
        is_startpos = 0;
        copy_trimmed_text(base_fen, sizeof(base_fen), fen + 4);
    } else {
        is_startpos = 1;
    }

    if (moves) {
        char *tok = strtok(moves, " \t\r\n");
        while (tok && move_count < MAX_HISTORY) {
            strncpy(parsed_moves[move_count], tok, 5);
            parsed_moves[move_count][5] = '\0';
            move_count++;
            tok = strtok(NULL, " \t\r\n");
        }
    }

    if (g_pos_cache.valid &&
        g_pos_cache.is_startpos == is_startpos &&
        (is_startpos || strcmp(g_pos_cache.base_fen, base_fen) == 0) &&
        move_count >= g_pos_cache.move_count) {
        reused = 1;
        for (i = 0; i < g_pos_cache.move_count; i++) {
            if (strcmp(g_pos_cache.moves[i], parsed_moves[i]) != 0) {
                reused = 0;
                break;
            }
        }
        if (reused) {
            *pos = g_pos_cache.pos;
            start_index = g_pos_cache.move_count;
        }
    }

    if (!reused) {
        clear_search_heuristics();
        if (is_startpos) setup_classic_startpos(pos);
        else if (!parse_fen(pos, base_fen)) setup_classic_startpos(pos);
        start_index = 0;
    }

    for (i = start_index; i < move_count; i++) {
        if (!apply_move_text(pos, parsed_moves[i])) break;
    }

    g_pos_cache.valid = 1;
    g_pos_cache.is_startpos = is_startpos;
    if (is_startpos) g_pos_cache.base_fen[0] = '\0';
    else copy_trimmed_text(g_pos_cache.base_fen, sizeof(g_pos_cache.base_fen), base_fen);
    g_pos_cache.move_count = i;
    memcpy(g_pos_cache.pos.board, pos->board, sizeof(pos->board));
    memcpy(g_pos_cache.pos.bb, pos->bb, sizeof(pos->bb));
    memcpy(g_pos_cache.pos.occ, pos->occ, sizeof(pos->occ));
    g_pos_cache.pos.king_sq[WHITE] = pos->king_sq[WHITE];
    g_pos_cache.pos.king_sq[BLACK] = pos->king_sq[BLACK];
    g_pos_cache.pos.side = pos->side;
    g_pos_cache.pos.castling = pos->castling;
    g_pos_cache.pos.ep = pos->ep;
    g_pos_cache.pos.halfmove = pos->halfmove;
    g_pos_cache.pos.fullmove = pos->fullmove;
    g_pos_cache.pos.key = pos->key;
    g_pos_cache.pos.pawn_key = pos->pawn_key;
    g_pos_cache.pos.rep_len = pos->rep_len;
    memcpy(g_pos_cache.pos.rep_keys, pos->rep_keys, (size_t)pos->rep_len * sizeof(U64));
    for (i = 0; i < g_pos_cache.move_count; i++) {
        memcpy(g_pos_cache.moves[i], parsed_moves[i], sizeof(g_pos_cache.moves[i]));
    }
}

static void init_search_limits(SearchLimits *limits) {
    memset(limits, 0, sizeof(*limits));
    limits->depth = 64;
}

static void parse_go_command(SearchLimits *limits, char *line) {
    char *tok = strtok(line, " \t\r\n");
    init_search_limits(limits);
    while ((tok = strtok(NULL, " \t\r\n")) != NULL) {
        if (strcmp(tok, "ponder") == 0) {
            limits->ponder = 1;
        } else if (strcmp(tok, "depth") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->depth = atoi(tok);
        } else if (strcmp(tok, "movetime") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->movetime_ms = atoll(tok);
        } else if (strcmp(tok, "wtime") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->wtime_ms = atoll(tok);
        } else if (strcmp(tok, "btime") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->btime_ms = atoll(tok);
        } else if (strcmp(tok, "winc") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->winc_ms = atoll(tok);
        } else if (strcmp(tok, "binc") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->binc_ms = atoll(tok);
        } else if (strcmp(tok, "movestogo") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->movestogo = atoi(tok);
        } else if (strcmp(tok, "nodes") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok) limits->node_limit = (U64)strtoull(tok, NULL, 10);
        } else if (strcmp(tok, "infinite") == 0) {
            limits->infinite = 1;
        }
    }
}

static void compute_time_budget_for_side(int side, const SearchLimits *limits) {
    I64 now = now_ms();
    I64 time_left = 0;
    I64 inc = 0;
    I64 alloc = 0;

    g_si.start_ms = now;
    g_si.soft_stop_ms = 0;
    g_si.hard_stop_ms = 0;
    g_si.node_limit = limits->node_limit;

    if (limits->movetime_ms > 0) {
        g_si.soft_stop_ms = now + limits->movetime_ms;
        g_si.hard_stop_ms = now + limits->movetime_ms;
        return;
    }

    if (side == WHITE) {
        time_left = limits->wtime_ms;
        inc = limits->winc_ms;
    } else {
        time_left = limits->btime_ms;
        inc = limits->binc_ms;
    }

    if (time_left > 0) {
        int mtg = limits->movestogo > 0 ? limits->movestogo : 30;
        alloc = time_left / mtg + inc / 2;
        if (alloc < 10) alloc = 10;
        if (alloc > time_left / 2) alloc = time_left / 2;
        if (alloc < 1) alloc = 1;
        g_si.soft_stop_ms = now + alloc;
        g_si.hard_stop_ms = now + (alloc * 2 < time_left ? alloc * 2 : time_left - 1);
        if (g_si.hard_stop_ms < g_si.soft_stop_ms) g_si.hard_stop_ms = g_si.soft_stop_ms;
    } else if (!limits->infinite && limits->depth == 64) {
        g_si.soft_stop_ms = now + 1000;
        g_si.hard_stop_ms = now + 1000;
    }
}

static void search_activate_ponderhit(void) {
    if (!g_active_ponder) return;
    g_active_ponder = 0;
    g_active_ponderhit = 1;
    compute_time_budget_for_side(g_active_root_side, &g_active_limits);
}

static void begin_search_command(const Position *pos, const SearchLimits *limits) {
    g_active_limits = *limits;
    g_active_root_side = pos->side;
    g_active_ponder = limits->ponder;
    g_active_ponderhit = 0;

    if (limits->ponder) {
        g_si.start_ms = now_ms();
        g_si.soft_stop_ms = 0;
        g_si.hard_stop_ms = 0;
        g_si.node_limit = limits->node_limit;
    } else {
        compute_time_budget_for_side(pos->side, limits);
    }
}

static void end_search_command(void) {
    memset(&g_active_limits, 0, sizeof(g_active_limits));
    g_active_root_side = WHITE;
    g_active_ponder = 0;
    g_active_ponderhit = 0;
}

static int search_root_window(Position *pos, int depth, int alpha, int beta, int *out_score, U32 *out_move) {
    int best_score = -INF;
    U32 best_move = 0;
    int legal = 0;
    int i;
    MoveList list;
    TTEntry *tt = tt_probe(pos->key);
    U32 tt_move = tt->key == pos->key ? tt->move : 0;

    generate_moves(pos, &list, 0);
    score_moves(pos, &list, 0, tt_move);
    stage_moves(pos, &list);

    g_si.root_depth = depth;
    g_si.pv_len[0] = 0;

    for (i = 0; i < list.count; i++) {
        Undo undo;
        U32 move;
        int score;

        pick_next_move(&list, i);
        move = list.moves[i].move;
        if (!make_move(pos, move, &undo)) continue;
        g_si.current_move[0] = move;
        legal++;

        if (legal == 1) {
            score = -search(pos, depth - 1, -beta, -alpha, 1, 1);
        } else {
            score = -search(pos, depth - 1, -alpha - 1, -alpha, 1, 1);
            if (!g_si.stop && score > alpha && score < beta) {
                score = -search(pos, depth - 1, -beta, -alpha, 1, 1);
            }
        }

        unmake_move(pos, move, &undo);
        if (g_si.stop) break;

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            g_si.pv[0][0] = move;
            memcpy(&g_si.pv[0][1], &g_si.pv[1][1],
                   (size_t)(g_si.pv_len[1] - 1) * sizeof(U32));
            g_si.pv_len[0] = g_si.pv_len[1];
            if (alpha >= beta) break;
        }
    }

    *out_score = best_score;
    *out_move = best_move;
    return legal;
}

static U32 search_best_move(Position *pos, const SearchLimits *limits) {
    LATENCY_SCOPE(LAT_SEARCH_BEST);
    MoveList root_moves;
    U32 best_move = 0;
    int best_score = -INF;
    int depth_limit;
    int depth;
    int i;

    reset_search_runtime();
    begin_search_command(pos, limits);

    generate_moves(pos, &root_moves, 0);
    for (i = 0; i < root_moves.count; i++) {
        Undo undo;
        if (make_move(pos, root_moves.moves[i].move, &undo)) {
            unmake_move(pos, root_moves.moves[i].move, &undo);
            best_move = root_moves.moves[i].move;
            break;
        }
    }

    if (!best_move) {
        end_search_command();
        LATENCY_SCOPE_END();
        return 0;
    }

    depth_limit = limits->depth;
    if ((limits->infinite || limits->ponder) && limits->depth == 64) depth_limit = MAX_PLY - 1;
    if (depth_limit < 1) depth_limit = 1;
    if (depth_limit > MAX_PLY - 1) depth_limit = MAX_PLY - 1;

    for (depth = 1; depth <= depth_limit; depth++) {
        int alpha = -INF;
        int beta = INF;
        int iter_best_score = -INF;
        U32 iter_best_move = best_move;
        int legal = 0;

        if (g_si.stop) break;

        if (depth >= 4 && best_score > -MATE_BOUND && best_score < MATE_BOUND) {
            alpha = best_score - 24;
            beta = best_score + 24;
        }
        legal = search_root_window(pos, depth, alpha, beta, &iter_best_score, &iter_best_move);
        if (!g_si.stop && legal > 0 && (iter_best_score <= alpha || iter_best_score >= beta)) {
            legal = search_root_window(pos, depth, -INF, INF, &iter_best_score, &iter_best_move);
        }

        if (!g_si.stop && legal > 0) {
            best_move = iter_best_move;
            best_score = iter_best_score;
            print_pv_line(depth, best_score);
            if (g_si.soft_stop_ms && now_ms() >= g_si.soft_stop_ms) break;
            if (best_score > MATE_BOUND || best_score < -MATE_BOUND) break;
        } else {
            break;
        }
    }
    if (best_move && g_si.pv_len[0] > 0) {
        if (!limits->ponder || g_active_ponderhit) update_search_reuse(pos);
    } else if (!limits->ponder || g_active_ponderhit) {
        clear_search_reuse();
    }
    end_search_command();
    LATENCY_SCOPE_END();
    return best_move;
}

static U32 search_ponder_move(const Position *pos, U32 best_move) {
    Position next = *pos;
    Undo undo;
    U32 ponder;
    Undo ponder_undo;

    if (!best_move) return 0;
    if (!make_move(&next, best_move, &undo)) return 0;
    ponder = reuse_move_for_position(&next);
    if (!ponder) return 0;
    return make_move(&next, ponder, &ponder_undo) ? ponder : 0;
}

static void handle_go(Position *pos, char *line) {
    SearchLimits limits;
    U32 best_move;
    U32 ponder_move;
    char buf[6];
    char ponder_buf[6];

    parse_go_command(&limits, line);
    if (!g_uci_ponder_enabled) limits.ponder = 0;
    best_move = search_best_move(pos, &limits);
    if (!best_move) {
        printf("bestmove 0000\n");
    } else {
        move_to_string(best_move, buf);
        ponder_move = search_ponder_move(pos, best_move);
        if (ponder_move && g_uci_ponder_enabled) {
            move_to_string(ponder_move, ponder_buf);
            printf("bestmove %s ponder %s\n", buf, ponder_buf);
        } else {
            printf("bestmove %s\n", buf);
        }
    }
    fflush(stdout);
}

static void init_masks(void) {
    LATENCY_SCOPE(LAT_INIT_MASKS);
    int sq, file, rank, f, r;
    U64 bit;

    memset(pawn_attacks, 0, sizeof(pawn_attacks));
    memset(knight_attacks, 0, sizeof(knight_attacks));
    memset(king_attacks, 0, sizeof(king_attacks));
    memset(ray_north, 0, sizeof(ray_north));
    memset(ray_south, 0, sizeof(ray_south));
    memset(ray_east, 0, sizeof(ray_east));
    memset(ray_west, 0, sizeof(ray_west));
    memset(ray_ne, 0, sizeof(ray_ne));
    memset(ray_nw, 0, sizeof(ray_nw));
    memset(ray_se, 0, sizeof(ray_se));
    memset(ray_sw, 0, sizeof(ray_sw));
    memset(ortho_lines, 0, sizeof(ortho_lines));
    memset(diag_lines, 0, sizeof(diag_lines));
    memset(between_masks, 0, sizeof(between_masks));
    memset(castle_rights_mask, 15, sizeof(castle_rights_mask));
    memset(rook_occ_masks, 0, sizeof(rook_occ_masks));
    memset(bishop_occ_masks, 0, sizeof(bishop_occ_masks));
    memset(rook_attack_table, 0, sizeof(rook_attack_table));
    memset(bishop_attack_table, 0, sizeof(bishop_attack_table));
    memset(rook_rel_index, -1, sizeof(rook_rel_index));
    memset(bishop_rel_index, -1, sizeof(bishop_rel_index));
    memset(file_masks, 0, sizeof(file_masks));
    memset(isolated_masks, 0, sizeof(isolated_masks));
    memset(passed_masks, 0, sizeof(passed_masks));

    for (file = 0; file < 8; file++) {
        for (rank = 0; rank < 8; rank++) {
            file_masks[file] |= BIT(SQ(file, rank));
        }
    }

    for (sq = 0; sq < 64; sq++) {
        file = FILE_OF(sq);
        rank = RANK_OF(sq);

        if (file > 0 && rank < 7) pawn_attacks[WHITE][sq] |= BIT(sq + 7);
        if (file < 7 && rank < 7) pawn_attacks[WHITE][sq] |= BIT(sq + 9);
        if (file > 0 && rank > 0) pawn_attacks[BLACK][sq] |= BIT(sq - 9);
        if (file < 7 && rank > 0) pawn_attacks[BLACK][sq] |= BIT(sq - 7);

        for (f = file - 1; f <= file + 1; f++) {
            for (r = rank - 1; r <= rank + 1; r++) {
                if (f == file && r == rank) continue;
                if (f >= 0 && f < 8 && r >= 0 && r < 8) king_attacks[sq] |= BIT(SQ(f, r));
            }
        }

        {
            static const int knight_df[8] = { -2, -2, -1, -1, 1, 1, 2, 2 };
            static const int knight_dr[8] = { -1, 1, -2, 2, -2, 2, -1, 1 };
            int i;
            for (i = 0; i < 8; i++) {
                f = file + knight_df[i];
                r = rank + knight_dr[i];
                if (f >= 0 && f < 8 && r >= 0 && r < 8) knight_attacks[sq] |= BIT(SQ(f, r));
            }
        }

        for (r = rank + 1; r < 8; r++) ray_north[sq] |= BIT(SQ(file, r));
        for (r = rank - 1; r >= 0; r--) ray_south[sq] |= BIT(SQ(file, r));
        for (f = file + 1; f < 8; f++) ray_east[sq] |= BIT(SQ(f, rank));
        for (f = file - 1; f >= 0; f--) ray_west[sq] |= BIT(SQ(f, rank));
        for (f = file + 1, r = rank + 1; f < 8 && r < 8; f++, r++) ray_ne[sq] |= BIT(SQ(f, r));
        for (f = file - 1, r = rank + 1; f >= 0 && r < 8; f--, r++) ray_nw[sq] |= BIT(SQ(f, r));
        for (f = file + 1, r = rank - 1; f < 8 && r >= 0; f++, r--) ray_se[sq] |= BIT(SQ(f, r));
        for (f = file - 1, r = rank - 1; f >= 0 && r >= 0; f--, r--) ray_sw[sq] |= BIT(SQ(f, r));
        ortho_lines[sq] = ray_north[sq] | ray_south[sq] | ray_east[sq] | ray_west[sq];
        diag_lines[sq] = ray_ne[sq] | ray_nw[sq] | ray_se[sq] | ray_sw[sq];

        bit = 0ULL;
        for (r = rank + 1; r < 7; r++) bit |= BIT(SQ(file, r));
        for (r = rank - 1; r > 0; r--) bit |= BIT(SQ(file, r));
        for (f = file + 1; f < 7; f++) bit |= BIT(SQ(f, rank));
        for (f = file - 1; f > 0; f--) bit |= BIT(SQ(f, rank));
        rook_occ_masks[sq] = bit;

        bit = 0ULL;
        for (f = file + 1, r = rank + 1; f < 7 && r < 7; f++, r++) bit |= BIT(SQ(f, r));
        for (f = file - 1, r = rank + 1; f > 0 && r < 7; f--, r++) bit |= BIT(SQ(f, r));
        for (f = file + 1, r = rank - 1; f < 7 && r > 0; f++, r--) bit |= BIT(SQ(f, r));
        for (f = file - 1, r = rank - 1; f > 0 && r > 0; f--, r--) bit |= BIT(SQ(f, r));
        bishop_occ_masks[sq] = bit;

        if (file > 0) isolated_masks[sq] |= file_masks[file - 1];
        if (file < 7) isolated_masks[sq] |= file_masks[file + 1];

        bit = 0ULL;
        for (r = rank + 1; r < 8; r++) {
            for (f = file - 1; f <= file + 1; f++) {
                if (f >= 0 && f < 8) bit |= BIT(SQ(f, r));
            }
        }
        passed_masks[WHITE][sq] = bit;

        bit = 0ULL;
        for (r = rank - 1; r >= 0; r--) {
            for (f = file - 1; f <= file + 1; f++) {
                if (f >= 0 && f < 8) bit |= BIT(SQ(f, r));
            }
        }
        passed_masks[BLACK][sq] = bit;
    }

    for (sq = 0; sq < 64; sq++) {
        U64 mask = rook_occ_masks[sq];
        int bit_index = 0;
        unsigned subset_limit;
        unsigned subset;

        while (mask) {
            int rel_sq = lsb_index(mask);
            mask &= mask - 1ULL;
            rook_rel_index[sq][rel_sq] = (int8_t)bit_index++;
        }
        subset_limit = 1u << (unsigned)bit_index;
        for (subset = 0; subset < subset_limit; subset++) {
            U64 subset_occ = 0ULL;
            U64 subset_mask = rook_occ_masks[sq];
            int subset_bit = 0;
            while (subset_mask) {
                int rel_sq = lsb_index(subset_mask);
                subset_mask &= subset_mask - 1ULL;
                if (subset & (1u << (unsigned)subset_bit)) subset_occ |= BIT(rel_sq);
                subset_bit++;
            }
            rook_attack_table[sq][subset] = build_rook_attack(sq, subset_occ);
        }

        mask = bishop_occ_masks[sq];
        bit_index = 0;
        while (mask) {
            int rel_sq = lsb_index(mask);
            mask &= mask - 1ULL;
            bishop_rel_index[sq][rel_sq] = (int8_t)bit_index++;
        }
        subset_limit = 1u << (unsigned)bit_index;
        for (subset = 0; subset < subset_limit; subset++) {
            U64 subset_occ = 0ULL;
            U64 subset_mask = bishop_occ_masks[sq];
            int subset_bit = 0;
            while (subset_mask) {
                int rel_sq = lsb_index(subset_mask);
                subset_mask &= subset_mask - 1ULL;
                if (subset & (1u << (unsigned)subset_bit)) subset_occ |= BIT(rel_sq);
                subset_bit++;
            }
            bishop_attack_table[sq][subset] = build_bishop_attack(sq, subset_occ);
        }
    }

    for (sq = 0; sq < 64; sq++) {
        int to;
        for (to = 0; to < 64; to++) {
            int df = FILE_OF(to) - FILE_OF(sq);
            int dr = RANK_OF(to) - RANK_OF(sq);
            int step_file = 0;
            int step_rank = 0;

            if (sq == to) continue;
            if (df == 0) step_rank = dr > 0 ? 1 : -1;
            else if (dr == 0) step_file = df > 0 ? 1 : -1;
            else if ((df > 0 ? df : -df) == (dr > 0 ? dr : -dr)) {
                step_file = df > 0 ? 1 : -1;
                step_rank = dr > 0 ? 1 : -1;
            } else {
                continue;
            }

            {
                int f = FILE_OF(sq) + step_file;
                int r = RANK_OF(sq) + step_rank;
                while (f != FILE_OF(to) || r != RANK_OF(to)) {
                    between_masks[sq][to] |= BIT(SQ(f, r));
                    f += step_file;
                    r += step_rank;
                }
                between_masks[sq][to] &= ~(BIT(sq) | BIT(to));
            }
        }
    }
    castle_rights_mask[0] = (uint8_t)~CASTLE_WQ & 15u;
    castle_rights_mask[4] = (uint8_t)~(CASTLE_WK | CASTLE_WQ) & 15u;
    castle_rights_mask[7] = (uint8_t)~CASTLE_WK & 15u;
    castle_rights_mask[56] = (uint8_t)~CASTLE_BQ & 15u;
    castle_rights_mask[60] = (uint8_t)~(CASTLE_BK | CASTLE_BQ) & 15u;
    castle_rights_mask[63] = (uint8_t)~CASTLE_BK & 15u;
    LATENCY_SCOPE_END();
}

static void init_zobrist(void) {
    LATENCY_SCOPE(LAT_INIT_ZOBRIST);
    U64 seed = 0x4d41544956414348ULL;
    int p, sq, i;
    for (p = 0; p < 12; p++) {
        for (sq = 0; sq < 64; sq++) zobrist_piece[p][sq] = splitmix64(&seed);
    }
    zobrist_castle[0] = 0ULL;
    for (i = 1; i < 16; i++) zobrist_castle[i] = splitmix64(&seed);
    for (i = 0; i < 8; i++) zobrist_ep[i] = splitmix64(&seed);
    zobrist_side = splitmix64(&seed);
    LATENCY_SCOPE_END();
}

static void init_engine(void) {
    LATENCY_SCOPE(LAT_INIT_ENGINE);
    if (engine_initialized) {
        LATENCY_SCOPE_END();
        return;
    }
    init_masks();
    init_zobrist();
    engine_initialized = 1;
    LATENCY_SCOPE_END();
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

#ifdef _WIN32
static int gui_min_int(int a, int b) {
    return a < b ? a : b;
}

static int gui_max_int(int a, int b) {
    return a > b ? a : b;
}

static void gui_set_status(GuiState *gs, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gs->status, sizeof(gs->status), fmt, ap);
    va_end(ap);
    gs->status[sizeof(gs->status) - 1] = '\0';
}

static void gui_get_layout(HWND hwnd, GuiLayout *layout) {
    RECT rc;
    int width;
    int height;
    int board_px;
    int panel_w;

    GetClientRect(hwnd, &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    panel_w = width >= 760 ? 240 : 200;
    board_px = gui_min_int(width - panel_w - 56, height - 48);
    if (board_px < 240) board_px = gui_min_int(width - 32, height - 160);
    if (board_px < 160) board_px = gui_min_int(width - 24, height - 96);
    if (board_px < 80) board_px = 80;
    board_px -= board_px % 8;
    if (board_px < 8) board_px = 8;

    layout->cell = board_px / 8;
    layout->board.left = 20;
    layout->board.top = gui_max_int(20, (height - board_px) / 2);
    layout->board.right = layout->board.left + board_px;
    layout->board.bottom = layout->board.top + board_px;

    layout->panel.left = layout->board.right + 20;
    layout->panel.top = 20;
    layout->panel.right = width - 20;
    layout->panel.bottom = height - 20;

    layout->btn_time_minus.left = layout->panel.left;
    layout->btn_time_minus.top = layout->panel.top + 122;
    layout->btn_time_minus.right = layout->panel.left + (layout->panel.right - layout->panel.left - 8) / 2;
    layout->btn_time_minus.bottom = layout->btn_time_minus.top + 34;

    layout->btn_time_plus = layout->btn_time_minus;
    layout->btn_time_plus.left = layout->btn_time_minus.right + 8;
    layout->btn_time_plus.right = layout->panel.right;

    layout->btn_white.left = layout->panel.left;
    layout->btn_white.top = layout->panel.top + 176;
    layout->btn_white.right = layout->panel.right;
    layout->btn_white.bottom = layout->btn_white.top + 36;

    layout->btn_black = layout->btn_white;
    layout->btn_black.top += 46;
    layout->btn_black.bottom += 46;

    layout->btn_flip = layout->btn_black;
    layout->btn_flip.top += 46;
    layout->btn_flip.bottom += 46;
}

static int gui_display_file(const GuiState *gs, int sq) {
    int file = FILE_OF(sq);
    return gs->flip ? 7 - file : file;
}

static int gui_display_rank(const GuiState *gs, int sq) {
    int rank = RANK_OF(sq);
    return gs->flip ? rank : 7 - rank;
}

static RECT gui_square_rect(const GuiState *gs, const GuiLayout *layout, int sq) {
    RECT rc;
    int file = gui_display_file(gs, sq);
    int rank = gui_display_rank(gs, sq);
    rc.left = layout->board.left + file * layout->cell;
    rc.top = layout->board.top + rank * layout->cell;
    rc.right = rc.left + layout->cell;
    rc.bottom = rc.top + layout->cell;
    return rc;
}

static void gui_invalidate_panel(GuiState *gs, int erase) {
    GuiLayout layout;
    if (!gs || !gs->hwnd) return;
    gui_get_layout(gs->hwnd, &layout);
    InvalidateRect(gs->hwnd, &layout.panel, erase);
}

static int gui_point_in_rect(const RECT *rc, int x, int y) {
    return x >= rc->left && x < rc->right && y >= rc->top && y < rc->bottom;
}

static int gui_point_to_square(const GuiState *gs, const GuiLayout *layout, int x, int y) {
    int disp_file;
    int disp_rank;
    if (!gui_point_in_rect(&layout->board, x, y)) return -1;
    disp_file = (x - layout->board.left) / layout->cell;
    disp_rank = (y - layout->board.top) / layout->cell;
    if (disp_file < 0 || disp_file > 7 || disp_rank < 0 || disp_rank > 7) return -1;
    if (gs->flip) return SQ(7 - disp_file, disp_rank);
    return SQ(disp_file, 7 - disp_rank);
}

static void gui_ensure_fonts(GuiState *gs, int cell) {
    int piece_px = cell - cell / 5;
    if (piece_px < 28) piece_px = 28;
    if (!gs->text_font) {
        gs->text_font = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    }
    if (!gs->piece_font || gs->piece_font_px != piece_px) {
        if (gs->piece_font) DeleteObject(gs->piece_font);
        gs->piece_font = CreateFontW(-piece_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI Symbol");
        gs->piece_font_px = piece_px;
    }
}

static int gui_collect_legal_moves(const Position *src, int from, int to, U32 *out_moves, int cap, U64 *out_targets) {
    MoveList list;
    Position work = *src;
    int count = 0;
    int i;
    if (out_targets) *out_targets = 0ULL;
    generate_moves(src, &list, 0);
    for (i = 0; i < list.count; i++) {
        Undo undo;
        U32 move = list.moves[i].move;
        if (from >= 0 && MOVE_FROM(move) != from) continue;
        if (to >= 0 && MOVE_TO(move) != to) continue;
        if (!make_move(&work, move, &undo)) continue;
        if (out_targets) *out_targets |= BIT(MOVE_TO(move));
        if (out_moves && count < cap) out_moves[count] = move;
        count++;
        unmake_move(&work, move, &undo);
    }
    return count;
}

static int gui_legal_move_count(const Position *pos) {
    MoveList list;
    Position work = *pos;
    int i;
    int legal = 0;
    generate_moves(pos, &list, 0);
    for (i = 0; i < list.count; i++) {
        Undo undo;
        if (!make_move(&work, list.moves[i].move, &undo)) continue;
        legal++;
        unmake_move(&work, list.moves[i].move, &undo);
    }
    return legal;
}

static U32 gui_validate_move(const Position *pos, U32 move) {
    Position work = *pos;
    Undo undo;
    if (!move) return 0;
    return make_move(&work, move, &undo) ? move : 0;
}

static void gui_update_status(GuiState *gs) {
    char ponder_buf[6];
    if (gs->pos.halfmove >= 100) {
        gs->game_over = 1;
        gui_set_status(gs, "Draw by fifty-move rule.");
        return;
    }
    if (insufficient_material(&gs->pos)) {
        gs->game_over = 1;
        gui_set_status(gs, "Draw by insufficient material.");
        return;
    }
    if (is_repetition(&gs->pos)) {
        gs->game_over = 1;
        gui_set_status(gs, "Draw by repetition.");
        return;
    }
    if (gui_legal_move_count(&gs->pos) == 0) {
        gs->game_over = 1;
        if (in_check(&gs->pos, gs->pos.side)) {
            gui_set_status(gs, gs->pos.side == gs->human_side ? "Checkmate. Engine wins." : "Checkmate. You win.");
        } else {
            gui_set_status(gs, "Draw by stalemate.");
        }
        return;
    }

    gs->game_over = 0;
    if (gs->engine_thinking) {
        gui_set_status(gs, in_check(&gs->pos, gs->pos.side) ? "Engine thinking. Check." : "Engine thinking...");
    } else if (gs->pos.side == gs->human_side && gs->engine_pondering && gs->ponder_move) {
        move_to_string(gs->ponder_move, ponder_buf);
        snprintf(gs->status, sizeof(gs->status),
                 "%s Engine pondering %s.",
                 in_check(&gs->pos, gs->pos.side) ? "Your move. Check." : "Your move.",
                 ponder_buf);
    } else if (gs->pos.side == gs->human_side) {
        gui_set_status(gs, in_check(&gs->pos, gs->pos.side) ? "Your move. Check." : "Your move.");
    } else {
        gui_set_status(gs, in_check(&gs->pos, gs->pos.side) ? "Engine to move. Check." : "Engine to move.");
    }
}

static int gui_choose_promotion(HWND hwnd, int x, int y) {
    HMENU menu = CreatePopupMenu();
    int cmd;
    AppendMenuW(menu, MF_STRING, GUI_CMD_PROMO_QUEEN, L"Queen");
    AppendMenuW(menu, MF_STRING, GUI_CMD_PROMO_ROOK, L"Rook");
    AppendMenuW(menu, MF_STRING, GUI_CMD_PROMO_BISHOP, L"Bishop");
    AppendMenuW(menu, MF_STRING, GUI_CMD_PROMO_KNIGHT, L"Knight");
    cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, x, y, 0, hwnd, NULL);
    DestroyMenu(menu);
    switch (cmd) {
    case GUI_CMD_PROMO_QUEEN: return QUEEN;
    case GUI_CMD_PROMO_ROOK: return ROOK;
    case GUI_CMD_PROMO_BISHOP: return BISHOP;
    case GUI_CMD_PROMO_KNIGHT: return KNIGHT;
    default: return 0;
    }
}

static void gui_clear_selection(GuiState *gs) {
    gs->selected_sq = -1;
    gs->target_mask = 0ULL;
}

static void gui_clear_live_pv(GuiState *gs) {
    gs->live_pv[0] = '\0';
}

static int gui_can_edit_time(const GuiState *gs) {
    return gs->last_move == 0;
}

static int gui_adjust_engine_time(GuiState *gs, int dir) {
    static const int GUI_TIME_PRESETS[] = { 50, 100, 150, 200, 250, 350, 500, 750, 1000, 1500, 2000, 3000, 5000 };
    int i;
    int index = 0;
    int best_diff = INT_MAX;
    int next;
    int restart_search = 0;

    if (!gui_can_edit_time(gs) || dir == 0) return 0;

    for (i = 0; i < ARRAY_LEN(GUI_TIME_PRESETS); i++) {
        int diff = GUI_TIME_PRESETS[i] - gs->engine_ms;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) {
            best_diff = diff;
            index = i;
        }
    }

    next = index + (dir > 0 ? 1 : -1);
    if (next < 0) next = 0;
    if (next >= ARRAY_LEN(GUI_TIME_PRESETS)) next = ARRAY_LEN(GUI_TIME_PRESETS) - 1;
    if (GUI_TIME_PRESETS[next] == gs->engine_ms) return 0;

    restart_search = gs->engine_thread != NULL && gs->pos.side != gs->human_side;
    if (restart_search) gui_cancel_engine_search(gs);

    gs->engine_ms = GUI_TIME_PRESETS[next];
    gui_update_status(gs);
    gui_invalidate_panel(gs, FALSE);

    if (restart_search && !gs->game_over && gs->pos.side != gs->human_side) {
        gui_start_engine_search(gs);
    }
    return 1;
}

static int gui_apply_move(GuiState *gs, U32 move) {
    Undo undo;
    if (!make_move(&gs->pos, move, &undo)) return 0;
    gs->last_move = move;
    gui_clear_selection(gs);
    return 1;
}

static DWORD WINAPI gui_engine_thread_proc(LPVOID param) {
    GuiSearchTask *task = (GuiSearchTask *)param;
    int saved_emit = g_emit_search_info;
    g_emit_search_info = 0;
    gui_begin_search_reporting(task);
    task->best_move = search_best_move(&task->pos, &task->limits);
    gui_end_search_reporting();
    g_emit_search_info = saved_emit;
    if (!PostMessage(task->hwnd, GUI_WM_ENGINE_DONE, 0, (LPARAM)task)) free(task);
    return 0;
}

static void gui_cancel_engine_search(GuiState *gs) {
    if (!gs->engine_thread) return;
    gs->search_cookie++;
    g_si.stop = 1;
    WaitForSingleObject(gs->engine_thread, 5000);
    CloseHandle(gs->engine_thread);
    gs->engine_thread = NULL;
    gs->engine_thinking = 0;
    gs->engine_pondering = 0;
    gs->ponder_move = 0;
    gui_clear_live_pv(gs);
}

static void gui_start_engine_search(GuiState *gs) {
    GuiSearchTask *task;
    DWORD thread_id;
    if (gs->engine_thread || gs->engine_thinking || gs->game_over || gs->pos.side == gs->human_side) return;

    task = (GuiSearchTask *)malloc(sizeof(*task));
    if (!task) {
        gui_set_status(gs, "Unable to start search thread.");
        return;
    }

    memset(task, 0, sizeof(*task));
    task->hwnd = gs->hwnd;
    task->pos = gs->pos;
    init_search_limits(&task->limits);
    task->limits.movetime_ms = gs->engine_ms > 0 ? gs->engine_ms : 350;
    task->cookie = ++gs->search_cookie;
    task->mode = GUI_SEARCH_MOVE;
    task->ponder_move = 0;
    task->best_move = 0;

    gs->engine_thinking = 1;
    gs->engine_pondering = 0;
    gs->ponder_move = 0;
    gui_clear_live_pv(gs);
    gui_update_status(gs);
    InvalidateRect(gs->hwnd, NULL, FALSE);

    gs->engine_thread = CreateThread(NULL, 0, gui_engine_thread_proc, task, 0, &thread_id);
    if (!gs->engine_thread) {
        gs->engine_thinking = 0;
        gui_update_status(gs);
        free(task);
    }
    (void)thread_id;
}

static void gui_start_ponder_search(GuiState *gs) {
    GuiSearchTask *task;
    Position ponder_pos;
    Undo undo;
    U32 ponder_move;
    DWORD thread_id;

    if (gs->engine_thread || gs->engine_thinking || gs->engine_pondering || gs->game_over || gs->pos.side != gs->human_side) return;

    ponder_move = gui_validate_move(&gs->pos, reuse_move_for_position(&gs->pos));
    if (!ponder_move) return;

    ponder_pos = gs->pos;
    if (!make_move(&ponder_pos, ponder_move, &undo)) return;

    task = (GuiSearchTask *)malloc(sizeof(*task));
    if (!task) return;

    memset(task, 0, sizeof(*task));
    task->hwnd = gs->hwnd;
    task->pos = ponder_pos;
    init_search_limits(&task->limits);
    task->limits.movetime_ms = gs->engine_ms > 0 ? gs->engine_ms : 350;
    task->limits.ponder = 1;
    task->cookie = ++gs->search_cookie;
    task->ponder_move = ponder_move;
    task->mode = GUI_SEARCH_PONDER;

    gs->engine_pondering = 1;
    gs->ponder_move = ponder_move;
    gui_clear_live_pv(gs);
    gui_update_status(gs);
    InvalidateRect(gs->hwnd, NULL, FALSE);

    gs->engine_thread = CreateThread(NULL, 0, gui_engine_thread_proc, task, 0, &thread_id);
    if (!gs->engine_thread) {
        gs->engine_pondering = 0;
        gs->ponder_move = 0;
        gui_update_status(gs);
        free(task);
    }
    (void)thread_id;
}

static void gui_reset_game(GuiState *gs, int human_side) {
    gui_cancel_engine_search(gs);
    clear_hash();
    clear_search_heuristics();
    clear_position_cache();
    setup_classic_startpos(&gs->pos);
    gs->human_side = human_side;
    gs->flip = human_side == BLACK;
    gs->last_move = 0;
    gs->ponder_move = 0;
    gui_clear_selection(gs);
    gui_clear_live_pv(gs);
    gs->engine_thinking = 0;
    gs->engine_pondering = 0;
    gs->game_over = 0;
    gui_update_status(gs);
    InvalidateRect(gs->hwnd, NULL, FALSE);
    if (gs->pos.side != gs->human_side) gui_start_engine_search(gs);
}

static void gui_finish_turn(GuiState *gs) {
    gui_update_status(gs);
    InvalidateRect(gs->hwnd, NULL, FALSE);
    if (gs->game_over || gs->engine_thread) return;
    if (gs->pos.side != gs->human_side) gui_start_engine_search(gs);
    else gui_start_ponder_search(gs);
}

static void gui_try_move_from_selection(GuiState *gs, int to_sq, int x, int y) {
    U32 moves[8];
    int count = gui_collect_legal_moves(&gs->pos, gs->selected_sq, to_sq, moves, ARRAY_LEN(moves), NULL);
    U32 chosen = 0;
    int i;

    if (count <= 0) return;
    if (count == 1) {
        chosen = moves[0];
    } else {
        int promo = gui_choose_promotion(gs->hwnd, x, y);
        if (!promo) return;
        for (i = 0; i < count; i++) {
            if (MOVE_PROMO(moves[i]) == promo) {
                chosen = moves[i];
                break;
            }
        }
        if (!chosen) chosen = moves[0];
    }

    if (gs->engine_pondering && chosen != gs->ponder_move) {
        gui_cancel_engine_search(gs);
    }

    if (!gui_apply_move(gs, chosen)) return;

    gui_update_status(gs);
    InvalidateRect(gs->hwnd, NULL, FALSE);

    if (gs->game_over) {
        if (gs->engine_pondering) gui_cancel_engine_search(gs);
        return;
    }

    if (gs->engine_pondering && chosen == gs->ponder_move && gs->engine_thread) {
        gs->engine_pondering = 0;
        gs->ponder_move = 0;
        gs->engine_thinking = 1;
        search_activate_ponderhit();
        gui_update_status(gs);
        InvalidateRect(gs->hwnd, NULL, FALSE);
        return;
    }

    gui_finish_turn(gs);
}

static void gui_handle_board_click(GuiState *gs, int x, int y) {
    GuiLayout layout;
    int sq;
    int piece;
    POINT screen_pt;

    if (gs->engine_thinking || gs->game_over || gs->pos.side != gs->human_side) return;

    gui_get_layout(gs->hwnd, &layout);
    sq = gui_point_to_square(gs, &layout, x, y);
    if (sq < 0) {
        gui_clear_selection(gs);
        InvalidateRect(gs->hwnd, NULL, FALSE);
        return;
    }

    piece = gs->pos.board[sq];
    if (gs->selected_sq >= 0 && (gs->target_mask & BIT(sq))) {
        screen_pt.x = x;
        screen_pt.y = y;
        ClientToScreen(gs->hwnd, &screen_pt);
        gui_try_move_from_selection(gs, sq, screen_pt.x, screen_pt.y);
        return;
    }

    if (piece != NO_PIECE && PIECE_COLOR[piece] == gs->human_side) {
        gs->selected_sq = sq;
        gui_collect_legal_moves(&gs->pos, sq, -1, NULL, 0, &gs->target_mask);
    } else if (gs->selected_sq == sq) {
        gui_clear_selection(gs);
    } else {
        gui_clear_selection(gs);
    }
    InvalidateRect(gs->hwnd, NULL, FALSE);
}

static void gui_draw_button(HDC hdc, const RECT *rc, const char *label, int enabled) {
    COLORREF edge = enabled ? RGB(183, 145, 68) : RGB(92, 98, 106);
    COLORREF fill = enabled ? RGB(68, 77, 88) : RGB(52, 58, 64);
    COLORREF text_color = enabled ? RGB(244, 239, 224) : RGB(164, 168, 172);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    char text[64];

    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, 10, 10);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    strncpy(text, label, sizeof(text) - 1u);
    text[sizeof(text) - 1u] = '\0';
    SetTextColor(hdc, text_color);
    DrawTextA(hdc, text, -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void gui_paint(GuiState *gs, HDC hdc) {
    GuiLayout layout;
    HBRUSH bg = CreateSolidBrush(RGB(26, 31, 38));
    HBRUSH panel = CreateSolidBrush(RGB(35, 41, 48));
    RECT client;
    int sq;
    char info[1024];
    char last_move[8];
    char ponder_move[8];
    const char *live_label;
    const char *live_pv;
    const char *time_lock_text;
    int rank;
    int file;

    GetClientRect(gs->hwnd, &client);
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    gui_get_layout(gs->hwnd, &layout);
    gui_ensure_fonts(gs, layout.cell);
    FillRect(hdc, &layout.panel, panel);
    DeleteObject(panel);
    SetBkMode(hdc, TRANSPARENT);

    for (sq = 0; sq < 64; sq++) {
        RECT rc = gui_square_rect(gs, &layout, sq);
        int dark = ((FILE_OF(sq) + RANK_OF(sq)) & 1);
        COLORREF base = dark ? RGB(116, 137, 156) : RGB(226, 214, 190);
        HBRUSH square_brush;

        if (gs->last_move && (sq == MOVE_FROM(gs->last_move) || sq == MOVE_TO(gs->last_move))) {
            base = dark ? RGB(164, 140, 88) : RGB(236, 216, 148);
        }
        if (sq == gs->selected_sq) {
            base = dark ? RGB(87, 160, 144) : RGB(155, 220, 204);
        }

        square_brush = CreateSolidBrush(base);
        FillRect(hdc, &rc, square_brush);
        DeleteObject(square_brush);

        if (gs->target_mask & BIT(sq)) {
            HBRUSH dot_brush = CreateSolidBrush(RGB(210, 82, 82));
            HGDIOBJ old_brush = SelectObject(hdc, dot_brush);
            HGDIOBJ old_pen = SelectObject(hdc, GetStockObject(NULL_PEN));
            int dot = layout.cell / 5;
            if (dot < 6) dot = 6;
            Ellipse(hdc,
                    (rc.left + rc.right) / 2 - dot,
                    (rc.top + rc.bottom) / 2 - dot,
                    (rc.left + rc.right) / 2 + dot,
                    (rc.top + rc.bottom) / 2 + dot);
            SelectObject(hdc, old_pen);
            SelectObject(hdc, old_brush);
            DeleteObject(dot_brush);
        }

        if (gs->pos.board[sq] != NO_PIECE) {
            RECT text_rc = rc;
            int piece = gs->pos.board[sq];
            COLORREF color = PIECE_COLOR[piece] == WHITE ? RGB(250, 247, 238) : RGB(28, 32, 36);
            SetTextColor(hdc, RGB(0, 0, 0));
            SelectObject(hdc, gs->piece_font);
            OffsetRect(&text_rc, 2, 2);
            DrawTextW(hdc, GUI_PIECE_GLYPHS[piece], -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            OffsetRect(&text_rc, -2, -2);
            SetTextColor(hdc, color);
            DrawTextW(hdc, GUI_PIECE_GLYPHS[piece], -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    SelectObject(hdc, gs->text_font);
    SetTextColor(hdc, RGB(210, 200, 174));
    for (file = 0; file < 8; file++) {
        char label[2];
        RECT label_rc;
        label[0] = gs->flip ? (char)('h' - file) : (char)('a' + file);
        label[1] = '\0';
        label_rc.left = layout.board.left + file * layout.cell;
        label_rc.right = label_rc.left + layout.cell;
        label_rc.top = layout.board.bottom + 2;
        label_rc.bottom = layout.board.bottom + 24;
        DrawTextA(hdc, label, -1, &label_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    for (rank = 0; rank < 8; rank++) {
        char label[2];
        RECT label_rc;
        label[0] = gs->flip ? (char)('1' + rank) : (char)('8' - rank);
        label[1] = '\0';
        label_rc.left = layout.board.left - 20;
        label_rc.right = layout.board.left - 2;
        label_rc.top = layout.board.top + rank * layout.cell + layout.cell / 2 - 10;
        label_rc.bottom = label_rc.top + 20;
        DrawTextA(hdc, label, -1, &label_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    gui_draw_button(hdc, &layout.btn_time_minus, "Time -", gui_can_edit_time(gs));
    gui_draw_button(hdc, &layout.btn_time_plus, "Time +", gui_can_edit_time(gs));
    gui_draw_button(hdc, &layout.btn_white, "New Game: White", 1);
    gui_draw_button(hdc, &layout.btn_black, "New Game: Black", 1);
    gui_draw_button(hdc, &layout.btn_flip, "Flip Board", 1);

    if (gs->last_move) move_to_string(gs->last_move, last_move);
    else strcpy(last_move, "-");
    if (gs->engine_pondering && gs->ponder_move) move_to_string(gs->ponder_move, ponder_move);
    else strcpy(ponder_move, "-");
    if (gs->engine_pondering) live_label = "Ponder PV";
    else if (gs->engine_thinking) live_label = "Engine PV";
    else live_label = "Last PV";
    live_pv = gs->live_pv[0] ? gs->live_pv : "-";
    time_lock_text = gui_can_edit_time(gs) ? "editable now" : "locked after first move";

    snprintf(info, sizeof(info),
             "MativaChess\n\n"
             "You play: %s\n"
             "Engine move time: %d ms\n"
             "Time setting: %s\n"
             "Last move: %s\n"
             "Ponder move: %s\n"
             "%s: %s\n"
             "Turn: %s\n\n"
             "%s\n\n"
             "Controls:\n"
             "Click a piece, then click a highlighted square.\n"
             "Promotions open a small menu.\n"
             "Keys: N = new white, B = new black, F = flip, Esc = clear selection.",
             gs->human_side == WHITE ? "White" : "Black",
             gs->engine_ms,
             time_lock_text,
             last_move,
             ponder_move,
             live_label,
             live_pv,
             gs->pos.side == WHITE ? "White" : "Black",
             gs->status);
    SetTextColor(hdc, RGB(240, 238, 228));
    DrawTextA(hdc, info, -1, &layout.panel, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

static void gui_on_search_info(GuiState *gs, GuiSearchInfoMsg *msg) {
    if (msg->cookie == gs->search_cookie) {
        if (strcmp(gs->live_pv, msg->pv) != 0) {
            strncpy(gs->live_pv, msg->pv, sizeof(gs->live_pv) - 1u);
            gs->live_pv[sizeof(gs->live_pv) - 1u] = '\0';
            gui_invalidate_panel(gs, FALSE);
        }
    }
    free(msg);
}

static void gui_on_engine_done(GuiState *gs, GuiSearchTask *task) {
    if (task->cookie != gs->search_cookie) {
        free(task);
        return;
    }
    if (gs->engine_thread) {
        CloseHandle(gs->engine_thread);
        gs->engine_thread = NULL;
    }
    if (task->mode == GUI_SEARCH_PONDER) {
        gs->engine_pondering = 0;
        gs->ponder_move = 0;
    }
    gs->engine_thinking = 0;
    if (task->best_move && !gs->game_over && gs->pos.side != gs->human_side) gui_apply_move(gs, task->best_move);
    gui_finish_turn(gs);
    free(task);
}

static LRESULT CALLBACK gui_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    GuiState *gs = (GuiState *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lparam;
        gs = (GuiState *)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)gs);
        gs->hwnd = hwnd;
        gs->engine_ms = 350;
        gs->selected_sq = -1;
        gs->search_cookie = 1;
        gs->ponder_move = 0;
        gui_reset_game(gs, WHITE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lparam;
        mmi->ptMinTrackSize.x = 760;
        mmi->ptMinTrackSize.y = 620;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        GuiLayout layout;
        int x = (int)(short)LOWORD(lparam);
        int y = (int)(short)HIWORD(lparam);
        gui_get_layout(hwnd, &layout);
        if (gui_point_in_rect(&layout.btn_time_minus, x, y)) gui_adjust_engine_time(gs, -1);
        else if (gui_point_in_rect(&layout.btn_time_plus, x, y)) gui_adjust_engine_time(gs, 1);
        else if (gui_point_in_rect(&layout.btn_white, x, y)) gui_reset_game(gs, WHITE);
        else if (gui_point_in_rect(&layout.btn_black, x, y)) gui_reset_game(gs, BLACK);
        else if (gui_point_in_rect(&layout.btn_flip, x, y)) {
            gs->flip ^= 1;
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
            gui_handle_board_click(gs, x, y);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (!gs) break;
        if (wparam == 'N') gui_reset_game(gs, WHITE);
        else if (wparam == 'B') gui_reset_game(gs, BLACK);
        else if (wparam == 'F') {
            gs->flip ^= 1;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (wparam == VK_ESCAPE) {
            gui_clear_selection(gs);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case GUI_WM_SEARCH_INFO:
        if (gs) gui_on_search_info(gs, (GuiSearchInfoMsg *)lparam);
        return 0;
    case GUI_WM_ENGINE_DONE:
        if (gs) gui_on_engine_done(gs, (GuiSearchTask *)lparam);
        return 0;
    case WM_PAINT:
        if (gs) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client;
            int width;
            int height;
            HDC memdc;
            HBITMAP dib = NULL;
            HGDIOBJ oldbmp = NULL;

            GetClientRect(hwnd, &client);
            width = client.right - client.left;
            height = client.bottom - client.top;

            if (width > 0 && height > 0) {
                memdc = CreateCompatibleDC(hdc);
                if (memdc) {
                    dib = CreateCompatibleBitmap(hdc, width, height);
                    if (dib) {
                        oldbmp = SelectObject(memdc, dib);
                        gui_paint(gs, memdc);
                        BitBlt(hdc,
                               ps.rcPaint.left, ps.rcPaint.top,
                               ps.rcPaint.right - ps.rcPaint.left,
                               ps.rcPaint.bottom - ps.rcPaint.top,
                               memdc,
                               ps.rcPaint.left, ps.rcPaint.top,
                               SRCCOPY);
                        SelectObject(memdc, oldbmp);
                    } else {
                        gui_paint(gs, hdc);
                    }
                    if (dib) DeleteObject(dib);
                    DeleteDC(memdc);
                } else {
                    gui_paint(gs, hdc);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (gs) gui_cancel_engine_search(gs);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (gs) {
            if (gs->engine_thread) {
                CloseHandle(gs->engine_thread);
                gs->engine_thread = NULL;
            }
            if (gs->piece_font) DeleteObject(gs->piece_font);
            if (gs->text_font) DeleteObject(gs->text_font);
            gs->piece_font = NULL;
            gs->text_font = NULL;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static int gui_should_default(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || in == NULL) return 1;
    return GetFileType(in) != FILE_TYPE_PIPE;
}

static int gui_arg_matches(const char *arg, const char *a, const char *b) {
    return strcmp(arg, a) == 0 || strcmp(arg, b) == 0;
}

static int run_gui(int argc, char **argv) {
    GuiState gs;
    MSG msg;
    WNDCLASSA wc;
    HWND hwnd;
    HWND console;
    int i;

    (void)argc;
    (void)argv;
    memset(&gs, 0, sizeof(gs));
    init_engine();

    console = GetConsoleWindow();
    if (console) ShowWindow(console, SW_HIDE);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = gui_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MativaChessWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassA(&wc);

    hwnd = CreateWindowExA(0, wc.lpszClassName, "MativaChess",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 980, 760,
                           NULL, NULL, wc.hInstance, &gs);
    if (!hwnd) return 1;

    for (i = 1; i < argc; i++) {
        if (gui_arg_matches(argv[i], "black", "--black")) gui_reset_game(&gs, BLACK);
        else if (gui_arg_matches(argv[i], "white", "--white")) gui_reset_game(&gs, WHITE);
    }

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
#endif

static void uci_loop(void) {
    Position pos;
    char line[4096];

    init_engine();
    clear_search_heuristics();
    clear_position_cache();
    setup_classic_startpos(&pos);

    while (fgets(line, sizeof(line), stdin)) {
        trim_newline(line);

        if (strcmp(line, "uci") == 0) {
            printf("id name MativaChess\n");
            printf("id author OpenAI\n");
            printf("option name Ponder type check default true\n");
            printf("option name Clear Hash type button\n");
            printf("uciok\n");
            fflush(stdout);
        } else if (strcmp(line, "isready") == 0) {
            printf("readyok\n");
            fflush(stdout);
        } else if (strcmp(line, "ucinewgame") == 0) {
            clear_hash();
            clear_search_heuristics();
            clear_position_cache();
            setup_classic_startpos(&pos);
        } else if (strncmp(line, "position ", 9) == 0) {
            apply_position_command(&pos, line);
        } else if (strncmp(line, "go ", 3) == 0 || strcmp(line, "go") == 0) {
            handle_go(&pos, line);
            if (g_si.quit) break;
        } else if (strncmp(line, "perft ", 6) == 0) {
            int depth = atoi(line + 6);
            printf("%llu\n", (unsigned long long)perft(&pos, depth));
            fflush(stdout);
        } else if (strncmp(line, "divide ", 7) == 0) {
            int depth = atoi(line + 7);
            divide(&pos, depth);
        } else if (strcmp(line, "check") == 0) {
            validate_position(&pos, 1);
        } else if (strcmp(line, "eval") == 0) {
            printf("eval %d\n", evaluate(&pos));
            fflush(stdout);
        } else if (strcmp(line, "selftest") == 0) {
            run_selftest();
        } else if (strncmp(line, "bench", 5) == 0) {
            int depth = 0;
            if (line[5] == ' ') depth = atoi(line + 6);
            run_bench(depth);
        } else if (strncmp(line, "latency", 7) == 0) {
            int depth = 0;
            if (line[7] == ' ') depth = atoi(line + 8);
            run_latency_report(depth);
        } else if (strcmp(line, "d") == 0) {
            print_board(&pos);
        } else if (strncmp(line, "setoption name Clear Hash", 25) == 0) {
            clear_hash();
            clear_search_heuristics();
            clear_position_cache();
        } else if (strncmp(line, "setoption name Ponder value ", 28) == 0) {
            const char *value = line + 28;
            g_uci_ponder_enabled = (strcmp(value, "false") != 0 && strcmp(value, "0") != 0);
        } else if (strcmp(line, "stop") == 0) {
            g_si.stop = 1;
        } else if (strcmp(line, "ponderhit") == 0) {
            search_activate_ponderhit();
        } else if (strcmp(line, "quit") == 0) {
            break;
        } else if (strncmp(line, "position", 8) == 0) {
            apply_position_command(&pos, line);
        }
    }
}

int main(int argc, char **argv) {
#ifdef _WIN32
    if (argc > 1) {
        if (strcmp(argv[1], "gui") == 0 || strcmp(argv[1], "--gui") == 0 ||
            strcmp(argv[1], "play") == 0 || strcmp(argv[1], "--play") == 0) {
            return run_gui(argc, argv);
        }
        if (strcmp(argv[1], "uci") == 0 || strcmp(argv[1], "--uci") == 0 ||
            strcmp(argv[1], "console") == 0 || strcmp(argv[1], "--console") == 0) {
            uci_loop();
            return 0;
        }
    }
    if (argc == 1 && gui_should_default()) return run_gui(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    uci_loop();
    return 0;
}
