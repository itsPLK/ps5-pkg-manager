/* One process-owned scheduler. The browser only supplies bytes for a selected
 * browser job; package ordering and outcomes never depend on a React render. */
#include "install_queue.h"
#include "installer.h"
#include "pkg_scanner.h"
#include "app_info.h"
#include "ws_upload.h"
#include "split_pkg.h"
#include "sqlite3.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    install_queue_request_t request;
    uint64_t id, order, run_id;
    install_queue_state_t state;
    char error[256];
    char live_path[80];
    uint64_t downloaded;
    float progress;
    int direct_storage, waiting_for_disc;
    char prompt[256];
    time_t seen, selected, checked, finished;
} queue_job_t;

static queue_job_t *jobs;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
/* Serializes starting/attaching/canceling an active job. Never held during
 * package reads or while waiting for a worker to finish. */
static pthread_mutex_t dispatch = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static int running, created;
static uint64_t next_id, next_order, active_id, current_run;
static int finished_ttl_seconds = INSTALL_QUEUE_FINISHED_TTL_DEFAULT;

static queue_job_t *find(uint64_t id) {
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++)
        if (jobs[i].id == id && id) return &jobs[i];
    return NULL;
}

static int pending(install_queue_state_t state) {
    return state == QUEUE_QUEUED || state == QUEUE_CHECKING || state == QUEUE_PREPARING ||
           state == QUEUE_INSTALLING || state == QUEUE_CANCELING || state == QUEUE_BLOCKED;
}

static const char *state_name(install_queue_state_t state) {
    static const char *names[] = {"queued", "checking", "preparing", "installing", "canceling",
        "completed", "submitted", "failed", "blocked", "canceled", "aborted"};
    return names[state];
}

static void metadata(install_queue_request_t *request, const pkg_detail_t *pkg) {
    snprintf(request->title_id, sizeof(request->title_id), "%s", pkg->title_id);
    snprintf(request->title_name, sizeof(request->title_name), "%s", pkg->title_name);
    snprintf(request->content_id, sizeof(request->content_id), "%s", pkg->content_id);
    snprintf(request->kind, sizeof(request->kind), "%s", pkg->pkg_type_str);
    snprintf(request->version, sizeof(request->version), "%s", pkg->app_version);
    request->total_bytes = pkg->total_pkg_size ? pkg->total_pkg_size : pkg->file_size;
}

static void package_from_request(const install_queue_request_t *r, pkg_detail_t *pkg) {
    memset(pkg, 0, sizeof(*pkg));
    snprintf(pkg->path, sizeof(pkg->path), "%s", r->path);
    snprintf(pkg->title_id, sizeof(pkg->title_id), "%s", r->title_id);
    snprintf(pkg->title_name, sizeof(pkg->title_name), "%s", r->title_name);
    snprintf(pkg->content_id, sizeof(pkg->content_id), "%s", r->content_id);
    snprintf(pkg->pkg_type_str, sizeof(pkg->pkg_type_str), "%s", r->kind);
    snprintf(pkg->app_version, sizeof(pkg->app_version), "%s", r->version);
    pkg->pkg_type = !strcmp(r->kind, "update") ? PKG_TYPE_UPDATE :
                    !strcmp(r->kind, "dlc") ? PKG_TYPE_DLC : PKG_TYPE_BASE;
    pkg->total_pkg_size = pkg->file_size = r->total_bytes;
}

/* Called under mutex. A base queued later cannot satisfy an earlier job. */
static int earlier_base(const queue_job_t *job, int allow_submitted) {
    for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) {
        queue_job_t *base = &jobs[i];
        if (!base->id || base->order >= job->order || strcmp(base->request.kind, "base") ||
            strcmp(base->request.title_id, job->request.title_id)) continue;
        if (base->state == QUEUE_QUEUED || base->state == QUEUE_CHECKING || base->state == QUEUE_PREPARING ||
            base->state == QUEUE_INSTALLING || base->state == QUEUE_COMPLETED ||
            (allow_submitted && base->state == QUEUE_SUBMITTED)) return 1;
    }
    return 0;
}

