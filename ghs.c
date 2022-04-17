#include "ghs.h"
#include "create_table.h"
#include <curl/curl.h>
#include <jansson.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// libcurl related
const char *HEADER_ACCEPT = "Accept: application/vnd.github.v3.star+json";
const char *HEADER_UA = "User-Agent: ghs-client/0.1.0";
const char *API_ROOT = "https://api.github.com";
const char *GET_METHOD = "GET";
const char *DELETE_METHOD = "DELETE";

typedef struct {
  char *memory;
  size_t size;
} response;

static void free_response(response *resp) {
  if (resp->memory) {
#ifdef VERBOSE
    printf("free response, body is %s\n", resp->memory);
#endif
    free(resp->memory);
  }
}

static void free_curl_slist(struct curl_slist **lst) {
  if (*lst) {
#ifdef VERBOSE
    printf("free curl list, body is %s\n", (*lst)->data);
#endif
    curl_slist_free_all(*lst);
  }
}

static void free_curl_handler(CURL **curl) {
  if (*curl) {
#ifdef VERBOSE
    printf("free curl handler\n");
#endif
    curl_easy_cleanup(*curl);
  }
}

#define auto_curl_slist                                                        \
  struct curl_slist __attribute__((cleanup(free_curl_slist)))
#define auto_response response __attribute__((cleanup(free_response)))
#define auto_curl CURL __attribute__((cleanup(free_curl_handler)))

static size_t mem_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  response *mem = (response *)userp;

  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if (!ptr) {
    fprintf(stderr, "not enough memory (realloc returned NULL)\n");
    return 0;
  }

  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

// utils
const size_t PER_PAGE = 100;
const size_t SQL_DEFAULT_LEN = 512;

void ghs_free_char(char **buf) {
  if (*buf) {
#ifdef VERBOSE
    printf("free char %s\n", *buf);
#endif
    free((void *)*buf);
  }
}

static char *strdup_when_not_null(const unsigned char *input) {
  if (input) {
    return strdup((const char *)input);
  }
  return NULL;
}

static void free_stmt(sqlite3_stmt **stmt) {
  if (*stmt) {
#ifdef VERBOSE
    printf("free stmt\n");
#endif
    sqlite3_finalize(*stmt);
  }
}

#define auto_sqlite3_stmt sqlite3_stmt *__attribute__((cleanup(free_stmt)))

// actual ghs implementation
struct ghs_context {
  sqlite3 *db;
  CURL *curl;
  struct curl_slist *headers;
  /* int timeout; */
};

void print_error(ghs_error err) {
  printf("code:%d, msg:%s\n", err.code, err.message);
}

bool is_ok(ghs_error err) { return err.code == GHS_CODE_OK; }

const ghs_error NO_ERROR = {.code = GHS_CODE_OK, .message = NULL};

static bool empty_string(const char *s) {
  if (s) {
    return 0 == strlen(s);
  }
  return true;
}

void ghs_free_context(ghs_context *ctx) {
  if (*ctx) {
#ifdef VERBOSE
    printf("free ghs_context\n");
#endif
    curl_slist_free_all((*ctx)->headers);
    curl_easy_cleanup((*ctx)->curl);
    curl_global_cleanup();
    sqlite3_close((*ctx)->db);
    free(*ctx);
  }
}

static ghs_error init_db(const char *root, sqlite3 **out_db) {
  sqlite3 *db = NULL;
  int ret = sqlite3_open(root, &db);
  if (ret) {
    const char *msg = sqlite3_errmsg(db);
    sqlite3_close(db);
    return (ghs_error){.code = GHS_CODE_DB, .message = msg};
  }

  char *err_msg = NULL;
  ret = sqlite3_exec(db, (const char *)create_table_sql, NULL, NULL, &err_msg);
  if (ret) {
    fprintf(stderr, "exec create table sql failed:%s\n", err_msg);
    sqlite3_free(err_msg);
    return (ghs_error){.code = GHS_CODE_DB, .message = "exec sql failed"};
  }

  *out_db = db;
  return NO_ERROR;
}

