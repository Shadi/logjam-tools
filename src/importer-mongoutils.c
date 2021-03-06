#include "importer-mongoutils.h"

int num_databases = 0;
const char *databases[MAX_DATABASES];

mongoc_write_concern_t *wc_no_wait = NULL;
mongoc_write_concern_t *wc_wait = NULL;
static bson_t *bulk_opts = NULL;

static
void my_mongo_log_handler(mongoc_log_level_t log_level, const char *log_domain, const char *message, void *user_data)
{
   FILE *stream;
   char level = 'I';

   switch (log_level) {
   case MONGOC_LOG_LEVEL_ERROR:
   case MONGOC_LOG_LEVEL_CRITICAL:
       level = 'E';
       stream = stderr;
       break;
   case MONGOC_LOG_LEVEL_WARNING:
       level = 'W';
       stream = stderr;
       break;
   case MONGOC_LOG_LEVEL_MESSAGE:
   case MONGOC_LOG_LEVEL_INFO:
       level = 'I';
       stream = stdout;
       break;
   case MONGOC_LOG_LEVEL_DEBUG:
   case MONGOC_LOG_LEVEL_TRACE:
       level = 'D';
       stream = stdout;
       break;
   default:
       stream = stdout;
   }

   fprintf(stream, "[%c] monogc[%s]: %s\n", level, log_domain, message);
}

char* uri_with_server_selection_try_once_set_to_false(const char * uri) {
    const char* option = "&serverSelectionTryOnce=false";
    const size_t n = strlen(uri);
    const size_t m = strlen(option);
    char* muri = malloc((n+m+1) * sizeof(char));
    strcpy(muri, uri);
    strcpy(muri+n, option);
    return muri;
}

void initialize_mongo_db_globals(zconfig_t* config)
{
    mongoc_init();
    mongoc_log_set_handler(my_mongo_log_handler, NULL);

    wc_wait = mongoc_write_concern_new();
    mongoc_write_concern_set_w(wc_wait, MONGOC_WRITE_CONCERN_W_DEFAULT);

    wc_no_wait = mongoc_write_concern_new();
    if (USE_UNACKNOWLEDGED_WRITES)
        // TODO: this leads to illegal opcodes on the server
        mongoc_write_concern_set_w(wc_no_wait, MONGOC_WRITE_CONCERN_W_UNACKNOWLEDGED);
    else
        mongoc_write_concern_set_w(wc_no_wait, MONGOC_WRITE_CONCERN_W_DEFAULT);

    bulk_opts = bson_new();
    bson_append_bool(bulk_opts, "ordered", 7, false);
    mongoc_write_concern_append(wc_no_wait, bulk_opts);

    zconfig_t* dbs = zconfig_locate(config, "backend/databases");
    if (dbs) {
        zconfig_t *db = zconfig_child(dbs);
        while (db) {
            assert(num_databases < MAX_DATABASES);
            char *uri = zconfig_value(db);
            if (uri != NULL) {
                databases[num_databases] = uri_with_server_selection_try_once_set_to_false(uri);
                printf("[I] database[%d]: %s\n", num_databases, databases[num_databases]);
                num_databases++;
            }
            db = zconfig_next(db);
        }
    }
    if (num_databases == 0) {
        databases[num_databases] = DEFAULT_MONGO_URI;
        printf("[I] database[%d]: %s\n", num_databases, DEFAULT_MONGO_URI);
        num_databases++;
    }
}