static int submitted_base(const queue_job_t *job) {
    for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++)
        if (jobs[i].id && jobs[i].order < job->order && jobs[i].state == QUEUE_SUBMITTED &&
            !strcmp(jobs[i].request.kind, "base") && !strcmp(jobs[i].request.title_id, job->request.title_id)) return 1;
    return 0;
}

static int dependent(const queue_job_t *job) {
    return !strcmp(job->request.kind, "update") || !strcmp(job->request.kind, "dlc");
}

static void revalidate_dependents(const char *title_id) {
    if (!title_id || !title_id[0]) return;
    int installed = app_info_check_installed(title_id, NULL, 0);
    int offline = !installer_is_network_connected();
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++) {
        queue_job_t *job = &jobs[i];
        if (!job->id || !dependent(job) || strcmp(job->request.title_id, title_id) ||
            (job->state != QUEUE_QUEUED && job->state != QUEUE_BLOCKED)) continue;
        if (!installed && !earlier_base(job, offline && !job->request.owner[0])) {
            job->state = QUEUE_BLOCKED;
            snprintf(job->error, sizeof(job->error), "Base is not installed or queued before this package");
        } else if (job->state == QUEUE_BLOCKED) {
            job->state = QUEUE_QUEUED;
            job->error[0] = 0;
        }
    }
    pthread_mutex_unlock(&mutex);
}

/* Queue entries are bounded. Finished history is only discarded explicitly,
 * so an unavailable source keeps its metadata and a stable Retry action. */
int install_queue_add(const install_queue_request_t *requests, size_t count, uint64_t *ids) {
    if (!requests || !count || count > INSTALL_QUEUE_MAX_JOBS) return -1;
    for (size_t i = 0; i < count; i++) {
        const install_queue_request_t *r = &requests[i];
        if ((!r->owner[0] && (!r->path[0] || !strncmp(r->path, "live:", 5))) ||
            (r->owner[0] && (!r->file_key[0] || !r->total_bytes || !r->title_id[0] ||
             (strcmp(r->kind, "base") && strcmp(r->kind, "update") && strcmp(r->kind, "dlc")) ||
             (!strcmp(r->kind, "dlc") && !r->content_id[0])))) return -1;
    }
    pthread_mutex_lock(&mutex);
    if (!running || !jobs) { pthread_mutex_unlock(&mutex); return -1; }
    int has_pending = 0;
    for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) if (jobs[i].id && pending(jobs[i].state)) has_pending = 1;
    if (!has_pending) current_run++;
    size_t free_slots = 0;
    for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) if (!jobs[i].id) free_slots++;
    size_t needed = 0;
    for (size_t n = 0; n < count; n++) {
        const install_queue_request_t *r = &requests[n];
        int duplicate = 0;
        const char *key = r->owner[0] ? r->file_key : r->path;
        for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) {
            queue_job_t *existing = &jobs[i];
            if (existing->id && pending(existing->state) && !strcmp(existing->request.owner, r->owner) &&
                !strcmp(existing->request.owner[0] ? existing->request.file_key : existing->request.path, key)) duplicate = 1;
        }
        for (size_t i = 0; i < n; i++)
            if (!strcmp(requests[i].owner, r->owner) && !strcmp(r->owner[0] ? requests[i].file_key : requests[i].path, key)) duplicate = 1;
        if (!duplicate) needed++;
    }
    if (free_slots < needed) { pthread_mutex_unlock(&mutex); return -3; }
    for (size_t n = 0; n < count; n++) {
        queue_job_t *job = NULL;
        for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) {
            queue_job_t *existing = &jobs[i];
            if (existing->id && pending(existing->state) &&
                !strcmp(existing->request.owner, requests[n].owner) &&
                !strcmp(existing->request.owner[0] ? existing->request.file_key : existing->request.path,
                        requests[n].owner[0] ? requests[n].file_key : requests[n].path)) {
                job = existing;
                break;
            }
        }
        if (!job) {
            for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) if (!jobs[i].id) { job = &jobs[i]; break; }
            memset(job, 0, sizeof(*job));
            job->request = requests[n];
            job->id = ++next_id;
            job->run_id = current_run;
            job->order = ++next_order;
            job->seen = time(NULL);
            job->state = QUEUE_QUEUED;
            install_log("[QUEUE] Added job=%llu source=%s package=%.160s", (unsigned long long)job->id,
                        job->request.owner[0] ? "browser" : "file", job->request.owner[0] ? job->request.file_key : job->request.path);
            /* Adding a replacement base moves blocked dependents after it. */
            if (!strcmp(job->request.kind, "base")) {
                for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) {
                    queue_job_t *blocked = &jobs[i];
                    if (blocked->id && blocked->state == QUEUE_BLOCKED && dependent(blocked) &&
                        !strcmp(blocked->request.title_id, job->request.title_id)) {
                        blocked->order = ++next_order;
                        blocked->state = QUEUE_QUEUED;
                        blocked->error[0] = 0;
                    }
                }
            }
        }
        if (ids) ids[n] = job->id;
    }
    pthread_mutex_unlock(&mutex);
    return 0;
}