ghs_error ghs_setup_context(const char *path, const char *github_token,
                            ghs_context *out) {
  curl_global_init(CURL_GLOBAL_ALL);
  CURL *curl = curl_easy_init();
  if (!curl) {
    return (ghs_error){.code = GHS_CODE_CURL, .message = "curl init"};
  }
#ifdef CURL_VERBOSE
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
  struct curl_slist *req_headers = NULL;
  char header_auth[128];
  sprintf(header_auth, "Authorization: token %s", github_token);
  req_headers = curl_slist_append(
      req_headers, "Content-Type: application/json; charset=utf-8");
  if (req_headers == NULL) {
    return (ghs_error){.code = GHS_CODE_CURL, .message = "init curl header"};
  }
  req_headers = curl_slist_append(req_headers, HEADER_ACCEPT);
  if (req_headers == NULL) {
    curl_slist_free_all(req_headers);
    return (ghs_error){.code = GHS_CODE_CURL, .message = "append2 header"};
  }
  req_headers = curl_slist_append(req_headers, header_auth);
  req_headers = curl_slist_append(req_headers, HEADER_UA);
  if (req_headers == NULL) {
    curl_slist_free_all(req_headers);
    return (ghs_error){.code = GHS_CODE_CURL, .message = "append3 header"};
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req_headers);
  sqlite3 *db = NULL;
  ghs_error err = init_db(path, &db);
  if (!is_ok(err)) {
    return err;
  }
  ghs_context ctx = malloc(sizeof(struct ghs_context));
  ctx->db = db;
  ctx->curl = curl;
  ctx->headers = req_headers;
  *out = ctx;

  return NO_ERROR;
}

static ghs_error ghs_request(ghs_context ctx, const char *method,
                             const char *url, json_t *payload, json_t **out) {
  CURL *curl = ctx->curl;

  curl_easy_setopt(curl, CURLOPT_URL, url);
  if (payload) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_dumps(payload, 0));
  } else {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
  }
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

  auto_response chunk = {.memory = malloc(1), .size = 0};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    return (ghs_error){.code = GHS_CODE_CURL,
                       .message = curl_easy_strerror(res)};
  }

  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  if (response_code == 200) {
    json_error_t error;
    json_t *resp = json_loads(chunk.memory, JSON_COMPACT, &error);
    if (!resp) {
      return (ghs_error){.code = GHS_CODE_JSON, .message = error.text};
    }
    *out = resp;
  }
  return NO_ERROR;
}

/****************/
/* GitHub repos */
/****************/
#define FREE_OBJ_FIELD(obj, field)                                             \
  ({                                                                           \
    if ((obj)->field) {                                                        \
      free((void *)(obj)->field);                                              \
    }                                                                          \
  })

void ghs_free_repo(ghs_repo *repo) {
  if (repo) {
#ifdef VERBOSE
    printf("free ghs_repo, name is %s\n", repo->full_name);
#endif
    FREE_OBJ_FIELD(repo, full_name);
    FREE_OBJ_FIELD(repo, description);
    FREE_OBJ_FIELD(repo, created_at);
    FREE_OBJ_FIELD(repo, updated_at);
    FREE_OBJ_FIELD(repo, pushed_at);
    FREE_OBJ_FIELD(repo, lang);
    FREE_OBJ_FIELD(repo, homepage);
  }
}

void ghs_free_repo_list(ghs_repo_list *repo_lst) {
  if (repo_lst) {
#ifdef VERBOSE
    printf("free ghs_repo_list, length is %zu\n", repo_lst->length);
#endif

    for (size_t i = 0; i < repo_lst->length; i++) {
      ghs_free_repo(&(repo_lst->repo_array[i]));
    }
    free(repo_lst->repo_array);
  }
}

static char *dup_json_string(json_t *root, const char *key) {
  json_t *v = json_object_get(root, key);
  if (json_is_null(v)) {
    return NULL;
  } else {
    return strdup(json_string_value(v));
  }
}

