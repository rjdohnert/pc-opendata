#include "database_engine.h"
#include <math.h>

#define HNSW_M 16
#define HNSW_M0 32
#define HNSW_MAX_LAYERS 6
#define HNSW_EF_CONSTRUCTION 64
#define HNSW_EF_SEARCH 32
#define HNSW_INITIAL_CAPACITY 64
#define HNSW_MAX_NODES 4096

typedef struct {
    int count;
    int neighbors[HNSW_M0];
} HnswNeighborList;

typedef struct {
    int row_index;
    int top_layer;
    float vector[64];
    int dim;
    HnswNeighborList layers[HNSW_MAX_LAYERS];
} HnswGraphNode;

typedef struct {
    int node_count;
    int capacity;
    int entry_node;
    int max_layer;
    int dim;
    int metric; /* 0: cosine (<=>), 1: L2 (<->) */
    HnswGraphNode* nodes;
} HnswGraph;

static float hnsw_calc_dist(const HnswGraph* g, const float* a, const float* b, int dim) {
    int safe_dim = (dim > 64) ? 64 : (dim < 1 ? 1 : dim);
    if (g->metric == 1) {
        /* L2 Euclidean Distance */
        float sum = 0.0f;
        for (int i = 0; i < safe_dim; i++) {
            float diff = a[i] - b[i];
            sum += diff * diff;
        }
        return sqrtf(sum);
    } else {
        /* Cosine Distance: 1 - (a.b / (|a|*|b|)) */
        float dot = 0.0f, na = 0.0f, nb = 0.0f;
        for (int i = 0; i < safe_dim; i++) {
            dot += a[i] * b[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
        }
        float denom = sqrtf(na) * sqrtf(nb);
        if (denom <= 1e-7f) return 1.0f;
        float sim = dot / denom;
        if (sim > 1.0f) sim = 1.0f;
        if (sim < -1.0f) sim = -1.0f;
        return 1.0f - sim;
    }
}

static int hnsw_random_layer(void) {
    int layer = 0;
    while (layer + 1 < HNSW_MAX_LAYERS && ((unsigned)rand() % 4) == 0) {
        layer++;
    }
    return layer;
}

static void hnsw_add_neighbor(HnswNeighborList* list, int neighbor, int max_conn) {
    for (int i = 0; i < list->count; i++) {
        if (list->neighbors[i] == neighbor) return;
    }
    if (list->count < max_conn) {
        list->neighbors[list->count++] = neighbor;
    }
}

static void hnsw_insert_node_internal(HnswGraph* g, int row_idx, const float* vec, int dim) {
    if (g->node_count >= HNSW_MAX_NODES) return;
    if (g->node_count >= g->capacity) {
        int new_cap = g->capacity ? g->capacity * 2 : HNSW_INITIAL_CAPACITY;
        HnswGraphNode* new_nodes = realloc(g->nodes, (size_t)new_cap * sizeof(HnswGraphNode));
        if (!new_nodes) return;
        g->nodes = new_nodes;
        g->capacity = new_cap;
    }

    int new_id = g->node_count++;
    HnswGraphNode* node = &g->nodes[new_id];
    memset(node, 0, sizeof(*node));
    node->row_index = row_idx;
    node->dim = dim;
    node->top_layer = hnsw_random_layer();
    int copy_dim = dim < 64 ? dim : 64;
    memcpy(node->vector, vec, (size_t)copy_dim * sizeof(float));

    if (new_id == 0) {
        g->entry_node = 0;
        g->max_layer = node->top_layer;
        return;
    }

    int curr = g->entry_node;
    float curr_dist = hnsw_calc_dist(g, vec, g->nodes[curr].vector, dim);

    /* 1. Greedy search from top down to node->top_layer + 1 */
    for (int l = g->max_layer; l > node->top_layer; l--) {
        bool changed = true;
        while (changed) {
            changed = false;
            HnswNeighborList* nlist = &g->nodes[curr].layers[l];
            for (int i = 0; i < nlist->count; i++) {
                int n_id = nlist->neighbors[i];
                if (n_id < 0 || n_id >= g->node_count) continue;
                float d = hnsw_calc_dist(g, vec, g->nodes[n_id].vector, dim);
                if (d < curr_dist) {
                    curr_dist = d;
                    curr = n_id;
                    changed = true;
                }
            }
        }
    }

    /* 2. Connect at layers min(max_layer, node->top_layer) down to 0 */
    int start_l = g->max_layer < node->top_layer ? g->max_layer : node->top_layer;
    for (int l = start_l; l >= 0; l--) {
        int max_m = (l == 0) ? HNSW_M0 : HNSW_M;

        /* Greedy search to find best candidate in layer l */
        bool changed = true;
        while (changed) {
            changed = false;
            HnswNeighborList* nlist = &g->nodes[curr].layers[l];
            for (int i = 0; i < nlist->count; i++) {
                int n_id = nlist->neighbors[i];
                if (n_id < 0 || n_id >= g->node_count) continue;
                float d = hnsw_calc_dist(g, vec, g->nodes[n_id].vector, dim);
                if (d < curr_dist) {
                    curr_dist = d;
                    curr = n_id;
                    changed = true;
                }
            }
        }

        /* Connect bidirectional */
        hnsw_add_neighbor(&node->layers[l], curr, max_m);
        hnsw_add_neighbor(&g->nodes[curr].layers[l], new_id, max_m);

        /* Connect to a few other neighbors of curr in layer l */
        HnswNeighborList* curr_nlist = &g->nodes[curr].layers[l];
        for (int i = 0; i < curr_nlist->count && node->layers[l].count < max_m; i++) {
            int cand = curr_nlist->neighbors[i];
            if (cand != new_id) {
                hnsw_add_neighbor(&node->layers[l], cand, max_m);
                hnsw_add_neighbor(&g->nodes[cand].layers[l], new_id, max_m);
            }
        }
    }

    if (node->top_layer > g->max_layer) {
        g->max_layer = node->top_layer;
        g->entry_node = new_id;
    }
}

bool hnsw_index_build(SecondaryIndex* idx, const Table* table, int col_idx) {
    if (!idx || !table || col_idx < 0 || col_idx >= table->col_count) return false;
    hnsw_index_destroy(idx);

    HnswGraph* g = calloc(1, sizeof(HnswGraph));
    if (!g) return false;
    g->metric = idx->metric;
    g->entry_node = -1;
    g->max_layer = 0;

    float vec[64];
    for (int r = 0; r < table->row_count; r++) {
        if (!table->rows[r].values[col_idx].is_null) {
            const char* txt = table->rows[r].values[col_idx].text;
            int dim = parse_vector_floats(txt, vec, 64);
            if (dim > 0) {
                if (g->dim == 0) g->dim = dim;
                hnsw_insert_node_internal(g, r, vec, dim);
            }
        }
    }

    idx->hnsw_graph = g;
    idx->entry_count = g->node_count;
    idx->is_hnsw = true;
    return true;
}

void hnsw_index_insert_row(SecondaryIndex* idx, const Table* table, int row_idx) {
    if (!idx || !table || !idx->hnsw_graph || row_idx < 0 || row_idx >= table->row_count) return;
    HnswGraph* g = (HnswGraph*)idx->hnsw_graph;
    int col_idx = idx->col_idx;
    if (col_idx < 0 || col_idx >= table->col_count) return;
    if (table->rows[row_idx].values[col_idx].is_null) return;

    float vec[64];
    int dim = parse_vector_floats(table->rows[row_idx].values[col_idx].text, vec, 64);
    if (dim > 0) {
        if (g->dim == 0) g->dim = dim;
        hnsw_insert_node_internal(g, row_idx, vec, dim);
        idx->entry_count = g->node_count;
    }
}

void hnsw_index_destroy(SecondaryIndex* idx) {
    if (!idx || !idx->hnsw_graph) return;
    HnswGraph* g = (HnswGraph*)idx->hnsw_graph;
    free(g->nodes);
    free(g);
    idx->hnsw_graph = NULL;
    idx->entry_count = 0;
}

void* hnsw_index_copy(const void* graph) {
    if (!graph) return NULL;
    const HnswGraph* src = (const HnswGraph*)graph;
    HnswGraph* dst = calloc(1, sizeof(HnswGraph));
    if (!dst) return NULL;
    *dst = *src;
    if (src->capacity > 0 && src->nodes) {
        dst->nodes = malloc((size_t)src->capacity * sizeof(HnswGraphNode));
        if (dst->nodes) {
            memcpy(dst->nodes, src->nodes, (size_t)src->node_count * sizeof(HnswGraphNode));
        } else {
            free(dst);
            return NULL;
        }
    } else {
        dst->nodes = NULL;
    }
    return dst;
}

typedef struct {
    int row_index;
    float dist;
} HnswCandidate;

int hnsw_index_search(const SecondaryIndex* idx, const float* query_vec, int dim, int k, int* out_rows, float* out_dists) {
    if (!idx || !idx->hnsw_graph || !query_vec || k <= 0 || !out_rows) return 0;
    const HnswGraph* g = (const HnswGraph*)idx->hnsw_graph;
    if (g->node_count == 0 || g->entry_node < 0) return 0;

    int curr = g->entry_node;
    float curr_dist = hnsw_calc_dist(g, query_vec, g->nodes[curr].vector, dim);

    /* 1. Greedy top-down to layer 0 */
    for (int l = g->max_layer; l > 0; l--) {
        bool changed = true;
        while (changed) {
            changed = false;
            const HnswNeighborList* nlist = &g->nodes[curr].layers[l];
            for (int i = 0; i < nlist->count; i++) {
                int n_id = nlist->neighbors[i];
                if (n_id < 0 || n_id >= g->node_count) continue;
                float d = hnsw_calc_dist(g, query_vec, g->nodes[n_id].vector, dim);
                if (d < curr_dist) {
                    curr_dist = d;
                    curr = n_id;
                    changed = true;
                }
            }
        }
    }

    /* 2. Beam search on layer 0 */
    HnswCandidate pool[HNSW_EF_SEARCH];
    int pool_size = 0;
    bool visited[HNSW_MAX_NODES];
    memset(visited, 0, sizeof(visited));

    pool[pool_size++] = (HnswCandidate){ g->nodes[curr].row_index, curr_dist };
    if (curr < HNSW_MAX_NODES) visited[curr] = true;

    /* Explore neighbors of candidates */
    for (int step = 0; step < 16; step++) {
        const HnswNeighborList* nlist = &g->nodes[curr].layers[0];
        int best_next = -1;
        float best_next_d = 1e9f;

        for (int i = 0; i < nlist->count; i++) {
            int n_id = nlist->neighbors[i];
            if (n_id < 0 || n_id >= g->node_count) continue;
            if (n_id < HNSW_MAX_NODES && visited[n_id]) continue;
            if (n_id < HNSW_MAX_NODES) visited[n_id] = true;

            float d = hnsw_calc_dist(g, query_vec, g->nodes[n_id].vector, dim);
            if (pool_size < HNSW_EF_SEARCH) {
                pool[pool_size++] = (HnswCandidate){ g->nodes[n_id].row_index, d };
            } else {
                /* Replace maximum element if d is smaller */
                int max_idx = 0;
                for (int p = 1; p < pool_size; p++) {
                    if (pool[p].dist > pool[max_idx].dist) max_idx = p;
                }
                if (d < pool[max_idx].dist) {
                    pool[max_idx] = (HnswCandidate){ g->nodes[n_id].row_index, d };
                }
            }

            if (d < best_next_d) {
                best_next_d = d;
                best_next = n_id;
            }
        }
        if (best_next < 0) break;
        curr = best_next;
    }

    /* Sort pool ascending by distance */
    for (int i = 0; i < pool_size - 1; i++) {
        for (int j = i + 1; j < pool_size; j++) {
            if (pool[j].dist < pool[i].dist) {
                HnswCandidate tmp = pool[i];
                pool[i] = pool[j];
                pool[j] = tmp;
            }
        }
    }

    int returned = pool_size < k ? pool_size : k;
    for (int i = 0; i < returned; i++) {
        out_rows[i] = pool[i].row_index;
        if (out_dists) out_dists[i] = pool[i].dist;
    }
    return returned;
}

bool execute_create_hnsw_index_in_db(Database* db, const char* idx_name, const char* tbl_name, const char* col_name, int metric) {
    if (!db) return report_database_error("database not open");
    if (!idx_name || !*idx_name || !tbl_name || !*tbl_name || !col_name || !*col_name) {
        return report_database_error("invalid index definition");
    }

    for (int i = 0; i < db->index_count; i++) {
        if (!strcmp(db->indexes[i].name, idx_name)) {
            return report_database_error("index already exists: %s", idx_name);
        }
    }

    int t_idx = find_table_index(db, tbl_name);
    if (t_idx < 0) return report_database_error("table does not exist: %s", tbl_name);
    if (g_network_user[0] && g_network_user_role != USER_ROLE_ADMIN)
        return report_database_error("permission denied: CREATE INDEX requires the ADMIN role");
    const Table* table = &db->tables[t_idx];

    int c_idx = find_column_index(table, col_name);
    if (c_idx < 0) return report_database_error("column does not exist: %s", col_name);
    if (table->columns[c_idx].type == TYPE_VECTOR && table->columns[c_idx].size > 64) {
        return report_database_error("HNSW index supports maximum 64 vector dimensions (column has %d)", table->columns[c_idx].size);
    }

    if (db->index_count >= MAX_SECONDARY_INDEXES) {
        return report_database_error("maximum secondary indexes (%d) reached", MAX_SECONDARY_INDEXES);
    }

    SecondaryIndex* idx = &db->indexes[db->index_count];
    memset(idx, 0, sizeof(*idx));
    snprintf(idx->name, sizeof(idx->name), "%s", idx_name);
    snprintf(idx->table_name, sizeof(idx->table_name), "%s", tbl_name);
    idx->table_idx = t_idx;
    idx->col_idx = c_idx;
    idx->is_unique = false;
    idx->is_hnsw = true;
    idx->metric = (metric == 1) ? 1 : 0;

    if (!hnsw_index_build(idx, table, c_idx)) {
        return report_database_error("failed to build HNSW index: %s", idx_name);
    }

    db->index_count++;
    return true;
}
