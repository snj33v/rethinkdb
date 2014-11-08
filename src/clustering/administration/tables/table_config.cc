// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "clustering/administration/tables/table_config.hpp"

#include "clustering/administration/datum_adapter.hpp"
#include "clustering/administration/metadata.hpp"
#include "clustering/administration/tables/generate_config.hpp"
#include "clustering/administration/tables/split_points.hpp"
#include "concurrency/cross_thread_signal.hpp"

ql::datum_t convert_table_config_shard_to_datum(
        const table_config_t::shard_t &shard,
        admin_identifier_format_t identifier_format,
        server_name_client_t *name_client) {
    ql::datum_object_builder_t builder;

    ql::datum_array_builder_t replicas_builder(ql::configured_limits_t::unlimited);
    for (const server_id_t &replica : shard.replicas) {
        ql::datum_t replica;
        /* This will return `false` for replicas that have been permanently removed */
        if (convert_server_id_to_datum(
                replica, identifier_format, name_client, &replica)) {
            replicas_builder.add(replica);
        }
    }
    builder.overwrite("replicas", std::move(replicas_builder).to_datum());

    ql::datum_t director;
    if (!convert_server_id_to_datum(
            shard.director, identifier_format, name_client, &director)) {
        /* If the previous director was declared dead, just display `null`. The user will
        have to change this to a new server before the table will come back online. */
        director = ql::datum_t::null();
    }
    builder.overwrite("director", director);

    return std::move(builder).to_datum();
}

bool convert_table_config_shard_from_datum(
        ql::datum_t datum,
        admin_identifier_format_t identifier_format,
        server_name_client_t *name_client,
        table_config_t::shard_t *shard_out,
        std::string *error_out) {
    converter_from_datum_object_t converter;
    if (!converter.init(datum, error_out)) {
        return false;
    }

    ql::datum_t replicas_datum;
    if (!converter.get("replicas", &replicas_datum, error_out)) {
        return false;
    }
    if (replicas_datum.get_type() != ql::datum_t::R_ARRAY) {
        *error_out = "In `replicas`: Expected an array, got " +
            replica_names_datum.print();
        return false;
    }
    shard_out->replicas.clear();
    for (size_t i = 0; i < replicas_datum.arr_size(); ++i) {
        server_id_t server_id;
        if (!convert_server_id_from_datum(
                replicas_datum.get(i), identifier_format, name_client,
                &server_id, error_out)) {
            *error_out = "In `replicas`: " + *error_out;
            return false;
        }
        auto pair = shard_out->replicas.insert(server_id);
        if (!pair.second) {
            *error_out = "In `replicas`: A server is listed more than once.";
            return false;
        }
    }
    if (shard_out->replicas.empty()) {
        *error_out = "You must specify at least one replica for each shard.";
        return false;
    }

    ql::datum_t director_datum;
    if (!converter.get("director", &director_datum, error_out)) {
        return false;
    }
    if (director_datum.get_type() == ql::datum_t::R_NULL) {
        /* There's never a good reason for the user to intentionally set the director to
        `null`; setting the director to `null` will ensure that the table cannot accept
        queries. We allow it because if the director is declared dead, it will appear to
        the user as `null`; and we want to allow the user to do things like
        `r.table_config("foo").update({"name": "bar"})` even when the director is in that
        state. */
        shard_out->director = nil_uuid();
    } else {
        if (!convert_server_id_from_datum(director_datum, identifier_format, name_client,
                &shard_out->director, error_out)) {
            *error_out = "In `director`: " + *error_out;
            return false;
        }
        if (shard_out->replicas.count(shard_out->director) != 1) {
            *error_out = "The director must be one of the replicas.";
            return false;
        }
    }

    if (!converter.check_no_extra_keys(error_out)) {
        return false;
    }

    return true;
}

/* This is separate from `format_row()` because it needs to be publicly exposed so it can
   be used to create the return value of `table.reconfigure()`. */
ql::datum_t convert_table_config_to_datum(
        const table_config_t &config,
        admin_identifier_format_t identifier_format,
        server_name_client_t *name_client) {
    ql::datum_object_builder_t builder;
    builder.overwrite("shards",
        convert_vector_to_datum<table_config_t::shard_t>(
            [&](const table_config_t::shard_t &shard) {
                return convert_table_config_shard_to_datum(
                    shard, identifier_format, name_client);
            },
            config.shards));
    return std::move(builder).to_datum();
}

bool table_config_artificial_table_backend_t::format_row(
        namespace_id_t table_id,
        name_string_t table_name,
        const ql::datum_t &db,
        const namespace_semilattice_metadata_t &metadata,
        UNUSED signal_t *interruptor,
        ql::datum_t *row_out,
        UNUSED std::string *error_out) {
    assert_thread();

    ql::datum_t start = convert_table_config_to_datum(
        metadata.replication_info.get_ref().config, identifier_format, name_client);
    ql::datum_object_builder_t builder(start);
    builder.overwrite("name", convert_name_to_datum(table_name));
    builder.overwrite("db", db);
    builder.overwrite("id", convert_uuid_to_datum(table_id));
    builder.overwrite(
        "primary_key", convert_string_to_datum(metadata.primary_key.get_ref()));
    *row_out = std::move(builder).to_datum();

    return true;
}