int install_queue_add_paths(const char *base, const char *update) {
    install_queue_request_t requests[2] = {0};
    const char *paths[] = {base, update};
    size_t count = update && update[0] ? 2 : 1;
    for (size_t i = 0; i < count; i++) {
        if (!paths[i] || strlen(paths[i]) >= sizeof(requests[i].path)) return -1;
        snprintf(requests[i].path, sizeof(requests[i].path), "%s", paths[i]);
        pkg_detail_t pkg;
        if (pkg_scanner_find_by_path(paths[i], &pkg) == 0 || pkg_parser_parse(paths[i], &pkg) == 0)
            metadata(&requests[i], &pkg);
    }
    return install_queue_add(requests, count, NULL);
}

int install_queue_cancel(uint64_t id) {
    pthread_mutex_lock(&dispatch);
    pthread_mutex_lock(&mutex);
    queue_job_t *job = find(id);
    if (!job || !pending(job->state)) { pthread_mutex_unlock(&mutex); pthread_mutex_unlock(&dispatch); return -1; }
    if (job->state == QUEUE_INSTALLING && job->direct_storage) {
        pthread_mutex_unlock(&mutex); pthread_mutex_unlock(&dispatch); return -2;
    }
    int active = active_id == id;
    job->state = active ? QUEUE_CANCELING : QUEUE_CANCELED;
    if (!active) job->finished = time(NULL);
    snprintf(job->error, sizeof(job->error), "Canceled");
    char owner[65], live_path[80], pkg_path[512], title_id[32];
    snprintf(title_id, sizeof(title_id), "%s", job->request.title_id);
    snprintf(pkg_path, sizeof(pkg_path), "%s", job->request.path);
    snprintf(owner, sizeof(owner), "%s", job->request.owner);
    snprintf(live_path, sizeof(live_path), "%s", job->live_path);
    pthread_mutex_unlock(&mutex);
    int result = 0;
    if (active) {
        result = installer_cancel_path(live_path[0] ? live_path : pkg_path);
        if (owner[0] && live_path[0]) ws_direct_cancel_owned(owner, live_path + 5);
    }
    if (result == -2) {
        pthread_mutex_lock(&mutex);
        job = find(id);
        if (job) { job->state = QUEUE_INSTALLING; job->error[0] = 0; job->finished = 0; }
        pthread_mutex_unlock(&mutex);
    }
    pthread_mutex_unlock(&dispatch);
    if (result != -2) {
        install_log("[QUEUE] Canceled job=%llu", (unsigned long long)id);
        revalidate_dependents(title_id);
    }
    return result == -2 ? -2 : 0;
}