bool add_database_to_databases_collection(mongoc_client_t *client, const char* db_name)
{
    int stream_name_len = strlen(db_name)-7-10-1;
    const char* date = db_name + 7 + stream_name_len + 1;
    const char *env = date-2;
    int env_len = 0;
    while (*env != '-') { env--; env_len++; }
    env++;
    const char *app = db_name + 7;
    int app_len = stream_name_len - env_len - 1;

    mongoc_collection_t *db_collection = mongoc_client_get_collection(client, "logjam-global", "databases");
    bson_t *selector = bson_new();
    assert(bson_append_utf8(selector, "app", 3, app, app_len));
    assert(bson_append_utf8(selector, "env", 3, env, env_len));
    assert(bson_append_utf8(selector, "date", 4, date, 10));
    bson_t *document = bson_new();
    bson_t child;
    bson_append_document_begin(document, "$setOnInsert", 12, &child);
    assert(bson_append_utf8(&child, "app", 3, app, app_len));
    assert(bson_append_utf8(&child, "env", 3, env, env_len));
    assert(bson_append_utf8(&child, "date", 4, date, 10));
    bson_append_document_end(document, &child);

    bool ok = true;
    if (!dryrun) {
        bson_error_t error;
        if (!mongoc_collection_update(db_collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            ok = false;
            fprintf(stderr, "[E] update failed on logjam-global.databases: (%d) %s\n", error.code, error.message);
            if (1) {
                char *json = bson_as_json(document, NULL);
                printf("[E] document: %s\n", json);
                bson_free(json);
            }
        }
    }

    bson_destroy(selector);
    bson_destroy(document);

    mongoc_collection_destroy(db_collection);

    return ok;
}


bool ensure_known_database(mongoc_client_t *client, const char* db_name)
{
    mongoc_collection_t *meta_collection = mongoc_client_get_collection(client, "logjam-global", "metadata");
    bson_t *selector = bson_new();
    assert(bson_append_utf8(selector, "name", 4, "databases", 9));

    bson_t *document = bson_new();
    bson_t child;
    bson_append_document_begin(document, "$addToSet", 9, &child);
    bson_append_utf8(&child, "value", 5, db_name, -1);
    bson_append_document_end(document, &child);

    bool ok = true;
    if (!dryrun) {
        bson_error_t error;
        if (!mongoc_collection_update(meta_collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            ok = false;
            fprintf(stderr, "[E] update failed on logjam-global: (%d) %s\n", error.code, error.message);
        }
    }

    bson_destroy(selector);
    bson_destroy(document);

    mongoc_collection_destroy(meta_collection);

    return ok & add_database_to_databases_collection(client, db_name);
}

bool add_databases_to_databases_collection(mongoc_client_t *client, zlist_t *db_names)
{
    mongoc_collection_t *db_collection = mongoc_client_get_collection(client, "logjam-global", "databases");
    mongoc_bulk_operation_t *bulk = mongoc_collection_create_bulk_operation_with_opts(db_collection, bulk_opts);

    const char* db_name = zlist_first(db_names);
    while (db_name) {
        int stream_name_len = strlen(db_name)-7-10-1;
        const char* date = db_name + 7 + stream_name_len + 1;
        const char *env = date-2;
        int env_len = 0;
        while (*env != '-') { env--; env_len++; }
        env++;
        const char *app = db_name + 7;
        int app_len = stream_name_len - env_len - 1;

        bson_t *selector = bson_new();
        assert(bson_append_utf8(selector, "app", 3, app, app_len));
        assert(bson_append_utf8(selector, "env", 3, env, env_len));
        assert(bson_append_utf8(selector, "date", 4, date, 10));
        bson_t *document = bson_new();
        bson_t child;
        bson_append_document_begin(document, "$setOnInsert", 12, &child);
        assert(bson_append_utf8(&child, "app", 3, app, app_len));
        assert(bson_append_utf8(&child, "env", 3, env, env_len));
        assert(bson_append_utf8(&child, "date", 4, date, 10));
        bson_append_document_end(document, &child);

        mongoc_bulk_operation_update_one(bulk, selector, document, true);

        bson_destroy(selector);
        bson_destroy(document);

        db_name = zlist_next(db_names);
    }

    bool ok = true;
    if (!dryrun) {
        bson_error_t error;
        bson_t reply;
        if (!mongoc_bulk_operation_execute(bulk, &reply, &error)) {
            ok = false;
            fprintf(stderr, "[E] bulk update failed on logjam-global databases: (%d) %s\n", error.code, error.message);
        }
        if (0) {
            char *str = bson_as_canonical_extended_json(&reply, NULL);
            printf ("[D] bulk update reply %s\n", str);
            bson_free (str);
        }
    }

    mongoc_bulk_operation_destroy(bulk);
    mongoc_collection_destroy(db_collection);

    return ok;
}

bool ensure_known_databases(mongoc_client_t *client, zlist_t *db_names)
{
    mongoc_collection_t *meta_collection = mongoc_client_get_collection(client, "logjam-global", "metadata");
    bson_t *selector = bson_new();
    assert(bson_append_utf8(selector, "name", 4, "databases", 9));

    bson_t *document = bson_new();
    bson_t child1, child2, child3;
    bson_append_document_begin(document, "$addToSet", 9, &child1);
    bson_append_document_begin(&child1, "value", 5, &child2);
    bson_append_array_begin(&child2, "$each", 5, &child3);
    const char* db_name = zlist_first(db_names);
    int i = 0;
    while (db_name) {
        const char *key;
        char buf[16];
        int keylen = bson_uint32_to_string(i++, &key, buf, sizeof buf);
        bson_append_utf8(&child3, key, keylen, db_name, -1);
        db_name = zlist_next(db_names);
    }
    bson_append_array_end(&child2, &child3);
    bson_append_document_end(&child1, &child2);
    bson_append_document_end(document, &child1);

    bool ok = true;
    if (!dryrun) {
        bson_error_t error;
        if (!mongoc_collection_update(meta_collection, MONGOC_UPDATE_UPSERT, selector, document, wc_no_wait, &error)) {
            ok = false;
            fprintf(stderr, "[E] update failed on logjam-global: (%d) %s\n", error.code, error.message);
        }
    }

    bson_destroy(selector);
    bson_destroy(document);

    mongoc_collection_destroy(meta_collection);

    return ok & add_databases_to_databases_collection(client, db_names);
}

int mongo_client_ping(mongoc_client_t *client)
{
    int available = 1;
#if USE_PINGS == 1
    bson_t ping;
    bson_init(&ping);
    bson_append_int32(&ping, "ping", 4, 1);

    mongoc_database_t *database = mongoc_client_get_database(client, "logjam-global");
    mongoc_cursor_t *cursor = mongoc_database_command(database, 0, 0, 1, 0, &ping, NULL, NULL);

    const bson_t *reply;
    bson_error_t error;
    if (mongoc_cursor_next(cursor, &reply)) {
        available = 0;
        // char *str = bson_as_json(reply, NULL);
        // fprintf(stdout, "D %s\n", str);
        // bson_free(str);
    } else if (mongoc_cursor_error(cursor, &error)) {
        fprintf(stderr, "[E] ping failure: (%d) %s\n", error.code, error.message);
    }
    bson_destroy(&ping);
    mongoc_cursor_destroy(cursor);
    mongoc_database_destroy(database);
#endif
    return available;
}