bool convert_table_config_and_name_from_datum(
        ql::datum_t datum,
        bool existed_before,
        const namespaces_semilattice_metadata_t &all_table_metadata,
        admin_identifier_format_t identifier_format,
        server_name_client_t *name_client,
        signal_t *interruptor,
        name_string_t *table_name_out,
        ql::datum_t *db_out,
        namespace_id_t *id_out,
        table_config_t *config_out,
        std::string *primary_key_out,
        std::string *error_out) {
    /* In practice, the input will always be an object and the `id` field will always
    be valid, because `artificial_table_t` will check those thing before passing the
    row to `table_config_artificial_table_backend_t`. But we check them anyway for
    consistency. */
    converter_from_datum_object_t converter;
    if (!converter.init(datum, error_out)) {
        return false;
    }

    ql::datum_t name_datum;
    if (!converter.get("name", &name_datum, error_out)) {
        return false;
    }
    if (!convert_name_from_datum(name_datum, "table name", table_name_out, error_out)) {
        *error_out = "In `name`: " + *error_out;
        return false;
    }

    if (!converter.get("db", db_out, error_out)) {
        return false;
    }

    ql::datum_t id_datum;
    if (!converter.get("id", &id_datum, error_out)) {
        return false;
    }
    if (!convert_uuid_from_datum(id_datum, id_out, error_out)) {
        *error_out = "In `id`: " + *error_out;
        return false;
    }

    if (existed_before || converter.has("primary_key")) {
        ql::datum_t primary_key_datum;
        if (!converter.get("primary_key", &primary_key_datum, error_out)) {
            return false;
        }
        if (!convert_string_from_datum(primary_key_datum, primary_key_out, error_out)) {
            *error_out = "In `primary_key`: " + *error_out;
            return false;
        }
    } else {
        *primary_key_out = "id";
    }

    if (existed_before || converter.has("shards")) {
        ql::datum_t shards_datum;
        if (!converter.get("shards", &shards_datum, error_out)) {
            return false;
        }
        if (!convert_vector_from_datum<table_config_t::shard_t>(
                [&](ql::datum_t shard_datum, table_config_t::shard_t *shard_out,
                        std::string *error_out_2) {
                    return convert_table_config_shard_from_datum(
                        shard_datum, identifier_format, name_client,
                        shard_out, error_out_2);
                },
                shards_datum,
                &config_out->shards,
                error_out)) {
            *error_out = "In `shards`: " + *error_out;
            return false;
        }
        if (config_out->shards.empty()) {
            *error_out = "In `shards`: You must specify at least one shard.";
            return false;
        }
    } else {
        std::map<server_id_t, int> server_usage;
        for (const auto &pair : all_table_metadata.namespaces) {
            if (pair.second.is_deleted()) {
                continue;
            }
            calculate_server_usage(
                pair.second.get_ref().replication_info.get_ref().config, &server_usage);
        }
        if (!table_generate_config(
                name_client, nil_uuid(), nullptr, server_usage,
                table_generate_config_params_t::make_default(), table_shard_scheme_t(),
                interruptor, config_out, error_out)) {
            *error_out = "When generating configuration for new table: " + *error_out;
            return false;
        }
    }

    if (!converter.check_no_extra_keys(error_out)) {
        return false;
    }

    return true;
}

