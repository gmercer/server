/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#include "test.h"

// verify recovery of a put multiple log entry

static const int envflags = DB_INIT_MPOOL|DB_CREATE|DB_THREAD |DB_INIT_LOCK|DB_INIT_LOG|DB_INIT_TXN|DB_PRIVATE;

static int
get_key(int i, int dbnum) {
    return htonl(i + dbnum);
}

static void
get_data(int *v, int i, int ndbs) {
    for (int dbnum = 0; dbnum < ndbs; dbnum++) {
        v[dbnum] = get_key(i, dbnum);
    }
}

static int
put_callback(DB *dest_db, DB *src_db, DBT_ARRAY *dest_keys, DBT_ARRAY *dest_vals, const DBT *src_key, const DBT *src_val) {
    toku_dbt_array_resize(dest_keys, 1);
    toku_dbt_array_resize(dest_vals, 1);
    DBT *dest_key = &dest_keys->dbts[0];
    DBT *dest_val = &dest_vals->dbts[0];
    (void) dest_db; (void) src_db; (void) dest_key; (void) dest_val; (void) src_key; (void) src_val;
    assert(src_db == NULL);

    unsigned int dbnum;
    assert(dest_db->descriptor->dbt.size == sizeof dbnum);
    memcpy(&dbnum, dest_db->descriptor->dbt.data, sizeof dbnum);
    assert(dbnum < src_val->size / sizeof (int));

    int *pri_data = (int *) src_val->data;

    switch (dest_key->flags) {
    case 0:
        dest_key->size = sizeof (int);
        dest_key->data = &pri_data[dbnum];
        break;
    case DB_DBT_REALLOC:
        dest_key->size = sizeof (int);
        dest_key->data = toku_realloc(dest_key->data, dest_key->size);
        memcpy(dest_key->data, &pri_data[dbnum], dest_key->size);
        break;
    default:
        assert(0);
    }

    if (dest_val) {
        switch (dest_val->flags) {
        case 0:
            if (dbnum == 0) {
                dest_val->size = src_val->size;
                dest_val->data = src_val->data;
            } else {
                dest_val->size = 0;
            }
            break;
        case DB_DBT_REALLOC:
            assert(0);
        default:
            assert(0);
        }
    }
    
    return 0;
}

static int
del_callback(DB *dest_db, DB *src_db, DBT_ARRAY *dest_keys, const DBT *src_key, const DBT *src_data) {
    return put_callback(dest_db, src_db, dest_keys, NULL, src_key, src_data);
}

static void
run_test(int ndbs, int nrows) {
    int r;
    toku_os_recursive_delete(TOKU_TEST_FILENAME);
    r = toku_os_mkdir(TOKU_TEST_FILENAME, S_IRWXU+S_IRWXG+S_IRWXO); assert_zero(r);

    DB_ENV *env;
    r = db_env_create(&env, 0);                                                         assert_zero(r);
    r = env->set_generate_row_callback_for_put(env, put_callback);                      assert_zero(r);
    r = env->set_generate_row_callback_for_del(env, del_callback);                      assert_zero(r);
    r = env->open(env, TOKU_TEST_FILENAME, envflags, S_IRWXU+S_IRWXG+S_IRWXO);                      assert_zero(r);

    DB *db[ndbs];
    for (int dbnum = 0; dbnum < ndbs; dbnum++) {
        r = db_create(&db[dbnum], env, 0);                                                        
        assert_zero(r);
        DBT dbt_dbnum; dbt_init(&dbt_dbnum, &dbnum, sizeof dbnum);
        assert_zero(r);
        char dbname[32]; sprintf(dbname, "%d.tdb", dbnum);
        r = db[dbnum]->open(db[dbnum], NULL, dbname, NULL, DB_BTREE, DB_AUTO_COMMIT|DB_CREATE, 0666);    
        assert_zero(r);
        IN_TXN_COMMIT(env, NULL, txn_desc, 0, {
                { int chk_r = db[dbnum]->change_descriptor(db[dbnum], txn_desc, &dbt_dbnum, 0); CKERR(chk_r); }
        });
    }

    r = env->txn_checkpoint(env, 0, 0, 0);                                              assert_zero(r);

    DB_TXN *txn;
    r = env->txn_begin(env, NULL, &txn, 0);                                             assert_zero(r);

    for (int i = 0; i < nrows; i++) {
        int k = get_key(i, 0);
        int v[ndbs]; get_data(v, i, ndbs);
        DBT pri_key; dbt_init(&pri_key, &k, sizeof k);
        DBT pri_val; dbt_init(&pri_val, &v[0], sizeof v);
        DBT keys[ndbs]; memset(keys, 0, sizeof keys);
        DBT vals[ndbs]; memset(vals, 0, sizeof vals);
        uint32_t flags[ndbs]; memset(flags, 0, sizeof flags);
        r = env_put_multiple_test_no_array(env, NULL, txn, &pri_key, &pri_val, ndbs, db, keys, vals, flags); 
        assert_zero(r);
    }

    toku_hard_crash_on_purpose();
}