int install_queue_retry(uint64_t id) {
    pthread_mutex_lock(&mutex);
    queue_job_t *job = find(id);
    if (!job || (job->state != QUEUE_FAILED && job->state != QUEUE_CANCELED && job->state != QUEUE_BLOCKED)) {
        pthread_mutex_unlock(&mutex); return -1;
    }
    int has_pending = 0;
    for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) if (jobs[i].id && pending(jobs[i].state)) has_pending = 1;
    if (!has_pending) current_run++;
    job->run_id = current_run;
    job->order = ++next_order;
    job->state = QUEUE_QUEUED;
    job->finished = 0;
    job->error[0] = job->live_path[0] = 0;
    job->downloaded = 0;
    job->progress = 0;
    job->direct_storage = job->waiting_for_disc = 0;
    job->prompt[0] = 0;
    job->seen = time(NULL);
    install_log("[QUEUE] Retrying job=%llu", (unsigned long long)id);
    if (!strcmp(job->request.kind, "base")) {
        for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) {
            queue_job_t *other = &jobs[i];
            if (other->id && other->state == QUEUE_BLOCKED && dependent(other) &&
                !strcmp(other->request.title_id, job->request.title_id)) {
                other->order = ++next_order;
                other->state = QUEUE_QUEUED;
                other->error[0] = 0;
                other->finished = 0;
            }
        }
    }
    pthread_mutex_unlock(&mutex);
    return 0;
}

/* Submitted bases carry dependency evidence until their dependents
 * have been dispatched. Do not erase that evidence while needed. */
static int terminal_job_needed(const queue_job_t *job) {
    if (!job || strcmp(job->request.kind, "base") != 0) return 0;
    if (job->state != QUEUE_SUBMITTED && job->state != QUEUE_COMPLETED) return 0;
    for (size_t j = 0; jobs && j < INSTALL_QUEUE_MAX_JOBS; j++) {
        if (jobs[j].id && jobs[j].order > job->order && pending(jobs[j].state) && dependent(&jobs[j]) &&
            !strcmp(jobs[j].request.title_id, job->request.title_id)) return 1;
    }
    return 0;
}

int install_queue_get_finished_ttl(void) {
    pthread_mutex_lock(&mutex);
    int ttl = finished_ttl_seconds;
    pthread_mutex_unlock(&mutex);
    return ttl;
}

void install_queue_set_finished_ttl(int seconds) {
    pthread_mutex_lock(&mutex);
    finished_ttl_seconds = seconds;
    pthread_mutex_unlock(&mutex);
}

void install_queue_clear_finished(void) {
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++) {
        queue_job_t *job = &jobs[i];
        if (job->id && !pending(job->state) && !terminal_job_needed(job)) memset(job, 0, sizeof(*job));
    }
    pthread_mutex_unlock(&mutex);
}

void install_queue_heartbeat(const char *owner) {
    if (!owner || !owner[0]) return;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++)
        if (!strcmp(jobs[i].request.owner, owner)) jobs[i].seen = time(NULL);
    pthread_mutex_unlock(&mutex);
}

void install_queue_disconnect(const char *owner) {
    if (!owner || !owner[0]) return;
    uint64_t ids[INSTALL_QUEUE_MAX_JOBS];
    size_t count = 0;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++)
        if (jobs[i].id && pending(jobs[i].state) && !strcmp(jobs[i].request.owner, owner)) ids[count++] = jobs[i].id;
    pthread_mutex_unlock(&mutex);
    for (size_t i = 0; i < count; i++) install_queue_cancel(ids[i]);
}

int install_queue_browser_selected(uint64_t id, const char *owner) {
    pthread_mutex_lock(&mutex);
    queue_job_t *job = find(id);
    int selected = job && active_id == id && job->state == QUEUE_PREPARING && owner &&
                   !strcmp(job->request.owner, owner) && owner[0];
    pthread_mutex_unlock(&mutex);
    return selected;
}