bool table_config_artificial_table_backend_t::write_row(
        ql::datum_t primary_key,
        bool pkey_was_autogenerated,
        ql::datum_t *new_value_inout,
        signal_t *interruptor,
        std::string *error_out) {
    cross_thread_signal_t interruptor2(interruptor, home_thread());
    on_thread_t thread_switcher(home_thread());

    /* Look for an existing table with the given UUID */
    cow_ptr_t<namespaces_semilattice_metadata_t> md = table_sl_view->get();
    namespace_id_t table_id;
    std::string dummy_error;
    if (!convert_uuid_from_datum(primary_key, &table_id, &dummy_error)) {
        /* If the primary key was not a valid UUID, then it must refer to a nonexistent
        row. */
        guarantee(!pkey_was_autogenerated, "auto-generated primary key should have been "
            "a valid UUID string.");
        table_id = nil_uuid();
    }
    cow_ptr_t<namespaces_semilattice_metadata_t>::change_t md_change(&md);
    std::map<namespace_id_t, deletable_t<namespace_semilattice_metadata_t> >
        ::iterator it;
    bool existed_before = search_metadata_by_uuid(
        &md_change.get()->namespaces, table_id, &it);

    if (new_value_inout->has()) {
        /* We're updating an existing table (if `existed_before == true`) or creating
        a new one (if `existed_before == false`) */

        /* Parse the new value the user provided for the table */
        table_replication_info_t replication_info;
        name_string_t new_table_name;
        ql::datum_t new_db;
        namespace_id_t new_table_id;
        std::string new_primary_key;
        if (!convert_table_config_and_name_from_datum(*new_value_inout, existed_before,
                *md_change.get(), identifier_format, name_client, interruptor,
                &new_table_name, &new_db, &new_table_id,
                &replication_info.config, &new_primary_key, error_out)) {
            *error_out = "The change you're trying to make to "
                "`rethinkdb.table_config` has the wrong format. " + *error_out;
            return false;
        }
        guarantee(new_table_id == table_id, "artificial_table_t should ensure that the "
            "primary key doesn't change.");

        if (existed_before) {
            guarantee(!pkey_was_autogenerated, "UUID collision happened");
        } else {
            if (!pkey_was_autogenerated) {
                *error_out = "If you want to create a new table by inserting into "
                    "`rethinkdb.table_config`, you must use an auto-generated primary "
                    "key.";
                return false;
            }
            /* Assert that we didn't randomly generate the UUID of a table that used to
            exist but was deleted */
            guarantee(md_change.get()->namespaces.count(table_id) == 0,
                "UUID collision happened");
        }

        /* The way we handle the `db` field is a bit convoluted, but for good reason. If
        we're updating an existing table, we require that the DB field is the same as it
        is before. By not looking up the DB's UUID, we avoid any problems if there is a
        DB name collision or if the DB was deleted. If we're creating a new table, only
        then do we actually look up the DB's UUID. */
        database_id_t db_id;
        if (existed_before) {
            db_id = it->second.get_ref().database.get_ref();
            if (new_db != get_db_identifier(db_id)) {
                *error_out = "It's illegal to change a table's `database` field.";
                return false;
            }
        } else {
            databases_semilattice_metadata_t db_md = database_sl_view->get();
            if (!convert_database_id_from_datum(
                    new_db, identifier_format, db_md, &db_id, error_out)) {
                return false;
            }
        }

        if (existed_before) {
            if (new_primary_key != it->second.get_ref().primary_key.get_ref()) {
                *error_out = "It's illegal to change a table's primary key.";
                return false;
            }
        }

        /* Decide on the sharding scheme for the table */
        if (existed_before) {
            table_replication_info_t prev =
                it->second.get_mutable()->replication_info.get_ref();
            if (!calculate_split_points_intelligently(table_id, reql_cluster_interface,
                    replication_info.config.shards.size(), prev.shard_scheme,
                    &interruptor2, &replication_info.shard_scheme, error_out)) {
                return false;
            }
        } else {
            if (replication_info.config.shards.size() != 1) {
                *error_out = "Newly created tables must start with exactly one shard";
                return false;
            }
            replication_info.shard_scheme = table_shard_scheme_t::one_shard();
        }

        name_string_t old_table_name;
        if (existed_before) {
            old_table_name = it->second.get_ref().name.get_ref();
        }

        if (!existed_before || new_table_name != old_table_name) {
            /* Prevent name collisions if possible */
            metadata_searcher_t<namespace_semilattice_metadata_t> ns_searcher(
                &md_change.get()->namespaces);
            metadata_search_status_t status;
            namespace_predicate_t pred(&new_table_name, &db_id);
            ns_searcher.find_uniq(pred, &status);
            if (status != METADATA_ERR_NONE) {
                if (!existed_before) {
                    /* This message looks weird in the context of the variable named
                    `existed_before`, but it's correct. `existed_before` is true if a
                    table with the specified UUID already exists; but we're showing the
                    user an error if a table with the specified name already exists. */
                    *error_out = strprintf("Table `%s.%s` already exists.",
                        new_db_name.c_str(), new_table_name.c_str());
                } else {
                    *error_out = strprintf("Cannot rename table `%s.%s` to `%s.%s` "
                        "because table `%s.%s` already exists.", new_db_name.c_str(),
                        old_table_name.c_str(), new_db_name.c_str(),
                        new_table_name.c_str(), new_db_name.c_str(),
                        new_table_name.c_str());
                }
                return false;
            }
        }

        /* Update `md`. The change will be committed to the semilattices at the end of
        this function. */
        if (existed_before) {
            it->second.get_mutable()->name.set(new_table_name);
            it->second.get_mutable()->replication_info.set(replication_info);
        } else {
            namespace_semilattice_metadata_t table_md;
            table_md.name = versioned_t<name_string_t>(new_table_name);
            table_md.database = versioned_t<database_id_t>(db_id);
            table_md.primary_key = versioned_t<std::string>(new_primary_key);
            table_md.replication_info =
                versioned_t<table_replication_info_t>(replication_info);
            md_change.get()->namespaces[table_id] =
                deletable_t<namespace_semilattice_metadata_t>(table_md);
        }

        /* Because we might have filled in the `primary_key` and `shards` fields, we need
        to write back to `new_value_inout` */
        if (!format_row(table_id, new_table_name, new_db_name,
                md_change.get()->namespaces[table_id].get_ref(),
                interruptor, new_value_inout, error_out)) {
            return false;
        }

    } else {
        /* We're deleting a table (or it was already deleted) */
        if (existed_before) {
            guarantee(!pkey_was_autogenerated, "UUID collision happened");
            it->second.mark_deleted();
        }
    }

    table_sl_view->join(md);

    return true;
}