static void
verify_empty(DB_ENV *env, DB *db) {
    int r;
    DB_TXN *txn = NULL;
    r = env->txn_begin(env, NULL, &txn, 0); assert_zero(r);

    DBC *cursor = NULL;
    r = db->cursor(db, txn, &cursor, 0); assert_zero(r);

    DBT key; memset(&key, 0, sizeof key);
    DBT val; memset(&val, 0, sizeof val);
    r = cursor->c_get(cursor, &key, &val, DB_NEXT);
    assert(r == DB_NOTFOUND);

    r = cursor->c_close(cursor); assert_zero(r);
    r = txn->commit(txn, 0); assert_zero(r);
}

static void
verify_all(DB_ENV *env, int ndbs) {
    int r;
    for (int dbnum = 0; dbnum < ndbs; dbnum++) {
        DB *db = NULL;
        r = db_create(&db, env, 0);                                                        
        assert_zero(r);
        char dbname[32]; sprintf(dbname, "%d.tdb", dbnum);
        r = db->open(db, NULL, dbname, NULL, DB_BTREE, DB_AUTO_COMMIT|DB_CREATE, 0666);    
        assert_zero(r);
        verify_empty(env, db);
        r = db->close(db, 0); 
        assert_zero(r);
    }
}

static void
run_recover(int ndbs, int UU(nrows)) {
    int r;

    DB_ENV *env;
    r = db_env_create(&env, 0);                                                             assert_zero(r);
    r = env->set_generate_row_callback_for_put(env, put_callback);                          assert_zero(r);
    r = env->set_generate_row_callback_for_del(env, del_callback);                          assert_zero(r);
    r = env->open(env, TOKU_TEST_FILENAME, envflags|DB_RECOVER, S_IRWXU+S_IRWXG+S_IRWXO);               assert_zero(r);
    verify_all(env, ndbs);
    r = env->close(env, 0);                                                                 assert_zero(r);
}

static int
usage(void) {
    return 1;
}

int
test_main (int argc, char * const argv[]) {
    bool do_test = false;
    bool do_recover = false;
    int ndbs = 2;
    int nrows = 1;

    for (int i = 1; i < argc; i++) {
        char * const arg = argv[i];
        if (strcmp(arg, "-v") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(arg, "-q") == 0) {
            verbose--;
            if (verbose < 0)
                verbose = 0;
            continue;
        }
        if (strcmp(arg, "--test") == 0) {
            do_test = true;
            continue;
        }
        if (strcmp(arg, "--recover") == 0) {
            do_recover = true;
            continue;
        }
        if (strcmp(arg, "--ndbs") == 0 && i+1 < argc) {
            ndbs = atoi(argv[++i]);
            continue;
        }
        if (strcmp(arg, "--nrows") == 0 && i+1 < argc) {
            nrows = atoi(argv[++i]);
            continue;
        }
        if (strcmp(arg, "--help") == 0) {
            return usage();
        }
    }
    
    if (do_test)
        run_test(ndbs, nrows);
    if (do_recover)
        run_recover(ndbs, nrows);

    return 0;
}