static ghs_repo repo_from_json(json_t *root) {
  return (ghs_repo){
      .id = json_integer_value(json_object_get(root, "id")),
      .full_name = dup_json_string(root, "full_name"),
      .description = dup_json_string(root, "description"),
      .private = json_boolean_value(json_object_get(root, "private")),
      .created_at = dup_json_string(root, "created_at"),
      .updated_at = dup_json_string(root, "updated_at"),
      .pushed_at = dup_json_string(root, "pushed_at"),
      .stargazers_count =
          json_integer_value(json_object_get(root, "stargazers_count")),
      .watchers_count =
          json_integer_value(json_object_get(root, "watchers_count")),
      .forks = json_integer_value(json_object_get(root, "forks_count")),
      .lang = dup_json_string(root, "language"),
      .homepage = dup_json_string(root, "homepage"),
  };
}

static ghs_error save_repos(ghs_context ctx, ghs_repo_list repo_lst) {
  const char *sql =
      "INSERT INTO ghs_repo (id, full_name, description, private, "
      "created_at, updated_at, pushed_at, stargazers_count, watchers_count, "
      "forks, lang, homepage) "
      "   VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)        "
      "ON CONFLICT (id)                                                      "
      "   DO UPDATE SET "
      "   full_name=?2, description=?3, private =?4,             "
      "   created_at=?5, updated_at=?6, pushed_at=?7, "
      "   stargazers_count=?8, watchers_count=?9,forks=?10, "
      "   lang=?11, homepage=?12 ";

  auto_sqlite3_stmt stmt = NULL;
  int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
  if (rc) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  for (int i = 0; i < repo_lst.length; i++) {
    ghs_repo repo = repo_lst.repo_array[i];
    int column = 1;
    sqlite3_bind_int(stmt, column++, repo.id);
    sqlite3_bind_text(stmt, column++, repo.full_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.description, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, column++, repo.private);
    sqlite3_bind_text(stmt, column++, repo.created_at, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.updated_at, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.pushed_at, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, column++, repo.stargazers_count);
    sqlite3_bind_int(stmt, column++, repo.watchers_count);
    sqlite3_bind_int(stmt, column++, repo.forks);
    sqlite3_bind_text(stmt, column++, repo.lang, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.homepage, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      fprintf(stderr, "insert %s failed. code:%d, msg:%s\n", repo.full_name, rc,
              sqlite3_errmsg(ctx->db));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  return NO_ERROR;
}

/* Not use now */
/*
ghs_error fetch_repos_by_page(ghs_context ctx, size_t page_num,
                              ghs_repo_list *out) {
  char url[128];
  sprintf(url, "%s/user/repos?type=all&per_page=%zu&page=%zu", API_ROOT,
          PER_PAGE, page_num);
  json_auto_t *resp = NULL;
  ghs_error err = ghs_request(ctx, GET_METHOD, url, NULL, &resp);
  if (!is_ok(err)) {
    return err;
  }
  size_t resp_len = json_array_size(resp);
  ghs_repo *repos = malloc(sizeof(ghs_repo) * resp_len);
  for (size_t i = 0; i < resp_len; i++) {
    repos[i] = repo_from_json(json_array_get(resp, i));
  }
  *out = (ghs_repo_list){.repo_array = repos, .length = resp_len};
  return NO_ERROR;
}

ghs_error print_all_repos(ghs_context ctx) {
  size_t page_num = 1;
  while (true) {
    ghs_auto_repo_list repo_lst;
    ghs_error err = fetch_repos_by_page(ctx, page_num++, &repo_lst);
    if (!is_ok(err)) {
      return err;
    }

    for (size_t i = 0; i < repo_lst.length; i++) {
      ghs_repo r = repo_lst.repo_array[i];
      printf("id:%d, name:%s\n", r.id, r.full_name);
    }
    if (repo_lst.length < PER_PAGE) {
      break;
    }
  }

  return NO_ERROR;
}
*/

/****************/
/* GitHub stars */
/****************/

void ghs_free_star(ghs_star *star) {
  if (star) {
#ifdef VERBOSE
    printf("free ghs_star, starred_at is %s\n", star->starred_at);
#endif
    FREE_OBJ_FIELD(star, starred_at);
    ghs_free_repo(&(star->repo));
  }
}

void ghs_free_star_list(ghs_star_list *star_list) {
  if (star_list) {
#ifdef VERBOSE
    printf("free ghs_star_list, length is %zu\n", star_list->length);
#endif

    for (size_t i = 0; i < star_list->length; i++) {
      ghs_free_star(&(star_list->star_array[i]));
    }
    free(star_list->star_array);
  }
}

static ghs_error fetch_stars_by_page(ghs_context ctx, size_t page_num,
                                     ghs_star_list *out) {
  char url[128];
  sprintf(url, "%s/user/starred?type=all&per_page=%zu&page=%zu", API_ROOT,
          PER_PAGE, page_num);
  json_auto_t *resp = NULL;
  ghs_error err = ghs_request(ctx, GET_METHOD, url, NULL, &resp);
  if (!is_ok(err)) {
    return err;
  }

  size_t resp_len = json_array_size(resp);
  ghs_star *stars = malloc(sizeof(ghs_star) * resp_len);
  for (size_t i = 0; i < resp_len; i++) {
    json_t *one_star = json_array_get(resp, i);
    stars[i] =
        (ghs_star){.repo = repo_from_json(json_object_get(one_star, "repo")),
                   .starred_at = dup_json_string(one_star, "starred_at")};
  }
  *out = (ghs_star_list){.star_array = stars, .length = resp_len};

  return NO_ERROR;
}

ghs_error ghs_query_stars(ghs_context ctx, const char *keyword,
                          const char *language, ghs_star_list *out_lst) {
  ghs_auto_char sql = malloc(SQL_DEFAULT_LEN);
  sprintf(sql, "%s",
          "select "
          "datetime(starred_at, 'localtime') as starred_at,"
          "id,full_name,description,private,"
          "datetime(created_at, 'localtime'),"
          "datetime(updated_at, 'localtime'),"
          "datetime(pushed_at, 'localtime'),"
          "stargazers_count,watchers_count,forks,lang,homepage "
          "from ghs_star_view where 1");

  if (!empty_string(keyword)) {
    int current_len = sprintf(sql,
                              "%s and (full_name like '%%%s%%' COLLATE NOCASE "
                              "or description like '%%%s%%' COLLATE NOCASE)",
                              sql, keyword, keyword);
    if (current_len > SQL_DEFAULT_LEN) {
      fprintf(stderr, "sql:%s, keyword:%s\n", sql, keyword);
      return (ghs_error){.code = GHS_CODE_INTERNAL,
                         .message = "buffer not enough when append keyword"};
    }
  }

  if (!empty_string(language)) {
    int current_len =
        sprintf(sql, "%s and lang='%s' COLLATE NOCASE", sql, language);
    if (current_len > SQL_DEFAULT_LEN) {
      fprintf(stderr, "sql:%s, lang:%s\n", sql, language);
      return (ghs_error){.code = GHS_CODE_INTERNAL,
                         .message = "buffer not enough when append language"};
    }
  }
  int current_len = sprintf(sql, "%s order by starred_at desc", sql);
  if (current_len > SQL_DEFAULT_LEN) {
    fprintf(stderr, "sql:%s, current:%d, len:%zu\n", sql, current_len,
            strlen(sql));
    return (ghs_error){.code = GHS_CODE_INTERNAL,
                       .message = "buffer not enough when append order_by"};
  }
#ifdef VERBOSE
  printf("query sql:%s\n", sql);
#endif

  auto_sqlite3_stmt stmt = NULL;
  int rc = sqlite3_prepare_v2(ctx->db, sql, strlen(sql), &stmt, NULL);
  if (rc) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  size_t rows_count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    rows_count++;
  }
  ghs_star *star_arr = malloc(sizeof(ghs_star) * rows_count);
  sqlite3_reset(stmt);
  size_t row = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ghs_repo repo = {};
    {
      int column = 1;
      repo.id = sqlite3_column_int(stmt, column++);
      repo.full_name =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.description =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.private = sqlite3_column_int(stmt, column++);
      repo.created_at =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.updated_at =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.pushed_at =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.stargazers_count = sqlite3_column_int(stmt, column++);
      repo.watchers_count = sqlite3_column_int(stmt, column++);
      repo.forks = sqlite3_column_int(stmt, column++);
      repo.lang = strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.homepage = strdup_when_not_null(sqlite3_column_text(stmt, column++));
    };

    star_arr[row++] = (ghs_star){
        .starred_at = strdup_when_not_null(sqlite3_column_text(stmt, 0)),
        .repo = repo,
    };
  }

  if (rc != SQLITE_OK) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  *out_lst = (ghs_star_list){.star_array = star_arr, .length = rows_count};
  return NO_ERROR;
}

