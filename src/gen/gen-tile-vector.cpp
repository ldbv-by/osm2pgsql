/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2025 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "gen-tile-vector.hpp"

#include "logging.hpp"
#include "params.hpp"
#include "pgsql.hpp"
#include "tile.hpp"

gen_tile_vector_union_t::gen_tile_vector_union_t(pg_conn_t *connection,
                                                 bool append, params_t *params)
: gen_tile_t(connection, append, params),
  m_timer_simplify(add_timer("simplify"))
{
    check_src_dest_table_params_exist();

    if (!get_params().has("margin")) {
        params->set("margin", 0.0);
    } else {
        // We don't need the result, just checking that this is a real number
        get_params().get_double("margin");
    }

    if (!get_params().has("buffer_size")) {
        params->set("buffer_size", static_cast<int64_t>(10));
    } else {
        // We don't need the result, just checking that this is an integer
        get_params().get_int64("buffer_size");
    }

    if (with_group_by()) {
        dbprepare("gen_geoms", R"(
WITH gen_tile_input AS (
 SELECT "{group_by_column}" AS col, "{geom_column}" AS geom FROM {src}
  WHERE "{geom_column}" &&
        ST_TileEnvelope($1::int, $2::int, $3::int, margin => {margin})
),
buffered AS (
 SELECT col, ST_Buffer(geom, {buffer_size}) AS geom
  FROM gen_tile_input
),
merged AS (
 SELECT col, ST_Union(geom) AS geom
  FROM buffered GROUP BY col
),
unbuffered AS (
 SELECT col, ST_Buffer(ST_Buffer(geom, -2 * {buffer_size}), {buffer_size}) AS geom
  FROM merged
)
INSERT INTO {dest} (x, y, "{group_by_column}", "{geom_column}")
 SELECT $2::int, $3::int, col, (ST_Dump(geom)).geom FROM unbuffered
)");
    } else {
        dbprepare("gen_geoms", R"(
WITH gen_tile_input AS (
 SELECT "{geom_column}" AS geom FROM {src}
  WHERE "{geom_column}" &&
        ST_TileEnvelope($1::int, $2::int, $3::int, margin => {margin})
),
buffered AS (
 SELECT ST_Buffer(geom, {buffer_size}) AS geom
  FROM gen_tile_input
),
merged AS (
 SELECT ST_Union(geom) AS geom
  FROM buffered
),
unbuffered AS (
 SELECT ST_Buffer(ST_Buffer(geom, -2 * {buffer_size}), {buffer_size}) AS geom
  FROM merged
)
INSERT INTO {dest} (x, y, "{geom_column}")
 SELECT $2::int, $3::int, (ST_Dump(geom)).geom FROM unbuffered
)");
    }
}

void gen_tile_vector_union_t::process(tile_t const &tile)
{
    connection().exec("BEGIN");
    delete_existing(tile);

    log_gen("Generalize...");
    timer(m_timer_simplify).start();
    auto const result = connection().exec_prepared("gen_geoms", tile.zoom(),
                                                   tile.x(), tile.y());
    connection().exec("COMMIT");
    timer(m_timer_simplify).stop();
    log_gen("Inserted {} generalized polygons", result.affected_rows());
}

void gen_tile_vector_union_t::post()
{
    if (!append_mode()) {
        dbexec("ANALYZE {dest}");
    }
}