int install_queue_attach(uint64_t id, const char *owner, const char *live_path) {
    if (!live_path || strncmp(live_path, "live:", 5) || !ws_direct_owner_matches(owner, live_path + 5)) return -1;
    pthread_mutex_lock(&dispatch);
    if (!install_queue_browser_selected(id, owner)) { pthread_mutex_unlock(&dispatch); return -1; }
    int rc = installer_start_live(live_path);
    pthread_mutex_lock(&mutex);
    queue_job_t *job = find(id);
    if (job) {
        snprintf(job->live_path, sizeof(job->live_path), "%s", live_path);
        job->state = rc == 0 ? QUEUE_INSTALLING : QUEUE_FAILED;
        if (rc) {
            snprintf(job->error, sizeof(job->error), "Could not start browser install (code %d)", rc);
            job->finished = time(NULL);
        }
    }
    pthread_mutex_unlock(&mutex);
    if (rc) ws_direct_cancel_owned(owner, live_path + 5);
    pthread_mutex_unlock(&dispatch);
    return rc;
}

int install_queue_open_upload(uint64_t id, const char *owner, const char *filename,
                             uint64_t total, char *sid, size_t sid_size) {
    pthread_mutex_lock(&dispatch);
    if (!install_queue_browser_selected(id, owner)) { pthread_mutex_unlock(&dispatch); return -1; }
    pthread_mutex_lock(&mutex);
    queue_job_t *job = find(id);
    int matches = job && job->request.total_bytes == total;
    pthread_mutex_unlock(&mutex);
    int rc = matches ? ws_direct_init_owned(filename, total, owner, "", sid, sid_size) : -1;
    if (!rc) {
        pthread_mutex_lock(&mutex);
        job = find(id);
        if (job) snprintf(job->live_path, sizeof(job->live_path), "live:%s", sid);
        pthread_mutex_unlock(&mutex);
    }
    pthread_mutex_unlock(&dispatch);
    return rc;
}

/* SQLite's bundled JSON parser handles escaped filenames, nested input and
 * malformed batches. Parse every entry before atomically admitting the batch. */
int install_queue_add_json(const char *json, uint64_t *ids, size_t *out_count) {
    if (!json || !out_count) return -1;
    *out_count = 0;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    install_queue_request_t *requests = calloc(INSTALL_QUEUE_MAX_JOBS, sizeof(*requests));
    if (!requests || sqlite3_open(":memory:", &db) != SQLITE_OK) { free(requests); if (db) sqlite3_close(db); return -1; }
    const char *sql = "SELECT type, json_extract(value,'$.path'), json_extract(value,'$.owner'),"
        "json_extract(value,'$.file_key'), json_extract(value,'$.title_id'), json_extract(value,'$.title_name'),"
        "json_extract(value,'$.content_id'), json_extract(value,'$.kind'), json_extract(value,'$.version'),"
        "json_extract(value,'$.total_bytes') FROM json_each(?,'$.jobs') WHERE json_type(?,'$.jobs')='array'";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, json, -1, SQLITE_TRANSIENT);
        size_t count = 0;
        int step;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (count == INSTALL_QUEUE_MAX_JOBS || strcmp((const char *)sqlite3_column_text(stmt, 0), "object")) { rc = SQLITE_ERROR; break; }
            install_queue_request_t *request = &requests[count++];
            char *fields[] = {request->path, request->owner, request->file_key, request->title_id,
                request->title_name, request->content_id, request->kind, request->version};
            size_t sizes[] = {sizeof(request->path), sizeof(request->owner), sizeof(request->file_key), sizeof(request->title_id),
                sizeof(request->title_name), sizeof(request->content_id), sizeof(request->kind), sizeof(request->version)};
            for (int n = 0; n < 8; n++) {
                if (sqlite3_column_type(stmt, n + 1) == SQLITE_NULL) continue;
                const char *value = (const char *)sqlite3_column_text(stmt, n + 1);
                int bytes = sqlite3_column_bytes(stmt, n + 1);
                if (sqlite3_column_type(stmt, n + 1) != SQLITE_TEXT || bytes < 0 || (size_t)bytes >= sizes[n] || strlen(value) != (size_t)bytes) { rc = SQLITE_ERROR; break; }
                memcpy(fields[n], value, bytes + 1);
            }
            if (rc != SQLITE_OK) break;
            if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
                sqlite3_int64 bytes = sqlite3_column_int64(stmt, 9);
                if (sqlite3_column_type(stmt, 9) != SQLITE_INTEGER || bytes < 0 || (uint64_t)bytes > WS_DIRECT_MAX_TOTAL) { rc = SQLITE_ERROR; break; }
                request->total_bytes = bytes;
            }
            if (!request->owner[0]) {
                pkg_detail_t pkg;
                if (pkg_scanner_find_by_path(request->path, &pkg) == 0) metadata(request, &pkg);
            }
        }
        if (step != SQLITE_DONE || !count) rc = SQLITE_ERROR;
        if (rc == SQLITE_OK) {
            rc = install_queue_add(requests, count, ids);
            if (!rc) *out_count = count;
        } else rc = -1;
    } else rc = -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    free(requests);
    return rc;
}