static ghs_error save_stars(ghs_context ctx, ghs_star_list star_lst) {
  ghs_repo *repo_arr = malloc(sizeof(ghs_repo) * star_lst.length);
  for (int i = 0; i < star_lst.length; i++) {
    repo_arr[i] = star_lst.star_array[i].repo;
  }
  ghs_repo_list repo_lst = {.repo_array = repo_arr, .length = star_lst.length};
  ghs_error err = save_repos(ctx, repo_lst);
  free(repo_arr);
  if (!is_ok(err)) {
    return err;
  }

  auto_sqlite3_stmt stmt = NULL;
  const char *sql = "insert into ghs_star(starred_at, repo_id) values (?1, ?2) \
on conflict(repo_id) \
do update set starred_at = ?1";
  int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
  if (rc) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  for (int i = 0; i < star_lst.length; i++) {
    ghs_star star = star_lst.star_array[i];
    sqlite3_bind_text(stmt, 1, star.starred_at, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, star.repo.id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      fprintf(stderr, "insert %s failed. code:%d, msg:%s\n",
              star.repo.full_name, rc, sqlite3_errmsg(ctx->db));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  return NO_ERROR;
}

ghs_error ghs_sync_stars(ghs_context ctx) {
  size_t page_num = 1;
  while (true) {
#ifdef GHS_TEST
    if (page_num > 2) {
      break;
    }
#endif
    ghs_auto_star_list star_lst;
    ghs_error err = fetch_stars_by_page(ctx, page_num++, &star_lst);
    if (!is_ok(err)) {
      return err;
    }
    err = save_stars(ctx, star_lst);
    if (!is_ok(err)) {
      return err;
    }

    if (star_lst.length < PER_PAGE) {
      break;
    }
  };

  return NO_ERROR;
}

ghs_error ghs_unstar(ghs_context ctx, size_t repo_id) {
  const char *sql = "delete from ghs_repo where id = ? returning full_name";
  auto_sqlite3_stmt stmt = NULL;
  int rc = sqlite3_prepare_v2(ctx->db, sql, strlen(sql), &stmt, NULL);
  if (rc) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  sqlite3_bind_int64(stmt, 1, repo_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  ghs_auto_char full_name = strdup_when_not_null(sqlite3_column_text(stmt, 0));
  printf("delete repo %s\n", full_name);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  sql = "delete from ghs_star where repo_id = ?";
  if (sqlite3_prepare_v2(ctx->db, sql, strlen(sql), &stmt, NULL)) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  sqlite3_bind_int64(stmt, 1, repo_id);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    return (ghs_error){.code = GHS_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  char url[128];
  sprintf(url, "%s/user/starred/%s", API_ROOT, full_name);
  return ghs_request(ctx, DELETE_METHOD, url, NULL, NULL);
}