static void tick(void) {
    uint64_t expired[INSTALL_QUEUE_MAX_JOBS];
    size_t expired_count = 0;
    time_t now = time(NULL);
    static time_t last_ttl_check = 0;
    int check_ttl = (finished_ttl_seconds == 0) || (now != last_ttl_check);
    if (check_ttl) last_ttl_check = now;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++) {
        queue_job_t *job = &jobs[i];
        if (!job->id) continue;
        if (!pending(job->state)) {
            if (!job->finished || job->finished > now) job->finished = now;
            if (check_ttl && finished_ttl_seconds >= 0 && (now - job->finished) >= finished_ttl_seconds && !terminal_job_needed(job)) {
                install_log("[QUEUE] Auto-cleared job=%llu outcome=%s", (unsigned long long)job->id, state_name(job->state));
                memset(job, 0, sizeof(*job));
                continue;
            }
        }
        if (pending(job->state) && job->state != QUEUE_CANCELING &&
            job->request.owner[0] && now - job->seen > 20) expired[expired_count++] = job->id;
    }
    pthread_mutex_unlock(&mutex);
    for (size_t i = 0; i < expired_count; i++) install_queue_cancel(expired[i]);
    pthread_mutex_lock(&mutex);
    queue_job_t *active = find(active_id);
    queue_job_t snapshot = {0};
    if (active) snapshot = *active;
    pthread_mutex_unlock(&mutex);
    if (snapshot.id) {
        if (snapshot.request.owner[0] && time(NULL) - snapshot.seen > 20) {
            install_queue_disconnect(snapshot.request.owner);
            return;
        }
        if (snapshot.state == QUEUE_PREPARING) {
            if (snapshot.request.owner[0] && time(NULL) - snapshot.selected > 90) install_queue_cancel(snapshot.id);
            return;
        }
        installer_status_t status;
        installer_get_status(&status);
        int busy = installer_is_busy();
        pthread_mutex_lock(&mutex);
        active = find(snapshot.id);
        if (active) {
            active->downloaded = status.downloaded_bytes;
            active->progress = status.progress_percent;
            active->direct_storage = status.is_direct_storage;
            active->waiting_for_disc = status.waiting_for_disc;
            snprintf(active->prompt, sizeof(active->prompt), "%s", status.prompt_message);
            if (!busy) {
                if (active->state == QUEUE_FAILED) { /* Preserve a failed attach. */ }
                else if (active->state == QUEUE_CANCELING || !strcmp(status.status_str, "canceled")) active->state = QUEUE_CANCELED;
                else if (!strcmp(status.status_str, "submitted")) active->state = QUEUE_SUBMITTED;
                else if (status.completed) active->state = QUEUE_COMPLETED;
                else active->state = QUEUE_FAILED;
                if (active->state == QUEUE_FAILED) snprintf(active->error, sizeof(active->error), "%s", status.prompt_message[0] ? status.prompt_message : "Installation failed");
                if (!pending(active->state)) active->finished = time(NULL);
                install_log("[QUEUE] Finished job=%llu outcome=%s error=%.160s", (unsigned long long)active->id, state_name(active->state), active->error);
                active_id = 0;
            }
        }
        pthread_mutex_unlock(&mutex);
        if (!busy) revalidate_dependents(snapshot.request.title_id);
        return;
    }
    if (installer_is_busy()) return;
    pthread_mutex_lock(&mutex);
    queue_job_t *next = NULL;
    for (size_t i = 0; i < INSTALL_QUEUE_MAX_JOBS; i++) {
        queue_job_t *job = &jobs[i];
        if (!job->id) continue;
        if (job->state == QUEUE_QUEUED && (!next || job->order < next->order)) next = job;
    }
    if (next) { snapshot = *next; next->state = QUEUE_CHECKING; next->selected = next->checked = time(NULL); active_id = next->id; }
    pthread_mutex_unlock(&mutex);
    if (!snapshot.id) return;

    pkg_detail_t pkg;
    package_from_request(&snapshot.request, &pkg);
    int parse_rc = snapshot.request.owner[0] ? 0 : pkg_parser_parse(snapshot.request.path, &pkg);
    if (parse_rc && split_pkg_detect(snapshot.request.path, NULL)) {
        /* Numbered piece set whose header could not be parsed: keep the scanner's
         * name-derived metadata (already in pkg) and let the installer stream it. */
        parse_rc = 0;
    }
    pkg_install_eligibility_t eligibility = {0};
    if (!parse_rc) pkg_scanner_check_install_eligibility(&pkg, &eligibility);
    int installed = !parse_rc && app_info_check_installed(pkg.title_id, NULL, 0);
    int offline = !snapshot.request.owner[0] && !installer_is_network_connected();
    pthread_mutex_lock(&dispatch);
    pthread_mutex_lock(&mutex);
    active = find(snapshot.id);
    if (!active || active->state != QUEUE_CHECKING) {
        if (active && active->state == QUEUE_CANCELING) {
            active->state = QUEUE_CANCELED;
            active->finished = time(NULL);
        }
        if (active_id == snapshot.id) active_id = 0;
        pthread_mutex_unlock(&mutex); pthread_mutex_unlock(&dispatch); return;
    }
    if (!parse_rc) metadata(&active->request, &pkg);
    int base_ready = installed || earlier_base(active, offline);
    int missing_base = dependent(active) && !base_ready;
    /* Dependencies can satisfy the missing-base check, never unrelated
     * eligibility restrictions such as installed DLC or leftover packages. */
    int submitting_dependency = offline && dependent(active) && submitted_base(active);
    int base_reason = eligibility.disabled_reason &&
        (strstr(eligibility.disabled_reason, "Base package is not installed") ||
         strstr(eligibility.disabled_reason, "Base package installation was aborted") ||
         (submitting_dependency && strstr(eligibility.disabled_reason, "Leftovers detected")));
    if (parse_rc || missing_base || (!eligibility.can_install && !(base_ready && base_reason))) {
        active->state = parse_rc ? QUEUE_FAILED :
                        snapshot.request.owner[0] ? QUEUE_ABORTED : QUEUE_CANCELED;
        snprintf(active->error, sizeof(active->error), "%s", parse_rc ? "Package source unavailable or unreadable. Reconnect the drive/share and retry." :
                 missing_base ? "Base is not installed or queued before this package" : eligibility.disabled_reason);
        if (!pending(active->state)) active->finished = time(NULL);
        active_id = 0;
        pthread_mutex_unlock(&mutex); pthread_mutex_unlock(&dispatch);
        if (parse_rc) revalidate_dependents(snapshot.request.title_id);
        return;
    }
    active->error[0] = 0;
    if (snapshot.request.owner[0]) active->state = QUEUE_PREPARING;
    pthread_mutex_unlock(&mutex);
    int rc = snapshot.request.owner[0] ? 0 : installer_start(snapshot.request.path);
    pthread_mutex_lock(&mutex);
    active = find(snapshot.id);
    if (active && !snapshot.request.owner[0]) {
        active->state = rc == 0 ? QUEUE_INSTALLING : QUEUE_FAILED;
        if (rc) {
            const char *message = rc == -4 ? "Package source unavailable. Reconnect the drive/share and retry." :
                rc == -10 ? "Insufficient installation storage space" : rc == -11 ? "Could not create staging directory" : "Could not start installation";
            snprintf(active->error, sizeof(active->error), "%s (code %d)", message, rc);
            active->finished = time(NULL);
            active_id = 0;
        }
    }
    pthread_mutex_unlock(&mutex);
    pthread_mutex_unlock(&dispatch);
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&mutex);
        int run = running;
        pthread_mutex_unlock(&mutex);
        if (!run) break;
        tick();
        usleep(100000);
    }
    return NULL;
}

int install_queue_init(void) {
    pthread_mutex_lock(&mutex);
    if (running) { pthread_mutex_unlock(&mutex); return 0; }
    jobs = calloc(INSTALL_QUEUE_MAX_JOBS, sizeof(*jobs));
    if (!jobs) { pthread_mutex_unlock(&mutex); return -1; }
    active_id = 0;
    finished_ttl_seconds = INSTALL_QUEUE_FINISHED_TTL_DEFAULT;
    running = 1;
    if (pthread_create(&thread, NULL, worker, NULL)) { running = 0; free(jobs); jobs = NULL; pthread_mutex_unlock(&mutex); return -1; }
    created = 1;
    pthread_mutex_unlock(&mutex);
    return 0;
}

void install_queue_shutdown(void) {
    pthread_mutex_lock(&mutex);
    running = 0;
    int join = created;
    created = 0;
    pthread_mutex_unlock(&mutex);
    if (join) pthread_join(thread, NULL);
    pthread_mutex_lock(&mutex);
    free(jobs); jobs = NULL; active_id = 0;
    pthread_mutex_unlock(&mutex);
}

static void escape(const char *src, char *dst, size_t capacity) {
    size_t n = 0;
    for (; *src && n + 7 < capacity; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
        else if (c < 32) n += snprintf(dst + n, capacity - n, "\\u%04x", c);
        else dst[n++] = c;
    }
    dst[n] = 0;
}

char *install_queue_to_json(void) {
    pthread_mutex_lock(&mutex);
    size_t count = 0;
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++) if (jobs[i].id) count++;
    size_t capacity = count * 14000 + 128, used = 0;
    char *json = malloc(capacity);
    if (!json) { pthread_mutex_unlock(&mutex); return NULL; }
    used += snprintf(json, capacity, "{\"active_id\":%llu,\"jobs\":[", (unsigned long long)active_id);
    int first = 1;
    for (size_t i = 0; jobs && i < INSTALL_QUEUE_MAX_JOBS; i++) {
        queue_job_t *job = &jobs[i];
        if (!job->id) continue;
        char path[3100], name[1600], error[1600], key[3100], prompt[1600];
        escape(job->request.path, path, sizeof(path));
        escape(job->request.file_key, key, sizeof(key));
        escape(job->request.title_name, name, sizeof(name));
        escape(job->error, error, sizeof(error));
        escape(job->prompt, prompt, sizeof(prompt));
        char owner[400], title[200], kind[100];
        escape(job->request.owner, owner, sizeof(owner));
        escape(job->request.title_id, title, sizeof(title));
        escape(job->request.kind, kind, sizeof(kind));
        used += snprintf(json + used, capacity - used,
            "%s{\"id\":%llu,\"order\":%llu,\"run_id\":%llu,\"state\":\"%s\",\"path\":\"%s\",\"file_key\":\"%s\",\"source_id\":\"%.16s\","
            "\"title_name\":\"%s\",\"title_id\":\"%s\",\"kind\":\"%s\",\"error\":\"%s\",\"prompt\":\"%s\","
            "\"total_bytes\":%llu,\"downloaded_bytes\":%llu,\"progress\":%.1f,\"is_direct_storage\":%s,\"waiting_for_disc\":%s}",
            first ? "" : ",", (unsigned long long)job->id, (unsigned long long)job->order, (unsigned long long)job->run_id, state_name(job->state), path, key, owner,
            name, title, kind, error, prompt, (unsigned long long)job->request.total_bytes,
            (unsigned long long)job->downloaded, job->progress, job->direct_storage ? "true" : "false", job->waiting_for_disc ? "true" : "false");
        first = 0;
    }
    snprintf(json + used, capacity - used, "]}");
    pthread_mutex_unlock(&mutex);
    return json;
}
